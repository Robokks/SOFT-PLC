#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include "softplc/st/st_program.hpp"
#include "softplc/st/value_ops.hpp"

using namespace softplc;
using namespace softplc::st;
std::string quoted(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (c < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else o << c;
    }
    return o.str() + '"';
}
std::string literal(const tags::Value& value) {
    return std::visit([](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        std::ostringstream o; o << std::setprecision(17);
        if constexpr (std::is_same_v<T, bool>) return x ? "Value(true)" : "Value(false)";
        else if constexpr (std::is_same_v<T, std::string>) return "Value(std::string(" + quoted(x) + "))";
        else if constexpr (std::is_same_v<T, tags::TimeValue>) return "Value(tags::TimeValue(" + std::to_string(x.count()) + "LL))";
        else if constexpr (std::is_same_v<T, float>) { o << "Value(float(" << x << "))"; }
        else if constexpr (std::is_same_v<T, double>) { o << "Value(double(" << x << "))"; }
        else if constexpr (std::is_same_v<T, uint8_t>) { o << "Value(uint8_t(" << int(x) << "))"; }
        else if constexpr (std::is_same_v<T, int16_t>) { o << "Value(int16_t(" << x << "))"; }
        else { o << "Value(int32_t(" << x << "LL))"; }
        return o.str();
    }, value);
}
struct Generator {
    const StProgramAst& ast; const std::vector<tags::Tag>& tags; std::ostream& out;
    std::set<uint32_t> temps; std::set<std::string> natives;
    tags::TypeId type(const Expr& e) {
        using T = tags::TypeId;
        if (e.kind == ExprKind::Literal) return tags::typeOf(static_cast<const LiteralExpr&>(e).value);
        if (e.kind == ExprKind::Identifier) return tags.at(static_cast<const IdentifierExpr&>(e).tagId).type;
        if (e.kind == ExprKind::Unary) {
            const auto& x=static_cast<const UnaryExpr&>(e); auto t=type(*x.operand);
            if (x.op==UnaryOp::Not && t!=T::Bool) throw std::runtime_error("NOT requires BOOL");
            if (x.op==UnaryOp::Neg && st::ops::numericRank(t)<0 && t!=T::Time) throw std::runtime_error("minus requires number/TIME");
            return t;
        }
        const auto& x=static_cast<const BinaryExpr&>(e); auto a=type(*x.lhs), b=type(*x.rhs);
        auto numeric=st::ops::numericRank(a)>=0 && st::ops::numericRank(b)>=0;
        if (x.op>=BinaryOp::And) {
            if (a!=T::Bool || b!=T::Bool) throw std::runtime_error("logical operator requires BOOL");
            return T::Bool;
        }
        if (x.op>=BinaryOp::Eq) {
            if (!numeric && (a!=b || (a!=T::Bool && a!=T::Time && a!=T::String))) throw std::runtime_error("incompatible comparison");
            if (a==T::Bool && x.op!=BinaryOp::Eq && x.op!=BinaryOp::Ne) throw std::runtime_error("BOOL comparison supports = and <>");
            return T::Bool;
        }
        if (a==T::Time && b==T::Time && (x.op==BinaryOp::Add || x.op==BinaryOp::Sub)) return T::Time;
        if (!numeric) throw std::runtime_error("arithmetic requires numeric values");
        return st::ops::numericResultType(a,b);
    }
    void assignable(uint32_t id, const Expr& e) {
        auto a=tags.at(id).type, b=type(e);
        if (a!=b && !(st::ops::numericRank(a)>=0 && st::ops::numericRank(b)>=0))
            throw std::runtime_error("type mismatch assigning " + std::string(tags::toString(b)) + " to " + tags.at(id).name);
    }
    std::string expr(const Expr& e) {
        type(e);
        switch(e.kind) {
            case ExprKind::Literal: return literal(static_cast<const LiteralExpr&>(e).value);
            case ExprKind::Identifier: return "read(c,"+std::to_string(static_cast<const IdentifierExpr&>(e).tagId)+")";
            case ExprKind::Unary: {
                auto& x=static_cast<const UnaryExpr&>(e);
                return "evalUnary(static_cast<st::UnaryOp>("+std::to_string(int(x.op))+"),"+expr(*x.operand)+")";
            }
            case ExprKind::Binary: {
                auto& x=static_cast<const BinaryExpr&>(e);
                std::string fn=x.op>=BinaryOp::And?"evalLogical":x.op>=BinaryOp::Eq?"evalComparison":"evalArithmetic";
                return fn+"(static_cast<st::BinaryOp>("+std::to_string(int(x.op))+"),"+expr(*x.lhs)+","+expr(*x.rhs)+")";
            }
        }
        throw std::runtime_error("invalid expression");
    }
    void write(uint32_t id,const std::string& v) { out << "write(c,"<<id<<",static_cast<TypeId>("<<int(tags.at(id).type)<<"),"<<v<<");\n"; }
    void block(const StmtList& stmts) { for(auto& s:stmts) statement(*s); }
    void statement(const Stmt& s) {
        switch(s.kind) {
            case StmtKind::Assign: {
                const auto& x=static_cast<const AssignStmt&>(s); assignable(x.targetId,*x.value);
                if (x.target.starts_with("__plc_network_")) {
                    out<<"network(c,"<<std::stoul(x.target.substr(14))<<","<<expr(*x.value)<<");\n";
                } else if (x.target.starts_with("__plc_native_")) {
                    natives.insert(x.target);
                    out<<"if("<<x.target<<"(c)!=0) throw std::runtime_error(\"native network returned an error\");\ncheck(c);\n";
                } else write(x.targetId,expr(*x.value));
                break;
            }
            case StmtKind::If: {
                const auto& x=static_cast<const IfStmt&>(s);
                bool first=true;
                for(auto& b:x.branches) {
                    if(type(*b.condition)!=tags::TypeId::Bool) throw std::runtime_error("IF requires BOOL");
                    out<<(first?"if(":"else if(")<<"truth("<<expr(*b.condition)<<")){\n"; block(b.body); out<<"}\n"; first=false;
                }
                if(!x.elseBody.empty()) { out<<"else {\n"; block(x.elseBody);out<<"}\n"; }
                break;
            }
            case StmtKind::While: {
                const auto& x=static_cast<const WhileStmt&>(s);
                if(type(*x.condition)!=tags::TypeId::Bool) throw std::runtime_error("WHILE requires BOOL");
                out<<"{ uint64_t iterations=0; while(truth("<<expr(*x.condition)<<")){ check(c); if(++iterations>1000000) throw std::runtime_error(\"loop limit exceeded\");\n";
                block(x.body);out<<"}}\n";break;
            }
            case StmtKind::Call: {
                const auto& x=static_cast<const CallStmt&>(s);out<<"{\n";
                size_t n=0;
                for(auto& a:x.args) if(!a.isOutput) { assignable(a.paramTagId,*a.inputExpr);out<<"const Value arg"<<n++<<"="<<expr(*a.inputExpr)<<";\n"; }
                n=0; for(auto& a:x.args) if(!a.isOutput) write(a.paramTagId,"arg"+std::to_string(n++));
                out<<"frame_"<<x.frameIndex<<"(c);\n";
                for(auto& a:x.args) if(a.isOutput) {
                    IdentifierExpr e(""); e.tagId=a.paramTagId;assignable(a.outputTargetId,e);
                    write(a.outputTargetId,"read(c,"+std::to_string(a.paramTagId)+")");
                }
                out<<"}\n";break;
            }
            case StmtKind::Rung: {
                const auto& x=static_cast<const RungStmt&>(s);
                if(type(*x.condition)!=tags::TypeId::Bool) throw std::runtime_error("LAD requires BOOL contacts");
                out<<"{ const bool power=truth("<<expr(*x.condition)<<");\n";
                for(auto& a:x.outputs) {
                    if(a.kind!=RungOutputKind::Direct) out<<"if(power) ";
                    write(a.targetId,a.kind==RungOutputKind::Direct?"Value(power)":a.kind==RungOutputKind::Set?"Value(true)":"Value(false)");
                }
                out<<"}\n";break;
            }
        }
    }
    void generate() {
        for(size_t i=0;i<ast.frames.size();i++) out<<"static void frame_"<<i<<"(PlcContext*);\n";
        for(size_t i=0;i<ast.frames.size();i++) {
            const auto& f=ast.frames[i];
            out<<"static void frame_"<<i<<"(PlcContext* c){ Scope scope(c,"<<quoted(f.scopeName)<<"); check(c);\n";
            for(auto& [id,v]:f.tempResets) { temps.insert(id);write(id,literal(v)); }
            block(f.body);out<<"}\n";
        }
        out<<"static void generated_run(PlcContext* c){ Scope scope(c,\"\"); check(c);\n";block(ast.body);out<<"}\n";
        out<<"static const PlcValue generated_initials[]={\n";
        for(auto& t:tags) {
            if(tags::typeOf(t.value)!=t.type) throw std::runtime_error("initializer type mismatch: "+t.name);
            out<<"toAbi("<<literal(t.value)<<"),\n";
        }
        out<<"};\n";
    }
};
int main(int argc,char** argv) {
    try {
        if(argc!=4) throw std::runtime_error("usage: plc_codegen source.st generated.cpp symbols.json");
        std::ifstream f(argv[1]); if(!f) throw std::runtime_error("cannot open input");
        std::string source((std::istreambuf_iterator<char>(f)),{});
        tags::TagStore store; auto program=StProgram::load(source,store);auto tags=store.snapshot();
        std::ostringstream body; Generator g{program->boundAst(),tags,body};g.generate();
        std::ofstream cpp(argv[2]); if(!cpp) throw std::runtime_error("cannot write generated source");
        cpp<<"#include \"softplc/runtime/program_support.hpp\"\nusing namespace softplc;\nusing namespace softplc::compiled;\n";
        for(auto& name:g.natives) cpp<<"extern \"C\" int32_t "<<name<<"(PlcContext*);\n";
        cpp<<body.str();
        std::ofstream json(argv[3]);json<<"[\n";
        for(size_t i=0;i<tags.size();i++) {
            auto& t=tags[i];json<<(i?",\n":"")<<"{\"id\":"<<i<<",\"name\":"<<quoted(t.name)<<",\"type\":"<<int(t.type)<<",\"temporary\":"<<(g.temps.contains(uint32_t(i))?"true":"false");
            if(t.address) json<<",\"area\":"<<int(t.address->area)<<",\"offset\":"<<t.address->byteOffset<<",\"bit\":"<<(t.address->bitOffset==tags::Address::kNoBit?-1:int(t.address->bitOffset));
            json<<"}";
        }
        json<<"\n]\n";
        std::cout<<"Compiled "<<tags.size()<<" tags and "<<program->boundAst().frames.size()<<" block instances\n";
    } catch(const std::exception& e) { std::cerr<<"Compile error: "<<e.what()<<'\n';return 1; }
}
