// interp.cpp - IR interpreter with full bus semantics.
//
// Used as the reference executor (Machine::run(p, false)) and as the JIT's
// slow path whenever a block might touch the device window.  It must not hold
// RAII locals: a stop request may abandon this frame from the exception handler.
#include "internal.h"

namespace bf {

namespace {

// An access off the tape records the cell for the fault report, as the address it would
// have (as a native fault does).
inline bool off_tape(Ctx* c, int64_t a) {
    if ((uint64_t)a < c->tape_size) return false;
    c->fault_ptr = (uintptr_t)c->tape + (uintptr_t)a;
    return true;
}
inline bool cell_read(Ctx* c, int64_t a, uint32_t& v) {
    if (off_tape(c, a)) return false;
    v = (a < (int64_t)c->mmio_size) ? c->m->mmio_read((uint32_t)a) : c->tape[a];
    return true;
}
inline bool cell_write(Ctx* c, int64_t a, uint32_t v) {
    if (off_tape(c, a)) return false;
    if (a < (int64_t)c->mmio_size) c->m->mmio_write((uint32_t)a, (uint8_t)v);
    else c->tape[a] = (uint8_t)v;
    for (int32_t i = 0; i < c->watch_n; i++) if (c->watch[i] == a) c->watch_hit = a;   // data breakpoints
    return true;
}

} // namespace

int32_t idiom_thunk(Ctx* c, int32_t idx) {
    const Idiom& d = c->idioms[idx];
    int64_t p = c->ptr - c->tape;
    if (p + d.lo < (int64_t)c->mmio_size || p + d.hi >= (int64_t)c->tape_size) return 0;   // the body faults or uses the bus
    return idiom_exec(d, c->ptr) ? 1 : 0;
}

int32_t interp_run(Ctx* c, int32_t pc, bool until_label) {
    const Instr* code = c->prog->code.data();
    int64_t p = c->ptr - c->tape;
    const int64_t ram = c->mmio_size;
    uint32_t v, w;
    int32_t start = pc;
    int64_t executed = 0;
    int64_t last_p = p;
    uint64_t* prof = c->prog == c->lit ? c->lit_prof : c->prof;   // the literal program counts in its own part

    for (;;) {
        if (c->stop) {
            c->status = (int32_t)(c->halt_req ? Status::Halted : Status::Stopped);
            break;
        }
        if (until_label && pc != start && p >= ram && c->labels[pc]) break;   // back to native code
        if (c->guard_min && c->guard_min[pc] && p + c->guard_min[pc] < ram) {  // block may reach the device window
            c->status = STATUS_NEED_LITERAL;
            break;
        }
        if (c->dbg || c->bp || c->watch_n || c->watch_mmio || c->temp_bp >= 0) {
            // Decide whether to pause before this instruction.  Reasons are
            // bf::Debugger::Reason values: 1 step, 2 breakpoint, 3 run-to,
            // 4 watched cell written, 5 pointer entered device window, 6 pause().
            int reason = 0;
            if (c->watch_hit >= 0) reason = 4;
            else if (c->watch_mmio && p < ram && last_p >= ram) reason = 5;
            else if (c->dbg == 2) reason = 6;
            else if (executed > 0) {
                if (c->dbg == 1) reason = 1;
                else if (c->dbg == 3) {
                    bool inside = code[pc].src >= c->skip_lo && code[pc].src < c->skip_hi;
                    if (!inside) c->skip_left = 1;
                    if (c->stmt_mark && c->stmt_mark[pc] && (!inside || c->skip_left) &&
                        (c->cond_cell < 0 || (int32_t)(c->tape[c->cond_cell] | (c->cond_bytes > 1 ? c->tape[c->cond_cell + 1] << 8 : 0)) == c->cond_val)) reason = 1;
                }
                if (!reason && c->bp && c->bp[pc]) reason = 2;
                else if (!reason && pc == c->temp_bp) reason = 3;
            }
            if (reason) { c->pause_reason = reason; c->status = (int32_t)Status::Paused; break; }
            c->ptr = c->tape + p;                     // publish the live pointer for the debugger UI
            last_p = p;
        }
        executed++;
        if (prof) prof[pc]++;
        const Instr& x = code[pc];
        switch (x.op) {
        case ADD:
            if (!cell_read(c, p + x.off, v) || !cell_write(c, p + x.off, v + x.arg)) goto oob;
            pc++; break;
        case SET:
            if (!cell_write(c, p + x.off, x.arg)) goto oob;
            pc++; break;
        case MUL:                              // of 0: its loop as written does not run, nor touch the cell
            if (!cell_read(c, p + x.target, v) || (v && (!cell_read(c, p + x.off, w) ||
                !cell_write(c, p + x.off, w + v * x.arg)))) goto oob;
            pc++; break;
        case MOVE:
            p += x.arg; pc++; break;
        case OUT:
            if (!cell_read(c, p + x.off, v)) goto oob;
            c->out_fn(c, v);
            pc++; break;
        case IN:
            v = c->in_fn(c);
            if (v < 256 && !cell_write(c, p + x.off, v)) goto oob;
            pc++; break;
        case JZ:
            if (!cell_read(c, p, v)) goto oob;
            pc = v ? pc + 1 : x.target; break;
        case JNZ:
            if (!cell_read(c, p, v)) goto oob;
            pc = v ? x.target : pc + 1; break;
        case ENDIF:
            pc++; break;
        case SCAN: {
            int64_t guard = 0;
            for (;;) {
                if (!cell_read(c, p, v)) goto oob;
                if (!v) break;
                p += x.arg;
                if ((++guard & 0xFFFF) == 0 && c->stop) break;
            }
            if (c->stop) continue;             // top of loop reports the stop
            pc++; break;
        }
        case LOOPX: {                          // literal program: the loop at once if every cell is RAM or plain
            const std::vector<Instr>& ops = c->prog->lit_loops[(size_t)x.arg];
            auto fast = [&](int64_t a) { return a >= ram ? (uint64_t)a < c->tape_size : a >= 0 && c->plain && c->plain[a]; };
            bool ok = fast(p);
            for (size_t k = 0; k < ops.size() && ok; k++) ok = fast(p + ops[k].off) && (ops[k].op != MUL || fast(p + ops[k].target));
            if (!ok) { pc++; break; }
            for (const Instr& y : ops) {
                switch (y.op) {
                case ADD: cell_read(c, p + y.off, v); cell_write(c, p + y.off, v + y.arg); break;
                case SET: cell_write(c, p + y.off, y.arg); break;
                default:  cell_read(c, p + y.target, v); cell_read(c, p + y.off, w); cell_write(c, p + y.off, w + v * y.arg); break;
                }
            }
            pc = x.target;
            break;
        }
        case IDIOM:                            // guard_min has kept it out of the device window
            c->ptr = c->tape + p;
            pc = idiom_thunk(c, x.arg) ? x.target : pc + 1;
            break;
        case END:
            c->status = (int32_t)Status::Ok;
            goto done;
        }
        continue;
    oob:
        c->status = (int32_t)Status::Fault;          // fault_ptr was set by cell_read / cell_write
        c->msg = "pointer left the tape";
        break;
    }
done:
    c->ptr = c->tape + p;
    return pc;
}

} // namespace bf
