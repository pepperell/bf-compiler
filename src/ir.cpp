// ir.cpp - parser and optimiser producing the IR described in bf.h.
#include "bf.h"
#include <map>
#include <algorithm>
#include <cstdio>

namespace bf {

using Code = std::vector<Instr>;

static bool is_branchy(Op o) { return o == JZ || o == JNZ || o == ENDIF || o == SCAN || o == END; }

static uint8_t inv8(uint8_t x) {            // multiplicative inverse mod 256, x odd
    uint8_t y = x;                           // Newton iteration: x*x == 1 (mod 8) for odd x
    for (int i = 0; i < 4; i++) y = (uint8_t)(y * (2 - x * y));
    return y;
}

// ---- pass 1: parse with run folding and offset fusion -----------------------
// fold=false emits exactly one instruction per command, in source order.
static Code parse(const std::string& s, bool fold, size_t& raw, std::string& err) {
    Code c;
    int32_t cur = 0, cur_src = 0;
    int depth = 0;
    auto flush = [&] { if (cur) { c.push_back({MOVE, 0, cur, 0, cur_src}); cur = 0; } };
    for (size_t i = 0; i < s.size(); i++) {
        int32_t at = (int32_t)i;
        switch (s[i]) {
        // A run of '<'/'>' becomes one MOVE (or one per command when not
        // folding).  It is flushed before any other command so the first
        // instruction of every block carries the source offset of the block's
        // first command; the device-window slow path relies on that.
        case '>': if (!cur) cur_src = at; cur++; raw++; if (!fold) flush(); break;
        case '<': if (!cur) cur_src = at; cur--; raw++; if (!fold) flush(); break;
        case '+': flush(); c.push_back({ADD, 0, 1, 0, at});  raw++; break;
        case '-': flush(); c.push_back({ADD, 0, -1, 0, at}); raw++; break;
        case '.': flush(); c.push_back({OUT, 0, 0, 0, at});  raw++; break;
        case ',': flush(); c.push_back({IN, 0, 0, 0, at});   raw++; break;
        case '[': flush(); c.push_back({JZ, 0, 0, 0, at}); depth++; raw++; break;
        case ']':
            flush(); raw++;
            if (depth == 0) { err = "unmatched ']' at byte " + std::to_string(i); return c; }
            c.push_back({JNZ, 0, 0, 0, at}); depth--;
            break;
        default: break;
        }
    }
    if (depth) { err = "unmatched '['"; return c; }
    flush();
    c.push_back({END, 0, 0, 0, (int32_t)s.size()});
    return c;
}

// ---- block canonicalisation ----------------------------------------------------
// A block is a maximal run of non-branch instructions.  Canonical form:
//   {ADD,SET,MUL,OUT,IN} with absolute offsets, at most one pending ADD/SET per
//   offset between barriers (OUT/IN/MUL), then at most one trailing MOVE.
struct Cell { int32_t off; bool has_set = false; int32_t set_val = 0; int32_t add = 0; };

// Pending writes are kept in first-touch order, not sorted: for RAM the order
// is irrelevant, but device registers with triggers (load a latch, then write
// the trigger) must see the program's order.
static void flush_pending(std::vector<Cell>& pend, Code& out, int32_t src) {
    for (Cell& c : pend) {
        if (c.has_set) out.push_back({SET, c.off, (c.set_val + c.add) & 255, 0, src});
        else if (c.add & 255) out.push_back({ADD, c.off, (int32_t)(int8_t)(c.add & 255), 0, src});
    }
    pend.clear();
}

// Writes to one cell merge only while it is the most recently touched cell;
// touching it again after other cells flushes first, so "load latch, write
// trigger, load latch, write trigger" keeps every intermediate value.
static Cell& pending_cell(std::vector<Cell>& pend, int32_t off, Code& out, int32_t src) {
    if (!pend.empty() && pend.back().off == off) return pend.back();
    for (Cell& c : pend) if (c.off == off) { flush_pending(pend, out, src); break; }
    pend.push_back(Cell{off});
    return pend.back();
}

static void canon_block(const Code& in, size_t b, size_t e, Code& out) {
    std::vector<Cell> pend;
    int32_t delta = 0;
    int32_t src = in[b].src;
    size_t first = out.size();
    for (size_t i = b; i < e; i++) {
        const Instr& x = in[i];
        switch (x.op) {
        case MOVE: delta += x.arg; break;
        case ADD:  pending_cell(pend, x.off + delta, out, src).add += x.arg; break;
        case SET: { Cell& c = pending_cell(pend, x.off + delta, out, src); c.has_set = true; c.set_val = x.arg; c.add = 0; break; }
        case OUT:  flush_pending(pend, out, src); out.push_back({OUT, x.off + delta, 0, 0, x.src}); break;
        case IN:   flush_pending(pend, out, src); out.push_back({IN, x.off + delta, 0, 0, x.src}); break;
        case MUL:  flush_pending(pend, out, src); out.push_back({MUL, x.off + delta, x.arg, x.target + delta, x.src}); break;
        default: break;
        }
    }
    flush_pending(pend, out, src);
    if (delta) out.push_back({MOVE, 0, delta, 0, src});
    // The block's first instruction is its resume point for the device-window
    // slow path, so it must carry the source offset of the block's first command
    // even when it was produced by a loop transform (which carries the '[').
    if (out.size() > first) out[first].src = src;
}

static Code canonicalise(const Code& in) {
    Code out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (is_branchy(in[i].op)) { out.push_back(in[i]); i++; continue; }
        size_t j = i;
        while (j < in.size() && !is_branchy(in[j].op)) j++;
        canon_block(in, i, j, out);
        i = j;
    }
    return out;
}

// ---- loop transforms -----------------------------------------------------------
// Innermost loop: i = JZ, j = JNZ, body = in[i+1 .. j) is one canonical block.
static bool transform_loop(const Code& in, size_t i, size_t j, Code& rep) {
    size_t n = j - (i + 1);
    const Instr* body = &in[i + 1];
    if (n == 1 && body[0].op == MOVE) {                 // [>>>]  scan loop
        rep.push_back({SCAN, 0, body[0].arg, 0});
        return true;
    }
    if (n == 0) return false;                            // [] : spins or no-op, leave it
    int32_t d0 = 0; bool add0 = false, set0 = false; int32_t set0v = 0;
    bool other_set = false;
    for (size_t k = 0; k < n; k++) {
        const Instr& x = body[k];
        if (x.op == MOVE || x.op == MUL || x.op == OUT || x.op == IN) return false;
        for (size_t q = 0; q < k; q++) if (body[q].off == x.off) return false;   // a cell written twice: keep the loop literal
        if (x.off == 0) {
            if (x.op == ADD) { add0 = true; d0 = x.arg; }
            else             { set0 = true; set0v = x.arg; }
        } else if (x.op == SET) other_set = true;
    }

    if (add0 && (d0 & 1)) {
        // Cell 0 changes by an odd amount per pass, so the loop runs exactly
        // v * inv(-d0) (mod 256) times and every ADD becomes a multiply-add.
        uint8_t mult = inv8((uint8_t)(-d0));
        if (other_set) rep.push_back({JZ, 0, 0, 0});   // SETs only happen if the loop runs
        for (size_t k = 0; k < n; k++) {
            const Instr& x = body[k];
            if (x.off == 0) continue;
            if (x.op == ADD) rep.push_back({MUL, x.off, (uint8_t)(x.arg * mult), 0});
            else             rep.push_back({SET, x.off, x.arg, 0});
        }
        rep.push_back({SET, 0, 0, 0});
        if (other_set) rep.push_back({ENDIF, 0, 0, 0});
        return true;
    }
    if (set0 && !add0 && set0v == 0) {
        // Body zeroes cell 0, so it executes at most once.
        bool only = (n == 1);
        if (!only) rep.push_back({JZ, 0, 0, 0});
        for (size_t k = 0; k < n; k++) if (body[k].off != 0) rep.push_back(body[k]);
        rep.push_back({SET, 0, 0, 0});
        if (!only) rep.push_back({ENDIF, 0, 0, 0});
        return true;
    }
    return false;
}

// Is the cell under the final pointer of canonical block [b,e) known to be zero?
static bool block_leaves_zero(const Code& c, size_t b, size_t e, bool zero_at_entry) {
    int32_t net = 0;
    if (e > b && c[e - 1].op == MOVE) net = c[e - 1].arg;
    bool zero = zero_at_entry && net == 0;
    for (size_t k = b; k < e; k++) {
        const Instr& x = c[k];
        if (x.op == MOVE && k != e - 1) return false;        // not canonical yet: offsets unreliable
        if (x.op == MOVE || x.op == OUT || x.off != net) continue;
        zero = (x.op == SET && x.arg == 0);
    }
    return zero;
}

static bool is_zero_before(const Code& c, size_t i) {
    if (i == 0) return true;                                    // fresh tape, pointer in RAM
    Op prev = c[i - 1].op;
    if (prev == JNZ || prev == ENDIF || prev == SCAN) return true;
    if (is_branchy(prev)) return false;
    size_t b = i;
    while (b > 0 && !is_branchy(c[b - 1].op)) b--;
    bool entry_zero = (b == 0) || c[b - 1].op == JNZ || c[b - 1].op == ENDIF || c[b - 1].op == SCAN;
    return block_leaves_zero(c, b, i, entry_zero);
}

static size_t find_closer(const Code& c, size_t i) {       // matching JNZ/ENDIF of the JZ at i
    int depth = 0;
    for (size_t k = i; k < c.size(); k++) {
        if (c[k].op == JZ) depth++;
        else if (c[k].op == JNZ || c[k].op == ENDIF) { if (--depth == 0) return k; }
    }
    return c.size();
}

static bool optimise_once(Code& c) {
    bool changed = false;
    Code out;
    out.reserve(c.size());
    size_t i = 0;
    while (i < c.size()) {
        if (c[i].op == JZ) {
            size_t j = find_closer(c, i);
            if (is_zero_before(out, out.size())) {            // loop can never be entered
                i = j + 1; changed = true; continue;
            }
            if (j < c.size() && c[j].op == JNZ) {
                bool innermost = true;
                for (size_t k = i + 1; k < j; k++) if (is_branchy(c[k].op)) { innermost = false; break; }
                if (innermost) {
                    Code rep;
                    if (transform_loop(c, i, j, rep)) {
                        for (Instr& r : rep) r.src = c[i].src;
                        out.insert(out.end(), rep.begin(), rep.end());
                        i = j + 1; changed = true; continue;
                    }
                }
            }
        }
        out.push_back(c[i]);
        i++;
    }
    c = canonicalise(out);
    return changed;
}

static bool fix_targets(Code& c, std::string& err) {
    std::vector<int32_t> st;
    for (size_t i = 0; i < c.size(); i++) {
        if (c[i].op == JZ) st.push_back((int32_t)i);
        else if (c[i].op == JNZ || c[i].op == ENDIF) {
            if (st.empty()) { err = "internal: unbalanced IR"; return false; }
            int32_t o = st.back(); st.pop_back();
            c[o].target = (int32_t)i + 1;
            c[i].target = o + 1;
        }
    }
    if (!st.empty()) { err = "internal: unbalanced IR"; return false; }
    return true;
}

Program compile(const std::string& src, bool optimise) {
    Program p;
    p.src_size = src.size();
    // The literal program is always built: it is what executes inside the
    // device window, where optimised instructions must not be used.
    Code lit = parse(src, false, p.raw_ops, p.error);
    if (!p.error.empty()) return p;
    if (!fix_targets(lit, p.error)) return p;
    if (!optimise) { p.literal = lit; p.code = std::move(lit); return p; }
    size_t raw = 0;
    Code c = parse(src, true, raw, p.error);
    // For the device-window slow path the literal program may fold runs of '<' / '>'
    // into one move: moving touches no cell, so devices see exactly the same accesses,
    // and slow-path entries and exits are block starts, which begin a run.  Without
    // this, a loop copying a value into a register one unit at a time walks the
    // pointer between RAM and the window cell by cell on every turn.
    p.literal = c;
    if (!fix_targets(p.literal, p.error)) return p;
    c = canonicalise(c);
    for (int iter = 0; iter < 32 && optimise_once(c); iter++) {}
    if (!fix_targets(c, p.error)) return p;
    p.code = std::move(c);
    return p;
}

std::string disassemble(const Program& p) {
    std::string s;
    char buf[160];
    int indent = 0;
    for (size_t i = 0; i < p.code.size(); i++) {
        const Instr& x = p.code[i];
        if (x.op == JNZ || x.op == ENDIF) indent--;
        snprintf(buf, sizeof buf, "%5zu: %*s", i, indent * 2, "");
        s += buf;
        switch (x.op) {
        case ADD:   snprintf(buf, sizeof buf, "ADD   [%d] += %d\n", x.off, x.arg); break;
        case SET:   snprintf(buf, sizeof buf, "SET   [%d] = %d\n", x.off, x.arg); break;
        case MUL:   snprintf(buf, sizeof buf, "MUL   [%d] += [%d] * %d\n", x.off, x.target, x.arg); break;
        case MOVE:  snprintf(buf, sizeof buf, "MOVE  %+d\n", x.arg); break;
        case OUT:   snprintf(buf, sizeof buf, "OUT   [%d]\n", x.off); break;
        case IN:    snprintf(buf, sizeof buf, "IN    [%d]\n", x.off); break;
        case JZ:    snprintf(buf, sizeof buf, "JZ    -> %d\n", x.target); break;
        case JNZ:   snprintf(buf, sizeof buf, "JNZ   -> %d\n", x.target); break;
        case ENDIF: snprintf(buf, sizeof buf, "ENDIF\n"); break;
        case SCAN:  snprintf(buf, sizeof buf, "SCAN  %+d\n", x.arg); break;
        case END:   snprintf(buf, sizeof buf, "END\n"); break;
        }
        s += buf;
        if (x.op == JZ) indent++;
    }
    return s;
}

const char* status_name(Status s) {
    switch (s) {
    case Status::Ok:      return "ok";
    case Status::Running: return "running";
    case Status::Halted:  return "halted";
    case Status::Stopped: return "stopped";
    case Status::Fault:   return "fault";
    case Status::Error:   return "error";
    case Status::Paused:  return "paused";
    }
    return "?";
}

} // namespace bf
