// debugger.cpp - resumable interpreter session with breakpoints (see bf.h).
#include "internal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bf {

// Console thunks shared with Machine::run (defined in machine.cpp).
void     machine_out_thunk(Ctx* c, uint32_t v);
uint32_t machine_in_thunk(Ctx* c);

struct Debugger::Impl {
    Machine& m;
    const Program& prog;
    std::function<void(Event)> on_event;
    Ctx ctx{};
    std::vector<uint8_t> bp;
    std::vector<uint8_t> marks;
    std::vector<int64_t> watches;
    std::atomic<int32_t> pc{0};
    std::atomic<bool> running{false}, finished{false};
    Break last{Reason::None, -1};
    RunResult result{Status::Running, "", 0.0, 0};
    double elapsed = 0;

    enum Cmd { None, Step, StepStmt, Cont, Quit };
    std::mutex mu;
    std::condition_variable cv;
    Cmd cmd = None;
    std::thread thread;

    Impl(Machine& mm, const Program& p, std::function<void(Event)> ev) : m(mm), prog(p), on_event(std::move(ev)) {
        bp.assign(p.code.size(), 0);
        ctx.ptr = m.tape_ + m.mmio_size_;
        ctx.ram_base = ctx.ptr;
        ctx.m = &m;
        ctx.status = (int32_t)Status::Running;
        ctx.out_fn = machine_out_thunk;
        ctx.in_fn = machine_in_thunk;
        ctx.tape = m.tape_;
        ctx.tape_size = m.tape_size_;
        ctx.mmio_size = m.mmio_size_;
        ctx.prog = &p;
        ctx.console = m.console_;
        ctx.bp = bp.data();
        ctx.temp_bp = -1;
        ctx.watch_hit = -1;
        ctx.skip_lo = ctx.skip_hi = -1;
        m.protect_ram(true);
        m.ctx_ = &ctx;
        m.running_ = true;
        thread = std::thread([this] { loop(); });
    }
    ~Impl() {
        post(Quit);
        if (thread.joinable()) thread.join();
        m.ctx_ = nullptr;
        m.running_ = false;
        m.protect_ram(true);
    }

    void post(Cmd c) { { std::lock_guard<std::mutex> g(mu); cmd = c; } cv.notify_all(); }
    void sync_watches() { ctx.watch = watches.data(); ctx.watch_n = (int32_t)watches.size(); }

    void loop() {
        for (;;) {
            Cmd c;
            {
                std::unique_lock<std::mutex> g(mu);
                cv.wait(g, [&] { return cmd != None; });
                c = cmd; cmd = None;
            }
            if (c == Quit) break;
            if (finished) continue;
            ctx.dbg = (c == Step) ? 1 : (c == StepStmt) ? 3 : 0;
            ctx.watch_hit = -1;
            ctx.pause_reason = 0;
            ctx.status = (int32_t)Status::Running;
            running = true;
            auto t0 = std::chrono::steady_clock::now();
            int32_t at = interp_run(&ctx, pc, false);
            elapsed += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            pc = at;
            ctx.temp_bp = -1;
            ctx.skip_lo = ctx.skip_hi = -1;
            running = false;
            if (ctx.status == (int32_t)Status::Paused) {
                last = Break{(Reason)ctx.pause_reason, ctx.pause_reason == (int)Reason::Watch ? ctx.watch_hit : -1};
                if (on_event) on_event(Event::Paused);
                continue;
            }
            result.status = (Status)ctx.status;
            result.seconds = elapsed;
            result.ptr = (int64_t)(ctx.ptr - ctx.tape);
            if (result.status == Status::Fault) result.message = ctx.msg + " (cell " + std::to_string((int64_t)(ctx.fault_ptr)) + ")";
            if (ctx.console) ctx.console->flush();
            finished = true;
            if (on_event) on_event(Event::Finished);
        }
    }
};

Debugger::Debugger(Machine& m, const Program& p, std::function<void(Event)> on_event) : impl_(new Impl(m, p, std::move(on_event))) {}
Debugger::~Debugger() { delete impl_; }

void Debugger::set_breakpoint(int32_t pc, bool on) { if (pc >= 0 && (size_t)pc < impl_->bp.size()) impl_->bp[pc] = on ? 1 : 0; }
void Debugger::clear_breakpoints() { std::fill(impl_->bp.begin(), impl_->bp.end(), (uint8_t)0); }
void Debugger::set_watch(int64_t cell, bool on) {
    auto& w = impl_->watches;
    auto it = std::find(w.begin(), w.end(), cell);
    if (on && it == w.end()) w.push_back(cell);
    else if (!on && it != w.end()) w.erase(it);
    impl_->sync_watches();
}
void Debugger::clear_watches() { impl_->watches.clear(); impl_->sync_watches(); }
void Debugger::set_watch_device_window(bool on) { impl_->ctx.watch_mmio = on ? 1 : 0; }
void Debugger::step()  { if (!impl_->running && !impl_->finished) impl_->post(Impl::Step); }
void Debugger::set_statement_marks(const std::vector<uint8_t>& marks) {
    impl_->marks = marks;
    impl_->marks.resize(impl_->prog.code.size(), 0);
    impl_->ctx.stmt_mark = impl_->marks.data();
}
void Debugger::step_statement(int32_t skip_lo, int32_t skip_hi) {
    if (impl_->running || impl_->finished) return;
    impl_->ctx.skip_lo = skip_lo; impl_->ctx.skip_hi = skip_hi;
    impl_->post(Impl::StepStmt);
}
void Debugger::run_to(int32_t pc) { if (!impl_->running && !impl_->finished) { impl_->ctx.temp_bp = pc; impl_->post(Impl::Cont); } }
void Debugger::cont()  { if (!impl_->running && !impl_->finished) impl_->post(Impl::Cont); }
void Debugger::pause() { impl_->ctx.dbg = 2; }
void Debugger::stop()  {
    impl_->m.request_stop();
    if (!impl_->running && !impl_->finished) impl_->post(Impl::Cont);    // let the loop observe the stop flag
}
bool    Debugger::running() const  { return impl_->running; }
bool    Debugger::finished() const { return impl_->finished; }
int32_t Debugger::pc() const       { return impl_->pc; }
int64_t Debugger::ptr() const      { return (int64_t)(impl_->ctx.ptr - impl_->ctx.tape); }
Debugger::Break Debugger::last_break() const { return impl_->last; }
RunResult Debugger::result() const { return impl_->result; }

} // namespace bf
