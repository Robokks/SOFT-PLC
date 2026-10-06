#ifndef SOFTPLC_PROGRAM_ABI_H
#define SOFTPLC_PROGRAM_ABI_H
/* Versioned C boundary shared by the runtime, generated SCL/LAD/STL and native C/C++. */
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PLC_ABI_VERSION 1u
#if defined(_WIN32)
#define PLC_EXPORT __declspec(dllexport)
#else
#define PLC_EXPORT __attribute__((visibility("default")))
#endif
enum PlcType { PLC_BOOL, PLC_BYTE, PLC_INT, PLC_DINT, PLC_REAL, PLC_LREAL, PLC_TIME, PLC_STRING };
enum PlcArea { PLC_LOCAL, PLC_INPUT, PLC_OUTPUT, PLC_MEMORY, PLC_DB };
enum PlcTaskKind { PLC_STARTUP, PLC_MAIN, PLC_CYCLIC };
typedef struct PlcValue { uint32_t type; int64_t integer; double real; char text[256]; } PlcValue;
typedef struct PlcContext PlcContext;
typedef struct PlcHostApi {
    uint32_t version;
    int32_t (*read_tag)(PlcContext*, uint32_t, PlcValue*);
    int32_t (*write_tag)(PlcContext*, uint32_t, const PlcValue*);
    int32_t (*find_tag)(PlcContext*, const char*);
    int32_t (*read_db)(PlcContext*, uint32_t, uint32_t, int32_t, uint32_t, PlcValue*);
    int32_t (*write_db)(PlcContext*, uint32_t, uint32_t, int32_t, const PlcValue*);
    int32_t (*checkpoint)(PlcContext*);
    int32_t (*network)(PlcContext*, uint32_t, int32_t, const char*);
    void (*fault)(PlcContext*, const char*);
} PlcHostApi;
struct PlcContext { const PlcHostApi* api; void* user; uint64_t elapsed_us; const char* scope; };
typedef int32_t (*PlcEntry)(PlcContext*);
typedef struct PlcTagDef {
    const char* name; uint32_t type; PlcValue initial;
    uint32_t area; uint32_t db; uint32_t offset; int32_t bit; const char* address;
} PlcTagDef;
typedef struct PlcTaskDef {
    const char* name; uint32_t ob; uint32_t kind; uint64_t period_us;
    uint32_t priority; uint64_t watchdog_us; PlcEntry entry;
} PlcTaskDef;
typedef struct PlcNetworkDef { uint32_t id; const char* block; const char* title; const char* language; } PlcNetworkDef;
typedef struct PlcProgramV1 {
    uint32_t abi_version; uint32_t struct_size; const char* name; const char* build_id;
    uint32_t tag_count; const PlcTagDef* tags;
    uint32_t task_count; const PlcTaskDef* tasks;
    uint32_t network_count; const PlcNetworkDef* networks;
} PlcProgramV1;
typedef const PlcProgramV1* (*PlcProgramGetter)(void);
/* Native network helpers. Symbolic names may be global or local to ctx->scope. */
static inline int32_t plc_read(PlcContext* c, const char* name, PlcValue* v) {
    int32_t id = c->api->find_tag(c, name);
    return id < 0 ? -1 : c->api->read_tag(c, (uint32_t)id, v);
}
static inline int32_t plc_write(PlcContext* c, const char* name, const PlcValue* v) {
    int32_t id = c->api->find_tag(c, name);
    return id < 0 ? -1 : c->api->write_tag(c, (uint32_t)id, v);
}
static inline int32_t plc_write_dint(PlcContext* c, const char* name, int32_t n) {
    PlcValue v; memset(&v, 0, sizeof(v)); v.type = PLC_DINT; v.integer = n; return plc_write(c, name, &v);
}
static inline int32_t plc_write_bool(PlcContext* c, const char* name, int32_t n) {
    PlcValue v; memset(&v, 0, sizeof(v)); v.type = PLC_BOOL; v.integer = !!n; return plc_write(c, name, &v);
}
#define PLC_TRY(expression) do { if ((expression) != 0) return -1; } while (0)
#ifdef __cplusplus
}
#endif
#endif
