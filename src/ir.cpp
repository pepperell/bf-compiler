// ir.cpp - parser and optimiser producing the IR described in bf.h.
#include "bf.h"
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <optional>

namespace bf {

using Code = std::vector<Instr>;

static bool is_branchy(Op o) { return o == JZ || o == JNZ || o == ENDIF || o == SCAN || o == IDIOM || o == LOOPX || o == END; }

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
    {
        // A body whose last write to the loop cell clears it runs at most once,
        // whatever else it writes: it becomes an if-block in the same order.
        int last = -1;
        bool moves = false;
        for (size_t k = 0; k < n; k++) {
            const Instr& x = body[k];
            if (x.op == MOVE) moves = true;
            else if (x.op != OUT && x.off == 0) last = (int)k;
        }
        if (!moves && last >= 0 && body[last].op == SET && body[last].arg == 0) {
            bool only = (n == 1);
            if (!only) rep.push_back({JZ, 0, 0, 0});
            for (size_t k = 0; k < n; k++) rep.push_back(body[k]);
            if (!only) rep.push_back({ENDIF, 0, 0, 0});
            return true;
        }
    }
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

// Loop [b-1 .. e] whose body holds only straight-line code and pointer-neutral
// if-blocks: does every pass end by clearing the loop cell?  Then it runs at most once.
static bool clears_loop_cell(const Code& c, size_t b, size_t e) {
    int32_t d = 0;
    std::vector<int32_t> st;
    int last = -1;                       // 1: the last write to the loop cell is a top-level SET 0
    for (size_t i = b; i < e; i++) {
        const Instr& x = c[i];
        switch (x.op) {
        case MOVE: d += x.arg; break;
        case JZ: st.push_back(d); break;
        case ENDIF: if (st.empty() || st.back() != d) return false; st.pop_back(); break;
        case OUT: break;
        case ADD: case SET: case MUL: case IN:
            if (x.off + d == 0) last = (x.op == SET && x.arg == 0 && st.empty()) ? 1 : 0;
            break;
        default: return false;
        }
    }
    return d == 0 && st.empty() && last == 1;
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
                } else if (clears_loop_cell(c, i + 1, j)) {
                    c[j].op = ENDIF;                           // an if-block with if-blocks inside
                    changed = true;
                }
            }
        }
        out.push_back(c[i]);
        i++;
    }
    c = canonicalise(out);
    return changed;
}

// ---- known values across blocks ------------------------------------------------------
// A forward pass over the structured program tracks cells holding known constants
// (relative to the pointer): through if-blocks (the meet of both ways), into loops
// (what the body does not write), and out of loops (the loop cell is 0).  Each
// straight-line block is then rebuilt from its net effect, a linear function of
// the cells' values on entry, with the known values folded in: a copy through a
// scratch cell that is known to be zero becomes one multiply-add, a store of the
// value a cell already holds disappears.
//
// Device registers: a block that can reach the device window runs the literal
// program instead (see Machine::run), whatever its optimised form, so rebuilt
// blocks keep their lowest reachable offset (adding an "ADD +0" there if the
// lowest access was removed) and keep the source offset of their first command.
namespace {

using Known = std::map<int32_t, uint8_t>;     // cell offset (from the pointer) -> value

static void shift_known(Known& k, int32_t d) {
    if (!d) return;
    Known n;
    for (auto& kv : k) n[kv.first - d] = kv.second;
    k.swap(n);
}
static Known meet(const Known& a, const Known& b) {
    Known r;
    for (auto& kv : a) { auto it = b.find(kv.first); if (it != b.end() && it->second == kv.second) r.insert(kv); }
    return r;
}
static std::optional<uint8_t> known_val(const Known& k, int32_t o) {
    auto it = k.find(o);
    if (it == k.end()) return std::nullopt;
    return it->second;
}

// Cells written by [b, e) relative to the pointer at b; false if the pointer
// does not come back to where it started (or could wander: a scan).
static bool written_cells(const Code& c, size_t b, size_t e, std::set<int32_t>& w) {
    int32_t d = 0;
    std::vector<int32_t> st;
    for (size_t i = b; i < e; i++) {
        const Instr& x = c[i];
        switch (x.op) {
        case MOVE: d += x.arg; break;
        case ADD: case SET: case MUL: case IN: w.insert(x.off + d); break;
        case OUT: case END: break;
        case JZ: st.push_back(d); break;
        case JNZ: case ENDIF: if (st.empty() || st.back() != d) return false; st.pop_back(); break;
        default: return false;
        }
    }
    return d == 0 && st.empty();
}

struct SLin {                                  // c + sum k * entry[cell]
    uint8_t c = 0;
    std::map<int32_t, uint8_t> k;
    bool is_const() const { return k.empty(); }
};

// Rebuild the straight-line run [b, e) of ADD/SET/MUL (no moves) at the pointer,
// appending to out; k holds the known values and is updated.
static void rebuild_run(const Code& c, size_t b, size_t e, Known& k, Code& out, int32_t src) {
    std::map<int32_t, SLin> val;               // current value of every touched cell
    auto cur = [&](int32_t o) -> SLin& {
        auto it = val.find(o);
        if (it != val.end()) return it->second;
        SLin l;
        if (auto v = known_val(k, o)) l.c = *v; else l.k[o] = 1;
        return val[o] = l;
    };
    for (size_t i = b; i < e; i++) {
        const Instr& x = c[i];
        SLin& l = cur(x.off);
        if (x.op == ADD) l.c = (uint8_t)(l.c + x.arg);
        else if (x.op == SET) { l = SLin(); l.c = (uint8_t)x.arg; }
        else {                                  // MUL
            SLin s = cur(x.target);
            SLin& d = cur(x.off);
            uint8_t m = (uint8_t)x.arg;
            d.c = (uint8_t)(d.c + s.c * m);
            for (auto& kv : s.k) { uint8_t& q = d.k[kv.first]; q = (uint8_t)(q + kv.second * m); if (!q) d.k.erase(kv.first); }
        }
    }
    // Emit: a cell is written only after every cell that reads its entry value.
    std::vector<int32_t> todo;
    for (auto& kv : val) {
        const SLin& l = kv.second;
        int32_t o = kv.first;
        bool same = l.k.size() == 1 && l.k.begin()->first == o && l.k.begin()->second == 1 && l.c == 0;
        if (same) continue;
        if (l.is_const()) { if (auto v = known_val(k, o); v && *v == l.c) continue; }
        todo.push_back(o);
    }
    bool ok = true;
    for (int32_t o : todo) {                    // own coefficient must be 0 or 1
        auto it = val[o].k.find(o);
        if (it != val[o].k.end() && it->second != 1) ok = false;
    }
    std::vector<int32_t> order;
    if (ok) {
        std::set<int32_t> pending(todo.begin(), todo.end());
        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                int32_t o = *it;
                bool blocked = false;           // another pending cell still reads o's entry value
                for (int32_t r : pending) if (r != o && val[r].k.count(o)) { blocked = true; break; }
                if (blocked) { ++it; continue; }
                order.push_back(o);
                it = pending.erase(it);
                progress = true;
            }
            if (!progress) { ok = false; break; }
        }
    }
    Code built;
    if (ok) {
        for (int32_t o : order) {
            const SLin& l = val[o];
            bool keep = l.k.count(o) > 0;
            if (!keep) {
                auto kv = known_val(k, o);              // known cells have no own coefficient: SET unless it holds c already
                if (!(kv && *kv == l.c)) built.push_back({SET, o, l.c, 0, src});
            } else if (l.c) built.push_back({ADD, o, (int32_t)(int8_t)l.c, 0, src});
            for (auto& kv : l.k) if (kv.first != o) built.push_back({MUL, o, kv.second, kv.first, src});
        }
    }
    if (!ok || built.size() > e - b) {
        // as written, minus what the known values make redundant
        built.clear();
        Known kk = k;
        for (size_t i = b; i < e; i++) {
            Instr x = c[i];
            x.src = src;
            auto kv = known_val(kk, x.off);
            if (x.op == SET) { if (kv && *kv == (uint8_t)x.arg) continue; kk[x.off] = (uint8_t)x.arg; }
            else if (x.op == ADD) { if (kv) kk[x.off] = (uint8_t)(*kv + x.arg); }
            else {
                auto sv = known_val(kk, x.target);
                if (sv && *sv == 0) continue;
                if (sv) { x.op = ADD; x.arg = (int32_t)(int8_t)(uint8_t)(*sv * x.arg); x.target = 0; if (kv) kk[x.off] = (uint8_t)(*kv + x.arg); }
                else kk.erase(x.off);
            }
            built.push_back(x);
        }
    }
    out.insert(out.end(), built.begin(), built.end());
    for (auto& kv : val) {
        if (kv.second.is_const()) k[kv.first] = kv.second.c;
        else k.erase(kv.first);
    }
}

static void rebuild_range(const Code& c, size_t b, size_t e, Known& k, Code& out, const std::vector<size_t>& closer, bool dry = false);

// One block: the maximal run [b, e) of non-branch instructions.
static void rebuild_block(const Code& c, size_t b, size_t e, Known& k, Code& out) {
    size_t first = out.size();
    int32_t src = c[b].src;
    int64_t minoff = 0, delta = 0;             // the block's lowest reach, as block_min_reach
    for (size_t i = b; i < e; i++) {
        const Instr& y = c[i];
        if (y.op == MOVE) { delta += y.arg; minoff = std::min(minoff, delta); }
        else {
            minoff = std::min(minoff, delta + y.off);
            if (y.op == MUL) minoff = std::min(minoff, delta + y.target);
        }
    }
    size_t i = b;
    while (i < e) {
        Op o = c[i].op;
        if (o == MOVE) { out.push_back(c[i]); out.back().src = src; shift_known(k, c[i].arg); i++; continue; }
        if (o == OUT) { out.push_back(c[i]); i++; continue; }
        if (o == IN) { out.push_back(c[i]); k.erase(c[i].off); i++; continue; }
        size_t j = i;
        while (j < e && (c[j].op == ADD || c[j].op == SET || c[j].op == MUL)) j++;
        rebuild_run(c, i, j, k, out, src);
        i = j;
    }
    int64_t nmin = 0;
    delta = 0;
    for (size_t q = first; q < out.size(); q++) {
        const Instr& y = out[q];
        if (y.op == MOVE) { delta += y.arg; nmin = std::min(nmin, delta); }
        else {
            nmin = std::min(nmin, delta + y.off);
            if (y.op == MUL) nmin = std::min(nmin, delta + y.target);
        }
    }
    if (nmin > minoff) out.insert(out.begin() + (ptrdiff_t)first, Instr{ADD, (int32_t)minoff, 0, 0, src});
    if (out.size() > first) out[first].src = src;
}

static void rebuild_range(const Code& c, size_t b, size_t e, Known& k, Code& out, const std::vector<size_t>& closer, bool dry) {
    size_t i = b;
    while (i < e) {
        const Instr& x = c[i];
        switch (x.op) {
        case JZ: {
            size_t j = closer[i];
            auto z = known_val(k, 0);
            if (z && *z == 0) { i = j + 1; break; }               // never entered
            if (c[j].op == ENDIF) {
                std::set<int32_t> w;
                bool neutral = written_cells(c, i + 1, j, w);
                if (z && neutral) { rebuild_range(c, i + 1, j, k, out, closer, dry); i = j + 1; break; }   // always entered
                Known skip = k; skip[0] = 0;
                Known kb = k;
                out.push_back(x);
                rebuild_range(c, i + 1, j, kb, out, closer, dry);
                out.push_back(c[j]);
                if (neutral) k = meet(skip, kb); else k.clear();
            } else {
                std::set<int32_t> w;
                bool neutral = written_cells(c, i + 1, j, w);
                Known kl;
                if (neutral) {
                    kl = k;
                    for (int32_t o : w) kl.erase(o);
                    // Cells the body writes but leaves as they were on entry stay known:
                    // what holds on entry and after a pass from the weaker start holds at
                    // the top of every pass.  (Inner loops of the trial use the weaker rule,
                    // so the cost stays linear in the nesting depth.)
                    if (!dry) {
                        Known kt = kl;
                        Code scratch;
                        rebuild_range(c, i + 1, j, kt, scratch, closer, true);
                        Known strong = meet(k, kt);
                        for (auto& kv : kl) strong.insert(kv);
                        kl.swap(strong);
                    }
                }
                Known kb = kl;
                out.push_back(x);
                rebuild_range(c, i + 1, j, kb, out, closer, dry);
                out.push_back(c[j]);
                k = kl;
                k[0] = 0;
            }
            i = j + 1;
            break;
        }
        case SCAN: out.push_back(x); k.clear(); k[0] = 0; i++; break;
        case END: out.push_back(x); i++; break;
        case JNZ: case ENDIF: case IDIOM: i++; break;   // not reached: closers are consumed with their JZ
        default: {
            size_t j = i;
            while (j < e && !is_branchy(c[j].op)) j++;
            rebuild_block(c, i, j, k, out);
            i = j;
        }
        }
    }
}

} // namespace

static void rebuild_blocks(Code& c) {
    std::vector<size_t> closer(c.size(), SIZE_MAX), st;
    for (size_t i = 0; i < c.size(); i++) {
        if (c[i].op == JZ) st.push_back(i);
        else if ((c[i].op == JNZ || c[i].op == ENDIF) && !st.empty()) { closer[st.back()] = i; st.pop_back(); }
    }
    Code out;
    out.reserve(c.size());
    Known k;                                   // a fresh tape is zero, but cells are not tracked until written
    rebuild_range(c, 0, c.size(), k, out, closer);
    c.swap(out);
}

// ---- loop idioms -----------------------------------------------------------------
// The BFL compiler builds multi-byte arithmetic from a few byte loops that run a
// cell's value times: adding a byte into a multi-byte number one unit at a time
// with a carry test, a saturating subtract (the byte compare), halving with a
// remainder bit, and byte division.  The loop transforms above leave them alone
// because their bodies contain if-blocks.
//
// Here the body of each innermost loop with if-blocks is executed symbolically:
// one pass as a function of the cells' values on entry, as a set of paths, each
// with its conditions and the cells' final values as linear expressions.  Cells
// that end every pass holding the same constant are scratch cells; after the
// first pass they hold it on entry too, so a second symbolic run assumes it.
// The resulting one-pass function is compared with each idiom's rule on a set of
// points that covers every case: conditions and results each involve one cell
// compared with or offset by a constant, so the points are those constants and
// two other values per cell.  On a match an IDIOM instruction at the top of the
// body runs the whole loop at once, provided the scratch cells hold their values
// (always, for compiled code); otherwise the body runs as written.
namespace {

constexpr int NV = 24;                       // cells a candidate body may touch

struct Lin {                                 // c + sum k[v] * entry[v]  (mod 256)
    uint8_t c = 0;
    uint8_t k[NV] = {};
    bool is_const() const { for (int i = 0; i < NV; i++) if (k[i]) return false; return true; }
    bool same_vars(const Lin& o) const { return memcmp(k, o.k, NV) == 0; }
    uint8_t eval(const uint8_t* in) const {
        unsigned s = c;
        for (int i = 0; i < NV; i++) if (k[i]) s += (unsigned)k[i] * in[i];
        return (uint8_t)s;
    }
    // one cell with coefficient 1 (or -1 when allow_neg) plus a constant, or a constant (v = -1)
    bool single(int& v, bool allow_neg = false) const {
        v = -1;
        for (int i = 0; i < NV; i++) {
            if (!k[i]) continue;
            if (v >= 0 || !(k[i] == 1 || (allow_neg && k[i] == 255))) return false;
            v = i;
        }
        return true;
    }
};
struct Cond { Lin e; bool nz; };
struct Path { std::vector<Lin> v; std::vector<Cond> conds; };

struct Body {
    std::vector<Instr> ins;                  // offsets relative to the loop cell; no MOVEs
    std::map<int32_t, int> slot;             // cell offset -> variable
    std::vector<int32_t> off;                // variable -> cell offset
    int slot_of(int32_t o) {
        auto it = slot.find(o);
        if (it != slot.end()) return it->second;
        int s = (int)off.size();
        slot[o] = s; off.push_back(o);
        return s;
    }
};

// The body (b, e) of a loop: straight-line code and pointer-neutral if-blocks only.
static bool build_body(const Code& c, size_t b, size_t e, Body& B) {
    int32_t d = 0;
    std::vector<std::pair<int32_t, size_t>> st;
    B.slot_of(0);
    for (size_t i = b; i < e; i++) {
        const Instr& x = c[i];
        switch (x.op) {
        case MOVE: d += x.arg; break;
        case ADD: case SET: B.slot_of(x.off + d); B.ins.push_back({x.op, x.off + d, x.arg, 0, 0}); break;
        case MUL: B.slot_of(x.off + d); B.slot_of(x.target + d); B.ins.push_back({MUL, x.off + d, x.arg, x.target + d, 0}); break;
        case JZ: B.slot_of(d); st.push_back({d, B.ins.size()}); B.ins.push_back({JZ, d, 0, 0, 0}); break;
        case ENDIF:
            if (st.empty() || st.back().first != d) return false;
            B.ins[st.back().second].target = (int32_t)B.ins.size();
            st.pop_back();
            B.ins.push_back({ENDIF, 0, 0, 0, 0});
            break;
        default: return false;
        }
        if (B.off.size() > (size_t)NV) return false;
    }
    return d == 0 && st.empty();
}

// Is e known zero (0), known non-zero (1) or open (-1) under the path's conditions?
static int decide(const Lin& e, const std::vector<Cond>& conds) {
    if (e.is_const()) return e.c != 0;
    for (const Cond& cd : conds) {
        if (!cd.e.same_vars(e)) continue;
        uint8_t diff = (uint8_t)(e.c - cd.e.c);
        if (!cd.nz) return diff != 0;            // e = cd.e + diff with cd.e == 0
        if (diff == 0) return 1;
    }
    return -1;
}

static bool run_symbolic(const Body& B, const std::vector<Lin>& init, std::vector<Path>& out) {
    struct Work { Path p; size_t pc; };
    std::vector<Work> work;
    work.push_back({Path{init, {}}, 0});
    while (!work.empty()) {
        Work w = std::move(work.back());
        work.pop_back();
        while (w.pc < B.ins.size()) {
            const Instr& x = B.ins[w.pc];
            switch (x.op) {
            case ADD: { Lin& l = w.p.v[B.slot.at(x.off)]; l.c = (uint8_t)(l.c + x.arg); break; }
            case SET: { Lin& l = w.p.v[B.slot.at(x.off)]; l = Lin(); l.c = (uint8_t)x.arg; break; }
            case MUL: {
                Lin s = w.p.v[B.slot.at(x.target)];
                Lin& l = w.p.v[B.slot.at(x.off)];
                uint8_t m = (uint8_t)x.arg;
                l.c = (uint8_t)(l.c + s.c * m);
                for (int i = 0; i < NV; i++) l.k[i] = (uint8_t)(l.k[i] + s.k[i] * m);
                break;
            }
            case JZ: {
                const Lin l = w.p.v[B.slot.at(x.off)];
                // on the path where it is zero, every cell holding that value is known zero
                auto zero = [&](Path& p) { for (Lin& v : p.v) if (v.c == l.c && v.same_vars(l)) v = Lin(); };
                int d = decide(l, w.p.conds);
                if (d == 0) { zero(w.p); w.pc = (size_t)x.target; break; }
                if (d < 0) {
                    Work z = w;
                    z.p.conds.push_back({l, false});
                    zero(z.p);
                    z.pc = (size_t)x.target;
                    work.push_back(std::move(z));
                    w.p.conds.push_back({w.p.v[B.slot.at(x.off)], true});
                    if (work.size() + out.size() > 64) return false;
                }
                break;
            }
            default: break;                       // ENDIF
            }
            w.pc++;
        }
        out.push_back(std::move(w.p));
        if (out.size() > 64) return false;
    }
    return true;
}

// The one-pass rule of an idiom; roles are variables.
struct Cand { IdiomKind kind; int sign = 1; std::vector<int> role; };

static void rule_step(const Cand& c, uint8_t* v) {
    const int X = 0;
    switch (c.kind) {
    case IDIOM_ADD:
        v[X]--;
        for (int r : c.role) {
            v[r] = (uint8_t)(v[r] + c.sign);
            if (v[r] != (c.sign > 0 ? 0 : 255)) break;
        }
        break;
    case IDIOM_SATSUB: {
        int b = c.role[0];
        if (v[b]) { v[X]--; v[b]--; } else v[X] = 0;
        break;
    }
    case IDIOM_HALVE:
        v[X]--;
        if (v[X]) { v[X]--; v[c.role[0]]++; } else v[c.role[1]]++;
        break;
    case IDIOM_DIVMOD: {
        int r = c.role[0], cc = c.role[1], q = c.role[2], b = c.role[3];
        v[X]--; v[r]++; v[cc]--;
        if (!v[cc]) { v[q]++; v[r] = 0; v[cc] = v[b]; }
        break;
    }
    }
}

struct Summary {
    std::vector<Path> paths;
    int n = 0;                                   // variables
    std::vector<bool> guard;                     // scratch variable
    std::vector<uint8_t> gval;
};

static bool summary_eval(const Summary& S, const uint8_t* in, uint8_t* outv) {
    const Path* hit = nullptr;
    for (const Path& p : S.paths) {
        bool ok = true;
        for (const Cond& cd : p.conds) if ((cd.e.eval(in) != 0) != cd.nz) { ok = false; break; }
        if (!ok) continue;
        if (hit) return false;
        hit = &p;
    }
    if (!hit) return false;
    for (int s = 0; s < S.n; s++) outv[s] = hit->v[(size_t)s].eval(in);
    return true;
}

// Compare the summary with the rule on enough points to cover every case.
static bool verify(const Summary& S, const Cand& c) {
    std::vector<std::vector<uint8_t>> vals((size_t)S.n);
    std::vector<int> vars;
    for (int s = 0; s < S.n; s++) {
        if (S.guard[(size_t)s]) continue;
        std::set<int> special = {0, 1, 255};
        for (const Path& p : S.paths)
            for (const Cond& cd : p.conds) {
                int v;
                if (!cd.e.single(v, true)) return false;
                if (v == s) special.insert(cd.e.k[v] == 1 ? (uint8_t)(-cd.e.c) : cd.e.c);
            }
        std::vector<uint8_t>& vs = vals[(size_t)s];
        for (int x : special) if (!(s == 0 && x == 0)) vs.push_back((uint8_t)x);
        int others = 0;
        for (int x : {37, 100, 201, 150, 77, 13}) if (others < 2 && !special.count(x)) { vs.push_back((uint8_t)x); others++; }
        vars.push_back(s);
    }
    for (const Path& p : S.paths)
        for (int s = 0; s < S.n; s++) { int v; if (!S.guard[(size_t)s] && !p.v[(size_t)s].single(v)) return false; }
    size_t total = 1;
    for (int s : vars) { total *= vals[(size_t)s].size(); if (total > 40000) return false; }
    std::vector<size_t> idx(vars.size(), 0);
    uint8_t in[NV] = {}, a[NV], b[NV];
    for (int s = 0; s < S.n; s++) if (S.guard[(size_t)s]) in[s] = S.gval[(size_t)s];
    for (;;) {
        for (size_t i = 0; i < vars.size(); i++) in[vars[i]] = vals[(size_t)vars[i]][idx[i]];
        if (!summary_eval(S, in, a)) return false;
        memcpy(b, in, NV);
        rule_step(c, b);
        for (int s = 0; s < S.n; s++) if (!S.guard[(size_t)s] && a[s] != b[s]) return false;
        size_t i = 0;
        while (i < vars.size() && ++idx[i] == vals[(size_t)vars[i]].size()) idx[i++] = 0;
        if (i == vars.size()) break;
    }
    return true;
}

static bool identity(const Lin& l, int s) { int v; return l.single(v) && v == s && l.c == 0; }

// Guess an idiom's roles from the summary; verify() decides.
static bool match_summary(const Summary& S, Cand& out) {
    const int X = 0;
    std::vector<int> data, changed((size_t)S.n, 0);
    for (int s = 1; s < S.n; s++) {
        if (S.guard[(size_t)s]) continue;
        for (const Path& p : S.paths) if (!identity(p.v[(size_t)s], s)) changed[(size_t)s]++;
        if (changed[(size_t)s]) data.push_back(s);
    }
    if (data.empty()) return false;
    std::set<std::pair<int, uint8_t>> xs;              // the loop cell's final value: (variable or -1, constant)
    for (const Path& p : S.paths) {
        int v;
        if (!p.v[X].single(v) || (v != X && v != -1)) return false;
        xs.insert({v, p.v[X].c});
    }
    auto path_changes = [&](int s, int xv, uint8_t xc) {    // s changes on a path ending with X = xv + xc
        for (const Path& p : S.paths) {
            int v; p.v[X].single(v);
            if (v == xv && p.v[X].c == xc && !identity(p.v[(size_t)s], s)) return true;
        }
        return false;
    };
    if (xs.size() == 1 && *xs.begin() == std::make_pair(X, (uint8_t)255)) {
        Cand c; c.kind = IDIOM_ADD;
        std::vector<int> d = data;
        std::stable_sort(d.begin(), d.end(), [&](int a, int b) { return changed[(size_t)a] > changed[(size_t)b]; });
        for (size_t i = 1; i < d.size(); i++) if (changed[(size_t)d[i]] == changed[(size_t)d[i - 1]]) goto not_add;
        for (const Path& p : S.paths) {
            const Lin& l = p.v[(size_t)d[0]];
            int v; if (l.single(v) && v == d[0]) { c.sign = l.c == 1 ? 1 : -1; break; }
        }
        c.role = d;
        if (verify(S, c)) { out = c; return true; }
    not_add:
        if (data.size() == 3) {
            int q = -1, r = -1, cc = -1, b = -1;
            for (int s : data) {
                bool inc = false, zero = false, dec = false; int from = -1;
                for (const Path& p : S.paths) {
                    const Lin& l = p.v[(size_t)s]; int v; l.single(v);
                    if (v == s && l.c == 1) inc = true;
                    else if (v == s && l.c == 255) dec = true;
                    else if (v == -1 && l.c == 0) zero = true;
                    else if (v >= 0 && v != s && l.c == 0) from = v;
                }
                if (inc && zero) r = s;
                else if (inc) q = s;
                else if (dec && from >= 0) { cc = s; b = from; }
            }
            if (q >= 0 && r >= 0 && cc >= 0 && b >= 0 && !S.guard[(size_t)b]) {
                Cand c2; c2.kind = IDIOM_DIVMOD; c2.role = {r, cc, q, b};
                if (verify(S, c2)) { out = c2; return true; }
            }
        }
        return false;
    }
    using XS = std::pair<int, uint8_t>;
    if (xs == std::set<XS>{XS(X, (uint8_t)255), XS(-1, (uint8_t)0)} && data.size() == 1) {
        Cand c; c.kind = IDIOM_SATSUB; c.role = data;
        if (verify(S, c)) { out = c; return true; }
        return false;
    }
    if ((xs == std::set<XS>{XS(X, (uint8_t)254), XS(X, (uint8_t)255)} || xs == std::set<XS>{XS(X, (uint8_t)254), XS(-1, (uint8_t)0)}) && data.size() == 2) {
        Cand c; c.kind = IDIOM_HALVE;
        int h = path_changes(data[0], X, 254) ? data[0] : data[1];
        c.role = {h, h == data[0] ? data[1] : data[0]};
        if (verify(S, c)) { out = c; return true; }
    }
    return false;
}

static bool match_idiom(const Code& c, size_t b, size_t e, Idiom& id) {
    Body B;
    if (!build_body(c, b, e, B)) return false;
    int n = (int)B.off.size();
    bool has_if = false;
    for (const Instr& x : B.ins) if (x.op == JZ) has_if = true;
    if (!has_if) return false;
    std::vector<Lin> init((size_t)n);
    for (int s = 0; s < n; s++) init[(size_t)s].k[s] = 1;
    std::vector<Path> p1;
    if (!run_symbolic(B, init, p1)) return false;
    Summary S;
    S.n = n;
    S.guard.assign((size_t)n, false);
    S.gval.assign((size_t)n, 0);
    std::vector<bool> used((size_t)n, false);
    for (const Path& p : p1) {
        for (const Cond& cd : p.conds) for (int s = 0; s < n; s++) if (cd.e.k[s]) used[(size_t)s] = true;
        for (const Lin& l : p.v) for (int s = 0; s < n; s++) if (l.k[s]) used[(size_t)s] = true;
    }
    for (int s = 1; s < n; s++) {
        // every path leaves it holding one constant, or leaves it alone
        int k = -1;
        bool ok = true;
        for (const Path& p : p1) {
            const Lin& l = p.v[(size_t)s];
            if (identity(l, s)) continue;
            if (!l.is_const() || (k >= 0 && l.c != k)) { ok = false; break; }
            k = l.c;
        }
        if (ok && k >= 0) { S.guard[(size_t)s] = true; S.gval[(size_t)s] = (uint8_t)k; }
    }
    for (int s = 0; s < n; s++) if (S.guard[(size_t)s]) { init[(size_t)s] = Lin(); init[(size_t)s].c = S.gval[(size_t)s]; }
    if (!run_symbolic(B, init, S.paths)) return false;
    Cand cand;
    static const bool debug = getenv("BF_IDIOM_DEBUG") != nullptr;
    if (debug) {
        fprintf(stderr, "candidate: %d cells, %zu paths\n", n, S.paths.size());
        for (const Path& p : S.paths) {
            fprintf(stderr, "  path:");
            for (const Cond& cd : p.conds) {
                fprintf(stderr, " (%u", cd.e.c);
                for (int s = 0; s < n; s++) if (cd.e.k[s]) fprintf(stderr, "+%u*[%d]", cd.e.k[s], B.off[(size_t)s]);
                fprintf(stderr, ")%s", cd.nz ? "!=0" : "==0");
            }
            fprintf(stderr, "\n   ");
            for (int s = 0; s < n; s++) {
                const Lin& l = p.v[(size_t)s];
                fprintf(stderr, " [%d]%s=%u", B.off[(size_t)s], S.guard[(size_t)s] ? "g" : "", l.c);
                for (int t = 0; t < n; t++) if (l.k[t]) fprintf(stderr, "+%u*[%d]", l.k[t], B.off[(size_t)t]);
            }
            fprintf(stderr, "\n");
        }
    }
    if (!match_summary(S, cand)) return false;
    id = Idiom{};
    id.kind = cand.kind;
    id.sign = (int8_t)cand.sign;
    for (int r : cand.role) id.cells.push_back(B.off[(size_t)r]);
    for (int s = 0; s < n; s++) {
        if (S.guard[(size_t)s]) id.guards.push_back({B.off[(size_t)s], S.gval[(size_t)s], (bool)used[(size_t)s]});
        id.lo = std::min(id.lo, B.off[(size_t)s]);
        id.hi = std::max(id.hi, B.off[(size_t)s]);
    }
    return true;
}

} // namespace

// Put an IDIOM instruction at the top of every loop that matches one.
static void find_idioms(Code& c, std::vector<Idiom>& table) {
    std::vector<size_t> closer(c.size(), SIZE_MAX), st;
    for (size_t i = 0; i < c.size(); i++) {
        if (c[i].op == JZ) st.push_back(i);
        else if ((c[i].op == JNZ || c[i].op == ENDIF) && !st.empty()) { closer[st.back()] = i; st.pop_back(); }
    }
    Code out;
    out.reserve(c.size() + c.size() / 16);
    for (size_t i = 0; i < c.size(); i++) {
        out.push_back(c[i]);
        if (c[i].op != JZ) continue;
        size_t j = closer[i];
        if (j == SIZE_MAX || c[j].op != JNZ) continue;
        bool simple = true;
        for (size_t k = i + 1; k < j && simple; k++) {
            Op o = c[k].op;
            if (o == JNZ || o == SCAN || o == OUT || o == IN || o == END || o == IDIOM) simple = false;
        }
        if (!simple) continue;
        Idiom id;
        if (!match_idiom(c, i + 1, j, id)) continue;
        table.push_back(std::move(id));
        out.push_back({IDIOM, table.back().lo, (int32_t)table.size() - 1, 0, c[i + 1].src});
    }
    c = std::move(out);
}

// Run idiom d with the loop cell at p (in RAM, every touched cell addressable).
// Returns false, changing nothing, when a scratch cell does not hold its value.
bool idiom_exec(const Idiom& d, uint8_t* p) {
    for (const Idiom::Guard& g : d.guards) if (g.check && p[g.off] != g.val) return false;
    unsigned x = p[0];
    switch (d.kind) {
    case IDIOM_ADD: {
        unsigned carry = x;
        for (int32_t o : d.cells) {
            if (!carry) break;
            if (d.sign > 0) { unsigned s = p[o] + carry; p[o] = (uint8_t)s; carry = s >> 8; }
            else { int s = (int)p[o] - (int)carry; p[o] = (uint8_t)s; carry = s < 0; }
        }
        break;
    }
    case IDIOM_SATSUB: { uint8_t& b = p[d.cells[0]]; b = b > x ? (uint8_t)(b - x) : 0; break; }
    case IDIOM_HALVE: p[d.cells[0]] = (uint8_t)(p[d.cells[0]] + (x >> 1)); p[d.cells[1]] = (uint8_t)(p[d.cells[1]] + (x & 1)); break;
    case IDIOM_DIVMOD: {
        uint8_t& r = p[d.cells[0]]; uint8_t& cc = p[d.cells[1]]; uint8_t& q = p[d.cells[2]];
        uint8_t b = p[d.cells[3]];
        for (unsigned i = 0; i < x; i++) { r++; cc--; if (!cc) { q++; r = 0; cc = b; } }
        break;
    }
    }
    p[0] = 0;
    for (const Idiom::Guard& g : d.guards) p[g.off] = g.val;
    return true;
}

// The literal program keeps every command, but a loop that the optimiser would
// turn into multiply-adds gets a LOOPX in front of its body: the interpreter does
// the loop in one step when all the cells it touches behave like memory.
static void add_loop_accelerators(Code& c, std::vector<std::vector<Instr>>& table) {
    Code out;
    out.reserve(c.size() + c.size() / 8);
    for (size_t i = 0; i < c.size(); i++) {
        out.push_back(c[i]);
        if (c[i].op != JZ) continue;
        size_t j = i + 1;
        while (j < c.size() && (c[j].op == ADD || c[j].op == MOVE)) j++;
        if (j >= c.size() || c[j].op != JNZ || j == i + 1) continue;
        Code loop;
        loop.push_back(c[i]);
        canon_block(c, i + 1, j, loop);
        loop.push_back(c[j]);
        Code rep;
        if (!transform_loop(loop, 0, loop.size() - 1, rep)) continue;
        bool flat = true;
        for (const Instr& r : rep) if (r.op != MUL && r.op != SET && r.op != ADD) flat = false;
        if (!flat) continue;
        table.push_back(rep);
        out.push_back({LOOPX, 0, (int32_t)table.size() - 1, 0, c[i + 1].src});
    }
    c.swap(out);
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
    for (size_t i = 1; i < c.size(); i++) if (c[i].op == IDIOM || c[i].op == LOOPX) c[i].target = c[i - 1].target;   // the loop's exit
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
    add_loop_accelerators(p.literal, p.lit_loops);
    if (!fix_targets(p.literal, p.error)) return p;
    c = canonicalise(c);
    for (int iter = 0; iter < 32 && optimise_once(c); iter++) {}
    static const bool no_idioms = getenv("BF_NO_IDIOMS") != nullptr;    // for measuring
    static const bool no_rebuild = getenv("BF_NO_REBUILD") != nullptr;
    if (!no_rebuild) rebuild_blocks(c);
    if (!no_idioms) find_idioms(c, p.idioms);
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
        case IDIOM: {
            static const char* names[] = {"add", "satsub", "halve", "divmod"};
            const Idiom& d = p.idioms[(size_t)x.arg];
            std::string cells;
            for (int32_t o : d.cells) cells += (cells.empty() ? "" : ",") + std::to_string(o);
            std::string g;
            for (const Idiom::Guard& gd : d.guards) g += " [" + std::to_string(gd.off) + "]" + (gd.check ? "==" : "=") + std::to_string(gd.val);
            s += std::string("IDIOM ") + names[d.kind] + (d.kind == IDIOM_ADD ? (d.sign > 0 ? "+" : "-") : "") +
                 " [" + cells + "] scratch" + g + " -> " + std::to_string(x.target) + "\n";
            buf[0] = 0;
            break;
        }
        case LOOPX: snprintf(buf, sizeof buf, "LOOPX %d -> %d\n", x.arg, x.target); break;
        case END:  snprintf(buf, sizeof buf, "END\n"); break;
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
