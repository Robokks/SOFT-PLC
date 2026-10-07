#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include "softplc/runtime/program_support.hpp"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
using namespace softplc;
using Clock=std::chrono::steady_clock;
std::string q(const std::string& text) {
    std::string out="\"";
    for(unsigned char c:text) {
        if(c=='"'||c=='\\') {out+='\\';out+=char(c);}
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<32) { const char* hex="0123456789abcdef";out+="\\u00";out+=hex[c>>4];out+=hex[c&15]; }
        else out+=char(c);
    }
    return out+'"';
}
uint32_t bytes(uint32_t type) {
    static constexpr uint32_t sizes[]={1,1,2,4,4,8,8,256};
    if(type>PLC_STRING) throw std::runtime_error("invalid type");return sizes[type];
}
void validateValue(const PlcValue& v) {
    bytes(v.type);
    if((v.type==PLC_BOOL && v.integer!=0 && v.integer!=1) ||
       (v.type==PLC_BYTE && (v.integer<0 || v.integer>255)) ||
       (v.type==PLC_INT && (v.integer<-32768 || v.integer>32767)) ||
       (v.type==PLC_DINT && (v.integer<INT32_MIN || v.integer>INT32_MAX))) throw std::runtime_error("integer out of range");
    if((v.type==PLC_REAL||v.type==PLC_LREAL) && (!std::isfinite(v.real) || (v.type==PLC_REAL && std::abs(v.real)>std::numeric_limits<float>::max()))) throw std::runtime_error("real out of range");
    if(v.type==PLC_STRING && !std::memchr(v.text,0,255)) throw std::runtime_error("STRING must be terminated within 255 bytes");
}
struct Module {
    void* handle=nullptr;const PlcProgramV1* program=nullptr;
    explicit Module(const std::string& path) {
        if(!std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())).is_absolute()) throw std::runtime_error("module path must be absolute");
#if defined(_WIN32)
        handle=LoadLibraryExW(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto getter=handle?reinterpret_cast<PlcProgramGetter>(GetProcAddress(static_cast<HMODULE>(handle),"softplc_program_v1")):nullptr;
#else
        handle=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
        auto getter=handle?reinterpret_cast<PlcProgramGetter>(dlsym(handle,"softplc_program_v1")):nullptr;
#endif
        if(!getter) { close();throw std::runtime_error("cannot load module or softplc_program_v1 export"); }
        program=getter();
        if(!program || program->abi_version!=PLC_ABI_VERSION || program->struct_size!=sizeof(PlcProgramV1) ||
           !program->tags || !program->tasks || program->tag_count>10000 || program->task_count==0 || program->task_count>128 || program->network_count>10000) {
            close();throw std::runtime_error("incompatible or invalid program descriptor");
        }
    }
    Module(const Module&)=delete;
    ~Module(){close();}
    void close(){if(!handle)return;
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
        handle=nullptr;
    }
};
struct Memory {
    const PlcProgramV1* p;tags::TagStore store;
    std::map<std::pair<uint32_t,uint32_t>,std::vector<uint8_t>> images;
    std::unordered_map<std::string,uint32_t> names;
    explicit Memory(const PlcProgramV1* p):p(p) {
        uint64_t total=0;
        for(uint32_t i=0;i<p->tag_count;i++) {
            auto& d=p->tags[i];
            if(!d.name || d.type>PLC_STRING || d.area>PLC_DB || d.initial.type!=d.type || d.bit < -1 || d.bit>7 || (d.bit>=0 && d.type!=PLC_BOOL)) throw std::runtime_error("invalid tag descriptor");
            validateValue(d.initial);
            if(!names.emplace(d.name,i).second) throw std::runtime_error("duplicate tag");
            store.declare(d.name,static_cast<tags::TypeId>(d.type),compiled::fromAbi(d.initial));
            if(d.area) {
                if(d.offset>1048576 || d.offset+bytes(d.type)>1048576) throw std::runtime_error("DB exceeds 1 MiB");
                auto& data=images[{d.area,d.area==PLC_DB?d.db:0}];
                data.resize(std::max(data.size(),size_t(d.offset+bytes(d.type))));
            }
        }
        for(auto& [key,data]:images) total+=data.size();
        if(total>64*1048576ULL) throw std::runtime_error("memory image exceeds 64 MiB");
        reset();
    }
    void reset(){for(auto& [key,data]:images) std::fill(data.begin(),data.end(),0);for(uint32_t i=0;i<p->tag_count;i++)write(i,p->tags[i].initial);}
    std::vector<uint8_t>& image(uint32_t area,uint32_t db,uint32_t offset,uint32_t type,int32_t bit){
        auto it=images.find({area,area==PLC_DB?db:0});
        if(it==images.end() || uint64_t(offset)+bytes(type)>it->second.size() || bit < -1 || bit>7 || (bit>=0 && type!=PLC_BOOL)) throw std::runtime_error("DB address out of bounds");
        return it->second;
    }
    PlcValue readAddress(uint32_t area,uint32_t db,uint32_t offset,int32_t bit,uint32_t type){
        auto& data=image(area,db,offset,type,bit);PlcValue v{};v.type=type;
        if(type==PLC_BOOL){v.integer=(data[offset]>>(bit<0?0:bit))&1;return v;}
        if(type==PLC_STRING){std::memcpy(v.text,data.data()+offset,255);v.text[255]=0;return v;}
        uint64_t n=0;for(uint32_t i=0;i<bytes(type);i++) n=(n<<8)|data[offset+i];
        switch(type){
            case PLC_BYTE:v.integer=n;break;
            case PLC_INT:v.integer=std::bit_cast<int16_t>(uint16_t(n));break;
            case PLC_DINT:v.integer=std::bit_cast<int32_t>(uint32_t(n));break;
            case PLC_TIME:v.integer=std::bit_cast<int64_t>(n);break;
            case PLC_REAL:v.real=std::bit_cast<float>(uint32_t(n));break;
            case PLC_LREAL:v.real=std::bit_cast<double>(n);break;
        }
        validateValue(v);return v;
    }
    void writeAddress(uint32_t area,uint32_t db,uint32_t offset,int32_t bit,const PlcValue& v){
        validateValue(v);auto& data=image(area,db,offset,v.type,bit);
        if(v.type==PLC_BOOL){const auto mask=uint8_t(1<<(bit<0?0:bit));data[offset]=uint8_t((data[offset]&~mask)|(v.integer?mask:0));return;}
        if(v.type==PLC_STRING){std::fill_n(data.data()+offset,256,0);std::memcpy(data.data()+offset,v.text,std::strlen(v.text));return;}
        uint64_t n=uint64_t(v.integer);
        if(v.type==PLC_REAL)n=std::bit_cast<uint32_t>(float(v.real));
        if(v.type==PLC_LREAL)n=std::bit_cast<uint64_t>(v.real);
        for(uint32_t i=0;i<bytes(v.type);i++) data[offset+bytes(v.type)-i-1]=uint8_t(n>>(8*i));
    }
    PlcValue read(uint32_t id){if(id>=p->tag_count)throw std::runtime_error("invalid tag ID");auto& d=p->tags[id];return d.area?readAddress(d.area,d.db,d.offset,d.bit,d.type):compiled::toAbi(store.read(id));}
    void write(uint32_t id,const PlcValue& v){
        if(id>=p->tag_count || p->tags[id].type!=v.type)throw std::runtime_error("tag type mismatch");validateValue(v);auto& d=p->tags[id];
        if(d.area)writeAddress(d.area,d.db,d.offset,d.bit,v);else store.write(id,compiled::fromAbi(v));
    }
    void safeOutputs(){for(auto& [key,data]:images)if(key.first==PLC_OUTPUT)std::fill(data.begin(),data.end(),0);}
};
struct TaskState {
    const PlcTaskDef* def;Clock::time_point next{},last{},started{};
    bool active=false;uint64_t cycles=0,missed=0,overruns=0,last_us=0,max_us=0,jitter_us=0;
};
struct Trace {uint64_t count=0;bool power=false;};
struct Runtime {
    std::recursive_mutex mutex;std::atomic<bool> cancel{false};std::unique_ptr<Module> module;std::unique_ptr<Memory> memory;
    std::vector<TaskState> tasks;std::map<std::pair<uint32_t,std::string>,Trace> traces;
    uint64_t ioEpoch=0;std::string state="EMPTY",error;bool startupPending=false;int timeTag=-1;std::vector<size_t> stack;
    static Runtime& self(PlcContext* c){return *static_cast<Runtime*>(c->user);}
    void fault(const std::string& message){if(state!="FAULT")++ioEpoch;if(error.empty())error=message;state="FAULT";cancel=true;if(memory)memory->safeOutputs();}
    template<class F> static int32_t protect(PlcContext* c,F fn){try{fn(self(c));return 0;}catch(const std::exception& e){self(c).fault(e.what());return -1;}}
    static const PlcHostApi api;

    void load(const std::string& path){
        if(state=="RUN")throw std::runtime_error("STOP before loading a program");
        auto candidate=std::make_unique<Module>(path);auto mem=std::make_unique<Memory>(candidate->program);
        std::vector<TaskState> next;std::set<uint32_t> ids;unsigned mainCount=0;
        for(uint32_t i=0;i<candidate->program->task_count;i++){
            auto& t=candidate->program->tasks[i];
            if(!t.entry || !t.name || !ids.insert(t.ob).second || t.kind>PLC_CYCLIC || t.period_us<1000 || t.period_us>60000000 || t.watchdog_us<1000 || t.watchdog_us>60000000 || t.priority>99)throw std::runtime_error("invalid OB configuration");
            if(t.kind==PLC_MAIN){mainCount++;if(t.ob!=1)throw std::runtime_error("main task must be OB1");}
            next.push_back(TaskState{&t});
        }
        if(mainCount!=1)throw std::runtime_error("exactly one main OB1 is required");
        ++ioEpoch;memory.reset();module=std::move(candidate);memory=std::move(mem);tasks=std::move(next);traces.clear();stack.clear();
        auto it=memory->names.find("System.CycleTime");timeTag=it==memory->names.end()?-1:int(it->second);error.clear();state="STOP";cancel=false;memory->safeOutputs();
    }
    void start(){if(state!="STOP")throw std::runtime_error("RUN requires a loaded, stopped program (RESET after a fault)");cancel=false;error.clear();auto now=Clock::now();for(auto&t:tasks){t.last={};t.next=now+(t.def->kind==PLC_CYCLIC?std::chrono::microseconds(t.def->period_us):std::chrono::microseconds(0));}startupPending=true;state="RUN";++ioEpoch;}
    void stop(){++ioEpoch;if(memory)memory->safeOutputs();if(state!="EMPTY"&&state!="FAULT")state="STOP";}
    int32_t checkpoint(){
        if(cancel)return -1;auto now=Clock::now();
        for(auto i:stack)if(std::chrono::duration_cast<std::chrono::microseconds>(now-tasks[i].started).count()>int64_t(tasks[i].def->watchdog_us)){fault("OB"+std::to_string(tasks[i].def->ob)+" watchdog exceeded");return -1;}
        if(state=="RUN"&&!stack.empty()&&tasks[stack.back()].def->kind!=PLC_STARTUP)serviceDue(tasks[stack.back()].def->priority,true);
        return cancel?-1:0;
    }
    void dispatch(size_t i){
        auto& t=tasks[i];auto now=Clock::now();auto elapsed=t.last==Clock::time_point{}?t.def->period_us:uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now-t.last).count());
        t.last=now;t.started=now;t.active=true;
        if(t.def->kind!=PLC_STARTUP){
            if(now>=t.next){auto late=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now-t.next).count());t.jitter_us=std::max(t.jitter_us,late);auto skips=late/t.def->period_us;t.missed+=skips;t.next+=std::chrono::microseconds((skips+1)*t.def->period_us);}
        }
        PlcValue previous{};
        if(timeTag>=0){previous=memory->read(timeTag);PlcValue dt{};dt.type=PLC_TIME;dt.integer=int64_t(elapsed/1000);memory->write(timeTag,dt);}
        stack.push_back(i);PlcContext ctx{&api,this,elapsed,""};
        int32_t result=t.def->entry(&ctx);
        stack.pop_back();t.active=false;
        if(timeTag>=0&&!stack.empty())memory->write(timeTag,previous);
        t.last_us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-now).count());t.max_us=std::max(t.max_us,t.last_us);t.cycles++;
        if(t.last_us>t.def->period_us)t.overruns++;
        if(t.last_us>t.def->watchdog_us && error.empty())fault(std::string(t.def->name)+" watchdog exceeded");
        if(result!=0&&!cancel)fault(std::string(t.def->name)+" execution failed");
        if(cancel&&memory)memory->safeOutputs();
    }
    void serviceDue(uint32_t above,bool cyclicOnly){
        std::vector<size_t> due;
        for(size_t i=0;i<tasks.size();i++){auto&t=tasks[i];if(!t.active&&t.def->kind!=PLC_STARTUP&&(!cyclicOnly||t.def->kind==PLC_CYCLIC)&&(!cyclicOnly||t.def->priority>above)&&Clock::now()>=t.next)due.push_back(i);}
        std::stable_sort(due.begin(),due.end(),[&](size_t a,size_t b){return tasks[a].def->priority>tasks[b].def->priority;});
        for(auto i:due){if(cancel)break;if(Clock::now()>=tasks[i].next)dispatch(i);}
    }
    void tick(){
        if(state!="RUN"||cancel)return;
        if(startupPending){startupPending=false;for(size_t i=0;i<tasks.size();i++)if(tasks[i].def->kind==PLC_STARTUP&&!cancel)dispatch(i);}
        if(!cancel)serviceDue(0,false);
    }
    void step(){if(state!="STOP")throw std::runtime_error("STEP requires STOP");++ioEpoch;cancel=false;for(size_t i=0;i<tasks.size();i++)if(tasks[i].def->kind==PLC_MAIN){dispatch(i);break;}if(memory)memory->safeOutputs();}
    void reset(){if(state=="RUN")throw std::runtime_error("STOP before RESET");if(!memory)throw std::runtime_error("no program loaded");++ioEpoch;memory->reset();traces.clear();for(auto&t:tasks)t=TaskState{t.def};error.clear();state="STOP";cancel=false;memory->safeOutputs();}
    std::string snapshot(){
        std::ostringstream o;o.precision(17);o<<"{\"ok\":true,\"state\":"<<q(state)<<",\"io_epoch\":"<<ioEpoch<<",\"fault\":"<<q(error);
        if(module){auto*p=module->program;o<<",\"program\":"<<q(p->name)<<",\"build_id\":"<<q(p->build_id)<<",\"tasks\":[";
            for(size_t i=0;i<tasks.size();i++){auto&t=tasks[i];o<<(i?",":"")<<"{\"name\":"<<q(t.def->name)<<",\"ob\":"<<t.def->ob<<",\"period_us\":"<<t.def->period_us<<",\"priority\":"<<t.def->priority<<",\"cycles\":"<<t.cycles<<",\"last_us\":"<<t.last_us<<",\"max_us\":"<<t.max_us<<",\"missed\":"<<t.missed<<",\"overruns\":"<<t.overruns<<",\"jitter_us\":"<<t.jitter_us<<"}";}
            o<<"],\"tags\":[";bool first=true;
            for(uint32_t i=0;i<p->tag_count;i++){auto&d=p->tags[i];if(std::string(d.name).starts_with("__plc_")||std::string(d.name).find(".__plc_tmp_")!=std::string::npos)continue;auto v=memory->read(i);o<<(first?"":",")<<"{\"id\":"<<i<<",\"name\":"<<q(d.name)<<",\"type\":"<<q(tags::toString(static_cast<tags::TypeId>(d.type)))<<",\"type_id\":"<<d.type<<",\"area\":"<<d.area<<",\"address\":"<<q(d.address?d.address:"")<<",\"value\":";
                if(v.type==PLC_STRING)o<<q(v.text);else if(v.type==PLC_BOOL)o<<(v.integer?"true":"false");else if(v.type==PLC_REAL||v.type==PLC_LREAL)o<<v.real;else o<<v.integer;o<<"}";first=false;}
            o<<"],\"networks\":[";first=true;for(auto&[key,t]:traces){o<<(first?"":",")<<"{\"id\":"<<key.first<<",\"scope\":"<<q(key.second)<<",\"count\":"<<t.count<<",\"power\":"<<(t.power?"true":"false")<<"}";first=false;}o<<"]";
        }
        o<<"}";return o.str();
    }
};
const PlcHostApi Runtime::api={PLC_ABI_VERSION,
        [](PlcContext*c,uint32_t id,PlcValue*v){return protect(c,[&](Runtime&r){*v=r.memory->read(id);});},
        [](PlcContext*c,uint32_t id,const PlcValue*v){return protect(c,[&](Runtime&r){r.memory->write(id,*v);});},
        [](PlcContext*c,const char*name)->int32_t{auto&r=self(c);std::string scoped=std::string(c->scope?c->scope:"");if(!scoped.empty())scoped+='.';scoped+=name;auto it=r.memory->names.find(scoped);if(it==r.memory->names.end())it=r.memory->names.find(name);if(it==r.memory->names.end()){r.fault("unknown native tag: "+std::string(name));return -1;}return int32_t(it->second);},
        [](PlcContext*c,uint32_t db,uint32_t off,int32_t bit,uint32_t type,PlcValue*v){return protect(c,[&](Runtime&r){*v=r.memory->readAddress(PLC_DB,db,off,bit,type);});},
        [](PlcContext*c,uint32_t db,uint32_t off,int32_t bit,const PlcValue*v){return protect(c,[&](Runtime&r){r.memory->writeAddress(PLC_DB,db,off,bit,*v);});},
        [](PlcContext*c){return self(c).checkpoint();},
        [](PlcContext*c,uint32_t id,int32_t power,const char* scope)->int32_t{auto&r=self(c);if(r.checkpoint())return -1;auto&t=r.traces[{id,scope?scope:""}];t.count++;t.power=power!=0;return 0;},
        [](PlcContext*c,const char*message){self(c).fault(message);}
    };
int main(){
    Runtime runtime;
    std::jthread worker([&](std::stop_token st){while(!st.stop_requested()){
        {std::lock_guard lock(runtime.mutex);try{runtime.tick();}catch(const std::exception&e){runtime.fault(e.what());}catch(...){runtime.fault("unhandled runtime error");}}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }});
    std::string line;
    while(std::getline(std::cin,line)){
        if(line=="STOP"||line=="QUIT")runtime.cancel=true;
        std::lock_guard lock(runtime.mutex);
        try{
            auto tab=line.find('\t');auto cmd=line.substr(0,tab);auto arg=tab==std::string::npos?"":line.substr(tab+1);
            if(cmd=="LOAD")runtime.load(arg);
            else if(cmd=="RUN")runtime.start();
            else if(cmd=="STOP")runtime.stop();
            else if(cmd=="RESET")runtime.reset();
            else if(cmd=="STEP")runtime.step();
            else if(cmd=="IOWRITE"||cmd=="IOFAULT"){
                std::istringstream input(arg);std::string build;uint64_t epoch;
                if(!(input>>build>>epoch)||!runtime.module||build!=runtime.module->program->build_id||epoch!=runtime.ioEpoch)
                    throw std::runtime_error("stale I/O generation");
                if(cmd=="IOFAULT"){
                    std::string link;input>>link;if(runtime.state=="RUN")runtime.fault("I/O connection failed: "+link);
                }else{
                    uint32_t count;if(!(input>>count)||count>128)throw std::runtime_error("invalid I/O batch size");
                    std::vector<std::pair<uint32_t,PlcValue>> values;std::set<uint32_t> ids;
                    for(uint32_t i=0;i<count;i++){
                        uint32_t id;std::string text;
                        if(!(input>>id>>text)||id>=runtime.module->program->tag_count||!ids.insert(id).second)
                            throw std::runtime_error("invalid I/O tag ID");
                        auto& d=runtime.module->program->tags[id];std::string name=d.name;
                        if(d.area==PLC_OUTPUT||d.type==PLC_STRING||name.starts_with("System.")||name.starts_with("__plc_")||name.find(".__plc_")!=std::string::npos)
                            throw std::runtime_error("I/O input tag is not writable");
                        PlcValue v{};v.type=d.type;size_t consumed=0;
                        if(v.type==PLC_REAL||v.type==PLC_LREAL)v.real=std::stod(text,&consumed);else v.integer=std::stoll(text,&consumed);
                        if(consumed!=text.size())throw std::runtime_error("invalid I/O numeric value");
                        validateValue(v);values.emplace_back(id,v);
                    }
                    std::string extra;if(input>>extra)throw std::runtime_error("trailing I/O batch data");
                    for(auto& [id,value]:values)runtime.memory->write(id,value);
                }
                std::cout<<"{\"ok\":true}"<<std::endl;continue;
            }
            else if(cmd=="SET"){
                if(!runtime.memory)throw std::runtime_error("no program loaded");
                std::istringstream input(arg);uint32_t id;std::string value;if(!(input>>id>>value)||id>=runtime.module->program->tag_count)throw std::runtime_error("invalid write request");
                const auto& d=runtime.module->program->tags[id];
                if(std::string(d.name).starts_with("__plc_")||std::string(d.name)=="System.CycleTime")throw std::runtime_error("system tag is read-only");
                if(runtime.state=="FAULT")throw std::runtime_error("RESET after fault before writing");
                if(d.area==PLC_OUTPUT&&runtime.state!="RUN")throw std::runtime_error("outputs are held at zero in STOP");
                PlcValue v{};v.type=d.type;size_t consumed=0;
                if(v.type==PLC_REAL||v.type==PLC_LREAL)v.real=std::stod(value,&consumed);
                else if(v.type!=PLC_STRING)v.integer=std::stoll(value,&consumed);
                else throw std::runtime_error("online STRING writes are not supported");
                if(consumed!=value.size())throw std::runtime_error("invalid numeric value");runtime.memory->write(id,v);
            }
            else if(cmd=="QUIT"){runtime.stop();std::cout<<"{\"ok\":true}"<<std::endl;break;}
            else if(cmd!="STATUS")throw std::runtime_error("unknown command");
            std::cout<<runtime.snapshot()<<std::endl;
        }catch(const std::exception&e){std::cout<<"{\"ok\":false,\"error\":"<<q(e.what())<<"}"<<std::endl;}
    }
    runtime.cancel=true;worker.request_stop();worker.join();
}
