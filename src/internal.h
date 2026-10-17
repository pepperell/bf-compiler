// internal.h - shared between the JIT, the interpreter and the machine.
#pragma once
#include "bf.h"
#include <cstddef>

namespace bf {

// The first 72 bytes are addressed by machine code; offsets are fixed.
struct Ctx {
    uint8_t*  ptr;                                 // 0   current cell (absolute)
    uint8_t*  ram_base;                            // 8   tape + mmio_size  (held in r13)
    void*     saved_rsp;                           // 16  rsp inside the JIT frame
    Machine*  m;                                   // 24
    int32_t   status;                              // 32  bf::Status
    volatile int32_t stop;                         // 36  set by request_stop / halt
    void*     exit_common;                         // 40  JIT exit stub, expects eax = status
    void    (*out_fn)(Ctx*, uint32_t);             // 48
    uint32_t(*in_fn)(Ctx*);                        // 56  0..255, or 256 = leave the cell alone
    void*   (*slow_fn)(Ctx*, int32_t pc);          // 64  interpret from pc, return resume address
    // ---- not touched by machine code ----
    uint8_t*       tape;
    size_t         tape_size;
    uint32_t       mmio_size;
    const Program* prog;
    ConsoleIo*     console;
    const void* const* labels;    // native address per IR index, or nullptr
    void*          exit_slow;     // stub: eax = ctx->status, then exit
    const uint8_t* jit_begin;
    const uint8_t* jit_end;
    volatile int32_t halt_req;
    // debugger (interpreter only)
    const uint8_t* bp;            // per-pc breakpoint flags, or nullptr
    volatile int32_t dbg;         // 0 run, 1 step (one instruction), 2 pause now
    int32_t        temp_bp;       // one-shot breakpoint pc (run to cursor / step over), -1 none
    const int64_t* watch;         // cells that pause the program after being written
    int32_t        watch_n;
    volatile int32_t watch_mmio;  // pause when the pointer enters the device window
    int64_t        watch_hit;     // address written since the last resume, -1 none
    int32_t        pause_reason;  // bf::Debugger::Reason
    uintptr_t      fault_addr;
    uintptr_t      fault_ptr;
    std::string    msg;
};
static_assert(offsetof(Ctx, ptr) == 0 && offsetof(Ctx, ram_base) == 8 && offsetof(Ctx, saved_rsp) == 16, "ctx layout");
static_assert(offsetof(Ctx, status) == 32 && offsetof(Ctx, stop) == 36 && offsetof(Ctx, exit_common) == 40, "ctx layout");
static_assert(offsetof(Ctx, out_fn) == 48 && offsetof(Ctx, in_fn) == 56 && offsetof(Ctx, slow_fn) == 64, "ctx layout");

// Interpreter.  Runs from pc.  With until_label set it returns as soon as it
// reaches an instruction that has a native label while the pointer is in RAM
// (the JIT slow path); otherwise it runs to END.  Returns the IR index where it
// stopped; ctx->status != Running means the run ended.
int32_t interp_run(Ctx* c, int32_t pc, bool until_label);

inline bool is_special(Op o) { return o == JZ || o == JNZ || o == ENDIF || o == SCAN || o == END; }
inline bool has_label(const Program& p, size_t i) {
    return i == 0 || is_special(p.code[i].op) || is_special(p.code[i - 1].op);
}

struct JitCode {
    uint8_t* mem = nullptr;
    size_t   size = 0;
    int32_t (*entry)(Ctx*) = nullptr;
    void*    exit_common = nullptr;
    void*    exit_slow = nullptr;
    std::vector<const void*> labels;
    std::string error;
    ~JitCode();
};
bool jit_compile(const Program& p, JitCode& out);

} // namespace bf
