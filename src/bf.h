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
    END
};

struct Instr {
    Op      op;
    int32_t off;
    int32_t arg;
    int32_t target;
    int32_t src;            // byte offset in the source of the command this came from
};

struct Program {
    std::vector<Instr> code;     // what runs: optimised, or == literal when optimise=false
    std::vector<Instr> literal;  // one instruction per source command, always present
    std::string error;           // non-empty => compile failed
    size_t raw_ops = 0;          // number of brainfuck commands in the source
    size_t src_size = 0;
};

// optimise=false produces one IR instruction per source command (no run
// folding), which is what the debugger steps through.
Program     compile(const std::string& source, bool optimise = true);
std::string disassemble(const Program& p);

// ---- hardware -----------------------------------------------------------------
struct Device {
    virtual ~Device() {}
    virtual uint8_t read(uint32_t rel)            { (void)rel; return 0; }
    virtual void    write(uint32_t rel, uint8_t v) { (void)rel; (void)v; }
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
    void set_statement_marks(const std::vector<uint8_t>& marks);
    void step_statement(int32_t skip_lo = -1, int32_t skip_hi = -1);
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
