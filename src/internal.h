// internal.h - shared between the JIT, the interpreter and the machine.
#pragma once
#include "bf.h"
#include <cstddef>

namespace bf {

// The first 88 bytes are addressed by machine code; offsets are fixed.
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
    int32_t (*idiom_fn)(Ctx*, int32_t idx);        // 72  run Program::idioms[idx] at ptr: 1 done, 0 run the body
    uint8_t*  tape_end;                            // 80  tape + tape_size
    // ---- not touched by machine code ----
    uint8_t*       tape;
    size_t         tape_size;
    uint32_t       mmio_size;
    const Program* prog;          // the code being interpreted (optimised or literal)
    const Idiom*   idioms;        // of the optimised program
    uint64_t*      prof;          // per optimised pc execution counts, or nullptr
    uint64_t*      lit_prof;      // per literal pc execution counts (follow prof's), or nullptr
    const uint8_t* plain;         // per device-window cell: 1 for a plain register (Device::plain)
    ConsoleIo*     console;
    const void* const* labels;    // per IR index: non-null where native code can resume
    // Device-window execution: the literal program and the maps between it and
    // the optimised code (see Machine::run).  guard_min is per optimised pc:
    // the lowest pointer offset a block starting there can reach (0 = no check).
    const Program* lit;
    const void* const* lit_labels;
    const int32_t* lit_pc_of_src;
    const int32_t* guard_min;
    void*          exit_slow;     // stub: eax = ctx->status, then exit
    const uint8_t* jit_begin;
    const uint8_t* jit_end;
    const struct JitProbe* probes;    // JitCode::probes, by address
    size_t         n_probes;
    volatile int32_t halt_req;
    // debugger (interpreter only)
    const uint8_t* bp;            // per-pc breakpoint flags, or nullptr
    volatile int32_t dbg;         // 0 run, 1 step (one instruction), 2 pause now, 3 step to a statement mark
    int32_t        temp_bp;       // one-shot breakpoint pc (run to cursor / step over), -1 none
    const int64_t* watch;         // cells that pause the program after being written
    int32_t        watch_n;
    volatile int32_t watch_mmio;  // pause when the pointer enters the device window
    int64_t        watch_hit;     // address written since the last resume, -1 none
    const uint8_t* stmt_mark;     // per-pc: 1 where a source statement starts (dbg == 3 pauses there)
    int32_t        skip_lo, skip_hi;   // source range [lo, hi) whose marks are ignored while dbg == 3 ...
    int32_t        skip_left;     // ... until the program has executed something outside it (then re-entry counts)
    int64_t        cond_cell;     // dbg == 3: pause only while the 16-bit word at cond_cell == cond_val (-1: always)
    int32_t        cond_val;
    int32_t        cond_bytes;    // 1 or 2
    int32_t        pause_reason;  // bf::Debugger::Reason
    uintptr_t      fault_addr;
    uintptr_t      fault_ptr;
    std::string    msg;
};
static_assert(offsetof(Ctx, ptr) == 0 && offsetof(Ctx, ram_base) == 8 && offsetof(Ctx, saved_rsp) == 16, "ctx layout");
static_assert(offsetof(Ctx, status) == 32 && offsetof(Ctx, stop) == 36 && offsetof(Ctx, exit_common) == 40, "ctx layout");
static_assert(offsetof(Ctx, out_fn) == 48 && offsetof(Ctx, in_fn) == 56 && offsetof(Ctx, slow_fn) == 64, "ctx layout");
static_assert(offsetof(Ctx, idiom_fn) == 72 && offsetof(Ctx, tape_end) == 80, "ctx layout");

// Ctx::idiom_fn: checks that every cell the idiom touches is on the tape first.
int32_t idiom_thunk(Ctx* c, int32_t idx);

// Interpreter.  Runs from pc.  With until_label set it returns as soon as it
// reaches an instruction that has a native label while the pointer is in RAM
// (the JIT slow path); otherwise it runs to END.  Returns the IR index where it
// stopped; ctx->status != Running means the run ended.
int32_t interp_run(Ctx* c, int32_t pc, bool until_label);

// interp_run status meaning "this block may touch the device window: run the literal program"
constexpr int32_t STATUS_NEED_LITERAL = 100;

inline bool is_special(Op o) { return o == JZ || o == JNZ || o == ENDIF || o == SCAN || o == IDIOM || o == LOOPX || o == END; }

// Run a loop idiom (ir.cpp) with the loop cell at p; false (nothing changed) when a guard fails.
bool idiom_exec(const Idiom& d, uint8_t* p);

// Lowest pointer offset (<= 0) reachable by the straight-line block starting at i;
// sets j to the index after the block.
inline int64_t block_min_reach(const std::vector<Instr>& code, size_t i, size_t& j) {
    int64_t minoff = 0, delta = 0;
    j = i;
    while (j < code.size() && !is_special(code[j].op)) {
        const Instr& y = code[j];
        if (y.op == MOVE) { delta += y.arg; if (delta < minoff) minoff = delta; }
        else {
            if (delta + y.off < minoff) minoff = delta + y.off;
            if (y.op == MUL && delta + y.target < minoff) minoff = delta + y.target;
        }
        j++;
    }
    return minoff;
}
inline bool has_label(const Program& p, size_t i) {
    return i == 0 || is_special(p.code[i].op) || is_special(p.code[i - 1].op);
}

// An inline idiom's opening instructions only read cells, among them its highest: a fault there (the idiom
// reaches past the end of the tape) resumes at `resume`, the loop as written, with nothing changed.
struct JitProbe { const uint8_t* begin; const uint8_t* end; const void* resume; };

struct JitCode {
    uint8_t* mem = nullptr;
    size_t   size = 0;
    int32_t (*entry)(Ctx*) = nullptr;
    void*    exit_common = nullptr;
    void*    exit_slow = nullptr;
    std::vector<const void*> labels;
    std::vector<JitProbe> probes;    // by address
    std::string error;
    ~JitCode();
};
bool jit_compile(const Program& p, JitCode& out);

} // namespace bf
