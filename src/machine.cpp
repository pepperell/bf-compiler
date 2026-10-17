// machine.cpp - tape allocation, device bus, run loop, stop/fault handling.
#include "internal.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <mutex>
#include <cstring>

namespace bf {

namespace {

constexpr size_t GUARD = (size_t)1 << 30;    // reserved, never committed: runaway pointers fault here

thread_local Ctx* t_active = nullptr;        // ctx of the run executing on this thread
std::once_flag g_veh_once;

// A stop request revokes access to RAM, so the very next tape access in JIT
// code faults.  We land here, record what happened and redirect the thread to
// the JIT exit stub with the frame it saved in its prologue.  Genuine faults
// (pointer ran off the tape) are reported the same way with Status::Fault.
LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    Ctx* c = t_active;
    if (!c || !c->exit_common) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* cr = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    bool in_jit = cr->Rip >= (DWORD64)c->jit_begin && cr->Rip < (DWORD64)c->jit_end;
    if (!in_jit && !c->stop) return EXCEPTION_CONTINUE_SEARCH;    // a real bug somewhere else

    c->fault_addr = (uintptr_t)er->ExceptionInformation[1];
    c->fault_ptr = in_jit ? (uintptr_t)cr->R12 : (uintptr_t)c->ptr;
    if (c->stop) c->status = (int32_t)(c->halt_req ? Status::Halted : Status::Stopped);
    else {
        c->status = (int32_t)Status::Fault;
        c->msg = "pointer left the tape";
    }
    cr->Rip = (DWORD64)c->exit_common;
    cr->Rsp = (DWORD64)c->saved_rsp;
    cr->Rbx = (DWORD64)c;
    cr->Rax = (DWORD64)c->status;
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

// Console thunks reached from generated code and from the interpreter.
void     machine_out_thunk(Ctx* c, uint32_t v) { if (c->console) c->console->out((uint8_t)v); }
uint32_t machine_in_thunk(Ctx* c) {
    ConsoleIo* io = c->console;
    int v = io ? io->in() : -1;
    if (v >= 0) return (uint32_t)v & 255;
    switch (c->m->config().eof) {
    case EofMode::Zero:     return 0;
    case EofMode::MinusOne: return 255;
    default:                return 256;
    }
}
static void* slow_thunk(Ctx* c, int32_t pc) {
    int32_t at = interp_run(c, pc, true);
    if (c->status != (int32_t)Status::Running) return c->exit_slow;
    return const_cast<void*>(c->labels[at]);
}

// ---- Machine ----------------------------------------------------------------------
Machine::Machine(const Config& cfg) : cfg_(cfg) {
    mmio_size_ = cfg.mmio_size;
    tape_size_ = (size_t)mmio_size_ + cfg.ram_size;
    size_t page = 4096;
    size_t tape_bytes = (tape_size_ + page - 1) & ~(page - 1);
    region_size_ = GUARD + tape_bytes + GUARD;
    region_ = (uint8_t*)VirtualAlloc(nullptr, region_size_, MEM_RESERVE, PAGE_NOACCESS);
    if (!region_) throw std::bad_alloc();
    tape_ = region_ + GUARD;
    if (!VirtualAlloc(tape_, tape_bytes, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc();
    dev_.assign(mmio_size_, nullptr);
    dev_base_.assign(mmio_size_, 0);
    view_mu_ = new std::mutex();
    std::call_once(g_veh_once, [] { AddVectoredExceptionHandler(1, veh); });
}

Machine::~Machine() {
    if (region_) VirtualFree(region_, 0, MEM_RELEASE);
    delete static_cast<std::mutex*>(view_mu_);
}

void Machine::map(uint32_t addr, uint32_t size, Device* dev) {
    for (uint32_t a = addr; a < addr + size && a < mmio_size_; a++) { dev_[a] = dev; dev_base_[a] = addr; }
}
void Machine::set_console(ConsoleIo* io) { console_ = io; }

uint8_t Machine::mmio_read(uint32_t a) {
    Device* d = a < mmio_size_ ? dev_[a] : nullptr;
    return d ? d->read(a - dev_base_[a]) : 0;
}
void Machine::mmio_write(uint32_t a, uint8_t v) {
    Device* d = a < mmio_size_ ? dev_[a] : nullptr;
    if (d) d->write(a - dev_base_[a], v);
}
// Host-side access.  RAM reads are serialised against request_stop(), which
// revokes page access; while that lock is in force RAM reads return 0.
uint8_t Machine::bus_read(int64_t a) {
    if (a < 0 || (uint64_t)a >= tape_size_) return 0;
    if (a < mmio_size_) return mmio_read((uint32_t)a);
    std::lock_guard<std::mutex> g(*static_cast<std::mutex*>(view_mu_));
    return ram_locked_ ? 0 : tape_[a];
}
void Machine::bus_write(int64_t a, uint8_t v) {
    if (a < 0 || (uint64_t)a >= tape_size_) return;
    if (a < mmio_size_) { mmio_write((uint32_t)a, v); return; }
    std::lock_guard<std::mutex> g(*static_cast<std::mutex*>(view_mu_));
    if (!ram_locked_) tape_[a] = v;
}

void Machine::reset() {
    protect_ram(true);
    memset(tape_ + mmio_size_, 0, tape_size_ - mmio_size_);
}

void Machine::protect_ram(bool accessible) {
    std::lock_guard<std::mutex> g(*static_cast<std::mutex*>(view_mu_));
    DWORD old;
    size_t page = 4096;
    size_t bytes = (tape_size_ + page - 1) & ~(page - 1);
    VirtualProtect(tape_, bytes, accessible ? PAGE_READWRITE : PAGE_NOACCESS, &old);
    ram_locked_ = !accessible;
}

void Machine::request_stop() {
    Ctx* c = ctx_;
    if (!c) return;
    c->stop = 1;
    if (console_) console_->cancel();
    if (c->exit_common) protect_ram(false);     // JIT is running: make its next access fault
}

void Machine::halt() {
    Ctx* c = ctx_;
    if (!c) return;
    c->halt_req = 1;
    request_stop();
}

RunResult Machine::run(const Program& p, bool use_jit) {
    RunResult r{Status::Error, "", 0.0, 0};
    if (!p.error.empty()) { r.message = p.error; return r; }
    if (running_) { r.message = "already running"; return r; }

    Ctx ctx{};
    ctx.ptr = tape_ + mmio_size_;
    ctx.ram_base = tape_ + mmio_size_;
    ctx.m = this;
    ctx.status = (int32_t)Status::Running;
    ctx.out_fn = machine_out_thunk;
    ctx.in_fn = machine_in_thunk;
    ctx.slow_fn = slow_thunk;
    ctx.tape = tape_;
    ctx.tape_size = tape_size_;
    ctx.mmio_size = mmio_size_;
    ctx.prog = &p;
    ctx.console = console_;
    ctx.temp_bp = -1;             // no debugger features in a normal run
    ctx.watch_hit = -1;

    JitCode jit;
    if (use_jit) {
        if (!jit_compile(p, jit)) { r.message = "jit: " + jit.error; return r; }
        ctx.labels = jit.labels.data();
        ctx.exit_common = jit.exit_common;
        ctx.exit_slow = jit.exit_slow;
        ctx.jit_begin = jit.mem;
        ctx.jit_end = jit.mem + jit.size;
    }

    protect_ram(true);
    running_ = true;
    ctx_ = &ctx;
    t_active = &ctx;
    auto t0 = std::chrono::steady_clock::now();
    if (use_jit) jit.entry(&ctx);
    else         interp_run(&ctx, 0, false);
    auto t1 = std::chrono::steady_clock::now();
    t_active = nullptr;
    ctx_ = nullptr;
    running_ = false;
    protect_ram(true);
    if (console_) console_->flush();

    r.status = (Status)ctx.status;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.ptr = (int64_t)(ctx.ptr - tape_);
    if (r.status == Status::Fault) {
        int64_t idx = (int64_t)(ctx.fault_ptr - (uintptr_t)tape_);
        r.ptr = idx;
        r.message = ctx.msg.empty() ? "memory fault" : ctx.msg;
        r.message += " (cell " + std::to_string(idx) + ")";
    } else if (r.status == Status::Running) {
        r.status = Status::Error;
        r.message = "executor returned without a status";
    }
    return r;
}

} // namespace bf
