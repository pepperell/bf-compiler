// bf.h - public interface of the brainfuck executor core.
//
// Pipeline:  source text -> compile() -> Program (optimised IR)
//            Machine::run(Program)   -> x64 JIT (default) or IR interpreter
//
// Memory map (cell indices):
//   [0, mmio_size)          device window: every access goes through the bus
//   [mmio_size, tape_size)  RAM; the pointer starts at mmio_size
// The pointer may wander into the device window; the JIT then hands control
// to the interpreter until the pointer is back in RAM.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// windows.h defines IN and OUT as empty macros; they would mangle the opcodes.
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

namespace bf {

// ---- intermediate representation -------------------------------------------
enum Op : uint8_t {
    ADD,    // tape[p+off] += arg
    SET,    // tape[p+off]  = arg
    MUL,    // tape[p+off] += tape[p+target] * arg      (target = source offset)
    MOVE,   // p += arg
    OUT,    // console.out(tape[p+off])
    IN,     // tape[p+off] = console.in()  (cell unchanged on EOF in NoChange mode)
    JZ,     // if tape[p]==0 goto target   ('[', or the start of an if-block)
    JNZ,    // if tape[p]!=0 goto target   (']')
    ENDIF,  // no-op; closes a JZ that has no JNZ (loop proven to run <= once)
    SCAN,   // while tape[p]!=0: p += arg
    IDIOM,  // first instruction of a recognised loop body: run the whole loop in one step
            // (Program::idioms[arg]) and continue at target, the loop exit; off = lowest
            // cell offset it touches.  Falls through into the body when a guard fails.
    LOOPX,  // literal program only: the loop that follows as multiply-adds (Program::lit_loops[arg]),
            // done in one step when every cell it touches is RAM or a plain device register;
            // continues at target, the loop exit.  Otherwise falls through into the loop body.
    END
};

struct Instr {
    Op      op;
    int32_t off;
    int32_t arg;
    int32_t target;
    int32_t src;            // byte offset in the source of the command this came from
};

// A loop of the BFL compiler's multi-byte arithmetic that runs a cell's value
// times (ir.cpp, "loop idioms").  Offsets are relative to the loop cell, which
// counts the passes and ends at 0.
enum IdiomKind : uint8_t {
    IDIOM_ADD,      // cells: the bytes of a little-endian number, += sign * passes (carry/borrow)
    IDIOM_SATSUB,   // cells[0] = max(cells[0] - passes, 0)
    IDIOM_HALVE,    // cells[0] += passes / 2, cells[1] += passes % 2
    IDIOM_DIVMOD,   // cells R, C, Q, B: per pass R++, C--, and when C reaches 0: Q++, R = 0, C = B
    IDIOM_DIVIDE,   // the division loop (division_loop): cells the bytes of A, R and B, `bytes` each;
                    // per pass (A:R) <<= 1, and when R >= B: R -= B, A |= 1
};
struct Idiom {
    IdiomKind kind;
    int8_t    sign = 1;                 // IDIOM_ADD
    uint8_t   bytes = 0;                // IDIOM_DIVIDE
    std::vector<int32_t> cells;
    struct Guard { int32_t off; uint8_t val; bool check; };
    std::vector<Guard> guards;          // scratch cells: hold val after every pass; check: must hold it on entry
    int32_t lo = 0, hi = 0;             // range of offsets touched
};

struct Program {
    std::vector<Instr> code;     // what runs: optimised, or == literal when optimise=false
    std::vector<Instr> literal;  // one instruction per source command (runs of < > folded into one
                                 // move when optimising), always present: it runs in the device window
    std::vector<Idiom> idioms;   // referenced by IDIOM instructions
    std::vector<std::vector<Instr>> lit_loops;   // referenced by LOOPX instructions of the literal program
    std::string error;           // non-empty => compile failed
    size_t raw_ops = 0;          // number of brainfuck commands in the source
    size_t src_size = 0;
};

// optimise=false produces one IR instruction per source command (no run
// folding), which is what the debugger steps through.
Program     compile(const std::string& source, bool optimise = true);
std::string disassemble(const Program& p);
// Size in bytes of the x64 code the JIT produces for p (0 if it cannot be compiled).
size_t      jit_code_size(const Program& p);

// The BFL compiler's bit-by-bit division of n-byte numbers (2 <= n <= 8) as brainfuck text:
// one loop on the pass counter at cell 0, which the optimiser recognises by its text (for
// n = 2 and 4, the widths BFL divides) and runs in one step (ir.cpp, "the division loop").
// Each pass shifts the 2n-byte number (A:R) left by one bit and, when R >= B, subtracts B
// from R and sets A's low bit; after 8n passes from R = 0, A holds the quotient and R the
// remainder.  Cells, relative to the counter: a group of 6 per byte of the chain A0..An-1,
// R0..Rn-1, X (the byte, two cells for its zero test, a counter, and in R's groups B's byte
// and a temporary).  Every cell but A, R and B is zero on entry and after every pass; B is
// unchanged.
struct DivisionLayout {
    int n;
    static int group(int j) { return 1 + 6 * j; }
    int a(int k) const { return group(k); }
    int r(int k) const { return group(n + k); }
    int x() const { return group(2 * n); }       // R's borrow byte
    int b(int k) const { return group(n + k) + 4; }
    int cells() const { return group(2 * n) + 1; }
};
const std::string& division_loop(int n);

// ---- hardware -----------------------------------------------------------------
struct Device {
    virtual ~Device() {}
    virtual uint8_t read(uint32_t rel)            { (void)rel; return 0; }
    virtual void    write(uint32_t rel, uint8_t v) { (void)rel; (void)v; }
    // A plain register behaves like memory: reading returns exactly the last value
    // written and neither has side effects.  Loops over plain registers may then run
    // as single multiply-adds instead of one bus access per step.
    virtual bool    plain(uint32_t rel) const      { (void)rel; return false; }
};

struct ConsoleIo {               // backs the '.' and ',' commands
    virtual ~ConsoleIo() {}
    virtual void out(uint8_t c) = 0;
    virtual int  in()           = 0;   // 0..255, or -1 at EOF / cancelled
    virtual void flush()        {}
    virtual void cancel()       {}     // wake a blocked in(); called by request_stop
};

enum class EofMode { NoChange, Zero, MinusOne };
enum class Status  { Ok = 0, Running = 1, Halted = 2, Stopped = 3, Fault = 4, Error = 5, Paused = 6 };
const char* status_name(Status s);

struct RunResult {
    Status      status;
    std::string message;
    double      seconds;
    int64_t     ptr;        // final pointer (cell index)
};

struct Ctx;

class Machine {
public:
    struct Config {
        uint32_t mmio_size = 256;
        size_t   ram_size  = 1u << 20;
        EofMode  eof       = EofMode::NoChange;
    };
    explicit Machine(const Config& cfg);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Map a device over [addr, addr+size) inside the device window.
    void map(uint32_t addr, uint32_t size, Device* dev);
    void set_console(ConsoleIo* io);

    uint8_t* tape() const       { return tape_; }
    size_t   tape_size() const  { return tape_size_; }
    uint32_t mmio_size() const  { return mmio_size_; }
    const Config& config() const { return cfg_; }

    void      reset();                           // zero RAM
    // When set, run() uses the interpreter and counts how often each instruction
    // executes: code.size() counts for the optimised code, then literal.size() for
    // the literal program (which runs while the pointer can reach the device window).
    std::vector<uint64_t>* profile = nullptr;
    RunResult run(const Program& p, bool use_jit = true);

    // Thread-safe, callable while run() is executing on another thread.
    void request_stop();
    // Callable from a device: ends the program with Status::Halted.
    void halt();
    bool running() const { return running_; }

    // Bus access for the host (memory viewers, device code).
    uint8_t  bus_read(int64_t addr);
    void     bus_write(int64_t addr, uint8_t v);
    uint8_t  mmio_read(uint32_t addr);
    void     mmio_write(uint32_t addr, uint8_t v);

private:
    friend struct Ctx;
    friend class Debugger;
    Config    cfg_;
    uint8_t*  region_ = nullptr;     // reserved guard + tape + guard
    size_t    region_size_ = 0;
    uint8_t*  tape_ = nullptr;
    size_t    tape_size_ = 0;
    uint32_t  mmio_size_ = 0;
    std::vector<Device*>  dev_;      // per device-window cell
    std::vector<uint32_t> dev_base_;
    ConsoleIo* console_ = nullptr;
    Ctx*      ctx_ = nullptr;        // live during run()
    volatile bool running_ = false;
    volatile bool ram_locked_ = false;   // page access revoked by request_stop()
    void*     view_mu_ = nullptr;        // serialises host reads against protect_ram(false)
    void protect_ram(bool accessible);
};

// ---- debugger -------------------------------------------------------------------
// A resumable interpreter session on its own thread.  Breakpoints and stepping
// are per IR instruction; compile with optimise=false for one instruction per
// source command.  on_event is called on the session thread.
class Debugger {
public:
    enum class Event { Paused, Finished };
    Debugger(Machine& m, const Program& p, std::function<void(Event)> on_event);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    enum class Reason { None = 0, Step, Breakpoint, RunTo, Watch, DeviceWindow, Pause };
    struct Break { Reason reason; int64_t addr; };     // addr: the written cell for Watch

    void set_breakpoint(int32_t pc, bool on);
    void clear_breakpoints();
    void set_watch(int64_t cell, bool on);     // pause after this cell is written (while paused)
    void clear_watches();
    void set_watch_device_window(bool on);     // pause when the pointer enters the device window
    void step();            // execute one instruction, then Paused
    // Statement-level stepping for compiled languages: marks[pc] != 0 where a
    // source statement starts.  step_statement runs until the next marked
    // instruction whose source offset is outside [skip_lo, skip_hi) - pass the
    // current statement's range to step over it, or -1, -1 to step into.
    // With cond_cell >= 0 the pause also requires the cond_bytes-byte little-endian value
    // at cond_cell to equal cond_val (used to wait for a recursion's stack pointer to come back).
    void set_statement_marks(const std::vector<uint8_t>& marks);
    void step_statement(int32_t skip_lo = -1, int32_t skip_hi = -1, int64_t cond_cell = -1, int cond_val = 0, int cond_bytes = 1);
    void run_to(int32_t pc);// continue until pc is reached (or any other pause cause)
    void cont();            // run until a breakpoint, pause(), stop() or the end
    void pause();           // from any thread while running
    void stop();            // end the session with Status::Stopped

    bool    running() const;      // executing (not paused, not finished)
    bool    finished() const;
    int32_t pc() const;           // next instruction to execute
    int64_t ptr() const;          // current cell index (live, approximate while running)
    Break   last_break() const;   // why the session paused last
    RunResult result() const;     // valid once finished()

private:
    struct Impl;
    Impl* impl_;
};

} // namespace bf
