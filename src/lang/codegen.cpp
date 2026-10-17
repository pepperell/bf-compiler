// codegen.cpp - BFL code generator: syntax tree -> brainfuck text.
//
// Model: the pointer position is known at every point of the output.  Every
// variable owns fixed cells; temporaries come from a LIFO pool just above the
// variables and are always returned to zero, so a freshly allocated cell is
// known to hold 0.  Loops keep the invariant "pointer on the same cell at '['
// and ']'" by construction: close() moves back to the loop cell.
//
// Expression evaluation is accumulating: eval8(e, dst, sign) adds sign*e to
// dst, which lets `x += e` and `a + b` reuse one cell.  The known-value map
// tracks cells holding compile-time constants in straight-line code so that
// constants and string output are emitted as short deltas.
//
// Procedures are inlined.  Recursion ("rec proc") uses a trampoline: the
// bodies of recursive procedures, of everything that calls them, and of the
// main program are cut into numbered blocks at every such call; a state cell
// names the next block and one dispatcher loop runs them.  The current
// activation's variables live in fixed cells; a call saves them to a stack
// array together with the state to resume, and the resume block restores them.
// See docs/TRAMPOLINE.md.
#include "lang.h"
#include "ast.h"
#include "../registers.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace bfl {

namespace {

// Procedures written in BFL itself; inlined like user procedures but kept
// out of the statement map so the debugger steps over them.
const char* PRELUDE = R"(
proc __print_byte(byte v) {
    byte h = v / 100;
    byte r = v % 100;
    byte t = r / 10;
    byte u = r % 10;
    if (h) { out h + '0'; }
    if (h || t) { out t + '0'; }
    out u + '0';
}
proc __pw_digit(ref word v, word d, ref byte started) {
    byte q = 0;
    while (v >= d) { v -= d; q++; }
    if (q || started) { out q + '0'; started = 1; }
}
proc __print_word(word v) {
    byte started = 0;
    __pw_digit(v, 10000, started);
    __pw_digit(v, 1000, started);
    __pw_digit(v, 100, started);
    __pw_digit(v, 10, started);
    out v + '0';
}
proc __read_word(ref word w) {
    w = 0;
    byte c = in;
    while (c >= '0' && c <= '9') {
        w = w * 10 + (c - '0');
        c = in;
    }
}
)";

struct Fail { Pos pos; std::string msg; };

struct Var {
    std::string name;
    int64_t cell = 0;
    bool word = false;
    int count = -1;             // array length, -1 = scalar
    bool is_const = false;
    int64_t cval = 0;
    bool owned = true;          // false: ref alias, not cleared at scope exit
};

struct TProc;
struct ProcInfo {
    const Stmt* decl;
    bool prelude;
    bool tramp = false;         // compiled as trampoline blocks
    TProc* tp = nullptr;
    std::vector<std::string> callees;
};

// ---- trampoline structures -------------------------------------------------------------
struct Action {
    enum Kind { Plain, SetState, IfGoto, Call, Ret, Pop };
    Kind kind;
    const Stmt* s = nullptr;        // Plain: statement to generate (original or synthetic)
    const Stmt* src = nullptr;      // statement for the map
    ExprP cond;                     // IfGoto
    int a = -1, b = -1;             // SetState: a; IfGoto: true a, false b; Call: resume block
    TProc* callee = nullptr;        // Call / Pop
    std::vector<ExprP> args;        // Call
    std::string result;             // Pop: hidden variable receiving the callee's result ("" none)
    std::vector<std::pair<std::string, int>> copy_out;   // Pop: (caller variable, callee param index) for ref params
    ExprP ret;                      // Ret: value or null
};
struct LBlock {
    int id;
    TProc* owner;
    int break_state = -1, cont_state = -1;
    std::vector<Action> acts;
};
struct TProc {
    std::string name;               // "" for the main program
    const Stmt* decl = nullptr;     // nullptr for main
    int entry = 0;
    int ret = 0;                    // 0 none, 1 byte, 2 word
    int64_t retval = -1;
    std::vector<Var> frame;         // params, hoisted locals, hidden temporaries (saved on call unless main)
    std::vector<Var> consts;
    int hidden_n = 0;
    bool is_main() const { return decl == nullptr; }
    int save_count() const {        // cells pushed on a call (not for main)
        if (is_main()) return 0;
        int n = 0;
        for (const Var& v : frame) n += v.word ? 2 : 1;
        return n;
    }
};

struct Gen {
    const std::string& src;
    int64_t ram;
    Result res;
    std::string& out;
    int64_t cur;
    std::vector<int64_t> loops;
    std::unordered_map<int64_t, int> known;      // cell -> constant value (RAM cells only)
    int64_t next_free, high;
    std::vector<int32_t> line_starts;

    struct Scope { std::unordered_map<std::string, Var> vars; std::vector<std::string> order; int64_t mark; bool top; bool frame = false; };
    std::vector<Scope> scopes;
    std::unordered_map<std::string, ProcInfo> procs;
    std::vector<std::string> call_stack;
    std::string cur_proc;

    // Exit contexts: what break / continue / return leave.
    struct ExitCtx {
        enum Kind { Loop, Proc, Block } kind;
        int64_t alive = -1, brk = -1;
        bool needs = false;
        int64_t retval = -1;        // Proc: result cell(s)
        int ret = 0;                // Proc: 0 none, 1 byte, 2 word
        int break_state = -1, cont_state = -1;   // Block
    };
    std::vector<ExitCtx> ctxs;
    std::vector<size_t> stmt_stack;
    int prelude_depth = 0;
    Program prelude;

    // trampoline
    std::vector<std::unique_ptr<TProc>> tprocs;
    std::vector<LBlock> blocks;
    std::vector<StmtP> synth;
    TProc* cur_tp = nullptr;        // generating a trampolined body
    int64_t state_cell = -1, alive_cell = -1, sp_cell = -1, stack_base = -1;
    int stack_size = 128;

    Gen(const std::string& s, int64_t ram_start) : src(s), ram(ram_start), out(res.bf), cur(ram_start), next_free(ram_start), high(ram_start) {
        line_starts.push_back(0);
        for (size_t i = 0; i < s.size(); i++) if (s[i] == '\n') line_starts.push_back((int32_t)i + 1);
    }

    [[noreturn]] void fail(Pos p, const std::string& m) { throw Fail{p, m}; }
    [[noreturn]] void internal(const std::string& m) { throw Fail{Pos{0, 0, 0}, "internal compiler error: " + m}; }

    int line_of(int32_t off) const {
        auto it = std::upper_bound(line_starts.begin(), line_starts.end(), off);
        return (int)(it - line_starts.begin());
    }

    // ---- registers --------------------------------------------------------------
    const bf::RegisterInfo* find_reg(const std::string& n) const {
        for (const bf::RegisterInfo& r : bf::register_map()) if (n == r.name) return &r;
        return nullptr;
    }
    static bool reg_readable(const bf::RegisterInfo& r) { return r.kind != bf::REG_TRIGGER; }

    // ---- emission primitives ------------------------------------------------------------
    void rep(char c, int64_t n) { if (n > 0) out.append((size_t)n, c); }
    void go(int64_t c) { if (c > cur) rep('>', c - cur); else rep('<', cur - c); cur = c; }
    void open(int64_t c) { go(c); out += '['; loops.push_back(c); known.clear(); }
    void close() {
        int64_t c = loops.back(); loops.pop_back();
        go(c); out += ']';
        known.clear();
        if (c >= ram) known[c] = 0;
    }
    bool known_is(int64_t c, int v) const { auto it = known.find(c); return it != known.end() && it->second == v; }
    std::optional<int> known_val(int64_t c) const { auto it = known.find(c); if (it == known.end()) return std::nullopt; return it->second; }
    void forget(int64_t c) { known.erase(c); }
    void clear(int64_t c) { if (known_is(c, 0)) return; open(c); out += '-'; close(); }
    void add(int64_t c, int64_t v) {
        v &= 255;
        if (!v) return;
        go(c);
        if (v <= 128) rep('+', v); else rep('-', 256 - v);
        if (c >= ram) { auto it = known.find(c); if (it != known.end()) it->second = (int)((it->second + v) & 255); }
    }
    void set(int64_t c, int64_t v) {
        v &= 255;
        if (auto k = known_val(c)) { add(c, v - *k); return; }
        clear(c);
        add(c, v);
    }
    void set16(int64_t lo, int64_t v) { set(lo, v & 255); set(lo + 1, (v >> 8) & 255); }

    // ---- cells ----------------------------------------------------------------------------
    int64_t alloc(int n) {
        int64_t c = next_free;
        next_free += n;
        if (next_free > high) high = next_free;
        for (int i = 0; i < n; i++) known[c + i] = 0;
        return c;
    }
    int64_t tmp() { return alloc(1); }
    int64_t tmp2() { return alloc(2); }
    void release(int64_t c) {          // temp must be the most recent allocation; leaves it zero
        if (c != next_free - 1) internal("temporary released out of order");
        clear(c);
        next_free = c;
    }
    void release2(int64_t c) {
        if (c != next_free - 2) internal("temporary pair released out of order");
        clear(c + 1); clear(c);
        next_free = c;
    }
    void drop(int64_t c, int n = 1) {  // free cells the caller knows are zero
        if (c != next_free - n) internal("temporary dropped out of order");
        next_free = c;
    }

    struct Dst { int64_t c; int sign; };
    // dsts += sign*src for each; src becomes 0.
    void move(int64_t from, std::initializer_list<Dst> dsts) {
        if (known_is(from, 0)) return;
        if (auto k = known_val(from)) {
            for (Dst d : dsts) add(d.c, d.sign * *k);
            set(from, 0);
            return;
        }
        open(from);
        add(from, -1);
        for (Dst d : dsts) add(d.c, d.sign);
        close();
    }
    // dst += sign*src, src preserved.
    void copy(int64_t from, int64_t dst, int sign = 1) {
        if (from >= ram) if (auto k = known_val(from)) { add(dst, sign * *k); return; }
        int64_t t = tmp();
        move(from, {{dst, sign}, {t, 1}});
        move(t, {{from, 1}});
        release(t);
    }
    // body runs if z != 0; z is 0 afterwards.
    void if_nonzero(int64_t z, const std::function<void()>& body) {
        if (known_is(z, 0)) return;
        open(z);
        body();
        clear(z);
        close();
    }

    // dst += 1 if the register is non-zero, without writing the register.
    // After `reg[ ... P]` the pointer is on reg (not taken) or on P (taken);
    // moving k = M-reg cells right lands on M (holding 1) or on P+k (zero),
    // and `[- >*D ]` with D = P-reg brings the first case to P+k as well.
    void test_reg(uint32_t reg, int64_t dst) {
        int64_t M = tmp(), P = tmp();
        set(M, 1);
        go((int64_t)reg);
        out += '[';
        known.clear();
        add(dst, 1);
        go(P);
        out += ']';
        int64_t k = M - (int64_t)reg, D = P - (int64_t)reg;
        rep('>', k);
        out += "[-"; rep('>', D); out += ']';
        cur = P + k;
        if (P + k < next_free) internal("test_reg landing cell is in use");
        known.clear();
        clear(M);                   // still 1 on the taken path
        release(P); release(M);
    }

    // ---- byte arrays (see docs/LANG.md for the layout) --------------------------------------
    void array_read(int64_t base, int64_t idx, int64_t dst) {     // idx consumed; dst += a[idx]
        int64_t C0 = base + 3, V0 = base + 4;
        move(idx, {{C0, 1}});
        go(C0);
        out += "[-[->>>+<<<]+>>>]" ">>[-<+<+>>]<<[->>+<<]" "<<<[->>>>[-<<<+>>>]<<<<<<<]";
        cur = base; known.clear();
        move(V0, {{dst, 1}});
    }
    void array_write(int64_t base, int64_t idx, int64_t val) {    // idx and val consumed
        int64_t C0 = base + 3, V0 = base + 4;
        move(idx, {{C0, 1}});
        move(val, {{V0, 1}});
        go(C0);
        out += "[-[->>>+<<<]+>[->>>+<<<]>>]" ">>[-]<[->+<]<" "<<<[-<<<]";
        cur = base; known.clear();
    }
    static int64_t array_cells(int count) { return 3 * ((int64_t)count + 1); }

    // ---- 8-bit routines on cells ---------------------------------------------------------------
    void iszero8(int64_t c, int64_t z) {                // z += (c == 0)
        if (c >= ram) if (auto k = known_val(c)) { add(z, *k == 0 ? 1 : 0); return; }
        int64_t cc = tmp();
        copy(c, cc);
        add(z, 1);
        if_nonzero(cc, [&] { add(z, -1); });
        release(cc);
    }
    void not8(int64_t t, int64_t dst, int sign) {       // dst += sign*(t == 0); t consumed
        add(dst, sign);
        if_nonzero(t, [&] { add(dst, -sign); });
    }
    void lt8_core(int64_t t1, int64_t t2, int64_t dst, int sign) {   // dst += sign*(t1 < t2); both consumed
        open(t1);
        {
            int64_t c = tmp(); copy(t2, c);
            int64_t e = tmp(); set(e, 1);
            if_nonzero(c, [&] { add(e, -1); add(t1, -1); add(t2, -1); });
            if_nonzero(e, [&] { clear(t1); });
            release(e); release(c);
        }
        close();
        if_nonzero(t2, [&] { add(dst, sign); });
    }
    void eq8_cells(int64_t x, int64_t y, int64_t dst, int sign) {    // dst += sign*(x == y); preserved
        int64_t d = tmp();
        copy(x, d); copy(y, d, -1);
        not8(d, dst, sign);
        release(d);
    }
    void lt8_cells(int64_t x, int64_t y, int64_t dst, int sign) {    // dst += sign*(x < y); preserved
        int64_t t1 = tmp(); copy(x, t1);
        int64_t t2 = tmp(); copy(y, t2);
        lt8_core(t1, t2, dst, sign);
        release(t2); release(t1);
    }
    // q += a / b, r += a % b.  a consumed, b cleared.
    void divmod8(int64_t a, int64_t b, int64_t q, int64_t r) {
        int64_t c = tmp();
        copy(b, c);
        open(a);
        {
            add(a, -1); add(r, 1); add(c, -1);
            int64_t z = tmp();
            iszero8(c, z);
            if_nonzero(z, [&] { add(q, 1); clear(r); copy(b, c); });
            release(z);
        }
        close();
        release(c);
        clear(b);
    }
    void double8(int64_t t) {                           // t *= 2
        int64_t u = tmp();
        move(t, {{u, 1}});
        open(u); add(u, -1); add(t, 2); close();
        release(u);
    }
    void halve_bit(int64_t t, int64_t h, int64_t bit) { // h += t/2, bit += t%2; t consumed
        open(t);
        {
            add(t, -1); add(bit, 1);
            int64_t cc = tmp();
            copy(t, cc);
            if_nonzero(cc, [&] { add(t, -1); add(bit, -1); add(h, 1); });
            release(cc);
        }
        close();
    }
    void halve8(int64_t t) {                            // t /= 2
        int64_t h = tmp(), b = tmp();
        halve_bit(t, h, b);
        clear(b); release(b);
        move(h, {{t, 1}});
        release(h);
    }
    // dst += w * (ta op tb) for a single bit pair; ba and bb consumed
    void combine_bit(char op, int64_t ba, int64_t bb, int64_t dst, int64_t w) {
        if (op == '&') { if_nonzero(ba, [&] { if_nonzero(bb, [&] { add(dst, w); }); }); clear(bb); }
        else if (op == '|') { if_nonzero(ba, [&] { add(dst, w); clear(bb); }); if_nonzero(bb, [&] { add(dst, w); }); }
        else {
            int64_t s = tmp();
            move(ba, {{s, 1}}); move(bb, {{s, 1}});
            if_nonzero(s, [&] {
                add(s, -1);
                int64_t z = tmp();
                iszero8(s, z);
                if_nonzero(z, [&] { add(dst, w); });
                release(z);
            });
            release(s);
        }
    }
    void bitwise_core(char op, int64_t ta, int64_t tb, int64_t dst, int sign) {   // dst += sign*(ta op tb); both consumed
        for (int i = 0; i < 8; i++) {
            int64_t ba = tmp(), bb = tmp(), ha = tmp(), hb = tmp();
            halve_bit(ta, ha, ba);
            halve_bit(tb, hb, bb);
            combine_bit(op, ba, bb, dst, (int64_t)sign * ((int64_t)1 << i));
            move(ha, {{ta, 1}});
            move(hb, {{tb, 1}});
            release(hb); release(ha); release(bb); release(ba);
        }
        clear(ta); clear(tb);
    }

    // ---- 16-bit routines (lo cell, hi = lo + 1) ----------------------------------------------------
    bool known0_16(int64_t lo) const { return known_is(lo, 0) && known_is(lo + 1, 0); }
    void clear16(int64_t lo) { clear(lo); clear(lo + 1); }
    void inc16(int64_t lo) {
        add(lo, 1);
        int64_t z = tmp();
        iszero8(lo, z);
        if_nonzero(z, [&] { add(lo + 1, 1); });
        release(z);
    }
    void dec16(int64_t lo) {
        int64_t z = tmp();
        iszero8(lo, z);
        if_nonzero(z, [&] { add(lo + 1, -1); });
        release(z);
        add(lo, -1);
    }
    void add16(int64_t dst, int64_t s) {                 // dst += s; s consumed
        if (known0_16(dst)) { move(s, {{dst, 1}}); move(s + 1, {{dst + 1, 1}}); return; }
        if (!known_is(s, 0)) { open(s); add(s, -1); inc16(dst); close(); }
        move(s + 1, {{dst + 1, 1}});
    }
    void sub16(int64_t dst, int64_t s) {                 // dst -= s; s consumed
        if (!known_is(s, 0)) { open(s); add(s, -1); dec16(dst); close(); }
        move(s + 1, {{dst + 1, -1}});
    }
    void add16_const(int64_t dst, int64_t v) {
        v &= 0xFFFF;
        if (known0_16(dst)) { add(dst, v & 255); add(dst + 1, v >> 8); return; }
        if (v & 255) {
            int64_t t = tmp();
            set(t, v & 255);
            open(t); add(t, -1); inc16(dst); close();
            release(t);
        }
        add(dst + 1, v >> 8);
    }
    void copy16(int64_t s, int64_t dst) {                // dst += s; s preserved
        if (known0_16(dst)) { copy(s, dst); copy(s + 1, dst + 1); return; }
        int64_t t = tmp2();
        copy(s, t); copy(s + 1, t + 1);
        add16(dst, t);
        release2(t);
    }
    void move16(int64_t s, int64_t dst) {                // dst (zero) = s; s consumed
        move(s, {{dst, 1}}); move(s + 1, {{dst + 1, 1}});
    }
    void nonzero16(int64_t w, int64_t f) {              // f += (w != 0)
        int64_t g = tmp(), cc = tmp();
        copy(w, cc);     if_nonzero(cc, [&] { set(g, 1); });
        copy(w + 1, cc); if_nonzero(cc, [&] { set(g, 1); });
        release(cc);
        move(g, {{f, 1}});
        release(g);
    }
    void iszero16(int64_t w, int64_t z) {
        int64_t g = tmp();
        nonzero16(w, g);
        not8(g, z, 1);
        release(g);
    }
    void mul16(int64_t a, int64_t b, int64_t dst) {       // dst += a*b; a, b consumed
        int64_t f = tmp();
        nonzero16(b, f);
        open(f);
        {
            dec16(b);
            copy16(a, dst);
            clear(f);
            nonzero16(b, f);
        }
        close();
        release(f);
        clear16(a); clear16(b);
    }
    void mul16_const(int64_t a, int64_t k, int64_t dst) { // dst += a*k (k < 256); a consumed
        int64_t t = tmp();
        set(t, k);
        open(t); add(t, -1); copy16(a, dst); close();
        release(t);
        clear16(a);
    }
    void divmod16(int64_t a, int64_t b, int64_t q, int64_t r) {   // q += a/b, r += a%b; a consumed, b cleared
        int64_t c = tmp2();
        copy16(b, c);
        int64_t f = tmp();
        nonzero16(a, f);
        open(f);
        {
            dec16(a); inc16(r); dec16(c);
            int64_t z = tmp();
            iszero16(c, z);
            if_nonzero(z, [&] { inc16(q); clear16(r); copy16(b, c); });
            release(z);
            clear(f);
            nonzero16(a, f);
        }
        close();
        release(f);
        release2(c);
        clear16(b);
    }
    void eq16_cells(int64_t a, int64_t b, int64_t dst, int sign) {  // dst += sign*(a == b); preserved
        int64_t t = tmp(); eq8_cells(a, b, t, 1);
        int64_t t2 = tmp(); eq8_cells(a + 1, b + 1, t2, 1);
        if_nonzero(t, [&] { if_nonzero(t2, [&] { add(dst, sign); }); });
        release(t2); release(t);
    }
    void lt16_cells(int64_t a, int64_t b, int64_t dst, int sign) {  // dst += sign*(a < b); preserved
        int64_t hl = tmp(); lt8_cells(a + 1, b + 1, hl, 1);
        int64_t he = tmp(); eq8_cells(a + 1, b + 1, he, 1);
        int64_t ll = tmp(); lt8_cells(a, b, ll, 1);
        if_nonzero(hl, [&] { add(dst, sign); });
        if_nonzero(he, [&] { if_nonzero(ll, [&] { add(dst, sign); }); });
        release(ll); release(he); release(hl);
    }
    void shl16(int64_t w) {
        int64_t t = tmp2();
        copy16(w, t);
        add16(w, t);
        release2(t);
    }
    void shr16(int64_t w) {
        int64_t hh = tmp(), hb = tmp();
        halve_bit(w + 1, hh, hb);
        int64_t lh = tmp(), lb = tmp();
        halve_bit(w, lh, lb);
        move(lh, {{w, 1}});
        if_nonzero(hb, [&] { add(w, 128); });
        move(hh, {{w + 1, 1}});
        release(lb); release(lh); release(hb); release(hh);
    }

    // ---- symbols ------------------------------------------------------------------------------
    Var* find_var(const std::string& n) {
        for (size_t i = scopes.size(); i-- > 0;) {
            auto it = scopes[i].vars.find(n);
            if (it != scopes[i].vars.end()) return &it->second;
        }
        return nullptr;
    }
    void push_scope(bool top = false) { scopes.push_back(Scope{{}, {}, next_free, top}); }
    void pop_scope() {
        Scope& s = scopes.back();
        if (!s.top && !s.frame) {
            for (size_t i = s.order.size(); i-- > 0;) {
                const Var& v = s.vars[s.order[i]];
                if (!v.owned || v.is_const) continue;
                if (v.count >= 0) { for (int k = 0; k < v.count; k++) clear(array_cell(v.cell, k)); }
                else { clear(v.cell); if (v.word) clear(v.cell + 1); }
            }
            next_free = s.mark;
        }
        scopes.pop_back();
    }
    void declare(const Var& v, Pos p) {
        Scope& s = scopes.back();
        if (s.vars.count(v.name)) fail(p, "'" + v.name + "' is already declared in this scope");
        if (find_reg(v.name)) fail(p, "'" + v.name + "' is a register name");
        s.vars[v.name] = v;
        s.order.push_back(v.name);
    }
    ProcInfo* find_proc(const std::string& n) { auto it = procs.find(n); return it == procs.end() ? nullptr : &it->second; }

    // ---- expression analysis --------------------------------------------------------------------
    int width(const Expr& e) {
        switch (e.kind) {
        case Expr::Num: return e.value > 255 ? 2 : 1;
        case Expr::Str: fail(e.pos, "a string can only appear in an out statement");
        case Expr::Name: {
            if (Var* v = find_var(e.text)) { if (v->is_const) return v->cval > 255 ? 2 : 1; return v->word ? 2 : 1; }
            return 1;
        }
        case Expr::Index: case Expr::In: return 1;
        case Expr::Input: return 2;
        case Expr::Call: {
            ProcInfo* p = find_proc(e.text);
            if (!p) fail(e.pos, "'" + e.text + "' is not a procedure");
            if (!p->decl->ret) fail(e.pos, "'" + e.text + "' returns nothing and cannot be used in an expression");
            return p->decl->ret;
        }
        case Expr::Unary:
            if (e.text == "!") return 1;
            if (e.text == "word") return 2;
            if (e.text == "byte") return 1;
            return width(*e.a);
        case Expr::Binary: {
            const std::string& o = e.text;
            if (o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=" || o == "&&" || o == "||") return 1;
            return std::max(width(*e.a), width(*e.b));
        }
        }
        return 1;
    }

    // Compile-time value of e.  With use_known, variables whose cells hold a
    // known constant at this point count too (for code generation only).
    std::optional<int64_t> fold(const Expr& e, bool use_known = false) {
        switch (e.kind) {
        case Expr::Num: return e.value;
        case Expr::Name: {
            Var* v = find_var(e.text);
            if (!v) return std::nullopt;
            if (v->is_const) return v->cval;
            if (!use_known || v->count >= 0) return std::nullopt;
            auto lo = known_val(v->cell);
            if (!v->word) return lo;
            auto hi = known_val(v->cell + 1);
            if (lo && hi) return *lo | (*hi << 8);
            return std::nullopt;
        }
        case Expr::Index: {
            if (!use_known) return std::nullopt;
            Var* v = find_var(e.text);
            if (!v || v->count < 0) return std::nullopt;
            auto i = fold(*e.a, true);
            if (!i || *i < 0 || *i >= v->count) return std::nullopt;
            return known_val(array_cell(v->cell, (int)*i));
        }
        case Expr::Unary: {
            auto a = fold(*e.a, use_known);
            if (!a) return std::nullopt;
            int64_t mask = width(e) == 2 ? 0xFFFF : 0xFF;
            if (e.text == "-") return (-*a) & mask;
            if (e.text == "!") return *a == 0 ? 1 : 0;
            if (e.text == "~") return (~*a) & mask;
            return *a & mask;      // byte()/word() casts
        }
        case Expr::Binary: {
            auto a = fold(*e.a, use_known), b = fold(*e.b, use_known);
            if (!a || !b) return std::nullopt;
            int64_t mask = width(e) == 2 ? 0xFFFF : 0xFF;
            const std::string& o = e.text;
            if (o == "+") return (*a + *b) & mask;
            if (o == "-") return (*a - *b) & mask;
            if (o == "*") return (*a * *b) & mask;
            if (o == "/") { if (!*b) return std::nullopt; return (*a / *b) & mask; }
            if (o == "%") { if (!*b) return std::nullopt; return (*a % *b) & mask; }
            if (o == "<<") return *b >= 16 ? 0 : (*a << *b) & mask;
            if (o == ">>") return *b >= 16 ? 0 : (*a >> *b) & mask;
            if (o == "&") return *a & *b;
            if (o == "|") return *a | *b;
            if (o == "^") return *a ^ *b;
            if (o == "==") return *a == *b;
            if (o == "!=") return *a != *b;
            if (o == "<") return *a < *b;
            if (o == "<=") return *a <= *b;
            if (o == ">") return *a > *b;
            if (o == ">=") return *a >= *b;
            if (o == "&&") return (*a && *b) ? 1 : 0;
            if (o == "||") return (*a || *b) ? 1 : 0;
            return std::nullopt;
        }
        default: return std::nullopt;
        }
    }

    // Does evaluating e read or write any cell in [lo, hi)?
    bool touches(const Expr& e, int64_t lo, int64_t hi) {
        auto overlap = [&](int64_t a, int64_t b) { return a < hi && lo < b; };
        switch (e.kind) {
        case Expr::Name: {
            if (Var* v = find_var(e.text)) {
                if (v->is_const) return false;
                int64_t n = v->count >= 0 ? array_cells(v->count) : (v->word ? 2 : 1);
                return overlap(v->cell, v->cell + n);
            }
            if (const bf::RegisterInfo* r = find_reg(e.text)) return overlap(r->addr, r->addr + 1);
            return false;
        }
        case Expr::Index: {
            Var* v = find_var(e.text);
            if (v && v->count >= 0 && overlap(v->cell, v->cell + array_cells(v->count))) return true;
            return touches(*e.a, lo, hi);
        }
        case Expr::Unary: return touches(*e.a, lo, hi);
        case Expr::Binary: return touches(*e.a, lo, hi) || touches(*e.b, lo, hi);
        case Expr::Call: return true;      // a procedure body may touch anything
        default: return false;
        }
    }

    const bf::RegisterInfo* bare_reg(const Expr& e) {
        if (e.kind != Expr::Name || find_var(e.text)) return nullptr;
        return find_reg(e.text);
    }

    // Flatten a chain of + and - (and unary -) into signed terms plus one constant.
    void collect_terms(const Expr& e, int sign, std::vector<std::pair<const Expr*, int>>& terms, int64_t& constant) {
        if (auto v = fold(e, true)) { constant += sign * *v; return; }
        if (e.kind == Expr::Binary && (e.text == "+" || e.text == "-")) {
            collect_terms(*e.a, sign, terms, constant);
            collect_terms(*e.b, e.text == "+" ? sign : -sign, terms, constant);
            return;
        }
        if (e.kind == Expr::Unary && e.text == "-") { collect_terms(*e.a, -sign, terms, constant); return; }
        terms.emplace_back(&e, sign);
    }

    // ---- expressions: 8-bit --------------------------------------------------------------------------
    // dst += sign * e  (mod 256)
    void eval8(const Expr& e, int64_t dst, int sign = 1) {
        if (auto v = fold(e, true)) { add(dst, sign * (*v & 255)); return; }
        switch (e.kind) {
        case Expr::Num: return;        // folded
        case Expr::Str: fail(e.pos, "a string can only appear in an out statement");
        case Expr::Name: {
            if (Var* v = find_var(e.text)) {
                if (v->count >= 0) fail(e.pos, "array '" + e.text + "' needs an index");
                copy(v->cell, dst, sign);
                return;
            }
            if (const bf::RegisterInfo* r = find_reg(e.text)) {
                if (r->kind == bf::REG_TRIGGER) fail(e.pos, std::string(r->name) + " is write-only");
                if (r->kind == bf::REG_LATCH) { copy(r->addr, dst, sign); return; }
                if (sign == 1) { test_reg(r->addr, dst); return; }
                int64_t t = tmp(); test_reg(r->addr, t); move(t, {{dst, sign}}); release(t);
                return;
            }
            fail(e.pos, "'" + e.text + "' is not declared");
        }
        case Expr::Index: {
            Var* v = find_var(e.text);
            if (!v) fail(e.pos, "'" + e.text + "' is not declared");
            if (v->count < 0) fail(e.pos, "'" + e.text + "' is not an array");
            if (auto i = fold(*e.a)) {
                if (*i < 0 || *i >= v->count) fail(e.a->pos, "index " + std::to_string(*i) + " is outside '" + e.text + "[" + std::to_string(v->count) + "]'");
                copy(array_cell(v->cell, (int)*i), dst, sign);
                return;
            }
            int64_t i = tmp();
            eval8(*e.a, i);
            if (sign == 1) array_read(v->cell, i, dst);
            else { int64_t t = tmp(); array_read(v->cell, i, t); move(t, {{dst, sign}}); release(t); }
            drop(i);
            return;
        }
        case Expr::In: {
            if (sign == 1 && known_is(dst, 0)) { go(dst); out += ','; forget(dst); return; }
            int64_t t = tmp();
            go(t); out += ','; forget(t);
            move(t, {{dst, sign}});
            release(t);
            return;
        }
        case Expr::Input: {
            int64_t t = tmp2();
            eval16_fresh(e, t);
            move(t, {{dst, sign}});
            release2(t);
            return;
        }
        case Expr::Call: {
            int w = width(e);
            if (w == 2) { int64_t t = tmp2(); eval16_fresh(e, t); move(t, {{dst, sign}}); release2(t); return; }
            std::vector<const Expr*> args; for (auto& a : e.args) args.push_back(a.get());
            if (sign == 1) { call_user(e.text, args, e.pos, dst); return; }
            int64_t t = tmp(); call_user(e.text, args, e.pos, t); move(t, {{dst, sign}}); release(t);
            return;
        }
        case Expr::Unary: {
            if (e.text == "-") { eval8(*e.a, dst, -sign); return; }
            if (e.text == "~") { eval8(*e.a, dst, -sign); add(dst, sign * 255); return; }
            if (e.text == "!") {
                int64_t t = tmp();
                eval_cond(*e.a, t);
                not8(t, dst, sign);
                release(t);
                return;
            }
            if (e.text == "word") { int64_t t = tmp2(); eval16_fresh(*e.a, t); move(t, {{dst, sign}}); release2(t); return; }
            eval8(*e.a, dst, sign);     // byte()
            return;
        }
        case Expr::Binary: break;
        }
        const std::string& o = e.text;
        const Expr& a = *e.a; const Expr& b = *e.b;
        bool cmp = o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=";
        if (width(e) == 2 && (o == "/" || o == "%" || o == ">>")) {
            int64_t t = tmp2(); eval16_fresh(e, t); move(t, {{dst, sign}}); release2(t);
            return;
        }
        if (cmp && std::max(width(a), width(b)) == 2) { cmp16(o, a, b, dst, sign); return; }
        if (o == "+" || o == "-") {
            std::vector<std::pair<const Expr*, int>> terms; int64_t constant = 0;
            collect_terms(e, sign, terms, constant);
            for (auto& t : terms) eval8(*t.first, dst, t.second);
            add(dst, constant);
            return;
        }
        if (o == "*") {
            const Expr* var = &a; std::optional<int64_t> k = fold(b, true);
            if (!k) { k = fold(a, true); if (k) var = &b; }
            if (k) {
                int64_t t = tmp(); eval8(*var, t);
                open(t); add(t, -1); add(dst, sign * (*k & 255)); close();
                release(t);
                return;
            }
            int64_t t1 = tmp(); eval8(a, t1);
            int64_t t2 = tmp(); eval8(b, t2);
            open(t1); add(t1, -1); copy(t2, dst, sign); close();
            release(t2); release(t1);
            return;
        }
        if (o == "/" || o == "%") {
            int64_t t1 = tmp(); eval8(a, t1);
            int64_t t2 = tmp(); eval8(b, t2);
            int64_t q = tmp(), r = tmp();
            divmod8(t1, t2, q, r);
            move(o == "/" ? q : r, {{dst, sign}});
            release(r); release(q); release(t2); release(t1);
            return;
        }
        if (o == "<<" || o == ">>") {
            bool left = o == "<<";
            if (auto k = fold(b, true)) {
                int64_t t = tmp(); eval8(a, t);
                if (*k >= 8) clear(t); else for (int64_t i = 0; i < *k; i++) { if (left) double8(t); else halve8(t); }
                move(t, {{dst, sign}});
                release(t);
                return;
            }
            int64_t n = tmp(); eval8(b, n);
            int64_t t = tmp(); eval8(a, t);
            open(n); add(n, -1); if (left) double8(t); else halve8(t); close();
            move(t, {{dst, sign}});
            release(t); release(n);
            return;
        }
        if (cmp) { cmp8(o, a, b, dst, sign); return; }
        if (o == "&&") {
            int64_t t = tmp(); eval_cond(a, t);
            if_nonzero(t, [&] {
                int64_t t2 = tmp(); eval_cond(b, t2);
                if_nonzero(t2, [&] { add(dst, sign); });
                release(t2);
            });
            release(t);
            return;
        }
        if (o == "||") {
            int64_t t = tmp(); eval_cond(a, t);
            int64_t f = tmp(); set(f, 1);
            if_nonzero(t, [&] { add(dst, sign); clear(f); });
            if_nonzero(f, [&] {
                int64_t t2 = tmp(); eval_cond(b, t2);
                if_nonzero(t2, [&] { add(dst, sign); });
                release(t2);
            });
            release(f); release(t);
            return;
        }
        if (o == "&" || o == "|" || o == "^") {
            int64_t ta = tmp(); eval8(a, ta);
            int64_t tb = tmp(); eval8(b, tb);
            bitwise_core(o[0], ta, tb, dst, sign);
            release(tb); release(ta);
            return;
        }
        fail(e.pos, "unsupported operator '" + o + "'");
    }

    void cmp8(const std::string& o, const Expr& a, const Expr& b, int64_t dst, int sign) {
        if (o == "==" || o == "!=") {
            int64_t t = tmp(); eval8(a, t); eval8(b, t, -1);
            if (o == "==") not8(t, dst, sign); else if_nonzero(t, [&] { add(dst, sign); });
            release(t);
            return;
        }
        bool swap = o == ">" || o == "<=";          // a > b  ==  b < a ;  a <= b  ==  !(b < a)
        bool neg = o == "<=" || o == ">=";
        int64_t t1 = tmp(); eval8(swap ? b : a, t1);
        int64_t t2 = tmp(); eval8(swap ? a : b, t2);
        if (!neg) lt8_core(t1, t2, dst, sign);
        else { int64_t r = tmp(); lt8_core(t1, t2, r, 1); not8(r, dst, sign); release(r); }
        release(t2); release(t1);
    }
    void cmp16(const std::string& o, const Expr& a, const Expr& b, int64_t dst, int sign) {
        int64_t ta = tmp2(); eval16_fresh(a, ta);
        int64_t tb = tmp2(); eval16_fresh(b, tb);
        if (o == "==") eq16_cells(ta, tb, dst, sign);
        else if (o == "!=") { int64_t t = tmp(); eq16_cells(ta, tb, t, 1); not8(t, dst, sign); release(t); }
        else {
            bool swap = o == ">" || o == "<=", neg = o == "<=" || o == ">=";
            int64_t x = swap ? tb : ta, y = swap ? ta : tb;
            if (!neg) lt16_cells(x, y, dst, sign);
            else { int64_t r = tmp(); lt16_cells(x, y, r, 1); not8(r, dst, sign); release(r); }
        }
        release2(tb); release2(ta);
    }

    // c += (e != 0) as 0/1 for word-width e; for byte-width e the value itself (non-zero = true).
    // Registers are tested without being written.
    void eval_cond(const Expr& e, int64_t c) {
        if (auto v = fold(e, true)) { add(c, *v ? 1 : 0); return; }
        if (const bf::RegisterInfo* r = bare_reg(e)) {
            if (r->kind == bf::REG_TRIGGER) fail(e.pos, std::string(r->name) + " is write-only");
            test_reg(r->addr, c);
            return;
        }
        if (width(e) == 2) {
            int64_t t = tmp2(); eval16_fresh(e, t);
            nonzero16(t, c);
            release2(t);
            return;
        }
        eval8(e, c);
    }

    // ---- expressions: 16-bit ------------------------------------------------------------------------------
    void eval16(const Expr& e, int64_t dst) {          // dst += e (mod 65536)
        if (known0_16(dst)) { eval16_fresh(e, dst); return; }
        int64_t t = tmp2(); eval16_fresh(e, t); add16(dst, t); release2(t);
    }
    // dst (known zero pair) = e
    void eval16_fresh(const Expr& e, int64_t dst) {
        if (auto v = fold(e, true)) { add(dst, *v & 255); add(dst + 1, (*v >> 8) & 255); return; }
        switch (e.kind) {
        case Expr::Num: return;
        case Expr::Str: fail(e.pos, "a string can only appear in an out statement");
        case Expr::Name: {
            if (Var* v = find_var(e.text)) {
                if (v->count >= 0) fail(e.pos, "array '" + e.text + "' needs an index");
                copy(v->cell, dst);
                if (v->word) copy(v->cell + 1, dst + 1);
                return;
            }
            eval8(e, dst);
            return;
        }
        case Expr::Index: case Expr::In: eval8(e, dst); return;
        case Expr::Input: {
            Var w; w.name = "w"; w.cell = dst; w.word = true; w.owned = false;
            call_proc("__read_word", {w}, e.pos, -1);
            return;
        }
        case Expr::Call: {
            std::vector<const Expr*> args; for (auto& a : e.args) args.push_back(a.get());
            call_user(e.text, args, e.pos, dst);      // byte results land in the low cell
            return;
        }
        case Expr::Unary: {
            if (e.text == "!") { int64_t t = tmp(); eval_cond(*e.a, t); not8(t, dst, 1); release(t); return; }
            if (e.text == "byte") { eval8(*e.a, dst); return; }
            if (e.text == "word") { eval16_fresh(*e.a, dst); return; }
            int64_t t = tmp2(); eval16_fresh(*e.a, t); sub16(dst, t); release2(t);
            if (e.text == "~") add16_const(dst, 0xFFFF);
            return;
        }
        case Expr::Binary: break;
        }
        if (width(e) == 1) { eval8(e, dst); return; }
        const std::string& o = e.text;
        const Expr& a = *e.a; const Expr& b = *e.b;
        if (o == "+" || o == "-") {
            std::vector<std::pair<const Expr*, int>> terms; int64_t constant = 0;
            collect_terms(e, 1, terms, constant);
            for (auto& t : terms) {
                if (t.second > 0) eval16(*t.first, dst);
                else { int64_t tt = tmp2(); eval16_fresh(*t.first, tt); sub16(dst, tt); release2(tt); }
            }
            add16_const(dst, constant);
            return;
        }
        if (o == "*") {
            const Expr* var = &a; std::optional<int64_t> k = fold(b, true);
            if (!k) { k = fold(a, true); if (k) var = &b; }
            if (k && *k < 256) { int64_t t = tmp2(); eval16_fresh(*var, t); mul16_const(t, *k, dst); release2(t); return; }
            int64_t ta = tmp2(); eval16_fresh(a, ta);
            int64_t tb = tmp2(); eval16_fresh(b, tb);
            mul16(ta, tb, dst);
            release2(tb); release2(ta);
            return;
        }
        if (o == "/" || o == "%") {
            int64_t ta = tmp2(); eval16_fresh(a, ta);
            int64_t tb = tmp2(); eval16_fresh(b, tb);
            int64_t q = tmp2(), r = tmp2();
            divmod16(ta, tb, q, r);
            move16(o == "/" ? q : r, dst);
            release2(r); release2(q); release2(tb); release2(ta);
            return;
        }
        if (o == "<<" || o == ">>") {
            bool left = o == "<<";
            if (auto k = fold(b, true)) {
                eval16_fresh(a, dst);
                if (*k >= 16) clear16(dst); else for (int64_t i = 0; i < *k; i++) { if (left) shl16(dst); else shr16(dst); }
                return;
            }
            int64_t n = tmp(); eval8(b, n);
            eval16_fresh(a, dst);
            open(n); add(n, -1); if (left) shl16(dst); else shr16(dst); close();
            release(n);
            return;
        }
        if (o == "&" || o == "|" || o == "^") {
            int64_t ta = tmp2(); eval16_fresh(a, ta);
            int64_t tb = tmp2(); eval16_fresh(b, tb);
            bitwise_core(o[0], ta, tb, dst, 1);
            bitwise_core(o[0], ta + 1, tb + 1, dst + 1, 1);
            release2(tb); release2(ta);
            return;
        }
        fail(e.pos, "unsupported operator '" + o + "'");
    }

    // ---- statement map ----------------------------------------------------------------------------------
    bool mapping() const { return prelude_depth == 0; }
    void begin_stmt(const Stmt& s, bool cont = false) {
        if (!mapping()) return;
        int line = s.pos.line;
        size_t a = (size_t)s.pos.off, b = a;
        while (b < src.size() && src[b] != '\n' && b < (size_t)s.end.off) b++;
        std::string text = src.substr(a, b - a);
        for (char& c : text) if (strchr("+-<>.,[]", c)) c = '_';
        while (!text.empty() && isspace((unsigned char)text.back())) text.pop_back();
        out += '\n';
        out.append(stmt_stack.size() * 2, ' ');
        out += "# L" + std::to_string(line) + (cont ? " (continued): " : ": ") + text + "\n";
        out.append(stmt_stack.size() * 2, ' ');
        StmtRange r{line, s.pos.col, line_of(std::max(s.pos.off, s.end.off - 1)), (int32_t)out.size(), 0, (int)stmt_stack.size(), cont};
        stmt_stack.push_back(res.stmts.size());
        res.stmts.push_back(r);
    }
    void end_stmt() {
        if (!mapping()) return;
        res.stmts[stmt_stack.back()].end = (int32_t)out.size();
        stmt_stack.pop_back();
    }

    // ---- control-flow analysis ----------------------------------------------------------------------------
    static bool contains_return(const Stmt& s) {
        if (s.kind == Stmt::Return) return true;
        if (s.kind == Stmt::Proc) return false;
        for (auto& x : s.body) if (contains_return(*x)) return true;
        for (auto& x : s.else_body) if (contains_return(*x)) return true;
        return false;
    }
    static bool contains_return(const std::vector<StmtP>& v) { for (auto& x : v) if (contains_return(*x)) return true; return false; }
    // May executing s leave the innermost enclosing loop / block / procedure early?
    static bool may_exit(const Stmt& s) {
        switch (s.kind) {
        case Stmt::Break: case Stmt::Continue: case Stmt::Return: return true;
        case Stmt::If: for (auto& x : s.body) if (may_exit(*x)) return true; for (auto& x : s.else_body) if (may_exit(*x)) return true; return false;
        case Stmt::Block: for (auto& x : s.body) if (may_exit(*x)) return true; return false;
        case Stmt::While: case Stmt::For: return contains_return(s.body);
        default: return false;
        }
    }
    static bool has_exit(const std::vector<StmtP>& v) { for (auto& x : v) if (may_exit(*x)) return true; return false; }

    // ---- statements -------------------------------------------------------------------------------------------
    void gen_stmts(const std::vector<StmtP>& v, size_t from = 0) {
        for (size_t i = from; i < v.size(); i++) {
            gen_stmt(*v[i]);
            if (i + 1 < v.size() && !ctxs.empty() && ctxs.back().needs && may_exit(*v[i])) {
                int64_t t = tmp();
                copy(ctxs.back().alive, t);
                if_nonzero(t, [&] { gen_stmts(v, i + 1); });
                release(t);
                return;
            }
        }
    }
    void gen_scope(const std::vector<StmtP>& v) { push_scope(); gen_stmts(v); pop_scope(); }

    struct LValue { enum Kind { Byte, Word, Elem, ElemRT, Reg } kind; int64_t cell = 0; Var* arr = nullptr; const bf::RegisterInfo* reg = nullptr; };
    LValue lvalue(const Expr& t) {
        LValue lv{};
        if (t.kind == Expr::Index) {
            Var* v = find_var(t.text);
            if (!v) fail(t.pos, "'" + t.text + "' is not declared");
            if (v->count < 0) fail(t.pos, "'" + t.text + "' is not an array");
            lv.arr = v;
            if (auto i = fold(*t.a)) {
                if (*i < 0 || *i >= v->count) fail(t.a->pos, "index " + std::to_string(*i) + " is outside '" + t.text + "[" + std::to_string(v->count) + "]'");
                lv.kind = LValue::Elem; lv.cell = array_cell(v->cell, (int)*i);
            } else lv.kind = LValue::ElemRT;
            return lv;
        }
        if (Var* v = find_var(t.text)) {
            if (v->is_const) fail(t.pos, "'" + t.text + "' is a constant");
            if (v->count >= 0) fail(t.pos, "array '" + t.text + "' needs an index");
            lv.kind = v->word ? LValue::Word : LValue::Byte; lv.cell = v->cell;
            return lv;
        }
        if (const bf::RegisterInfo* r = find_reg(t.text)) { lv.kind = LValue::Reg; lv.cell = r->addr; lv.reg = r; return lv; }
        fail(t.pos, "'" + t.text + "' is not declared");
    }
    bool lv_touched(const LValue& lv, const Expr& e) {
        switch (lv.kind) {
        case LValue::Byte: case LValue::Elem: case LValue::Reg: return touches(e, lv.cell, lv.cell + 1);
        case LValue::Word: return touches(e, lv.cell, lv.cell + 2);
        case LValue::ElemRT: return touches(e, lv.arr->cell, lv.arr->cell + array_cells(lv.arr->count));
        }
        return true;
    }

    ExprP clone(const Expr& e) {
        auto c = std::make_unique<Expr>();
        c->kind = e.kind; c->pos = e.pos; c->value = e.value; c->text = e.text;
        if (e.a) c->a = clone(*e.a);
        if (e.b) c->b = clone(*e.b);
        for (auto& x : e.args) c->args.push_back(clone(*x));
        return c;
    }

    void gen_assign(const Stmt& s) {
        LValue lv = lvalue(*s.target);
        const Expr& e = *s.e;
        std::string op = s.op;
        if ((op != "=" && op != "+=" && op != "-=") || (lv.kind == LValue::ElemRT && op != "=")) {
            // x op= e  ->  x = x op e
            auto bin = std::make_unique<Expr>();
            bin->kind = Expr::Binary; bin->pos = e.pos; bin->text = op.substr(0, op.size() - 1);
            bin->a = clone(*s.target); bin->b = clone(e);
            Stmt t; t.kind = Stmt::Assign; t.op = "="; t.pos = s.pos; t.end = s.end; t.target = clone(*s.target); t.e = std::move(bin);
            gen_assign(t);
            return;
        }
        auto k = fold(e);
        switch (lv.kind) {
        case LValue::Byte: case LValue::Elem: {
            if (op == "=") {
                if (e.kind == Expr::In) { go(lv.cell); out += ','; forget(lv.cell); return; }   // ',' straight into the cell: unchanged at end of input
                if (k && *k > 255) fail(e.pos, "value " + std::to_string(*k) + " does not fit a byte");
                if (!k) k = fold(e, true);
                if (k) { set(lv.cell, *k); return; }
                if (!lv_touched(lv, e)) { clear(lv.cell); eval8(e, lv.cell); return; }
                int64_t t = tmp(); eval8(e, t); clear(lv.cell); move(t, {{lv.cell, 1}}); release(t);
                return;
            }
            int sign = op == "+=" ? 1 : -1;
            if (k || fold(e, true) || !lv_touched(lv, e)) { eval8(e, lv.cell, sign); return; }
            int64_t t = tmp(); eval8(e, t); move(t, {{lv.cell, sign}}); release(t);
            return;
        }
        case LValue::Word: {
            if (op == "=") {
                if (k && *k > 65535) fail(e.pos, "value " + std::to_string(*k) + " does not fit a word");
                if (!k) k = fold(e, true);
                if (k) { set16(lv.cell, *k); return; }
                if (!lv_touched(lv, e)) { clear16(lv.cell); eval16_fresh(e, lv.cell); return; }
                int64_t t = tmp2(); eval16_fresh(e, t); clear16(lv.cell); move16(t, lv.cell); release2(t);
                return;
            }
            if (!k) k = fold(e, true);
            if (k) { if (op == "+=") add16_const(lv.cell, *k); else add16_const(lv.cell, -*k); return; }
            int64_t t = tmp2(); eval16_fresh(e, t);
            if (op == "+=") add16(lv.cell, t); else sub16(lv.cell, t);
            release2(t);
            return;
        }
        case LValue::ElemRT: {
            int64_t t = tmp(); eval8(e, t);
            int64_t i = tmp(); eval8(*s.target->a, i);
            array_write(lv.arr->cell, i, t);
            drop(i); drop(t);
            return;
        }
        case LValue::Reg: {
            const bf::RegisterInfo& r = *lv.reg;
            if (r.kind != bf::REG_LATCH) {
                if (op == "=") fail(s.pos, std::string(r.name) + " cannot be assigned: write it with 'pulse " + r.name + "' or '" + r.name + " += 1'");
                if (!k) fail(e.pos, std::string(r.name) + " only accepts a constant with += or -=");
                add(lv.cell, op == "+=" ? *k : -*k);
                return;
            }
            if (op == "=") {
                if (k) { if (*k > 255) fail(e.pos, "value " + std::to_string(*k) + " does not fit a byte"); set(lv.cell, *k); return; }
                int64_t t = tmp(); eval8(e, t); clear(lv.cell); move(t, {{lv.cell, 1}}); release(t);
                return;
            }
            int sign = op == "+=" ? 1 : -1;
            if (k) { add(lv.cell, sign * *k); return; }
            int64_t t = tmp(); eval8(e, t); move(t, {{lv.cell, sign}}); release(t);
            return;
        }
        }
    }

    void gen_incdec(const Stmt& s) {
        LValue lv = lvalue(*s.target);
        int d = s.op == "++" ? 1 : -1;
        switch (lv.kind) {
        case LValue::Byte: case LValue::Elem: case LValue::Reg: add(lv.cell, d); return;
        case LValue::Word: if (d > 0) inc16(lv.cell); else dec16(lv.cell); return;
        case LValue::ElemRT: {
            Stmt t; t.kind = Stmt::Assign; t.op = d > 0 ? "+=" : "-="; t.pos = s.pos; t.end = s.end; t.target = clone(*s.target);
            t.e = std::make_unique<Expr>(); t.e->kind = Expr::Num; t.e->value = 1; t.e->pos = s.pos;
            gen_assign(t);
            return;
        }
        }
    }

    void gen_decl(const Stmt& s) {
        Var v; v.name = s.name; v.word = s.word; v.count = s.count;
        if (s.count >= 0) {
            if (s.word) fail(s.pos, "word arrays are not supported; use two byte arrays");
            v.cell = alloc((int)array_cells(s.count));
        } else {
            v.cell = alloc(s.word ? 2 : 1);
            if (s.e) {
                if (auto k = fold(*s.e)) {
                    int64_t max = s.word ? 65535 : 255;
                    if (*k > max) fail(s.e->pos, "value " + std::to_string(*k) + " does not fit a " + (s.word ? "word" : "byte"));
                    if (s.word) { add(v.cell, *k & 255); add(v.cell + 1, *k >> 8); } else add(v.cell, *k);
                } else if (s.word) eval16_fresh(*s.e, v.cell);
                else eval8(*s.e, v.cell);
            }
        }
        declare(v, s.pos);
        Symbol sym{v.name, v.cell, (int)(s.count >= 0 ? array_cells(s.count) : (s.word ? 2 : 1)),
                   s.count >= 0 ? 'a' : (s.word ? 'w' : 'b'), s.count >= 0 ? s.count : 1, cur_proc};
        if (mapping()) res.symbols.push_back(sym);
    }

    // Clear the alive flags from the innermost context down to (and including) index stop_at;
    // loops strictly inside get their break flag so they exit.
    void unwind_to(size_t stop_at) {
        for (size_t i = ctxs.size(); i-- > stop_at;) {
            ExitCtx& c = ctxs[i];
            if (c.needs && c.alive >= 0) clear(c.alive);
            if (c.kind == ExitCtx::Loop && c.needs && i != stop_at) set(c.brk, 1);
        }
    }

    void gen_while(const Stmt& s, const Expr* cond, const std::vector<StmtP>& body, const Stmt* step) {
        bool needs = has_exit(body);
        ExitCtx lc; lc.kind = ExitCtx::Loop; lc.needs = needs;
        if (needs) { lc.alive = tmp(); lc.brk = tmp(); }
        std::optional<int64_t> folded = cond ? fold(*cond) : std::optional<int64_t>(1);
        if (folded && *folded == 0) { if (needs) { release(lc.brk); release(lc.alive); } return; }
        const bf::RegisterInfo* r = cond ? bare_reg(*cond) : nullptr;
        if (r && !reg_readable(*r)) fail(cond->pos, std::string(r->name) + " is write-only");
        if (r && !needs) {
            // loop directly on the register: tests only, never writes it
            open(r->addr);
            ctxs.push_back(lc);
            gen_scope(body);
            ctxs.pop_back();
            if (step) gen_stmt(*step);
            close();
            return;
        }
        int64_t c = tmp();
        if (folded) set(c, 1); else eval_cond(*cond, c);
        open(c);
        {
            if (needs) set(lc.alive, 1);
            ctxs.push_back(lc);
            gen_scope(body);
            ctxs.pop_back();
            if (step) gen_stmt(*step);
            if (!folded) { clear(c); eval_cond(*cond, c); }
            if (needs) { int64_t t = tmp(); copy(lc.brk, t); if_nonzero(t, [&] { clear(c); }); release(t); }
        }
        close();
        release(c);
        if (needs) { release(lc.brk); release(lc.alive); }
        (void)s;
    }

    void gen_if(const Stmt& s) {
        if (auto k = fold(*s.e)) { if (*k) gen_scope(s.body); else gen_scope(s.else_body); return; }
        int64_t c = tmp();
        eval_cond(*s.e, c);
        if (s.else_body.empty()) { if_nonzero(c, [&] { gen_scope(s.body); }); release(c); return; }
        int64_t f = tmp(); set(f, 1);
        if_nonzero(c, [&] { gen_scope(s.body); clear(f); });
        if_nonzero(f, [&] { gen_scope(s.else_body); });
        release(f); release(c);
    }

    void gen_out(const Stmt& s) {
        for (const ExprP& ap : s.args) {
            const Expr& a = *ap;
            if (a.kind == Expr::Str) {
                int64_t t = tmp();
                for (unsigned char ch : a.text) { set(t, ch); go(t); out += '.'; }
                release(t);
                continue;
            }
            if (a.kind == Expr::Name) {
                if (Var* v = find_var(a.text)) {
                    if (v->count < 0 && !v->is_const) { go(v->cell); out += '.'; continue; }
                } else if (const bf::RegisterInfo* r = find_reg(a.text)) {
                    if (!reg_readable(*r)) fail(a.pos, std::string(r->name) + " is write-only");
                    go(r->addr); out += '.';
                    continue;
                }
            }
            if (a.kind == Expr::Index) {
                LValue lv = lvalue(a);
                if (lv.kind == LValue::Elem) { go(lv.cell); out += '.'; continue; }
            }
            int64_t t = tmp();
            eval8(a, t);
            go(t); out += '.';
            release(t);
        }
    }

    void gen_print(const Stmt& s) {
        for (const ExprP& ap : s.args) {
            if (ap->kind == Expr::Str) { Stmt o; o.kind = Stmt::Out; o.args.push_back(clone(*ap)); gen_out(o); continue; }
            call_user(width(*ap) == 2 ? "__print_word" : "__print_byte", {ap.get()}, s.pos, -1);
        }
    }

    void gen_pulse(const std::string& name, Pos p) {
        if (find_var(name)) fail(p, "'" + name + "' is a variable, not a register");
        const bf::RegisterInfo* r = find_reg(name);
        if (!r) fail(p, "'" + name + "' is not a register");
        go(r->addr); out += '+';
    }

    // return [e];  inside an inlined procedure, the main program, or a trampolined body.
    void gen_return(const Stmt& s) {
        if (cur_tp) { gen_tramp_return(s.e.get(), s.pos); return; }
        size_t p = ctxs.size();
        while (p-- > 0) if (ctxs[p].kind == ExitCtx::Proc) break;
        if (p == (size_t)-1) fail(s.pos, "'return' outside a procedure");
        ExitCtx& pc = ctxs[p];
        if (pc.ret && !s.e) fail(s.pos, "this procedure returns a value");
        if (!pc.ret && s.e) fail(s.pos, "this procedure returns nothing");
        if (s.e) {
            if (pc.ret == 2) { clear16(pc.retval); eval16_fresh(*s.e, pc.retval); }
            else { clear(pc.retval); eval8(*s.e, pc.retval); }
        }
        unwind_to(p);
    }

    // Inline a procedure with its parameters already bound to cells.  result_cell
    // receives the return value (-1 discards it).
    void call_proc(const std::string& name, const std::vector<Var>& bound, Pos p, int64_t result_cell) {
        auto it = procs.find(name);
        if (it == procs.end()) fail(p, "'" + name + "' is not a procedure");
        const Stmt& proc = *it->second.decl;
        if (it->second.tramp) fail(p, "'" + name + "' is recursive and cannot be inlined here");
        if (std::find(call_stack.begin(), call_stack.end(), name) != call_stack.end()) fail(p, "'" + name + "' calls itself; declare it 'rec proc'");
        if (bound.size() != proc.params.size()) fail(p, "'" + name + "' takes " + std::to_string(proc.params.size()) + " argument(s)");
        if (it->second.prelude) prelude_depth++;
        std::string saved_proc = cur_proc; cur_proc = name;
        std::vector<ExitCtx> saved_ctxs; saved_ctxs.swap(ctxs);
        TProc* saved_tp = cur_tp; cur_tp = nullptr;
        call_stack.push_back(name);
        ExitCtx pc; pc.kind = ExitCtx::Proc; pc.ret = proc.ret; pc.needs = contains_return(proc.body);
        int64_t mark = next_free;
        if (proc.ret) pc.retval = proc.ret == 2 ? tmp2() : tmp();
        if (pc.needs) { pc.alive = tmp(); set(pc.alive, 1); }
        ctxs.push_back(pc);
        push_scope();
        for (size_t i = 0; i < bound.size(); i++) { Var v = bound[i]; v.name = proc.params[i].name; declare(v, proc.params[i].pos); }
        gen_stmts(proc.body);
        pop_scope();
        ctxs.pop_back();
        if (pc.needs) release(pc.alive);
        if (proc.ret && result_cell >= 0) {
            move(pc.retval, {{result_cell, 1}});
            if (proc.ret == 2) move(pc.retval + 1, {{result_cell + 1, 1}});
        }
        if (proc.ret) { if (proc.ret == 2) release2(pc.retval); else release(pc.retval); }
        if (next_free != mark) internal("call frame not released");
        call_stack.pop_back();
        cur_tp = saved_tp;
        ctxs.swap(saved_ctxs);
        cur_proc = saved_proc;
        if (it->second.prelude) prelude_depth--;
    }

    // Evaluate arguments and inline.  result_cell: where the value goes (added), -1 to discard.
    void call_user(const std::string& name, const std::vector<const Expr*>& args, Pos p, int64_t result_cell) {
        auto it = procs.find(name);
        if (it == procs.end()) fail(p, "'" + name + "' is not a procedure");
        const Stmt& proc = *it->second.decl;
        if (it->second.tramp) fail(p, "'" + name + "' is recursive; it can only be called from the main program or from procedures compiled with it");
        if (args.size() != proc.params.size()) fail(p, "'" + name + "' takes " + std::to_string(proc.params.size()) + " argument(s), " + std::to_string(args.size()) + " given");
        int64_t mark = next_free;
        std::vector<Var> bound;
        for (size_t i = 0; i < args.size(); i++) {
            const Param& prm = proc.params[i];
            const Expr& a = *args[i];
            Var v; v.word = prm.word; v.count = -1;
            if (prm.ref) {
                if (a.kind != Expr::Name) fail(a.pos, "argument for 'ref " + prm.name + "' must be a variable");
                Var* sv = find_var(a.text);
                if (!sv || sv->is_const) fail(a.pos, "argument for 'ref " + prm.name + "' must be a variable");
                if (prm.array) { if (sv->count < 0) fail(a.pos, "'" + prm.name + "' needs a byte array"); }
                else {
                    if (sv->count >= 0) fail(a.pos, "'" + prm.name + "' needs a " + (prm.word ? "word" : "byte") + ", not an array");
                    if (sv->word != prm.word) fail(a.pos, "'" + prm.name + "' needs a " + (prm.word ? "word" : "byte") + " variable");
                }
                v.cell = sv->cell; v.count = sv->count; v.word = sv->word; v.owned = false;
            } else {
                v.cell = alloc(prm.word ? 2 : 1);
                if (prm.word) eval16_fresh(a, v.cell); else eval8(a, v.cell);
            }
            bound.push_back(v);
        }
        call_proc(name, bound, p, proc.ret ? result_cell : -1);
        next_free = mark;        // value parameters were cleared by pop_scope
    }

    void gen_stmt(const Stmt& s) {
        switch (s.kind) {
        case Stmt::Proc:
            if (scopes.size() > 1 || !call_stack.empty()) fail(s.pos, "procedures must be declared at the top level");
            return;                                  // registered up front
        case Stmt::Stack:
            if (scopes.size() > 1 || !call_stack.empty()) fail(s.pos, "'stack' must appear at the top level");
            return;                                  // read up front
        case Stmt::Const: {
            auto k = fold(*s.e);
            if (!k) fail(s.e->pos, "constant '" + s.name + "' needs a constant expression");
            Var v; v.name = s.name; v.is_const = true; v.cval = *k; v.owned = false;
            declare(v, s.pos);
            return;
        }
        default: break;
        }
        begin_stmt(s);
        switch (s.kind) {
        case Stmt::Decl:   gen_decl(s); break;
        case Stmt::Assign: gen_assign(s); break;
        case Stmt::IncDec: gen_incdec(s); break;
        case Stmt::If:     gen_if(s); break;
        case Stmt::While:  gen_while(s, s.e.get(), s.body, nullptr); break;
        case Stmt::For: {
            push_scope();
            if (s.init) gen_stmt(*s.init);
            gen_while(s, s.e.get(), s.body, s.step.get());
            pop_scope();
            break;
        }
        case Stmt::Break: case Stmt::Continue: {
            size_t i = ctxs.size();
            while (i-- > 0) if (ctxs[i].kind != ExitCtx::Proc) break;
            if (i == (size_t)-1) fail(s.pos, std::string(s.kind == Stmt::Break ? "'break'" : "'continue'") + " outside a loop");
            ExitCtx& lc = ctxs[i];
            if (lc.kind == ExitCtx::Block) {
                int target = s.kind == Stmt::Break ? lc.break_state : lc.cont_state;
                if (target < 0) fail(s.pos, std::string(s.kind == Stmt::Break ? "'break'" : "'continue'") + " outside a loop");
                set(state_cell, target);
                unwind_to(i);
                break;
            }
            if (!lc.needs) internal("loop without break support");
            unwind_to(i);
            if (s.kind == Stmt::Break) set(lc.brk, 1);
            break;
        }
        case Stmt::Return: gen_return(s); break;
        case Stmt::Out:    gen_out(s); break;
        case Stmt::Print:  gen_print(s); break;
        case Stmt::Pulse:  gen_pulse(s.name, s.pos); break;
        case Stmt::Halt:   gen_pulse("HALT", s.pos); break;
        case Stmt::Asm:    out += s.text; known.clear(); break;
        case Stmt::Call: {
            std::vector<const Expr*> args;
            for (auto& a : s.args) args.push_back(a.get());
            call_user(s.name, args, s.pos, -1);
            break;
        }
        case Stmt::Block:  gen_scope(s.body); break;
        default: break;
        }
        end_stmt();
    }

    // ---- procedures: registration and call graph --------------------------------------------------------
    void collect_calls(const Expr& e, std::vector<std::string>& v) {
        if (e.kind == Expr::Call) v.push_back(e.text);
        if (e.a) collect_calls(*e.a, v);
        if (e.b) collect_calls(*e.b, v);
        for (auto& x : e.args) collect_calls(*x, v);
    }
    void collect_calls(const Stmt& s, std::vector<std::string>& v) {
        if (s.kind == Stmt::Call) v.push_back(s.name);
        if (s.target) collect_calls(*s.target, v);
        if (s.e) collect_calls(*s.e, v);
        if (s.init) collect_calls(*s.init, v);
        if (s.step) collect_calls(*s.step, v);
        for (auto& x : s.args) collect_calls(*x, v);
        for (auto& x : s.body) collect_calls(*x, v);
        for (auto& x : s.else_body) collect_calls(*x, v);
    }
    void register_procs(const Program& p, bool is_prelude) {
        for (const StmtP& s : p.items) {
            if (s->kind != Stmt::Proc) continue;
            if (procs.count(s->name)) fail(s->pos, "procedure '" + s->name + "' is already defined");
            for (size_t i = 0; i < s->params.size(); i++)
                for (size_t j = 0; j < i; j++) if (s->params[i].name == s->params[j].name) fail(s->params[i].pos, "duplicate parameter '" + s->params[i].name + "'");
            ProcInfo pi{s.get(), is_prelude};
            for (auto& x : s->body) collect_calls(*x, pi.callees);
            procs[s->name] = pi;
        }
    }
    // Decide which procedures are trampolined: those declared rec, and everything that calls one.
    // A cycle without 'rec' is an error.  Returns true if the main program calls a trampolined procedure.
    bool plan_trampolines(const Program& prog) {
        for (auto& kv : procs) {
            for (const std::string& c : kv.second.callees)
                if (!procs.count(c)) fail(kv.second.decl->pos, "'" + c + "' is not a procedure");
            if (kv.second.decl->rec) kv.second.tramp = true;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& kv : procs) {
                if (kv.second.tramp) continue;
                for (const std::string& c : kv.second.callees) if (procs[c].tramp) { kv.second.tramp = true; changed = true; break; }
            }
        }
        // cycle check among the non-rec procedures (trampolined ones may recurse freely)
        for (auto& kv : procs) {
            if (kv.second.tramp) continue;
            std::vector<std::string> stack{kv.first};
            std::unordered_set<std::string> seen;
            while (!stack.empty()) {
                std::string n = stack.back(); stack.pop_back();
                for (const std::string& c : procs[n].callees) {
                    if (c == kv.first) fail(kv.second.decl->pos, "'" + kv.first + "' is recursive; declare it 'rec proc'");
                    if (seen.insert(c).second) stack.push_back(c);
                }
            }
        }
        std::vector<std::string> main_calls;
        for (auto& x : prog.items) if (x->kind != Stmt::Proc) collect_calls(*x, main_calls);
        for (const std::string& c : main_calls) { auto it = procs.find(c); if (it != procs.end() && it->second.tramp) return true; }
        return false;
    }

    // ---- trampoline: lowering ------------------------------------------------------------------------------
    struct LowCtx { TProc* p; int block; int break_state = -1, cont_state = -1; };

    bool is_tramp_call(const std::string& n) { ProcInfo* p = find_proc(n); return p && p->tramp; }
    bool calls_tramp(const Expr& e) {
        if (e.kind == Expr::Call && is_tramp_call(e.text)) return true;
        if (e.a && calls_tramp(*e.a)) return true;
        if (e.b && calls_tramp(*e.b)) return true;
        for (auto& x : e.args) if (calls_tramp(*x)) return true;
        return false;
    }
    bool calls_tramp(const Stmt& s) {
        if (s.kind == Stmt::Call && is_tramp_call(s.name)) return true;
        if (s.target && calls_tramp(*s.target)) return true;
        if (s.e && calls_tramp(*s.e)) return true;
        if (s.init && calls_tramp(*s.init)) return true;
        if (s.step && calls_tramp(*s.step)) return true;
        for (auto& x : s.args) if (calls_tramp(*x)) return true;
        for (auto& x : s.body) if (calls_tramp(*x)) return true;
        for (auto& x : s.else_body) if (calls_tramp(*x)) return true;
        return false;
    }

    int new_block(TProc* p, const LowCtx& L) {
        LBlock b; b.id = (int)blocks.size() + 1; b.owner = p; b.break_state = L.break_state; b.cont_state = L.cont_state;
        blocks.push_back(std::move(b));
        return blocks.back().id;
    }
    LBlock& B(int id) { return blocks[(size_t)id - 1]; }
    Action& act(LowCtx& L, Action::Kind k, const Stmt* from) { Action a; a.kind = k; a.src = from; B(L.block).acts.push_back(std::move(a)); return B(L.block).acts.back(); }

    void frame_add(TProc* p, Var v, Pos pos) {
        for (const Var& f : p->frame) if (f.name == v.name) fail(pos, "'" + v.name + "' is declared twice in " + (p->is_main() ? "the main program" : "'" + p->name + "'") + "; variables of code that calls a rec proc need unique names");
        for (const Var& f : p->consts) if (f.name == v.name) fail(pos, "'" + v.name + "' is already a constant");
        p->frame.push_back(v);
    }
    std::string hidden(TProc* p, bool word) {
        Var v; v.name = "__h" + std::to_string(p->hidden_n++); v.word = word;
        p->frame.push_back(v);
        return v.name;
    }

    // Clone e, replacing calls to trampolined procedures by hidden variables that
    // receive their results (each call becomes a Call action and a new resume block).
    ExprP hoist(const Expr& e, LowCtx& L, const Stmt& from) {
        if (e.kind == Expr::Call && is_tramp_call(e.text)) {
            ProcInfo* pi = find_proc(e.text);
            if (!pi->decl->ret) fail(e.pos, "'" + e.text + "' returns nothing and cannot be used in an expression");
            std::vector<ExprP> args;
            for (auto& a : e.args) args.push_back(hoist(*a, L, from));
            std::string h = hidden(L.p, pi->decl->ret == 2);
            emit_call(L, pi->tp, std::move(args), e.pos, from, h);
            auto n = std::make_unique<Expr>(); n->kind = Expr::Name; n->pos = e.pos; n->text = h;
            return n;
        }
        auto c = std::make_unique<Expr>();
        c->kind = e.kind; c->pos = e.pos; c->value = e.value; c->text = e.text;
        if (e.a) c->a = hoist(*e.a, L, from);
        if (e.b) c->b = hoist(*e.b, L, from);
        for (auto& x : e.args) c->args.push_back(hoist(*x, L, from));
        return c;
    }

    // A call to a trampolined procedure: ends the current block, resumes in a new one.
    void emit_call(LowCtx& L, TProc* callee, std::vector<ExprP> args, Pos pos, const Stmt& from, const std::string& result) {
        const Stmt& decl = *callee->decl;
        if (args.size() != decl.params.size()) fail(pos, "'" + callee->name + "' takes " + std::to_string(decl.params.size()) + " argument(s), " + std::to_string(args.size()) + " given");
        std::vector<std::pair<std::string, int>> copy_out;
        for (size_t i = 0; i < args.size(); i++) {
            if (!decl.params[i].ref) continue;
            if (args[i]->kind != Expr::Name) fail(args[i]->pos, "argument for 'ref " + decl.params[i].name + "' must be a variable");
            copy_out.emplace_back(args[i]->text, (int)i);
        }
        int resume = new_block(L.p, L);
        Action& a = act(L, Action::Call, &from);
        a.callee = callee; a.args = std::move(args); a.a = resume;
        L.block = resume;
        Action& p = act(L, Action::Pop, &from);
        p.callee = callee; p.result = result; p.copy_out = copy_out;
    }

    void set_state(LowCtx& L, int target, const Stmt* from) { Action& a = act(L, Action::SetState, from); a.a = target; }

    StmtP mk_assign(const Stmt& from, const std::string& name, ExprP value) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Assign; s->op = "="; s->pos = from.pos; s->end = from.end;
        s->target = std::make_unique<Expr>(); s->target->kind = Expr::Name; s->target->pos = from.pos; s->target->text = name;
        s->e = std::move(value);
        return s;
    }
    ExprP mk_num(Pos p, int64_t v) { auto e = std::make_unique<Expr>(); e->kind = Expr::Num; e->pos = p; e->value = (int32_t)v; return e; }

    void plain(LowCtx& L, const Stmt& s) { Action& a = act(L, Action::Plain, &s); a.s = &s; }
    void synth_plain(LowCtx& L, StmtP s, const Stmt& from) { Action& a = act(L, Action::Plain, &from); a.s = s.get(); synth.push_back(std::move(s)); }

    void lower_stmts(const std::vector<StmtP>& v, LowCtx& L) { for (auto& s : v) lower_stmt(*s, L); }

    void lower_stmt(const Stmt& s, LowCtx& L) {
        TProc* p = L.p;
        switch (s.kind) {
        case Stmt::Proc: case Stmt::Stack: return;
        case Stmt::Const: {
            Var v; v.name = s.name; v.is_const = true; v.owned = false;
            auto k = fold_const_only(*s.e, p);
            if (!k) fail(s.e->pos, "constant '" + s.name + "' needs a constant expression");
            v.cval = *k;
            for (const Var& f : p->consts) if (f.name == v.name) fail(s.pos, "'" + v.name + "' is already declared");
            p->consts.push_back(v);
            return;
        }
        case Stmt::Decl: {
            Var v; v.name = s.name; v.word = s.word; v.count = s.count;
            if (s.count >= 0 && !p->is_main()) fail(s.pos, "arrays cannot be local to a rec proc; declare '" + s.name + "' at the top level");
            if (s.count >= 0 && s.word) fail(s.pos, "word arrays are not supported; use two byte arrays");
            frame_add(p, v, s.pos);
            if (s.count >= 0) return;                      // global array: zero from the start
            ExprP init = s.e ? hoist(*s.e, L, s) : mk_num(s.pos, 0);
            synth_plain(L, mk_assign(s, s.name, std::move(init)), s);
            return;
        }
        default: break;
        }
        if (!calls_tramp(s)) { plain(L, s); return; }
        switch (s.kind) {
        case Stmt::Assign: {
            auto t = std::make_unique<Stmt>();
            t->kind = Stmt::Assign; t->op = s.op; t->pos = s.pos; t->end = s.end;
            t->target = hoist(*s.target, L, s);
            t->e = hoist(*s.e, L, s);
            synth_plain(L, std::move(t), s);
            return;
        }
        case Stmt::IncDec: {
            auto t = std::make_unique<Stmt>();
            t->kind = Stmt::IncDec; t->op = s.op; t->pos = s.pos; t->end = s.end; t->target = hoist(*s.target, L, s);
            synth_plain(L, std::move(t), s);
            return;
        }
        case Stmt::Out: case Stmt::Print: {
            auto t = std::make_unique<Stmt>();
            t->kind = s.kind; t->pos = s.pos; t->end = s.end;
            for (auto& a : s.args) t->args.push_back(hoist(*a, L, s));
            synth_plain(L, std::move(t), s);
            return;
        }
        case Stmt::Call: {
            ProcInfo* pi = find_proc(s.name);
            if (!pi) fail(s.pos, "'" + s.name + "' is not a procedure");
            std::vector<ExprP> args;
            for (auto& a : s.args) args.push_back(hoist(*a, L, s));
            if (pi->tramp) emit_call(L, pi->tp, std::move(args), s.pos, s, "");
            else {
                auto t = std::make_unique<Stmt>();
                t->kind = Stmt::Call; t->name = s.name; t->pos = s.pos; t->end = s.end; t->args = std::move(args);
                synth_plain(L, std::move(t), s);
            }
            return;
        }
        case Stmt::Return: {
            ExprP v = s.e ? hoist(*s.e, L, s) : nullptr;
            Action& a = act(L, Action::Ret, &s);
            a.ret = std::move(v);
            return;
        }
        case Stmt::Block: lower_stmts(s.body, L); return;
        case Stmt::If: {
            ExprP cond = hoist(*s.e, L, s);
            int then_b = new_block(p, L), join = new_block(p, L);
            int else_b = s.else_body.empty() ? join : new_block(p, L);
            { Action& a = act(L, Action::IfGoto, &s); a.cond = std::move(cond); a.a = then_b; a.b = else_b; }
            L.block = then_b; lower_stmts(s.body, L); set_state(L, join, &s);
            if (!s.else_body.empty()) { L.block = else_b; lower_stmts(s.else_body, L); set_state(L, join, &s); }
            L.block = join;
            return;
        }
        case Stmt::While: case Stmt::For: {
            if (s.init) lower_stmt(*s.init, L);
            int cond_b = new_block(p, L);
            set_state(L, cond_b, &s);
            L.block = cond_b;
            int body_b = new_block(p, L), exit_b = new_block(p, L), step_b = -1;
            if (s.e) {
                ExprP cond = hoist(*s.e, L, s);
                Action& a = act(L, Action::IfGoto, &s); a.cond = std::move(cond); a.a = body_b; a.b = exit_b;
            } else set_state(L, body_b, &s);
            LowCtx inner = L;
            inner.break_state = exit_b;
            inner.cont_state = cond_b;
            if (s.step) { step_b = new_block(p, inner); inner.cont_state = step_b; }
            B(body_b).break_state = inner.break_state; B(body_b).cont_state = inner.cont_state;
            inner.block = body_b;
            lower_stmts(s.body, inner);
            if (s.step) {
                set_state(inner, step_b, &s);
                inner.block = step_b;
                lower_stmt(*s.step, inner);
            }
            set_state(inner, cond_b, &s);
            L.block = exit_b;
            return;
        }
        default: fail(s.pos, "a call to a rec proc cannot appear here");
        }
    }

    // Constants visible while lowering: the procedure's own and the main program's.
    std::optional<int64_t> fold_const_only(const Expr& e, TProc* p) {
        if (e.kind == Expr::Num) return e.value;
        if (e.kind == Expr::Name) {
            for (const Var& c : p->consts) if (c.name == e.text) return c.cval;
            if (!tprocs.empty() && tprocs.front().get() != p) for (const Var& c : tprocs.front()->consts) if (c.name == e.text) return c.cval;
            return std::nullopt;
        }
        if (e.kind == Expr::Unary) {
            auto a = fold_const_only(*e.a, p);
            if (!a) return std::nullopt;
            if (e.text == "-") return (-*a) & 0xFFFF;
            if (e.text == "!") return *a == 0 ? 1 : 0;
            if (e.text == "~") return (~*a) & 0xFFFF;
            return *a;
        }
        if (e.kind == Expr::Binary) {
            auto a = fold_const_only(*e.a, p), b = fold_const_only(*e.b, p);
            if (!a || !b) return std::nullopt;
            const std::string& o = e.text;
            if (o == "+") return (*a + *b) & 0xFFFF;
            if (o == "-") return (*a - *b) & 0xFFFF;
            if (o == "*") return (*a * *b) & 0xFFFF;
            if (o == "/") return *b ? std::optional<int64_t>(*a / *b) : std::nullopt;
            if (o == "%") return *b ? std::optional<int64_t>(*a % *b) : std::nullopt;
            if (o == "<<") return (*a << *b) & 0xFFFF;
            if (o == ">>") return *a >> *b;
            if (o == "&") return *a & *b;
            if (o == "|") return *a | *b;
            if (o == "^") return *a ^ *b;
        }
        return std::nullopt;
    }

    // Lower one trampolined procedure (or main) into blocks.
    void lower_proc(TProc* p, const std::vector<StmtP>& body) {
        LowCtx L{p, 0};
        L.block = new_block(p, L);
        p->entry = L.block;
        if (p->decl) {
            for (const Param& prm : p->decl->params) {
                if (prm.array) fail(prm.pos, "rec procs cannot take array parameters; use a top-level array");
                Var v; v.name = prm.name; v.word = prm.word;
                frame_add(p, v, prm.pos);
            }
        }
        lower_stmts(body, L);
        act(L, Action::Ret, nullptr);      // implicit return at the end
    }

    // ---- trampoline: generation ------------------------------------------------------------------------------
    void push_frame_scope(TProc* p) {
        push_scope();
        scopes.back().frame = true;
        for (const Var& v : p->consts) scopes.back().vars[v.name] = v;
        for (const Var& v : p->frame) { scopes.back().vars[v.name] = v; scopes.back().order.push_back(v.name); }
    }

    // idx = sp + off into a temporary
    int64_t stack_index(int off) { int64_t i = tmp(); copy(sp_cell, i); add(i, off); return i; }
    void stack_store(int off, int64_t cell) { int64_t i = stack_index(off); int64_t v = tmp(); copy(cell, v); array_write(stack_base, i, v); drop(v); drop(i); }
    void stack_load(int off, int64_t cell) { int64_t i = stack_index(off); clear(cell); array_read(stack_base, i, cell); drop(i); }

    void gen_tramp_return(const Expr* value, Pos pos) {
        TProc* p = cur_tp;
        if (p->ret && !value) fail(pos, "'" + p->name + "' returns a value");
        if (!p->ret && value) fail(pos, p->is_main() ? "the main program returns nothing" : "'" + p->name + "' returns nothing");
        if (value) {
            if (p->ret == 2) { clear16(p->retval); eval16_fresh(*value, p->retval); }
            else { clear(p->retval); eval8(*value, p->retval); }
        }
        if (p->is_main()) set(state_cell, 0);
        else { int64_t i = stack_index(-1); clear(state_cell); array_read(stack_base, i, state_cell); drop(i); }
        size_t b = ctxs.size();
        while (b-- > 0) if (ctxs[b].kind == ExitCtx::Block) break;
        if (b == (size_t)-1) internal("return outside a block");
        unwind_to(b);
    }

    void gen_action(Action& a, LBlock& blk) {
        switch (a.kind) {
        case Action::Plain:
            gen_stmt(*a.s);
            return;
        case Action::SetState:
            set(state_cell, a.a);
            return;
        case Action::IfGoto: {
            if (a.src) begin_stmt(*a.src);
            int64_t c = tmp(); eval_cond(*a.cond, c);
            int64_t f = tmp(); set(f, 1);
            if_nonzero(c, [&] { set(state_cell, a.a); clear(f); });
            if_nonzero(f, [&] { set(state_cell, a.b); });
            release(f); release(c);
            if (a.src) end_stmt();
            return;
        }
        case Action::Call: {
            TProc* callee = a.callee;
            const Stmt& decl = *callee->decl;
            if (a.src) begin_stmt(*a.src);
            // arguments first (they may read the caller's frame)
            int64_t mark = next_free;
            std::vector<int64_t> vals;
            for (size_t i = 0; i < a.args.size(); i++) {
                bool w = decl.params[i].word;
                int64_t t = w ? tmp2() : tmp();
                if (w) eval16_fresh(*a.args[i], t); else eval8(*a.args[i], t);
                vals.push_back(t);
            }
            // save the caller's frame and the resume state
            TProc* me = blk.owner;
            int off = 0;
            if (!me->is_main()) for (const Var& v : me->frame) {
                stack_store(off++, v.cell);
                if (v.word) stack_store(off++, v.cell + 1);
            }
            { int64_t r = tmp(); set(r, a.a); int64_t i = stack_index(off); array_write(stack_base, i, r); drop(i); drop(r); }
            add(sp_cell, off + 1);
            // load the callee's parameters
            for (size_t i = 0; i < vals.size(); i++) {
                const Var& prm = callee->frame[i];
                clear(prm.cell); move(vals[i], {{prm.cell, 1}});
                if (prm.word) { clear(prm.cell + 1); move(vals[i] + 1, {{prm.cell + 1, 1}}); }
            }
            next_free = mark;
            set(state_cell, callee->entry);
            if (a.src) end_stmt();
            return;
        }
        case Action::Pop: {
            TProc* me = blk.owner;
            if (a.src) begin_stmt(*a.src, true);
            // ref parameters: take the callee's final values before the pop may overwrite them (self-recursion)
            int64_t mark = next_free;
            std::vector<int64_t> saved;
            for (auto& co : a.copy_out) {
                const Var& prm = a.callee->frame[(size_t)co.second];
                int64_t t = prm.word ? tmp2() : tmp();
                copy(prm.cell, t);
                if (prm.word) copy(prm.cell + 1, t + 1);
                saved.push_back(t);
            }
            int n = me->save_count();
            add(sp_cell, -(n + 1));
            int off = 0;
            if (!me->is_main()) for (const Var& v : me->frame) {
                stack_load(off++, v.cell);
                if (v.word) stack_load(off++, v.cell + 1);
            }
            for (size_t i = 0; i < a.copy_out.size(); i++) {
                auto& co = a.copy_out[i];
                Var* dst = find_var(co.first);
                if (!dst || dst->is_const || dst->count >= 0) fail(a.src ? a.src->pos : Pos{}, "argument for a ref parameter must be a variable");
                const Var& prm = a.callee->frame[(size_t)co.second];
                if (dst->word != prm.word) fail(a.src ? a.src->pos : Pos{}, "'" + co.first + "' has the wrong type for 'ref " + prm.name + "'");
                clear(dst->cell); move(saved[i], {{dst->cell, 1}});
                if (prm.word) { clear(dst->cell + 1); move(saved[i] + 1, {{dst->cell + 1, 1}}); }
            }
            next_free = mark;
            if (!a.result.empty()) {
                Var* h = find_var(a.result);
                if (!h) internal("hidden result variable missing");
                clear(h->cell); copy(a.callee->retval, h->cell);
                if (h->word) { clear(h->cell + 1); if (a.callee->ret == 2) copy(a.callee->retval + 1, h->cell + 1); }
            }
            if (a.src) end_stmt();
            return;
        }
        case Action::Ret: {
            if (a.src) begin_stmt(*a.src);
            gen_tramp_return(a.ret.get(), a.src ? a.src->pos : Pos{});
            if (a.src) end_stmt();
            return;
        }
        }
    }

    static bool action_may_exit(const Action& a) {
        if (a.kind == Action::Plain) return may_exit(*a.s);
        return a.kind == Action::Ret;
    }
    void gen_actions(std::vector<Action>& acts, LBlock& blk, size_t from = 0) {
        for (size_t i = from; i < acts.size(); i++) {
            gen_action(acts[i], blk);
            if (i + 1 < acts.size() && action_may_exit(acts[i])) {
                int64_t t = tmp();
                copy(alive_cell, t);
                if_nonzero(t, [&] { gen_actions(acts, blk, i + 1); });
                release(t);
                return;
            }
        }
    }

    void gen_block(LBlock& blk) {
        TProc* p = blk.owner;
        set(alive_cell, 1);
        cur_tp = p;
        cur_proc = p->name;
        push_frame_scope(p);
        ExitCtx bc; bc.kind = ExitCtx::Block; bc.needs = true; bc.alive = alive_cell; bc.break_state = blk.break_state; bc.cont_state = blk.cont_state;
        ctxs.push_back(bc);
        gen_actions(blk.acts, blk);
        ctxs.pop_back();
        pop_scope();
        cur_tp = nullptr;
        cur_proc.clear();
    }

    // Dispatch by countdown: level k runs block k when cnt (= state - k) is 0, else
    // decrements and tries the next level.  About ten commands per level passed.
    void gen_dispatch(int64_t cnt, size_t k) {
        if (k > blocks.size()) return;
        int64_t f = tmp(); set(f, 1);
        open(cnt);
        {
            clear(f);
            add(cnt, -1);
            gen_dispatch(cnt, k + 1);
        }
        close();
        if_nonzero(f, [&] { gen_block(blocks[k - 1]); });
        release(f);
    }

    void gen_trampoline(const Program& prog) {
        tprocs.push_back(std::make_unique<TProc>());          // main is tprocs[0]
        for (auto& kv : procs) {
            if (!kv.second.tramp) continue;
            auto tp = std::make_unique<TProc>();
            tp->name = kv.first; tp->decl = kv.second.decl; tp->ret = kv.second.decl->ret;
            kv.second.tp = tp.get();
            tprocs.push_back(std::move(tp));
        }
        for (auto& x : prog.items) if (x->kind == Stmt::Stack) stack_size = x->count;
        lower_proc(tprocs[0].get(), prog.items);               // main first: its consts are visible to the others
        for (size_t i = 1; i < tprocs.size(); i++) lower_proc(tprocs[i].get(), tprocs[i]->decl->body);
        if (blocks.size() > 254) fail(Pos{}, "too many trampoline blocks (" + std::to_string(blocks.size()) + "); the state cell holds 254");

        stack_base = alloc((int)array_cells(stack_size));     // lowest: variables and temporaries stay close together above it
        state_cell = alloc(1); alive_cell = alloc(1); sp_cell = alloc(1);
        res.symbols.push_back(Symbol{"__state", state_cell, 1, 'b', 1, ""});
        res.symbols.push_back(Symbol{"__sp", sp_cell, 1, 'b', 1, ""});
        res.sp_cell = sp_cell;
        for (auto& tp : tprocs) {
            for (Var& v : tp->frame) {
                int cells = v.count >= 0 ? (int)array_cells(v.count) : (v.word ? 2 : 1);
                v.cell = alloc(cells);
                if (v.name.rfind("__h", 0) != 0)
                    res.symbols.push_back(Symbol{tp->is_main() ? v.name : tp->name + "." + v.name, v.cell, cells, v.count >= 0 ? 'a' : (v.word ? 'w' : 'b'), v.count >= 0 ? v.count : 1, ""});
            }
            if (tp->ret) tp->retval = alloc(tp->ret == 2 ? 2 : 1);
        }

        out += "\n# trampoline: stack of " + std::to_string(stack_size) + " cells and " + std::to_string(blocks.size()) + " blocks\n";
        push_frame_scope(tprocs[0].get());
        scopes.back().top = true;
        set(state_cell, tprocs[0]->entry);
        open(state_cell);
        {
            int64_t cnt = tmp();
            copy(state_cell, cnt);
            add(cnt, -1);
            gen_dispatch(cnt, 1);
            release(cnt);
        }
        close();
        pop_scope();
    }

    void run(const Program& prog) {
        prelude = parse(PRELUDE);
        register_procs(prelude, true);
        register_procs(prog, false);
        out += "# compiled by bfl\n";
        bool tramp = plan_trampolines(prog);
        if (tramp) { gen_trampoline(prog); res.cells_used = high - ram; return; }
        push_scope(true);
        ExitCtx mc; mc.kind = ExitCtx::Proc; mc.needs = contains_return(prog.items);
        if (mc.needs) { mc.alive = tmp(); set(mc.alive, 1); }
        ctxs.push_back(mc);
        gen_stmts(prog.items);
        ctxs.pop_back();
        pop_scope();
        res.cells_used = high - ram;
    }
};

} // namespace

Result compile(const std::string& source, uint32_t ram_start) {
    Gen g(source, ram_start);
    try {
        Program prog = parse(source);
        g.run(prog);
    } catch (const ParseError& e) {
        g.res.errors.push_back(Error{e.pos.line, e.pos.col, e.msg});
    } catch (const Fail& f) {
        g.res.errors.push_back(Error{f.pos.line, f.pos.col, f.msg});
    }
    if (!g.res.errors.empty()) { g.res.bf.clear(); g.res.stmts.clear(); }
    return std::move(g.res);
}

std::string describe(const Result& r) {
    std::string s = "# statement map: S line col end_line start end depth cont\n";
    char buf[256];
    for (const StmtRange& m : r.stmts) { snprintf(buf, sizeof buf, "S %d %d %d %d %d %d %d\n", m.line, m.col, m.end_line, m.start, m.end, m.depth, m.cont ? 1 : 0); s += buf; }
    s += "# symbols: V name kind cell cells count scope\n";
    for (const Symbol& v : r.symbols) { snprintf(buf, sizeof buf, "V %s %c 0x%llX %d %d %s\n", v.name.c_str(), v.kind, (long long)v.cell, v.cells, v.count, v.scope.c_str()); s += buf; }
    if (r.sp_cell >= 0) { snprintf(buf, sizeof buf, "# trampoline stack pointer cell 0x%llX\n", (long long)r.sp_cell); s += buf; }
    return s;
}

} // namespace bfl
