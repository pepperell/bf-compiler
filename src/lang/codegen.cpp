// codegen.cpp - BFL code generator: annotated syntax tree -> brainfuck text.
//
// Model: the pointer position is known at every point of the output.  Every
// variable owns fixed cells; a value of type T occupies sizeof(T) consecutive
// cells, least significant byte first.  Temporaries come from a LIFO pool just
// above the variables and are always returned to zero, so a freshly allocated
// cell is known to hold 0.  Loops keep the invariant "pointer on the same cell
// at '[' and ']'" by construction: close() moves back to the loop cell.
//
// The semantic pass (sema.cpp) has already checked the program and given every
// expression its type; this file only generates code.
//
// Byte expressions use an accumulating evaluator, eval8(e, dst, sign), which
// adds sign*e to dst; it computes the low byte of any expression, directly when
// the low byte depends only on the operands' low bytes.  Wider values use the
// N-byte routines.  The known-value map tracks cells holding compile-time
// constants in straight-line code so that constants are emitted as short deltas.
//
// Storage: variables in cells; arrays in cells use a walking layout with one
// group of 1 + 2s cells per element of s bytes (docs/LANG.md), so one walk
// reaches all of an element; the heap uses the walking layout of docs/HEAP.md.
//
// Procedures are inlined.  Recursion ("rec proc") uses a trampoline: the
// bodies of recursive procedures, of everything that calls them, and of the
// main program are cut into numbered blocks at every such call; a state cell
// names the next block and one dispatcher loop runs them.  See docs/TRAMPOLINE.md.
#include "lang.h"
#include "ast.h"
#include "sema.h"
#include "../registers.h"
#include "../bf.h"
#include "stdlib_gen.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <climits>
#include <deque>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace bfl {

namespace {

// The standard library (lib/*.bfl), embedded at build time by tools/embed.cpp.
const char* stdlib_text(const std::string& name) {
    for (const EmbeddedFile& f : kStdLib) if (name == f.name) return f.text;
    return nullptr;
}

// Heap walking layout (docs/HEAP.md): HK cells per byte, coarse hops of HOP bytes.
constexpr int HK = 5, HOP = 16;
constexpr int HC = 0, HH = 1, HL = 2, HV = 3, HD = 4;

struct Fail { Pos pos; std::string msg; };

struct Var {
    std::string name;
    int64_t cell = 0;
    const Type* type = nullptr;  // value type; for arrays the element type
    int count = -1;              // array length, -1 = scalar
    bool is_const = false;       // constants and heap arrays (cval = address)
    int64_t cval = 0;
    bool owned = true;           // false: ref alias, not cleared at scope exit
    bool arr_ptr = false;        // an array in the heap reached through the pointer in this variable's cells
    int arr_off = -1;            // arr_ptr locals of a recursive procedure: offset in its heap frame
    int size() const { return type ? std::max(type->size, 1) : 1; }
    bool cell_array() const { return count >= 0 && !is_const; }
};

struct TProc;
struct ProcInfo {
    const Stmt* decl;
    bool prelude;
    bool tramp = false;          // compiled as trampoline blocks
    TProc* tp = nullptr;
};

// ---- trampoline structures -------------------------------------------------------------
struct Action {
    enum Kind { Plain, PlainRun, SetState, IfGoto, Call, Ret, Pop, Clear, FrameEnter };
    Kind kind;
    const Stmt* s = nullptr;        // Plain: statement to generate (original or synthetic)
    const Stmt* src = nullptr;      // statement for the map
    ExprP cond;                     // IfGoto
    int a = -1, b = -1;             // SetState: a; IfGoto: true a, false b; Call: resume block
    TProc* callee = nullptr;        // Call / Pop
    std::vector<ExprP> args;        // Call
    std::string result;             // Pop: hidden variable receiving the callee's result ("" none); Clear: variable
    std::vector<std::pair<const Expr*, int>> copy_out;   // Pop: (caller lvalue, callee param index) for ref params
    ExprP ret;                      // Ret: value or null
    const std::vector<StmtP>* list = nullptr;   // PlainRun: statements list[from..] in one scope
    size_t from = 0;
    bool cont = false;              // Plain: continues a statement split at a call (no debugger stop)
};
struct LBlock {
    int id;
    TProc* owner;
    int64_t temp_base = -1;         // where its temporaries start: above every frame live while it runs
    int break_state = -1, cont_state = -1;
    std::vector<Action> acts;
};
struct TProc {
    std::string name;               // "" for the main program
    const Stmt* decl = nullptr;     // nullptr for main
    int entry = 0;
    const Type* ret = nullptr;      // result type or null
    int64_t retval = -1;
    std::vector<Var> frame;         // params, hoisted locals, hidden temporaries (saved on call unless main)
    std::vector<Var> consts;
    int hidden_n = 0;
    int heap_frame = 0;             // bytes of local arrays taken from the top of the heap on entry
    int idx = 0;                    // index in Gen::tprocs
    // Re-entrant: it can be active more than once (it is in a cycle of calls), so its
    // resume state goes on the save stack.  Otherwise its one resume state lives in
    // ret_cell and its local arrays in cells.
    bool reentrant = true;
    int64_t ret_cell = -1;
    int64_t frame_hi = 0;           // first cell above its frame
    bool is_main() const { return decl == nullptr; }
    int save_count() const {        // cells pushed on a call (not for main)
        if (is_main()) return 0;
        int n = 0;
        for (const Var& v : frame) n += v.size();
        return n;
    }
};

// A storage location an expression denotes.
struct LV {
    enum Kind { Cells, Elem, Heap, Reg } kind = Cells;
    const Type* type = nullptr;
    int64_t cell = 0;                       // Cells: first cell
    const Var* arr = nullptr;               // Elem: the array variable
    const Expr* index = nullptr;            // Elem: runtime index (null: cidx)
    int64_t cidx = -1;                      // Elem: constant index
    int64_t icell = -1;                     // Elem: or the runtime index already computed into this cell (pin)
    int boff = 0;                           // Elem: byte offset within the element
    const Expr* ptr = nullptr;              // Heap: pointer-valued expression (null: none)
    int64_t ptr_cell = -1;                  // Heap: or the cells of a pointer variable
    int64_t base = 0;                       // Heap: constant address part (includes constant offsets)
    std::vector<std::pair<const Expr*, int>> hidx;   // Heap: runtime indices, each scaled by its element size
    const bf::RegisterInfo* reg = nullptr;  // Reg
    int size() const { return std::max(type->size, 1); }
    bool const_addr() const { return kind == Heap && !ptr && ptr_cell < 0 && hidx.empty(); }
};

struct Gen {
    const std::vector<std::string>& texts;      // source text by file index
    int64_t ram;
    Result res;
    std::string& out;
    int64_t cur;
    std::vector<int64_t> loops;
    std::unordered_map<int64_t, int> known;      // cell -> constant value (RAM cells below the heap only)
    int64_t next_free, high;
    std::vector<std::vector<int32_t>> line_starts;
    Types* T = nullptr;
    Options opt;
    const SemaResult* sr = nullptr;

    // heap: element 0 starts at heap_elem0; a guard of HOP elements before it
    int64_t heap_lo = INT64_MAX, heap_elem0 = 0;
    int heap_size = 0;

    struct Scope { std::unordered_map<std::string, Var> vars; std::vector<std::string> order; int64_t mark; bool top; bool frame = false; };
    // A deque, so that pushing a scope never moves the others: code holds Var*
    // into them across the evaluation of expressions that inline procedures.
    std::deque<Scope> scopes;
    std::unordered_map<std::string, ProcInfo> procs;
    std::vector<std::string> call_stack;
    std::string cur_proc;

    // Exit contexts: what break / continue / return leave.
    struct ExitCtx {
        enum Kind { Loop, Proc, Block } kind;
        int64_t alive = -1, brk = -1;
        bool needs = false;
        int64_t retval = -1;        // Proc: result cell(s)
        const Type* ret = nullptr;  // Proc: result type or null
        int break_state = -1, cont_state = -1;   // Block
    };
    std::vector<ExitCtx> ctxs;
    std::vector<size_t> stmt_stack;
    int prelude_depth = 0;
    Program prelude;
    std::vector<std::string> file_names;

    // trampoline
    std::vector<std::unique_ptr<TProc>> tprocs;
    std::vector<LBlock> blocks;
    std::vector<StmtP> synth;
    std::vector<ExprP> synth_exprs;
    TProc* cur_tp = nullptr;        // generating a trampolined body
    int64_t state_cell = -1, alive_cell = -1, sp_cell = -1, stack_e0 = -1, stack_base = -1;
    int state_n = 1;                // bytes of the state cell (2 above 254 blocks)
    int sp_n = 1;                   // bytes of the stack pointer: a byte-indexed cell array up to 256, a 16-bit walking region above
    int stack_size = 128;

    Gen(const std::vector<std::string>& t, int64_t ram_start) : texts(t), ram(ram_start), out(res.bf), cur(ram_start), next_free(ram_start), high(ram_start) {
        for (const std::string& s : texts) {
            line_starts.emplace_back(1, 0);
            for (size_t i = 0; i < s.size(); i++) if (s[i] == '\n') line_starts.back().push_back((int32_t)i + 1);
        }
    }

    [[noreturn]] void fail(Pos p, const std::string& m) { throw Fail{p, m}; }
    [[noreturn]] void internal(const std::string& m) { throw Fail{Pos{0, 0, 0, 0}, "internal compiler error: " + m}; }
    static const Type* ret_of(const Stmt* d) { return d->vtype; }

    int line_of(int file, int32_t off) const {
        if (file < 0 || file >= (int)line_starts.size()) return 0;
        const auto& ls = line_starts[(size_t)file];
        auto it = std::upper_bound(ls.begin(), ls.end(), off);
        return (int)(it - ls.begin());
    }

    // ---- registers --------------------------------------------------------------
    const bf::RegisterInfo* find_reg(const std::string& n) const {
        for (const bf::RegisterInfo& r : bf::register_map()) if (n == r.name) return &r;
        return nullptr;
    }
    static bool reg_readable(const bf::RegisterInfo& r) { return r.kind != bf::REG_TRIGGER; }

    // ---- emission primitives ------------------------------------------------------------
    bool dry = false;               // measuring pass: the heap position is a placeholder, so no long moves are written
    bool trackable(int64_t c) const { return c >= ram && c < heap_lo; }
    void rep(char c, int64_t n) { if (n > 0 && !dry) out.append((size_t)n, c); }
    void go(int64_t c) { if (c > cur) rep('>', c - cur); else rep('<', cur - c); cur = c; }
    void open(int64_t c) { go(c); out += '['; loops.push_back(c); known.clear(); }
    void close() {
        int64_t c = loops.back(); loops.pop_back();
        go(c); out += ']';
        known.clear();
        if (trackable(c)) known[c] = 0;
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
    void setN(int64_t c, int n, int64_t v) { for (int k = 0; k < n; k++) set(c + k, (v >> (8 * k)) & 255); }
    void clearN(int64_t c, int n) { for (int k = 0; k < n; k++) clear(c + k); }

    // ---- cells ----------------------------------------------------------------------------
    int64_t alloc(int n) {
        int64_t c = next_free;
        next_free += n;
        if (next_free > high) high = next_free;
        if (next_free > heap_lo) internal("variables collide with the heap");
        for (int i = 0; i < n; i++) known[c + i] = 0;
        return c;
    }
    int64_t tmp() { return alloc(1); }
    int64_t tmp2() { return alloc(2); }
    int64_t tmpN(int n) { return alloc(n); }
    void release(int64_t c) {          // temp must be the most recent allocation; leaves it zero
        if (c != next_free - 1) internal("temporary released out of order");
        clear(c);
        next_free = c;
    }
    void release2(int64_t c) { releaseN(c, 2); }
    void releaseN(int64_t c, int n) {
        if (c != next_free - n) internal("temporary group released out of order");
        for (int k = n - 1; k >= 0; k--) clear(c + k);
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
        if (trackable(from)) if (auto k = known_val(from)) { add(dst, sign * *k); return; }
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

    // ---- arrays in cells (see docs/LANG.md for the layout) ------------------------------------
    // Element k of an array of s-byte elements is the group (C, V0..Vs-1, D0..Ds-1) at
    // base + K*(k+1), K = 1 + 2s; the group at base is a zero header.  One walk reaches
    // an element whatever its size, carrying every byte of it.
    struct Walk {                       // brainfuck text with pointer moves merged
        std::string s;
        int64_t pend = 0;
        void mv(int64_t d) { pend += d; }
        void put(const char* t) { s += rel(pend); pend = 0; s += t; }
        std::string done() { s += rel(pend); pend = 0; return s; }
    };
    static int64_t stride(int s) { return 1 + 2 * (int64_t)s; }
    // dst+k += byte b+k of element idx, k < n; idx consumed
    void elem_read(int64_t base, int s, int b, int n, int64_t idx, int64_t dst) {
        int64_t K = stride(s), C0 = base + K;
        move(idx, {{C0, 1}});
        go(C0);
        Walk w;
        w.put("[-[-"); w.mv(K); w.put("+"); w.mv(-K); w.put("]+"); w.mv(K); w.put("]");
        for (int k = 0; k < n; k++) {                // at the element: D -> V and C, then C -> D
            int64_t d = 1 + s + b + k, v = 1 + b + k;
            w.mv(d); w.put("[-"); w.mv(v - d); w.put("+"); w.mv(-v); w.put("+"); w.mv(d); w.put("]"); w.mv(-d);
            w.put("[-"); w.mv(d); w.put("+"); w.mv(-d); w.put("]");
        }
        w.mv(-K); w.put("[-");                      // back over the markers, carrying the V cells down
        for (int k = 0; k < n; k++) {
            int64_t v = 1 + b + k;
            w.mv(K + v); w.put("[-"); w.mv(-K); w.put("+"); w.mv(K); w.put("]"); w.mv(-(K + v));
        }
        w.mv(-K); w.put("]");
        out += w.done();
        cur = base; known.clear();
        for (int k = 0; k < n; k++) move(C0 + 1 + b + k, {{dst + k, 1}});
    }
    // byte b+k of element idx = val+k, k < n; idx and val consumed
    void elem_write(int64_t base, int s, int b, int n, int64_t idx, int64_t val) {
        int64_t K = stride(s), C0 = base + K;
        move(idx, {{C0, 1}});
        for (int k = 0; k < n; k++) move(val + k, {{C0 + 1 + b + k, 1}});
        go(C0);
        Walk w;
        w.put("[-[-"); w.mv(K); w.put("+"); w.mv(-K); w.put("]+");
        for (int k = 0; k < n; k++) {               // carry the value forward
            int64_t v = 1 + b + k;
            w.mv(v); w.put("[-"); w.mv(K); w.put("+"); w.mv(-K); w.put("]"); w.mv(-v);
        }
        w.mv(K); w.put("]");
        for (int k = 0; k < n; k++) {               // at the element: D = V
            int64_t d = 1 + s + b + k, v = 1 + b + k;
            w.mv(d); w.put("[-]"); w.mv(v - d); w.put("[-"); w.mv(d - v); w.put("+"); w.mv(v - d); w.put("]"); w.mv(-v);
        }
        w.mv(-K); w.put("[-"); w.mv(-K); w.put("]");  // back, clearing the markers
        out += w.done();
        cur = base; known.clear();
    }
    void array_read(int64_t base, int64_t idx, int64_t dst) { elem_read(base, 1, 0, 1, idx, dst); }     // byte arrays
    void array_write(int64_t base, int64_t idx, int64_t val) { elem_write(base, 1, 0, 1, idx, val); }
    static int64_t array_cells(int count, int s = 1) { return stride(s) * ((int64_t)count + 1); }
    static int64_t array_total(const Var& v) { return array_cells(v.count, v.size()); }

    // ---- the heap (docs/HEAP.md) --------------------------------------------------------------
    int64_t heap_cell(int64_t addr, int field) const { return heap_elem0 + HK * addr + field; }
    static std::string rel(int64_t n) { return n > 0 ? std::string((size_t)n, '>') : std::string((size_t)-n, '<'); }
    static std::string carry(int f, int steps, int at) {    // move field f forward `steps` elements, standing at field `at`
        int64_t dist = (int64_t)steps * HK;
        return rel(f - at) + "[-" + std::string((size_t)dist, '>') + "+" + std::string((size_t)dist, '<') + "]" + rel(at - f);
    }
    static std::string carry_back(int stride, bool fine) {
        int64_t d = (int64_t)stride * HK;
        std::string L((size_t)d, '<'), R((size_t)d, '>');
        std::string mv = rel(HV - HC) + "[-" + L + "+" + R + "]" + rel(HC - HV);
        std::string s = mv + L + "[-" + mv + L + "]";
        if (fine) s += rel(HV - HC) + "[-" + R + "+" + L + "]" + rel(HC - HV) + R;
        return s;
    }
    // Walk to the byte at the address held in addr (2 cells, consumed) and read it (dst += value)
    // or write val (consumed) into it.
    void heap_walk(int64_t addr, int64_t val, int64_t dst, bool write) { walk(heap_elem0, addr, val, dst, write, opt.check_heap); }
    // The same walk over any region laid out like the heap (e0 = cell of element 0).
    void walk(int64_t e0, int64_t addr, int64_t val, int64_t dst, bool write, bool check) {
        if (check) heap_check(addr);
        move(addr + 1, {{e0 + HH, 1}});
        move(addr, {{e0 + HL, 1}});
        if (write) move(val, {{e0 + HV, 1}});
        go(e0 + HH);
        std::string hop((size_t)HOP * HK, '>'), backh((size_t)HOP * HK, '<');
        std::string body = "-" + rel(HC - HH) + std::string(256 / HOP, '+');
        std::string step = "-[-" + hop + "+" + backh + "]" + carry(HH, HOP, HC) + carry(HL, HOP, HC);
        if (write) step += carry(HV, HOP, HC);
        step += "+" + hop;
        body += "[" + step + "]" + rel(HH - HC);
        out += "[" + body + "]";
        std::string fine = rel(HL - HH) + "[-" + rel(HC - HL) + "+" + rel(HL - HC) + "]" + rel(HC - HL);
        std::string one((size_t)HK, '>'), back1((size_t)HK, '<');
        std::string fstep = "-[-" + one + "+" + back1 + "]";
        if (write) fstep += carry(HV, 1, HC);
        fstep += "+" + one;
        out += fine + "[" + fstep + "]";
        if (write) {
            out += rel(HD - HC) + "[-]" + rel(HV - HD) + "[-" + rel(HD - HV) + "+" + rel(HV - HD) + "]" + rel(HC - HV);
            out += back1 + "[-" + back1 + "]" + one;
            out += backh + "[-" + backh + "]";
        } else {
            out += rel(HD - HC) + "[-" + rel(HV - HD) + "+" + rel(HH - HV) + "+" + rel(HD - HH) + "]";
            out += rel(HH - HD) + "[-" + rel(HD - HH) + "+" + rel(HH - HD) + "]" + rel(HC - HH);
            out += carry_back(1, true);
            out += carry_back(HOP, false);
        }
        cur = e0 - (int64_t)HOP * HK + HC;
        known.clear();
        if (!write) move(e0 - (int64_t)HOP * HK + HV, {{dst, 1}});
    }
    // --check-heap: report and halt on a null or out-of-range address (2 cells, preserved).
    void heap_check(int64_t addr) {
        int64_t bad = tmp();
        iszeroN(addr, bad, 2);
        int64_t lim = tmp2(); setN(lim, 2, heap_size);
        int64_t lt = tmp(); ltN_cells(addr, lim, 2, lt, 1);
        int64_t ge = tmp(); not8(lt, ge, 1);
        if_nonzero(ge, [&] { set(bad, 1); });
        release(ge); release(lt); release2(lim);
        if_nonzero(bad, [&] {
            int64_t t = tmp();
            for (unsigned char ch : std::string("\nheap access out of range\n")) { set(t, ch); go(t); out += '.'; }
            release(t);
            pulse_reg("HALT");
            clearN(addr, 2);            // carry on harmlessly at address 0 until the halt takes effect
        });
        release(bad);
    }

    // ---- 8-bit routines on cells ---------------------------------------------------------------
    void iszero8(int64_t c, int64_t z) {                // z += (c == 0)
        if (trackable(c)) if (auto k = known_val(c)) { add(z, *k == 0 ? 1 : 0); return; }
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
    // f += 1 when the byte at c has its top bit set (c preserved)
    void sign_flag(int64_t c, int64_t f) {
        int64_t t1 = tmp(); copy(c, t1);
        int64_t t2 = tmp(); set(t2, 128);
        int64_t r = tmp();
        lt8_core(t1, t2, r, 1);
        not8(r, f, 1);
        release(r); release(t2); release(t1);
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

    // ---- N-byte routines (least significant byte first) ---------------------------------------------
    bool known0N(int64_t c, int n) const { for (int k = 0; k < n; k++) if (!known_is(c + k, 0)) return false; return true; }
    void incN(int64_t c, int n, int from = 0) {        // c += 256^from
        add(c + from, 1);
        if (from + 1 >= n) return;
        int64_t z = tmp();
        iszero8(c + from, z);
        if_nonzero(z, [&] { incN(c, n, from + 1); });
        release(z);
    }
    void decN(int64_t c, int n, int from = 0) {        // c -= 256^from
        if (from + 1 < n) {
            int64_t z = tmp();
            iszero8(c + from, z);
            if_nonzero(z, [&] { decN(c, n, from + 1); });
            release(z);
        }
        add(c + from, -1);
    }
    void moveN(int64_t s, int64_t dst, int n, int sign = 1) { for (int k = 0; k < n; k++) move(s + k, {{dst + k, sign}}); }
    void addN(int64_t dst, int64_t s, int n) {         // dst += s; s consumed
        if (known0N(dst, n)) { moveN(s, dst, n); return; }
        for (int k = 0; k < n - 1; k++) {
            if (known_is(s + k, 0)) continue;
            open(s + k); add(s + k, -1); incN(dst, n, k); close();
        }
        move(s + n - 1, {{dst + n - 1, 1}});
    }
    void subN(int64_t dst, int64_t s, int n) {         // dst -= s; s consumed
        for (int k = 0; k < n - 1; k++) {
            if (known_is(s + k, 0)) continue;
            open(s + k); add(s + k, -1); decN(dst, n, k); close();
        }
        move(s + n - 1, {{dst + n - 1, -1}});
    }
    void addN_const(int64_t dst, int n, int64_t v) {
        if (n == 1) { add(dst, v); return; }
        uint64_t u = (uint64_t)v & (n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1));
        if (known0N(dst, n)) { for (int k = 0; k < n; k++) add(dst + k, (u >> (8 * k)) & 255); return; }
        for (int k = 0; k < n - 1; k++) {
            int b = (int)((u >> (8 * k)) & 255);
            if (!b) continue;
            int64_t t = tmp();
            set(t, b);
            open(t); add(t, -1); incN(dst, n, k); close();
            release(t);
        }
        add(dst + n - 1, (u >> (8 * (n - 1))) & 255);
    }
    void copyN(int64_t s, int64_t dst, int n) {        // dst += s; s preserved
        if (n == 1) { copy(s, dst); return; }
        if (known0N(dst, n)) { for (int k = 0; k < n; k++) copy(s + k, dst + k); return; }
        int64_t t = tmpN(n);
        for (int k = 0; k < n; k++) copy(s + k, t + k);
        addN(dst, t, n);
        releaseN(t, n);
    }
    void nonzeroN(int64_t w, int64_t f, int n) {        // f += (w != 0)
        if (n == 1) { int64_t cc = tmp(); copy(w, cc); if_nonzero(cc, [&] { add(f, 1); }); release(cc); return; }
        int64_t g = tmp(), cc = tmp();
        for (int k = 0; k < n; k++) { copy(w + k, cc); if_nonzero(cc, [&] { set(g, 1); }); }
        release(cc);
        move(g, {{f, 1}});
        release(g);
    }
    void iszeroN(int64_t w, int64_t z, int n) {
        int64_t g = tmp();
        nonzeroN(w, g, n);
        not8(g, z, 1);
        release(g);
    }
    void notN(int64_t c, int n) {                      // c = ~c
        for (int k = 0; k < n; k++) {
            int64_t t = tmp();
            move(c + k, {{t, 1}});
            add(c + k, 255);
            move(t, {{c + k, -1}});
            release(t);
        }
    }
    void negN(int64_t c, int n) { notN(c, n); incN(c, n); }
    // dst += sign*(a == b); preserved
    void eqN_cells(int64_t a, int64_t b, int n, int64_t dst, int sign) {
        if (n == 1) { eq8_cells(a, b, dst, sign); return; }
        int64_t d = tmpN(n);
        for (int k = 0; k < n; k++) { copy(a + k, d + k); copy(b + k, d + k, -1); }
        int64_t z = tmp();
        iszeroN(d, z, n);
        move(z, {{dst, sign}});
        release(z);
        releaseN(d, n);
    }
    // dst += sign*(a < b) unsigned; preserved
    void ltN_cells(int64_t a, int64_t b, int n, int64_t dst, int sign) {
        int k = n - 1;
        lt8_cells(a + k, b + k, dst, sign);
        if (k == 0) return;
        int64_t e = tmp();
        eq8_cells(a + k, b + k, e, 1);
        if_nonzero(e, [&] { ltN_cells(a, b, k, dst, sign); });
        release(e);
    }
    // dst += sign*(a < b) for two's complement values; preserved
    void sltN_cells(int64_t a, int64_t b, int n, int64_t dst, int sign) {
        int64_t ta = tmpN(n), tb = tmpN(n);
        for (int k = 0; k < n; k++) { copy(a + k, ta + k); copy(b + k, tb + k); }
        add(ta + n - 1, 128); add(tb + n - 1, 128);
        ltN_cells(ta, tb, n, dst, sign);
        releaseN(tb, n); releaseN(ta, n);
    }
    void shlN(int64_t w, int n) {                      // w <<= 1
        if (n == 1) { double8(w); return; }
        int64_t f = tmpN(n - 1);
        for (int k = 0; k < n - 1; k++) sign_flag(w + k, f + k);
        for (int k = 0; k < n; k++) double8(w + k);
        for (int k = 0; k < n - 1; k++) if_nonzero(f + k, [&] { add(w + k + 1, 1); });
        releaseN(f, n - 1);
    }
    // w >>= 1 (arithmetic when sgn); the bit shifted out is added to lowbit when >= 0
    void shrN(int64_t w, int n, bool sgn, int64_t lowbit = -1) {
        int64_t s = -1;
        if (sgn) { s = tmp(); sign_flag(w + n - 1, s); }
        int64_t h = tmpN(n), b = tmpN(n);
        for (int k = n - 1; k >= 0; k--) halve_bit(w + k, h + k, b + k);
        for (int k = 0; k < n; k++) move(h + k, {{w + k, 1}});
        for (int k = 1; k < n; k++) if_nonzero(b + k, [&] { add(w + k - 1, 128); });
        if (lowbit >= 0) move(b, {{lowbit, 1}}); else clear(b);
        releaseN(b, n); releaseN(h, n);
        if (sgn) { if_nonzero(s, [&] { add(w + n - 1, 128); }); release(s); }
    }
    // The count of a shift into the zero cell n: the right operand's value, or 255 when that is
    // negative or above 255.  Either way every bit is shifted out, as by a count of the width or
    // more, instead of the count being taken modulo 256.
    void eval_count(const Expr& b, int64_t n) {
        int sz = b.type && b.type->is_int() ? b.type->size : 1;
        if (sz <= 1) { eval8(b, n); return; }
        int64_t c = tmpN(sz); eval_into(b, c, b.type);
        move(c, {{n, 1}});
        int64_t f = tmp();
        for (int k = 1; k < sz; k++) if_nonzero(c + k, [&] { set(f, 1); });
        if_nonzero(f, [&] { clear(n); add(n, -1); });
        release(f);
        drop(c, sz);
    }
    // w shifted by m whole bytes (left, or right with the sign copied when sgn): byte moves, no bit loops
    void shift_bytes(int64_t w, int n, int m, bool left, bool sgn) {
        int64_t s = -1;
        if (!left && sgn) { s = tmp(); sign_flag(w + n - 1, s); }
        int64_t t = tmpN(n);
        moveN(w, t, n);
        for (int k = 0; k < n; k++) {
            int src = left ? k - m : k + m;
            if (src >= 0 && src < n) move(t + src, {{w + k, 1}});
        }
        releaseN(t, n);                                // the bytes shifted out are cleared here
        if (s >= 0) {
            if_nonzero(s, [&] { for (int k = std::max(n - m, 0); k < n; k++) add(w + k, 255); });
            release(s);
        }
    }
    // The multiply loop, bf::multiply_loop(n, w), which the optimiser runs in one step: A and B (n cells
    // each, consumed) are moved into a block of fresh temporaries laid out as bf::MultiplyLayout says, and
    // after its 8n passes the block's P holds A * B mod 256^w; take(c, L) moves P out of the block at c.
    void multiply_block(int64_t a, int64_t b, int n, int w, const std::function<void(int64_t, const bf::MultiplyLayout&)>& take) {
        bf::MultiplyLayout L{n, w};
        int64_t c = tmpN(L.cells());
        for (int k = 0; k < n; k++) { move(a + k, {{c + L.a(k), 1}}); move(b + k, {{c + L.b(k), 1}}); }
        set(c, 8 * n);
        go(c);
        out += bf::multiply_loop(n, w);
        known.clear();
        for (int64_t s = c; s < c + L.cells(); s++) if (trackable(s)) known[s] = 0;     // B is shifted out
        for (int k = 0; k < w; k++) forget(c + L.p(k));
        for (int k = 0; k < n; k++) forget(c + L.a(k));
        take(c, L);
        for (int k = 0; k < n; k++) clear(c + L.a(k));
        releaseN(c, L.cells());
        for (int k = 0; k < n; k++) {
            if (trackable(a + k)) known[a + k] = 0;
            if (trackable(b + k)) known[b + k] = 0;
        }
    }
    // dst += a * b (mod 2^8n, the same for signed values); a and b consumed
    void mulN(int64_t a, int64_t b, int64_t dst, int n) {
        multiply_block(a, b, n, n, [&](int64_t c, const bf::MultiplyLayout& L) {
            if (known0N(dst, n)) { for (int k = 0; k < n; k++) move(c + L.p(k), {{dst + k, 1}}); return; }
            int64_t t = tmpN(n);
            for (int k = 0; k < n; k++) move(c + L.p(k), {{t + k, 1}});
            addN(dst, t, n);
            releaseN(t, n);
        });
    }
    // dst += a * k for a constant k; a consumed.  Up to 7 by copies, shifts and adds; from 8 the
    // multiply loop is smaller (and runs in one step): for a 16-bit a * 12 about 1 350 commands
    // against 1 600, and 59 IR instructions against 91.
    void mul_constN(int64_t a, int64_t k, int64_t dst, int n) {
        uint64_t u = (uint64_t)k & (n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1));
        if (u == 0) { clearN(a, n); return; }
        if (u <= 3) {
            for (uint64_t i = 1; i < u; i++) copyN(a, dst, n);
            addN(dst, a, n);
            return;
        }
        if (u >= 8 && n <= 4) {
            int64_t b = tmpN(n);
            setN(b, n, (int64_t)u);
            mulN(a, b, dst, n);
            releaseN(b, n);
            return;
        }
        int top = 63; while (!((u >> top) & 1)) top--;
        for (int i = 0; i <= top; i++) {
            if ((u >> i) & 1) { if (i == top) { addN(dst, a, n); break; } copyN(a, dst, n); }
            shlN(a, n);
        }
    }
    // dst (w cells) += src (m <= w cells, consumed).  The IR optimiser runs a carry chain of up to 5
    // cells (incN) as one step; below the top 5 cells the carry ripples through a cell instead.
    void add_wide(int64_t dst, int w, int64_t src, int m) {
        auto inc_carry = [&](int64_t x, int64_t cy) {  // x += 1, cy += 1 when x wraps
            add(x, 1);
            int64_t z = tmp(); iszero8(x, z);
            if_nonzero(z, [&] { add(cy, 1); });
            release(z);
        };
        int64_t c = -1;
        for (int i = 0; i < w; i++) {
            if (w - i <= 5) {
                if (c >= 0) { open(c); add(c, -1); incN(dst, w, i); close(); }
                for (int j = i; j < m; j++) { open(src + j); add(src + j, -1); incN(dst, w, j); close(); }
                break;
            }
            if (c < 0) c = tmp();
            int64_t c2 = tmp();
            if (i < m) { open(src + i); add(src + i, -1); inc_carry(dst + i, c2); close(); }
            if (i) { open(c); add(c, -1); inc_carry(dst + i, c2); close(); }
            move(c2, {{c, 1}});
            release(c2);
        }
        if (c >= 0) release(c);
    }
    // dst (n cells, known zero) = mulshr(ea, eb, k) at type t: floor(a * b / 2^k) of the exact product
    // (2n bytes), in n bytes.  The multiply loop computes the w = n + k/8 (+1 when k%8) low bytes of the
    // product (at most 2n): the bytes below k/8 are dropped, the n (+1) above them go to acc, and its last
    // k%8 bits are one pass of byte divisions.  Signed values are multiplied as magnitudes; for a negative
    // product the result is -(floor(m / 2^k) + 1 when bits shifted out were set), ~h or -h.
    void mulshr(const Expr& ea, const Expr& eb, int k, const Type* t, int64_t dst) {
        mulshr(k, t, dst, [&](int64_t a, int64_t b) { eval_into(ea, a, t); eval_into(eb, b, t); });
    }
    // the same with the factors put in a and b (n cells each, known zero) by load
    void mulshr(int k, const Type* t, int64_t dst, const std::function<void(int64_t, int64_t)>& load) {
        int n = std::max(t->size, 1);
        bool sgn = t->is_signed;
        k = std::min(k, 16 * n);                      // floor(a * b / 2^16n) is already 0 or -1
        int q = k / 8, r = k % 8, w = std::min(2 * n, n + q + (r ? 1 : 0));
        int64_t neg = sgn ? tmp() : -1, a = tmpN(n), b = tmpN(n);
        load(a, b);
        int64_t e = -1;                               // 1 while the bits shifted out are all zero
        if (sgn) {
            int64_t s2 = tmp(), c = tmp();
            sign_flag(a + n - 1, neg);
            copy(neg, c); if_nonzero(c, [&] { negN(a, n); });
            sign_flag(b + n - 1, s2);
            copy(s2, c); if_nonzero(c, [&] { negN(b, n); });
            release(c);
            move(s2, {{neg, -1}});                     // neg != 0: the signs differ
            release(s2);
            e = tmp();
            set(e, 1);
        }
        int64_t acc = tmpN(n + 1);
        multiply_block(a, b, n, w, [&](int64_t c, const bf::MultiplyLayout& L) {
            for (int i = 0; i < w; i++) {
                int64_t pc = c + L.p(i);
                if (i >= q) move(pc, {{acc + i - q, 1}});
                else if (sgn) if_nonzero(pc, [&] { clear(e); });
                else clear(pc);
            }
        });
        if (r) {                                       // acc >>= r: acc[i] = acc[i] / 2^r + (acc[i+1] % 2^r) << (8 - r)
            for (int i = 0; i <= n; i++) {
                int64_t d = tmp(), qq = tmp(), rr = tmp();
                set(d, int64_t(1) << r);
                divmod8(acc + i, d, qq, rr);
                move(qq, {{acc + i, 1}});
                if (i) move(rr, {{acc + i - 1, 1 << (8 - r)}});
                else if (sgn) if_nonzero(rr, [&] { clear(e); });
                else clear(rr);
                release(rr); release(qq); release(d);
            }
        }
        if (sgn) if_nonzero(neg, [&] { notN(acc, n); if_nonzero(e, [&] { incN(acc, n); }); });
        moveN(acc, dst, n);
        releaseN(acc, n + 1);
        if (sgn) release(e);
        releaseN(b, n);
        releaseN(a, n);
        if (sgn) release(neg);
    }
    // q += a / b, r += a % b (unsigned; b == 0 gives q = 0, r = a).  a consumed, b cleared; q and r start at zero.
    // The 8n passes of shift and subtract are bf::division_loop(n), which the optimiser runs in
    // one step; its cells are a block of fresh temporaries laid out as bf::DivisionLayout says.
    void divmodN(int64_t a, int64_t b, int64_t q, int64_t r, int n) {
        if (n == 1) { divmod8(a, b, q, r); return; }
        int64_t z = tmp();
        iszeroN(b, z, n);
        int64_t f = tmp(); set(f, 1);
        if_nonzero(z, [&] { moveN(a, r, n); clear(f); });
        if_nonzero(f, [&] {
            bf::DivisionLayout L{n};
            int64_t c = tmpN(L.cells());
            for (int k = 0; k < n; k++) { move(a + k, {{c + L.a(k), 1}}); move(b + k, {{c + L.b(k), 1}}); }
            set(c, 8 * n);
            go(c);
            out += bf::division_loop(n);
            known.clear();
            for (int64_t s = c; s < c + L.cells(); s++) if (trackable(s)) known[s] = 0;
            for (int k = 0; k < n; k++) { forget(c + L.a(k)); forget(c + L.r(k)); forget(c + L.b(k)); }
            for (int k = 0; k < n; k++) { move(c + L.a(k), {{q + k, 1}}); move(c + L.r(k), {{r + k, 1}}); clear(c + L.b(k)); }
            releaseN(c, L.cells());
        });
        release(f); release(z);
        for (int k = 0; k < n; k++) {                  // moved on either way, and b is 0 when not
            if (trackable(a + k)) known[a + k] = 0; else clear(a + k);
            if (trackable(b + k)) known[b + k] = 0; else clear(b + k);
        }
    }
    // signed: the quotient truncates towards zero, the remainder takes the sign of the dividend
    void sdivmodN(int64_t a, int64_t b, int64_t q, int64_t r, int n) {
        int64_t sa = tmp(); sign_flag(a + n - 1, sa);
        int64_t sb = tmp(); sign_flag(b + n - 1, sb);
        int64_t t = tmp();
        copy(sa, t); if_nonzero(t, [&] { negN(a, n); });
        copy(sb, t); if_nonzero(t, [&] { negN(b, n); });
        release(t);
        int64_t uq = tmpN(n), ur = tmpN(n);
        divmodN(a, b, uq, ur, n);
        int64_t x = tmp();
        copy(sa, x); copy(sb, x, -1);          // x != 0  <=>  signs differ
        if_nonzero(x, [&] { negN(uq, n); });
        release(x);
        if_nonzero(sa, [&] { negN(ur, n); });
        clear(sb);
        moveN(ur, r, n); moveN(uq, q, n);
        releaseN(ur, n); releaseN(uq, n);
        release(sb); release(sa);
    }

    // ---- arithmetic shared within a block (docs/LANG.md, "Idioms used by the compiler") ------------------
    // Several multiplies (or mulshr, divisions, remainders) of one kind and width run through one copy of the
    // routine: a loop runs once per operation, and the code around the routine, chosen by a flag per
    // operation, evaluates the next operation's factors into the routine's operand cells.  The first
    // operation's factors are evaluated before the loop and whatever follows the last runs after it, both
    // with the known cell values of straight-line code; the values known before the loop that it cannot
    // change are known again after it.  Two places use it: the terms of one sum (shared_terms, from
    // binary_wide) and a run of statements (find_run and gen_run, from gen_stmts).  Either shares a routine
    // that would otherwise be emitted at least share_min() times, at most SHARE_MAX of them in one loop.
    // The fewest operations worth sharing: opt.share (2 by default), 0 or less never shares, and 1 is
    // taken as 2 (one operation has nothing to share the routine with).
    int share_min() const { return opt.share <= 0 ? 0 : std::max(opt.share, 2); }
    // The most operations one loop runs.  Its flags and operand cells lie past the variables, so the code
    // between them grows with the count: beyond about 16 operations the loop gets bigger for each one
    // added, and a 16-bit run of 32 statements is bigger shared than inlined.  (The pass counter is also a
    // single cell, good for 255.)  A sum with more is cut into loops of at most this many.
    static constexpr int SHARE_MAX = 16;
    struct SharedKind {
        char op = 0;                // '*', 'm' (mulshr), '/', '%'
        int n = 0, k = 0;           // width; mulshr's shift count
        bool sgn = false;
        bool operator==(const SharedKind& o) const { return op == o.op && n == o.n && k == o.k && sgn == o.sgn; }
    };
    struct SharedOp { const Expr* e; int sign; };       // acc += sign * e
    using Terms = std::vector<std::pair<const Expr*, int>>;
    // Can evaluating e be moved to another place in its statement: no calls, input or registers?
    // run_names: variables declared by earlier statements of a run being looked at (find_run).
    std::unordered_set<std::string>* run_names = nullptr;
    bool movable(const Expr& e) {
        switch (e.kind) {
        case Expr::Num: case Expr::Str: case Expr::Sizeof: case Expr::Null: return true;
        case Expr::Name: return find_var(e.text) || (run_names && run_names->count(e.text));
        case Expr::Index: case Expr::Binary: case Expr::MulShr: return movable(*e.a) && movable(*e.b);
        case Expr::Field: case Expr::Deref: case Expr::AddrOf: case Expr::Unary: case Expr::Cast: return movable(*e.a);
        default: return false;
        }
    }
    // The routine e runs on as a term of an n-byte sum, if it is one that can be shared.
    std::optional<SharedKind> shared_kind(const Expr& e, int n) {
        if (!share_min() || n < 2 || sz(e) != n || !e.type || !e.type->is_int()) return std::nullopt;
        if (!precomputed.empty() && precomputed.count(&e)) return std::nullopt;    // computed already (gen_run)
        const Type* t = conv_type(e.type);
        if (e.kind == Expr::MulShr) return SharedKind{'m', n, std::min((int)e.value, 16 * n), t->is_signed};
        if (e.kind != Expr::Binary || !(e.text == "*" || e.text == "/" || e.text == "%")) return std::nullopt;
        if (e.a->type->is_ptr() || e.b->type->is_ptr()) return std::nullopt;
        if (e.text == "*" && (fold(*e.b, true) || fold(*e.a, true))) return std::nullopt;     // a constant factor: mul_constN
        return SharedKind{e.text[0], n, 0, t->is_signed};
    }
    // acc (kd.n cells) += sign * e for every operation, through one copy of the routine.  The hooks generate
    // code around operation i: before(i) and loaded(i) before and after its factors are evaluated, after(i)
    // once its result is in acc.  fresh: acc is zero before every operation (after(i) empties it).
    // written: the cells, other than acc and temporaries, that the hooks write; they may add to it as they run.
    // at: the statement the loop's own code is mapped to (a continuation), or null.
    struct SharedHooks {
        std::function<void(int)> before, loaded, after;
        std::vector<std::pair<int64_t, int64_t>>* written = nullptr;
        const Stmt* at = nullptr;
        bool fresh = false;
    };
    void run_shared(const SharedKind& kd, const std::vector<SharedOp>& ops, int64_t acc, const SharedHooks& h = {}) {
        int n = kd.n, m = (int)ops.size();
        if (m < 2 || m > SHARE_MAX) internal("shared routine for " + std::to_string(m) + " operations");
        bool pos = false, neg = false;
        for (const SharedOp& o : ops) (o.sign > 0 ? pos : neg) = true;
        auto snapshot = known;
        int64_t base = next_free;
        int64_t cnt = tmp(), F = tmpN(m - 1), NEG = pos && neg ? tmp() : -1;
        int64_t P = kd.op == 'm' && !(h.fresh && !neg) ? tmpN(n) : -1, X = tmpN(n), Y = tmpN(n);   // P: mulshr's result
        auto load = [&](int i) {                       // the factors of operation i into X and Y
            if (h.before) h.before(i);
            const Expr& e = *ops[(size_t)i].e;
            const Type* t = conv_type(e.type);
            eval_into(*e.a, X, t);
            eval_into(*e.b, Y, t);
            if (NEG >= 0 && ops[(size_t)i].sign < 0) add(NEG, 1);
            if (h.loaded) h.loaded(i);
        };
        load(0);
        if (h.at) begin_stmt(*h.at, true);             // the loop and the routine: the first statement's
        set(cnt, m);
        set(F, 1);
        open(cnt);
        {
            const Type* t = conv_type(ops[0].e->type);
            // the result r of the routine: acc += r, acc -= r, or by the sign flag
            auto accumulate = [&](int64_t r) {
                if (!pos) { subN(acc, r, n); return; }
                if (NEG >= 0) if_nonzero(NEG, [&] { negN(r, n); });
                if (h.fresh) moveN(r, acc, n); else addN(acc, r, n);
            };
            if (kd.op == '*') {
                if (!pos) negN(X, n);
                else if (NEG >= 0) if_nonzero(NEG, [&] { negN(X, n); });
                mulN(X, Y, acc, n);
            } else if (kd.op == 'm') {
                auto from_xy = [&](int64_t a, int64_t b) { moveN(X, a, n); moveN(Y, b, n); };
                if (P < 0) mulshr(kd.k, t, acc, from_xy);
                else { mulshr(kd.k, t, P, from_xy); accumulate(P); }
            } else {
                int64_t q = tmpN(n), r = tmpN(n);
                if (kd.sgn) sdivmodN(X, Y, q, r, n); else divmodN(X, Y, q, r, n);
                accumulate(kd.op == '/' ? q : r);
                releaseN(r, n); releaseN(q, n);
            }
            for (int64_t c = F + m - 1; c < next_free; c++) if (trackable(c)) known[c] = 0;     // the routine's cells are zero again
            add(cnt, -1);
            // The code after operation i runs if F[i] is set and sets F[i + 1].  It is generated lowest first,
            // the order of the statements it holds, each piece starting and ending on cnt; then the pieces
            // are put highest first, so that the flag set for the next operation waits for the next pass.
            std::vector<std::pair<size_t, size_t>> text, ranges;
            for (int i = 0; i < m - 1; i++) {
                size_t t0 = out.size(), r0 = res.stmts.size();
                if_nonzero(F + i, [&] { if (h.after) h.after(i); load(i + 1); if (i + 1 < m - 1) add(F + i + 1, 1); });
                go(cnt);
                text.emplace_back(t0, out.size());
                ranges.emplace_back(r0, res.stmts.size());
            }
            std::string moved;
            std::vector<StmtRange> moved_ranges;
            for (size_t i = text.size(); i-- > 0;) {
                int32_t delta = (int32_t)(text[0].first + moved.size()) - (int32_t)text[i].first;
                moved.append(out, text[i].first, text[i].second - text[i].first);
                for (size_t r = ranges[i].first; r < ranges[i].second; r++) {
                    StmtRange rr = res.stmts[r];
                    rr.start += delta; rr.end += delta;
                    moved_ranges.push_back(rr);
                }
            }
            out.replace(text[0].first, moved.size(), moved);
            std::copy(moved_ranges.begin(), moved_ranges.end(), res.stmts.begin() + (std::ptrdiff_t)ranges[0].first);
        }
        close();
        if (h.at) end_stmt();
        for (auto& kv : snapshot) {                    // what the loop left alone is known again
            if (kv.first >= base || (kv.first >= acc && kv.first < acc + n)) continue;
            bool w = false;
            if (h.written) for (auto& r : *h.written) if (kv.first >= r.first && kv.first < r.second) w = true;
            if (!w) known[kv.first] = kv.second;
        }
        for (int64_t c = cnt + 1; c < next_free; c++) if (trackable(c)) known[c] = 0;
        if (h.after) h.after(m - 1);
        releaseN(Y, n); releaseN(X, n);
        if (P >= 0) releaseN(P, n);
        if (NEG >= 0) release(NEG);
        releaseN(F, m - 1);
        release(cnt);
    }

    // A run of statements sharing a routine.  Its operations, wherever they are in the statements, are
    // computed first, each into cells of its own, which the statement's code then reads instead of
    // computing it: precomputed maps each operation to its cells.  A statement's operations go through
    // the loop in turn, and after the last of them the statement itself is generated, in the code run
    // before the next operation's factors (or after the loop).
    std::unordered_map<const Expr*, int64_t> precomputed;
    // Can s run inside a shared loop: an assignment or declaration whose expressions can be moved?
    bool shareable_stmt(const Stmt& s) {
        switch (s.kind) {                              // (a register is not movable)
        case Stmt::Assign: return movable(*s.target) && movable(*s.e);
        case Stmt::IncDec: return movable(*s.target);
        case Stmt::Decl: return !s.in_heap && s.count < 0 && s.e && s.vtype && s.vtype->is_scalar() && movable(*s.e);
        default: return false;
        }
    }
    // The shareable operations that evaluating e reaches through eval_same: in sums, operands, casts and
    // negations wider than a byte (a byte's operations go through eval8, which computes its own low byte).
    void wide_ops(const Expr& e, std::vector<const Expr*>& found) {
        if (sz(e) < 2 || !e.type || !e.type->is_int() || fold(e)) return;
        if (shared_kind(e, sz(e))) { found.push_back(&e); return; }
        switch (e.kind) {
        case Expr::Unary: if (e.text == "-" || e.text == "~") wide_ops(*e.a, found); return;
        case Expr::Cast: wide_ops(*e.a, found); return;
        case Expr::MulShr: wide_ops(*e.a, found); wide_ops(*e.b, found); return;
        case Expr::Binary:
            if (is_cmp(e.text) || e.text == "&&" || e.text == "||" || e.a->type->is_ptr() || e.b->type->is_ptr()) return;
            wide_ops(*e.a, found); wide_ops(*e.b, found);
            return;
        default: return;
        }
    }
    // the operations of statement s that could be shared
    std::vector<const Expr*> stmt_ops(const Stmt& s) {
        std::vector<const Expr*> found;
        const Expr* e = s.kind == Stmt::Decl ? s.e.get() : nullptr;
        // gen_assign rewrites other compound assignments as x = x op e with copies of the expressions
        if (s.kind == Stmt::Assign && (s.op == "=" || ((s.op == "+=" || s.op == "-=") && s.target->kind == Expr::Name))) e = s.e.get();
        int n = s.kind == Stmt::Decl ? std::max(s.vtype->size, 1) : e ? sz(*s.target) : 0;
        if (e && n >= 2 && sz(*e) >= 2) wide_ops(*e, found);
        return found;
    }
    // Does e read a variable the compiler knows the value of (a cell in vals)?  declared: the names declared
    // by earlier statements of the run, which have no cells yet.
    bool reads_known(const Expr& e, const std::unordered_map<int64_t, int>& vals, const std::unordered_set<std::string>& declared) {
        if (e.kind == Expr::Name) {
            const Var* v = declared.count(e.text) ? nullptr : find_var(e.text);
            if (!v || v->is_const) return false;
            for (int64_t c = v->cell, end = c + (v->count >= 0 ? array_total(*v) : v->size()); c < end; c++)
                if (vals.count(c)) return true;
            return false;
        }
        return (e.a && reads_known(*e.a, vals, declared)) || (e.b && reads_known(*e.b, vals, declared));
    }
    // The variable whose cells an assignment to target writes, if the compiler can tell.
    const Var* target_var(const Expr& target) {
        switch (target.kind) {
        case Expr::Name: return find_var(target.text);
        case Expr::Index: return target_var(*target.a);
        case Expr::Field: return target.arrow ? nullptr : target_var(*target.a);
        default: return nullptr;
        }
    }
    // The longest run of statements from v[i] whose operations of one kind, at least share_min() and at most
    // SHARE_MAX of them, can share a routine.  Statements without such an operation may sit between two that
    // have one, up to SHARE_GAP in a row.  An operation that reads a variable with a value the compiler knows
    // ends the run before its statement: generated outside the loop, it can fold or use a constant factor,
    // and its result can be known.  Returns the number of statements in the run (0: none), its kind and
    // operations.
    static constexpr int SHARE_GAP = 2;
    size_t find_run(const std::vector<StmtP>& v, size_t i, SharedKind& kind, std::vector<std::vector<const Expr*>>& ops) {
        if (!share_min() || !shareable_stmt(*v[i])) return 0;
        struct Found { std::vector<std::pair<const Expr*, SharedKind>> ops; bool in_sum = true; };
        std::vector<Found> all;
        std::unordered_map<int64_t, int> saved;
        std::swap(saved, known);                       // the run's later statements see other values
        std::unordered_map<int64_t, int> vals = saved; // known before the run and not written by it so far
        std::unordered_set<std::string> declared;
        run_names = &declared;
        for (size_t j = i; j < v.size(); j++) {
            const Stmt& s = *v[j];
            if (!shareable_stmt(s)) break;
            Found f;
            for (const Expr* x : stmt_ops(s)) f.ops.emplace_back(x, *shared_kind(*x, sz(*x)));
            if (std::any_of(f.ops.begin(), f.ops.end(), [&](auto& o) { return reads_known(*o.first, vals, declared); })) break;
            if (!f.ops.empty()) {                      // are they all terms of the statement's sum?
                Terms terms; int64_t c = 0;
                collect_terms(*s.e, 1, terms, c, sz(*s.e));
                for (auto& o : f.ops)
                    if (std::none_of(terms.begin(), terms.end(), [&](auto& t) { return t.first == o.first; })) f.in_sum = false;
            }
            all.push_back(f);
            if (all.size() > (size_t)SHARE_GAP && std::all_of(all.end() - SHARE_GAP - 1, all.end(), [](auto& x) { return x.ops.empty(); })) break;
            if (s.kind == Stmt::Decl) declared.insert(s.name);
            else if (const Var* tv = target_var(*s.target)) {
                for (int64_t c = tv->cell, end = c + (tv->count >= 0 ? array_total(*tv) : tv->size()); c < end; c++) vals.erase(c);
            } else vals.clear();                       // through a pointer: it could be anything
        }
        std::swap(saved, known);
        run_names = nullptr;
        if (all.empty() || all[0].ops.empty()) return 0;
        size_t best = 0; int best_count = 0;
        for (auto& first : all[0].ops) {
            const SharedKind& kd = first.second;
            int count = 0, stmts = 0, gap = 0;
            size_t end = 0;
            std::vector<std::vector<const Expr*>> picked;
            for (size_t j = 0; j < all.size(); j++) {
                std::vector<const Expr*> mine;
                for (auto& o : all[j].ops) if (o.second == kd) mine.push_back(o.first);
                if (mine.empty()) { if (++gap > SHARE_GAP) break; picked.push_back(mine); continue; }
                if (count + (int)mine.size() > SHARE_MAX) break;
                gap = 0; count += (int)mine.size(); stmts++; end = j + 1;
                picked.push_back(mine);
            }
            // the terms of a single statement's sum share without a run (shared_terms)
            if (count < share_min() || count <= best_count || (stmts == 1 && all[0].in_sum)) continue;
            best = end; best_count = count; kind = kd;
            picked.resize(end);
            ops = picked;
        }
        return best;
    }
    // Generates v[i, i + len) as a run sharing a routine of kind kd (find_run).
    void gen_run(const std::vector<StmtP>& v, size_t i, size_t len, const SharedKind& kd, const std::vector<std::vector<const Expr*>>& ops) {
        int n = kd.n;
        for (size_t j = i; j < i + len; j++) {         // declared variables outlive the loop: their cells come first
            const Stmt& d = *v[j];
            if (d.kind != Stmt::Decl) continue;
            if (scopes.size() == 1 && call_stack.empty()) place_group(d);
            if (prealloc.count(&d)) continue;
            Var tv; tv.type = d.vtype; tv.count = d.count;
            prealloc[&d] = alloc(tv.size());
        }
        std::vector<SharedOp> flat;
        std::vector<size_t> stmt_of, slot_of;          // statement (index in ops) and cells slot of each operation
        size_t slots = 0;
        for (size_t j = 0; j < ops.size(); j++) {
            for (size_t k = 0; k < ops[j].size(); k++) { flat.push_back({ops[j][k], 1}); stmt_of.push_back(j); slot_of.push_back(k); }
            slots = std::max(slots, ops[j].size());
        }
        int64_t H = tmpN((int)(slots * n)), acc = tmpN(n);
        std::vector<std::pair<int64_t, int64_t>> written;
        auto gen_one = [&](size_t j) {                 // statement i + j, its operations ready in H
            const Stmt& s = *v[i + j];
            next_is_cont = !ops[j].empty();
            gen_stmt(s);
            for (const Expr* x : ops[j]) {             // not read after all: their cells are cleared
                auto it = precomputed.find(x);
                if (it != precomputed.end()) { clearN(it->second, n); precomputed.erase(it); }
            }
            if (s.kind == Stmt::Decl) { const Var* dv = find_var(s.name); written.emplace_back(dv->cell, dv->cell + dv->size()); }
            else {
                LV lv = lvalue(*s.target);
                if (lv.kind == LV::Cells) written.emplace_back(lv.cell, lv.cell + lv.size());
                else if (lv.kind == LV::Elem) written.emplace_back(lv.arr->cell, lv.arr->cell + array_total(*lv.arr));
            }
        };
        SharedHooks h;
        h.written = &written;
        h.at = v[i].get();
        h.fresh = true;
        h.before = [&](int o) { begin_stmt(*v[i + stmt_of[(size_t)o]], slot_of[(size_t)o] > 0); };
        h.loaded = [&](int) { end_stmt(); };
        h.after = [&](int o) {
            size_t j = stmt_of[(size_t)o];
            int64_t cells = H + (int64_t)slot_of[(size_t)o] * n;
            begin_stmt(*v[i + j], true);
            moveN(acc, cells, n);
            end_stmt();
            precomputed[flat[(size_t)o].e] = cells;
            if (slot_of[(size_t)o] + 1 < ops[j].size()) return;
            gen_one(j);                                // the statement's last operation: the statement
            for (size_t g = j + 1; g < ops.size() && ops[g].empty(); g++) gen_one(g);    // and those after it without one
        };
        run_shared(kd, flat, acc, h);
        releaseN(acc, n);
        releaseN(H, (int)(slots * n));
    }

    // The terms of an n-byte sum that use one routine at least share_min() times, by routine, in loops of
    // at most SHARE_MAX of them (as even as possible); they are taken out of terms.
    std::vector<std::pair<SharedKind, std::vector<SharedOp>>> shared_terms(Terms& terms, int n) {
        std::vector<std::pair<SharedKind, std::vector<SharedOp>>> groups, cut;
        if (!share_min()) return groups;
        for (auto& tm : terms) if (!movable(*tm.first)) return groups;     // terms are evaluated in another order
        for (auto& tm : terms) if (auto kd = shared_kind(*tm.first, n)) {
            auto it = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == *kd; });
            if (it == groups.end()) groups.push_back({*kd, {}}), it = groups.end() - 1;
            it->second.push_back({tm.first, tm.second});
        }
        for (auto& g : groups) {
            size_t total = g.second.size();
            if ((int)total < share_min()) continue;
            size_t parts = (total + SHARE_MAX - 1) / SHARE_MAX, from = 0;
            for (size_t p = 0; p < parts; p++) {
                size_t to = total * (p + 1) / parts;   // more than SHARE_MAX / 2 in each when cut
                cut.push_back({g.first, std::vector<SharedOp>(g.second.begin() + (std::ptrdiff_t)from, g.second.begin() + (std::ptrdiff_t)to)});
                from = to;
            }
        }
        groups = cut;
        Terms rest;
        for (auto& tm : terms) {
            bool taken = false;
            for (auto& g : groups) for (SharedOp& o : g.second) if (o.e == tm.first) taken = true;
            if (!taken) rest.push_back(tm);
        }
        terms = rest;
        return groups;
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
                if (v.count >= 0) { for (int b = 0; b < v.size(); b++) for (int k = 0; k < v.count; k++) clear(array_cell(v.cell, k, b, v.size())); }
                else clearN(v.cell, v.size());
            }
            next_free = s.mark;
        }
        scopes.pop_back();
    }
    void declare(const Var& v, Pos p) {
        Scope& s = scopes.back();
        if (s.vars.count(v.name)) fail(p, "'" + v.name + "' is already declared in this scope");
        s.vars[v.name] = v;
        s.order.push_back(v.name);
    }
    ProcInfo* find_proc(const std::string& n) { auto it = procs.find(n); return it == procs.end() ? nullptr : &it->second; }
    void add_symbol(const std::string& name, int64_t cell, const Type* t, int count, const std::string& scope) {
        if (!mapping() || !t) return;
        int es = std::max(t->size, 1);
        if (count >= 0) {
            res.symbols.push_back(Symbol{name, cell, (int)array_cells(count, es), 'a', count, scope, es, t->is_signed});
            return;
        }
        if (t->kind == TK::Struct) {
            for (const Field& f : t->st->fields) {
                if (f.type->kind == TK::Array) continue;
                add_symbol(name + "." + f.name, cell + f.offset, f.type, -1, scope);
            }
            return;
        }
        char kind = es == 1 ? 'b' : es == 2 ? (t->is_ptr() ? 'p' : 'w') : 'l';
        res.symbols.push_back(Symbol{name, cell, es, kind, 1, scope, es, t->is_signed});
    }

    // ---- constant folding with known cell values ---------------------------------------------
    const Type* conv_type(const Type* t) const { return t->is_ptr() ? T->u16() : t; }
    std::optional<int64_t> known_int(int64_t c, const Type* t) {
        int64_t v = 0;
        for (int k = 0; k < t->size; k++) {
            if (!trackable(c + k)) return std::nullopt;
            auto b = known_val(c + k);
            if (!b) return std::nullopt;
            v |= (int64_t)*b << (8 * k);
        }
        return wrap(v, t);
    }
    // Compile-time value of e in its own type.  With use_known, variables whose
    // cells hold a known constant count too.
    std::optional<int64_t> fold(const Expr& e, bool use_known = false) {
        const Type* t = e.type;
        if (!precomputed.empty() && precomputed.count(&e)) return std::nullopt;
        if (!t || !t->is_scalar()) return std::nullopt;
        if (e.is_const) return wrap(e.cval, conv_type(t));
        switch (e.kind) {
        case Expr::Name: {
            Var* v = find_var(e.text);
            // a parameter bound to a constant argument (call_user)
            if (v && v->is_const && v->count < 0 && v->type && v->type->is_scalar() && !v->type->is_ptr()) return wrap(v->cval, conv_type(v->type));
            if (!use_known) return std::nullopt;
            if (!v || v->is_const || v->count >= 0 || !v->type || !v->type->is_scalar()) return std::nullopt;
            return known_int(v->cell, conv_type(v->type));
        }
        case Expr::Index: {
            if (!use_known || e.a->kind != Expr::Name) return std::nullopt;
            Var* v = find_var(e.a->text);
            if (!v || !v->cell_array() || v->size() != 1) return std::nullopt;
            auto i = fold(*e.b, true);
            if (!i || *i < 0 || *i >= v->count) return std::nullopt;
            auto b = known_val(array_cell(v->cell, (int)*i));
            if (!b) return std::nullopt;
            return wrap(*b, t);
        }
        case Expr::Cast: {
            auto a = fold(*e.a, use_known);
            if (!a) return std::nullopt;
            return wrap(*a, conv_type(t));
        }
        case Expr::Unary: {
            auto a = fold(*e.a, use_known);
            if (!a) return std::nullopt;
            if (e.text == "!") return *a == 0 ? 1 : 0;
            const Type* at = conv_type(t);
            if (e.text == "-") return wrap(-*a, at);
            if (e.text == "~") return wrap(~*a, at);
            return std::nullopt;
        }
        case Expr::Binary: {
            const std::string& o = e.text;
            if (o == "&&" || o == "||") {
                auto a = fold(*e.a, use_known), b = fold(*e.b, use_known);
                if (!a || !b) return std::nullopt;
                return o == "&&" ? ((*a && *b) ? 1 : 0) : ((*a || *b) ? 1 : 0);
            }
            if (t->is_ptr() || (e.a->type && e.a->type->is_ptr()) || (e.b->type && e.b->type->is_ptr())) return std::nullopt;
            const Type* ot = conv_type(e.optype ? e.optype : t);
            auto a = fold(*e.a, use_known), b = fold(*e.b, use_known);
            if (!a || !b) return std::nullopt;
            bool shift = o == "<<" || o == ">>";
            int64_t x = wrap(*a, ot), y = shift ? *b : wrap(*b, ot);     // a shift count keeps its own type
            if (o == "==") return x == y;
            if (o == "!=") return x != y;
            if (o == "<") return x < y;
            if (o == "<=") return x <= y;
            if (o == ">") return x > y;
            if (o == ">=") return x >= y;
            const Type* rt = conv_type(t);
            if (o == "+") return wrap(x + y, rt);
            if (o == "-") return wrap(x - y, rt);
            if (o == "*") return wrap((int64_t)((uint64_t)x * (uint64_t)y), rt);
            if (o == "/") return y ? wrap(x / y, rt) : 0;
            if (o == "%") return y ? wrap(x % y, rt) : wrap(x, rt);
            if (o == "<<") return (y >= 32 || y < 0) ? 0 : wrap(x << y, rt);
            if (o == ">>") return (y >= 32 || y < 0) ? (x < 0 ? wrap(-1, rt) : 0) : wrap(x >> y, rt);
            if (o == "&") return wrap(x & y, rt);
            if (o == "|") return wrap(x | y, rt);
            if (o == "^") return wrap(x ^ y, rt);
            return std::nullopt;
        }
        case Expr::MulShr: {
            auto a = fold(*e.a, use_known), b = fold(*e.b, use_known);
            if (!a || !b) return std::nullopt;
            return mulshr_at(*a, *b, e.value, conv_type(e.optype ? e.optype : t));
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
                int64_t n = v->count >= 0 ? array_total(*v) : v->size();
                return overlap(v->cell, v->cell + n);
            }
            if (const bf::RegisterInfo* r = find_reg(e.text)) return overlap(r->addr, r->addr + 1);
            return false;
        }
        case Expr::Index: return touches(*e.a, lo, hi) || touches(*e.b, lo, hi);
        case Expr::Field: case Expr::Deref: case Expr::AddrOf: case Expr::Unary: case Expr::Cast: return touches(*e.a, lo, hi);
        case Expr::Binary: case Expr::MulShr: return touches(*e.a, lo, hi) || touches(*e.b, lo, hi);
        case Expr::New: case Expr::Call: return true;      // a procedure body may touch anything
        default: return false;
        }
    }

    const bf::RegisterInfo* bare_reg(const Expr& e) {
        if (e.kind != Expr::Name || find_var(e.text)) return nullptr;
        return find_reg(e.text);
    }

    // ---- locations ------------------------------------------------------------------------------------
    LV lvalue(const Expr& t) {
        LV lv;
        lv.type = t.type;
        switch (t.kind) {
        case Expr::Name: {
            if (Var* v = find_var(t.text)) {
                if (v->is_const) fail(t.pos, "'" + t.text + "' is a constant");
                if (v->count >= 0) fail(t.pos, "array '" + t.text + "' needs an index");
                lv.kind = LV::Cells; lv.cell = v->cell;
                return lv;
            }
            if (const bf::RegisterInfo* r = find_reg(t.text)) { lv.kind = LV::Reg; lv.cell = r->addr; lv.reg = r; return lv; }
            fail(t.pos, "'" + t.text + "' is not declared");
        }
        case Expr::Index: {
            const Expr& base = *t.a;
            if (base.kind == Expr::Name) {
                Var* v = find_var(base.text);
                if (v && v->arr_ptr) {
                    lv.kind = LV::Heap; lv.ptr_cell = v->cell;
                    int es = std::max(t.type->size, 1);
                    if (auto i = fold(*t.b)) lv.base = *i * es; else lv.hidx.emplace_back(t.b.get(), es);
                    return lv;
                }
                if (v && v->cell_array()) {
                    lv.kind = LV::Elem; lv.arr = v;
                    if (auto i = fold(*t.b)) {
                        if (*i < 0 || *i >= v->count) fail(t.b->pos, "index " + std::to_string(*i) + " is outside '" + base.text + "[" + std::to_string(v->count) + "]'");
                        lv.cidx = *i;
                    } else lv.index = t.b.get();
                    return lv;
                }
            }
            if (base.type && base.type->is_ptr()) {
                lv.kind = LV::Heap;
                int es = std::max(t.type->size, 1);
                if (auto p = fold(base)) lv.base = *p; else lv.ptr = &base;
                if (auto i = fold(*t.b)) lv.base += *i * es; else lv.hidx.emplace_back(t.b.get(), es);
                return lv;
            }
            // an array field of a struct: the element sits at a constant or (in the heap) runtime offset
            LV b = lvalue(base);
            int es = std::max(t.type->size, 1);
            auto i = fold(*t.b);
            if (b.kind == LV::Heap) {
                if (i) b.base += *i * es;
                else b.hidx.emplace_back(t.b.get(), es);
            } else {
                if (!i) fail(t.b->pos, "an array field of a struct held in cells needs a constant index");
                if (b.kind == LV::Cells) b.cell += *i * es; else if (b.kind == LV::Elem) b.boff += (int)(*i * es);
                else fail(t.pos, "cannot index this");
            }
            b.type = t.type;
            return b;
        }
        case Expr::Field: {
            LV b;
            const Type* st;
            if (t.arrow) {
                b.kind = LV::Heap;
                if (auto p = fold(*t.a)) b.base = *p; else b.ptr = t.a.get();
                st = t.a->type->elem;
            } else {
                b = lvalue(*t.a);
                st = t.a->type;
            }
            const Field* f = st->st->field(t.text);
            if (!f) internal("unknown field");
            if (b.kind == LV::Cells) b.cell += f->offset;
            else if (b.kind == LV::Elem) b.boff += f->offset;
            else if (b.kind == LV::Heap) b.base += f->offset;
            else fail(t.pos, "a register has no fields");
            b.type = t.type;
            return b;
        }
        case Expr::Deref: {
            lv.kind = LV::Heap;
            if (auto p = fold(*t.a)) lv.base = *p; else lv.ptr = t.a.get();
            return lv;
        }
        default: fail(t.pos, "this cannot be assigned to");
        }
    }
    bool lv_touched(const LV& lv, const Expr& e) {
        switch (lv.kind) {
        case LV::Cells: case LV::Reg: return touches(e, lv.cell, lv.cell + lv.size());
        case LV::Elem: return touches(e, lv.arr->cell, lv.arr->cell + array_total(*lv.arr));
        case LV::Heap: return true;
        }
        return true;
    }
    int64_t elem_cell(const LV& lv, int k) const { return array_cell(lv.arr->cell, (int)lv.cidx, lv.boff + k, lv.arr->size()); }
    // Heap address of a location into dst (2 cells, known zero).
    void eval_address(const LV& lv, int64_t dst) {
        if (lv.ptr_cell >= 0) copyN(lv.ptr_cell, dst, 2);
        else if (lv.ptr) eval_into(*lv.ptr, dst, T->u16());
        for (auto& [idx, es] : lv.hidx) {
            int64_t t = tmp2();
            eval_into(*idx, t, T->u16());
            if (es == 1) addN(dst, t, 2);
            else { int64_t m = tmp2(); mul_constN(t, es, m, 2); addN(dst, m, 2); release2(m); }
            release2(t);
        }
        if (lv.base) addN_const(dst, 2, lv.base);
    }
    // Compute the runtime part of a location (an index into a cell array, a heap address) once, into
    // temporaries, so that it stays the same place while code in between changes what it reads.  Returns
    // the cells to clear once the location is no longer used, and how many.
    std::pair<int64_t, int> pin(LV& lv) {
        if (lv.kind == LV::Elem && lv.index) {
            lv.icell = tmp(); eval8(*lv.index, lv.icell); lv.index = nullptr;
            return {lv.icell, 1};
        }
        if (lv.kind == LV::Heap && !lv.const_addr()) {
            int64_t a = tmp2(); eval_address(lv, a);
            lv.ptr = nullptr; lv.ptr_cell = a; lv.hidx.clear(); lv.base = 0;
            return {a, 2};
        }
        return {-1, 0};
    }
    // dst (n cells, known zero) = the value at lv
    void read_lv(const LV& lv, int64_t dst) {
        int n = lv.size();
        switch (lv.kind) {
        case LV::Cells: for (int k = 0; k < n; k++) copy(lv.cell + k, dst + k); return;
        case LV::Reg:
            if (lv.reg->kind == bf::REG_TRIGGER) internal("read of a write-only register");
            if (lv.reg->kind == bf::REG_LATCH) copy(lv.reg->addr, dst); else test_reg(lv.reg->addr, dst);
            return;
        case LV::Elem: {
            const Var& v = *lv.arr;
            if (!lv.index && lv.icell < 0) { for (int k = 0; k < n; k++) copy(elem_cell(lv, k), dst + k); return; }
            int64_t i = tmp();
            if (lv.icell >= 0) copy(lv.icell, i); else eval8(*lv.index, i);
            elem_read(v.cell, v.size(), lv.boff, n, i, dst);
            drop(i);
            return;
        }
        case LV::Heap: {
            if (lv.const_addr()) { for (int k = 0; k < n; k++) copy(heap_cell(lv.base + k, HD), dst + k); return; }
            int64_t a = tmp2();
            eval_address(lv, a);
            for (int k = 0; k < n; k++) {
                int64_t c = tmp2();
                copyN(a, c, 2);
                addN_const(c, 2, k);
                heap_walk(c, -1, dst + k, false);
                drop(c, 2);
            }
            release2(a);
            return;
        }
        }
    }
    // the value in src (n cells, consumed) is stored at lv
    void write_lv(const LV& lv, int64_t val) {
        int n = lv.size();
        switch (lv.kind) {
        case LV::Cells: for (int k = 0; k < n; k++) { clear(lv.cell + k); move(val + k, {{lv.cell + k, 1}}); } return;
        case LV::Reg: clear(lv.cell); move(val, {{lv.cell, 1}}); return;
        case LV::Elem: {
            const Var& v = *lv.arr;
            if (!lv.index && lv.icell < 0) { for (int k = 0; k < n; k++) { int64_t c = elem_cell(lv, k); clear(c); move(val + k, {{c, 1}}); } return; }
            int64_t i = tmp();
            if (lv.icell >= 0) copy(lv.icell, i); else eval8(*lv.index, i);
            elem_write(v.cell, v.size(), lv.boff, n, i, val);
            drop(i);
            return;
        }
        case LV::Heap: {
            if (lv.const_addr()) { for (int k = 0; k < n; k++) { int64_t c = heap_cell(lv.base + k, HD); clear(c); move(val + k, {{c, 1}}); } return; }
            int64_t a = tmp2();
            eval_address(lv, a);
            for (int k = 0; k < n; k++) {
                int64_t c = tmp2();
                copyN(a, c, 2);
                addN_const(c, 2, k);
                heap_walk(c, val + k, -1, true);
                drop(c, 2);
            }
            release2(a);
            return;
        }
        }
    }

    // ---- expressions: byte path -------------------------------------------------------------------
    // Flatten a chain of + and - (and unary -) on integers into signed terms plus one constant,
    // for a sum `width` bytes wide.  A narrower part of the chain wraps at its own width before it is
    // widened, so it stays one term.
    void collect_terms(const Expr& e, int sign, std::vector<std::pair<const Expr*, int>>& terms, int64_t& constant, int width = 1) {
        if (auto v = fold(e, true)) { constant += sign * *v; return; }
        bool wide_enough = sz(e) >= width;
        if (wide_enough && e.kind == Expr::Binary && (e.text == "+" || e.text == "-") && e.type && e.type->is_int()
            && e.a->type && e.a->type->is_int() && e.b->type && e.b->type->is_int()) {
            collect_terms(*e.a, sign, terms, constant, width);
            collect_terms(*e.b, e.text == "+" ? sign : -sign, terms, constant, width);
            return;
        }
        if (wide_enough && e.kind == Expr::Unary && e.text == "-") { collect_terms(*e.a, -sign, terms, constant, width); return; }
        terms.emplace_back(&e, sign);
    }
    static bool is_cmp(const std::string& o) { return o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">="; }
    static int sz(const Expr& e) { return e.type ? std::max(e.type->size, 1) : 1; }

    // dst += sign * (low byte of e)
    void eval8(const Expr& e, int64_t dst, int sign = 1) {
        if (auto v = fold(e, true)) { add(dst, sign * (*v & 255)); return; }
        switch (e.kind) {
        case Expr::Num: case Expr::Null: case Expr::Sizeof: case Expr::Str: return;    // folded
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
            // byte arrays in cells: the direct path
            if (e.a->kind == Expr::Name) {
                Var* v = find_var(e.a->text);
                if (v && v->cell_array() && v->size() == 1) {
                    const std::string& an = e.a->text;
                    if (auto i = fold(*e.b)) {
                        if (*i < 0 || *i >= v->count) fail(e.b->pos, "index " + std::to_string(*i) + " is outside '" + an + "[" + std::to_string(v->count) + "]'");
                        copy(array_cell(v->cell, (int)*i), dst, sign);
                        return;
                    }
                    int64_t i = tmp();
                    eval8(*e.b, i);
                    if (sign == 1) array_read(v->cell, i, dst);
                    else { int64_t t = tmp(); array_read(v->cell, i, t); move(t, {{dst, sign}}); release(t); }
                    drop(i);
                    return;
                }
            }
            break;
        }
        case Expr::In: {
            if (sign == 1 && known_is(dst, 0)) { go(dst); out += ','; forget(dst); return; }
            int64_t t = tmp();
            go(t); out += ','; forget(t);
            move(t, {{dst, sign}});
            release(t);
            return;
        }
        case Expr::Call: {
            if (sz(e) == 1) {
                std::vector<const Expr*> args; for (auto& a : e.args) args.push_back(a.get());
                if (sign == 1) { call_user(e.text, args, e.pos, dst); return; }
                int64_t t = tmp(); call_user(e.text, args, e.pos, t); move(t, {{dst, sign}}); release(t);
                return;
            }
            break;
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
            break;
        }
        case Expr::Cast:
            eval8(*e.a, dst, sign);     // the low byte survives any cast
            return;
        case Expr::Binary: {
            const std::string& o = e.text;
            const Expr& a = *e.a; const Expr& b = *e.b;
            const Type* ot = e.optype ? e.optype : e.type;
            if (is_cmp(o)) {
                if (ot->size == 1 && !ot->is_signed) { cmp8(o, a, b, dst, sign); return; }
                cmp_generic(o, a, b, ot, dst, sign);
                return;
            }
            if (o == "&&" || o == "||") { logic8(o, a, b, dst, sign); return; }
            if (e.type->is_ptr() || (a.type && a.type->is_ptr())) break;
            bool narrow = e.type->size == 1;
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
            if ((o == "/" || o == "%") && narrow && !e.type->is_signed) {
                int64_t t1 = tmp(); eval8(a, t1);
                int64_t t2 = tmp(); eval8(b, t2);
                int64_t q = tmp(), r = tmp();
                divmod8(t1, t2, q, r);
                move(o == "/" ? q : r, {{dst, sign}});
                release(r); release(q); release(t2); release(t1);
                return;
            }
            if (o == "<<") {
                if (auto k = fold(b, true)) {
                    int64_t t = tmp(); eval8(a, t);
                    if (*k >= 8 || *k < 0) clear(t); else for (int64_t i = 0; i < *k; i++) double8(t);
                    move(t, {{dst, sign}});
                    release(t);
                    return;
                }
                int64_t n = tmp(); eval_count(b, n);
                int64_t t = tmp(); eval8(a, t);
                open(n); add(n, -1); double8(t); close();
                move(t, {{dst, sign}});
                release(t); release(n);
                return;
            }
            if (o == ">>" && narrow && !e.type->is_signed) {
                if (auto k = fold(b, true)) {
                    int64_t t = tmp(); eval8(a, t);
                    if (*k >= 8 || *k < 0) clear(t); else for (int64_t i = 0; i < *k; i++) halve8(t);
                    move(t, {{dst, sign}});
                    release(t);
                    return;
                }
                int64_t n = tmp(); eval_count(b, n);
                int64_t t = tmp(); eval8(a, t);
                open(n); add(n, -1); halve8(t); close();
                move(t, {{dst, sign}});
                release(t); release(n);
                return;
            }
            if (o == "&" || o == "|" || o == "^") {
                int64_t ta = tmp(); eval8(a, ta);
                int64_t tb = tmp(); eval8(b, tb);
                bitwise_core(o[0], ta, tb, dst, sign);
                release(tb); release(ta);
                return;
            }
            break;
        }
        default: break;
        }
        // general case: the full value, then its low byte
        int n = sz(e);
        int64_t t = tmpN(n);
        eval_same(e, t);
        move(t, {{dst, sign}});
        if (n > 1) clearN(t + 1, n - 1);
        releaseN(t, n);
    }

    void logic8(const std::string& o, const Expr& a, const Expr& b, int64_t dst, int sign) {
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
        int64_t t = tmp(); eval_cond(a, t);
        int64_t f = tmp(); set(f, 1);
        if_nonzero(t, [&] { add(dst, sign); clear(f); });
        if_nonzero(f, [&] {
            int64_t t2 = tmp(); eval_cond(b, t2);
            if_nonzero(t2, [&] { add(dst, sign); });
            release(t2);
        });
        release(f); release(t);
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
    // comparison at type ot (any width, signed or not)
    void cmp_generic(const std::string& o, const Expr& a, const Expr& b, const Type* ot, int64_t dst, int sign) {
        int n = std::max(ot->size, 1);
        const Type* ct = conv_type(ot);
        int64_t ta = tmpN(n); eval_into(a, ta, ct);
        int64_t tb = tmpN(n); eval_into(b, tb, ct);
        if (o == "==") eqN_cells(ta, tb, n, dst, sign);
        else if (o == "!=") { int64_t t = tmp(); eqN_cells(ta, tb, n, t, 1); not8(t, dst, sign); release(t); }
        else {
            bool swap = o == ">" || o == "<=", neg = o == "<=" || o == ">=";
            int64_t x = swap ? tb : ta, y = swap ? ta : tb;
            auto lt = [&](int64_t d, int s) { if (ct->is_signed) sltN_cells(x, y, n, d, s); else ltN_cells(x, y, n, d, s); };
            if (!neg) lt(dst, sign);
            else { int64_t r = tmp(); lt(r, 1); not8(r, dst, sign); release(r); }
        }
        releaseN(tb, n); releaseN(ta, n);
    }

    // c += (e != 0) as 0/1 for wide e; for byte e the value itself (non-zero = true).
    // Registers are tested without being written.
    void eval_cond(const Expr& e, int64_t c) {
        if (auto v = fold(e, true)) { add(c, *v ? 1 : 0); return; }
        if (const bf::RegisterInfo* r = bare_reg(e)) {
            if (r->kind == bf::REG_TRIGGER) fail(e.pos, std::string(r->name) + " is write-only");
            test_reg(r->addr, c);
            return;
        }
        int n = sz(e);
        if (n > 1) {
            int64_t t = tmpN(n); eval_same(e, t);
            nonzeroN(t, c, n);
            releaseN(t, n);
            return;
        }
        eval8(e, c);
    }

    // ---- expressions: any width -------------------------------------------------------------------
    // dst (size(t) cells, known zero) = e converted to type t
    void eval_into(const Expr& e, int64_t dst, const Type* t) {
        int n = std::max(t->size, 1);
        if (t->kind == TK::Struct) { eval_same(e, dst); return; }
        if (n == 1) { eval8(e, dst); return; }
        if (auto v = fold(e, true)) { for (int k = 0; k < n; k++) add(dst + k, (*v >> (8 * k)) & 255); return; }
        int m = sz(e);
        if (m == n) { eval_same(e, dst); return; }
        if (m < n) {
            if (m == 1) eval8(e, dst); else eval_same(e, dst);
            if (e.type->is_signed) {
                int64_t f = tmp();
                sign_flag(dst + m - 1, f);
                if_nonzero(f, [&] { for (int k = m; k < n; k++) add(dst + k, 255); });
                release(f);
            }
            return;
        }
        // narrowing: the low bytes
        int64_t w = tmpN(m);
        eval_same(e, w);
        moveN(w, dst, n);
        clearN(w + n, m - n);
        releaseN(w, m);
    }

    // A field or element of a struct returned by a call, reached through fields and constant indices
    // (mk().x, mk().p.d[2]): the call, with the part's byte offset in off; null when e is not one.
    const Expr* returned_part(const Expr& e, int64_t& off) {
        off = 0;
        const Expr* x = &e;
        for (;; x = x->a.get()) {
            if (x->kind == Expr::Field && !x->arrow) off += x->a->type->st->field(x->text)->offset;
            else if (x->kind == Expr::Index && x->a->type && x->a->type->kind == TK::Array && x->a->kind != Expr::Name) {
                auto i = fold(*x->b);
                if (!i) return nullptr;
                off += *i * std::max(x->type->size, 1);
            } else break;
        }
        return x != &e && x->kind == Expr::Call ? x : nullptr;
    }

    // dst (size(e.type) cells, known zero) = e
    void eval_same(const Expr& e, int64_t dst) {
        int n = sz(e);
        if (!precomputed.empty()) {                    // computed by a shared routine (gen_run)
            auto it = precomputed.find(&e);
            if (it != precomputed.end()) { moveN(it->second, dst, n); precomputed.erase(it); return; }
        }
        if (auto v = fold(e, true)) { for (int k = 0; k < n; k++) add(dst + k, (*v >> (8 * k)) & 255); return; }
        switch (e.kind) {
        case Expr::Name: {
            if (Var* v = find_var(e.text)) {
                if (v->count >= 0) fail(e.pos, "array '" + e.text + "' needs an index");
                for (int k = 0; k < n; k++) copy(v->cell + k, dst + k);
                return;
            }
            if (n == 1) { eval8(e, dst); return; }      // a register
            fail(e.pos, "'" + e.text + "' is not declared");
        }
        case Expr::Field: case Expr::Index: {
            int64_t off;
            if (const Expr* c = returned_part(e, off)) {         // a part of a returned struct
                int bn = sz(*c);
                int64_t t = tmpN(bn);
                eval_same(*c, t);
                for (int k = 0; k < n; k++) move(t + off + k, {{dst + k, 1}});
                clearN(t, bn);
                releaseN(t, bn);
                return;
            }
            read_lv(lvalue(e), dst);
            return;
        }
        case Expr::Deref:
            read_lv(lvalue(e), dst);
            return;
        case Expr::AddrOf:
            eval_address(lvalue(*e.a), dst);
            return;
        case Expr::In: eval8(e, dst); return;
        case Expr::Input: {
            Var w; w.name = "w"; w.cell = dst; w.type = T->u16(); w.owned = false;
            call_proc("__read_word", {w}, e.pos, -1);
            return;
        }
        case Expr::Call: {
            std::vector<const Expr*> args; for (auto& a : e.args) args.push_back(a.get());
            call_user(e.text, args, e.pos, dst);
            return;
        }
        case Expr::New: {
            // alloc(count * sizeof(T)) as a T*
            int es = std::max(e.type->elem->size, 1);
            int64_t bytes = tmp2();
            if (e.b) {
                int64_t c = tmp2(); eval_into(*e.b, c, T->u16());
                if (es == 1) addN(bytes, c, 2); else mul_constN(c, es, bytes, 2);
                release2(c);
            } else addN_const(bytes, 2, es);
            Var v; v.name = "n"; v.cell = bytes; v.type = T->u16(); v.owned = false;
            call_proc_values("alloc", {v}, e.pos, dst);
            release2(bytes);
            return;
        }
        case Expr::Cast: eval_into(*e.a, dst, e.type); return;
        case Expr::Unary: {
            if (e.text == "!") { int64_t t = tmp(); eval_cond(*e.a, t); not8(t, dst, 1); release(t); return; }
            eval_into(*e.a, dst, e.type);
            if (e.text == "-") negN(dst, n); else notN(dst, n);
            return;
        }
        case Expr::Binary:
            if (is_cmp(e.text) || e.text == "&&" || e.text == "||") { eval8(e, dst); return; }
            binary_wide(e, dst);
            return;
        case Expr::MulShr: mulshr(*e.a, *e.b, (int)e.value, conv_type(e.type), dst); return;
        default: fail(e.pos, "cannot evaluate this expression here");
        }
    }

    // arithmetic at any width (also 1 byte for the signed operations eval8 leaves out)
    void binary_wide(const Expr& e, int64_t dst) {
        const std::string& o = e.text;
        const Expr& a = *e.a; const Expr& b = *e.b;
        int n = sz(e);
        const Type* t = conv_type(e.type);
        if (e.type->is_ptr()) {          // pointer +/- integer: scaled
            const Expr& p = a.type->is_ptr() ? a : b;
            const Expr& i = a.type->is_ptr() ? b : a;
            int es = std::max(e.type->elem->size, 1);
            eval_into(p, dst, T->u16());
            int64_t ti = tmp2(); eval_into(i, ti, T->u16());
            int64_t m = ti;
            if (es != 1) { m = tmp2(); mul_constN(ti, es, m, 2); }
            if (o == "+") addN(dst, m, 2); else subN(dst, m, 2);
            if (es != 1) release2(m);
            release2(ti);
            return;
        }
        if (o == "-" && a.type->is_ptr() && b.type->is_ptr()) {     // pointer difference in elements
            int es = std::max(a.type->elem->size, 1);
            int64_t x = tmp2(); eval_into(a, x, T->u16());
            int64_t y = tmp2(); eval_into(b, y, T->u16());
            subN(x, y, 2);
            if (es == 1) moveN(x, dst, 2);
            else { int64_t d = tmp2(); setN(d, 2, es); int64_t r = tmp2(); sdivmodN(x, d, dst, r, 2); release2(r); release2(d); }
            release2(y); release2(x);
            return;
        }
        if (o == "+" || o == "-") {
            std::vector<std::pair<const Expr*, int>> terms; int64_t constant = 0;
            collect_terms(e, 1, terms, constant, n);
            auto groups = shared_terms(terms, n);
            for (auto& tm : terms) {
                if (known0N(dst, n) && tm.second > 0) { eval_into(*tm.first, dst, t); continue; }
                int64_t x = tmpN(n); eval_into(*tm.first, x, t);
                if (tm.second > 0) addN(dst, x, n); else subN(dst, x, n);
                releaseN(x, n);
            }
            addN_const(dst, n, constant);
            for (auto& g : groups) run_shared(g.first, g.second, dst);
            return;
        }
        if (o == "*") {
            const Expr* var = &a; std::optional<int64_t> k = fold(b, true);
            if (!k) { k = fold(a, true); if (k) var = &b; }
            if (k) { int64_t x = tmpN(n); eval_into(*var, x, t); mul_constN(x, *k, dst, n); releaseN(x, n); return; }
            int64_t x = tmpN(n); eval_into(a, x, t);
            int64_t y = tmpN(n); eval_into(b, y, t);
            mulN(x, y, dst, n);
            releaseN(y, n); releaseN(x, n);
            return;
        }
        if (o == "/" || o == "%") {
            int64_t x = tmpN(n); eval_into(a, x, t);
            int64_t y = tmpN(n); eval_into(b, y, t);
            int64_t q = tmpN(n), r = tmpN(n);
            if (t->is_signed) sdivmodN(x, y, q, r, n); else divmodN(x, y, q, r, n);
            moveN(o == "/" ? q : r, dst, n);
            releaseN(r, n); releaseN(q, n); releaseN(y, n); releaseN(x, n);
            return;
        }
        if (o == "<<" || o == ">>") {
            bool left = o == "<<";
            if (auto k = fold(b, true)) {
                eval_into(a, dst, t);
                if (*k >= 8 * n || *k < 0) { if (left || !t->is_signed) clearN(dst, n); else shift_bytes(dst, n, n, false, true); }
                else {
                    int m = (int)(*k / 8);                       // whole bytes first, then single bits
                    if (m) shift_bytes(dst, n, m, left, t->is_signed);
                    for (int64_t i = 0; i < *k % 8; i++) { if (left) shlN(dst, n); else shrN(dst, n, t->is_signed); }
                }
                return;
            }
            int64_t c = tmp(); eval_count(b, c);
            eval_into(a, dst, t);
            open(c); add(c, -1); if (left) shlN(dst, n); else shrN(dst, n, t->is_signed); close();
            release(c);
            return;
        }
        if (o == "&" || o == "|" || o == "^") {
            int64_t x = tmpN(n); eval_into(a, x, t);
            int64_t y = tmpN(n); eval_into(b, y, t);
            for (int k = 0; k < n; k++) bitwise_core(o[0], x + k, y + k, dst + k, 1);
            releaseN(y, n); releaseN(x, n);
            return;
        }
        fail(e.pos, "unsupported operator '" + o + "'");
    }

    // ---- statement map ----------------------------------------------------------------------------------
    bool mapping() const { return prelude_depth == 0; }
    bool next_is_cont = false;      // the next statement continues one split at a call
    void begin_stmt(const Stmt& s, bool cont = false) {
        if (!mapping() || s.pos.file < 0 || s.pos.file >= (int)texts.size()) return;
        const std::string& src = texts[(size_t)s.pos.file];
        int line = s.pos.line;
        size_t a = (size_t)s.pos.off, b = a;
        while (b < src.size() && src[b] != '\n' && b < (size_t)s.end.off) b++;
        std::string text = a < src.size() ? src.substr(a, b - a) : std::string();
        for (char& c : text) if (strchr("+-<>.,[]", c)) c = '_';
        while (!text.empty() && isspace((unsigned char)text.back())) text.pop_back();
        out += '\n';
        out.append(stmt_stack.size() * 2, ' ');
        std::string where = s.pos.file > 0 ? std::filesystem::path(file_names[(size_t)s.pos.file]).filename().string() + " " : std::string();
        for (char& c : where) if (strchr("+-<>.,[]", c)) c = '_';
        out += "# " + where + "L" + std::to_string(line) + (cont ? " (continued): " : ": ") + text + "\n";
        out.append(stmt_stack.size() * 2, ' ');
        StmtRange r{line, s.pos.col, line_of(s.pos.file, std::max(s.pos.off, s.end.off - 1)), (int32_t)out.size(), 0, (int)stmt_stack.size(), cont, s.pos.file};
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
    std::unordered_map<const Stmt*, int64_t> prealloc;     // cells reserved for declarations after an early exit
    void gen_stmts(const std::vector<StmtP>& v, size_t from = 0) {
        for (size_t i = from; i < v.size(); i++) {
            SharedKind kd;
            std::vector<std::vector<const Expr*>> ops;
            if (size_t len = find_run(v, i, kd, ops)) { gen_run(v, i, len, kd, ops); i += len - 1; continue; }
            gen_stmt(*v[i]);
            if (i + 1 < v.size() && !ctxs.empty() && ctxs.back().needs && may_exit(*v[i])) {
                // the rest runs only while the context is alive; its variables belong to
                // this scope, so their cells are reserved now, below the test's temporary
                for (size_t j = i + 1; j < v.size(); j++) {
                    const Stmt& d = *v[j];
                    if (d.kind != Stmt::Decl || d.in_heap || prealloc.count(&d)) continue;
                    Var tv; tv.type = d.vtype; tv.count = d.count;
                    prealloc[&d] = alloc((int)(d.count >= 0 ? array_total(tv) : tv.size()));
                }
                int64_t t = tmp();
                copy(ctxs.back().alive, t);
                if_nonzero(t, [&] { gen_stmts(v, i + 1); });
                release(t);
                return;
            }
        }
    }
    void gen_scope(const std::vector<StmtP>& v) { push_scope(); gen_stmts(v); pop_scope(); }

    static void copy_info(Expr& c, const Expr& e) {
        c.kind = e.kind; c.pos = e.pos; c.value = e.value; c.text = e.text; c.arrow = e.arrow; c.ts = e.ts;
        c.type = e.type; c.optype = e.optype; c.is_const = e.is_const; c.cval = e.cval;
    }
    ExprP clone(const Expr& e) {
        auto c = std::make_unique<Expr>();
        copy_info(*c, e);
        if (e.a) c->a = clone(*e.a);
        if (e.b) c->b = clone(*e.b);
        for (auto& x : e.args) c->args.push_back(clone(*x));
        return c;
    }
    // A typed binary expression built by the compiler (compound assignment).
    ExprP make_binary(const std::string& op, ExprP a, ExprP b, Pos p) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Binary; e->pos = p; e->text = op;
        const Type* ta = a->type;
        e->type = ta;
        e->optype = ta->is_ptr() ? T->u16() : ta;
        e->a = std::move(a); e->b = std::move(b);
        return e;
    }

    void gen_assign(const Stmt& s) {
        LV lv = lvalue(*s.target);
        const Expr& e = *s.e;
        std::string op = s.op;
        int n = lv.size();
        bool simple_target = lv.kind == LV::Cells || lv.kind == LV::Reg || (lv.kind == LV::Elem && !lv.index && n == 1);
        if (lv.kind == LV::Elem && lv.index && n == 1 && lv.type->kind != TK::Struct && op != "=") simple_target = false;
        if ((op != "=" && op != "+=" && op != "-=") || (op != "=" && !simple_target)) {
            // x op= e  ->  x = x op e
            Stmt t; t.kind = Stmt::Assign; t.op = "="; t.pos = s.pos; t.end = s.end; t.target = clone(*s.target);
            t.e = make_binary(op.substr(0, op.size() - 1), clone(*s.target), clone(e), e.pos);
            gen_assign(t);
            return;
        }
        if (lv.kind == LV::Reg) { gen_assign_reg(lv, op, e, s.pos); return; }
        if (lv.type->is_ptr() && op != "=") {      // p += n, p -= n (scaled)
            int es = std::max(lv.type->elem->size, 1);
            if (auto k = fold(e, true)) { addN_const(lv.cell, 2, (op == "+=" ? 1 : -1) * *k * es); return; }
            int64_t t = tmp2(); eval_into(e, t, T->u16());
            int64_t m = t;
            if (es != 1) { m = tmp2(); mul_constN(t, es, m, 2); }
            if (op == "+=") addN(lv.cell, m, 2); else subN(lv.cell, m, 2);
            if (es != 1) release2(m);
            release2(t);
            return;
        }
        if (n == 1 && lv.kind != LV::Heap && lv.type->kind != TK::Struct) {
            int64_t cell = lv.kind == LV::Cells ? lv.cell : (!lv.index ? elem_cell(lv, 0) : -1);
            if (cell >= 0) {
                if (op == "=") {
                    if (e.kind == Expr::In) { go(cell); out += ','; forget(cell); return; }   // ',' straight into the cell: unchanged at end of input
                    if (auto k = fold(e, true)) { set(cell, *k); return; }
                    if (!lv_touched(lv, e)) { clear(cell); eval8(e, cell); return; }
                    int64_t t = tmp(); eval8(e, t); clear(cell); move(t, {{cell, 1}}); release(t);
                    return;
                }
                int sign = op == "+=" ? 1 : -1;
                if (fold(e, true) || !lv_touched(lv, e)) { eval8(e, cell, sign); return; }
                int64_t t = tmp(); eval8(e, t); move(t, {{cell, sign}}); release(t);
                return;
            }
            // runtime index into a byte array
            int64_t t = tmp(); eval8(e, t);
            int64_t i = tmp(); eval8(*lv.index, i);
            elem_write(lv.arr->cell, lv.arr->size(), lv.boff, 1, i, t);
            drop(i); drop(t);
            return;
        }
        if (lv.kind == LV::Cells && lv.type->kind != TK::Struct) {
            if (op == "=") {
                if (auto k = fold(e, true)) { setN(lv.cell, n, *k); return; }
                if (!lv_touched(lv, e)) { clearN(lv.cell, n); eval_into(e, lv.cell, lv.type); return; }
                int64_t t = tmpN(n); eval_into(e, t, lv.type); clearN(lv.cell, n); moveN(t, lv.cell, n); releaseN(t, n);
                return;
            }
            if (auto k = fold(e, true)) { addN_const(lv.cell, n, op == "+=" ? *k : -*k); return; }
            int64_t t = tmpN(n); eval_into(e, t, lv.type);
            if (op == "+=") addN(lv.cell, t, n); else subN(lv.cell, t, n);
            releaseN(t, n);
            return;
        }
        // structs, wide array elements and heap locations: evaluate, then store
        int64_t t = tmpN(n);
        eval_into(e, t, lv.type);
        write_lv(lv, t);
        drop(t, n);
    }

    void gen_assign_reg(const LV& lv, const std::string& op, const Expr& e, Pos pos) {
        const bf::RegisterInfo& r = *lv.reg;
        auto k = fold(e);
        if (r.kind != bf::REG_LATCH) {
            if (op == "=") fail(pos, std::string(r.name) + " cannot be assigned: write it with 'pulse " + r.name + "' or '" + r.name + " += 1'");
            if (!k) fail(e.pos, std::string(r.name) + " only accepts a constant with += or -=");
            add(lv.cell, op == "+=" ? *k : -*k);
            return;
        }
        if (op == "=") {
            if (k) { set(lv.cell, *k); return; }
            int64_t t = tmp(); eval8(e, t); clear(lv.cell); move(t, {{lv.cell, 1}}); release(t);
            return;
        }
        int sign = op == "+=" ? 1 : -1;
        if (k) { add(lv.cell, sign * *k); return; }
        int64_t t = tmp(); eval8(e, t); move(t, {{lv.cell, sign}}); release(t);
    }

    void gen_incdec(const Stmt& s) {
        LV lv = lvalue(*s.target);
        int d = s.op == "++" ? 1 : -1;
        int n = lv.size();
        if (lv.kind == LV::Reg || (n == 1 && (lv.kind == LV::Cells || (lv.kind == LV::Elem && !lv.index)))) {
            int64_t cell = lv.kind == LV::Elem ? elem_cell(lv, 0) : lv.cell;
            add(cell, d);
            return;
        }
        if (lv.kind == LV::Cells) {
            if (lv.type->is_ptr()) { addN_const(lv.cell, 2, d * std::max(lv.type->elem->size, 1)); return; }
            if (d > 0) incN(lv.cell, n); else decN(lv.cell, n);
            return;
        }
        Stmt t; t.kind = Stmt::Assign; t.op = d > 0 ? "+=" : "-="; t.pos = s.pos; t.end = s.end; t.target = clone(*s.target);
        t.e = std::make_unique<Expr>(); t.e->kind = Expr::Num; t.e->value = 1; t.e->pos = s.pos;
        t.e->type = T->u8(); t.e->is_const = true; t.e->cval = 1;
        gen_assign(t);
    }

    void gen_decl(const Stmt& s) {
        Var v; v.name = s.name; v.type = s.vtype; v.count = s.count;
        if (s.in_heap) {                         // a heap array: a constant pointer
            v.is_const = true; v.cval = s.cval; v.owned = false;
            declare(v, s.pos);
            return;
        }
        if (scopes.size() == 1 && call_stack.empty()) place_group(s);
        auto pre = prealloc.find(&s);
        if (s.count >= 0) {
            v.cell = pre != prealloc.end() ? pre->second : alloc((int)array_total(v));
            if (pre != prealloc.end()) prealloc.erase(pre);
        } else {
            int n = v.size();
            v.cell = pre != prealloc.end() ? pre->second : alloc(n);
            if (pre != prealloc.end()) prealloc.erase(pre);
            if (s.e) {
                auto k = v.type->is_scalar() ? fold(*s.e) : std::nullopt;
                if (k) for (int b = 0; b < n; b++) add(v.cell + b, (*k >> (8 * b)) & 255);
                else eval_into(*s.e, v.cell, v.type);
            }
        }
        declare(v, s.pos);
        add_symbol(v.name, v.cell, v.type, v.count, cur_proc);
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
            if (step && !needs) gen_stmt(*step);
            else if (step) {
                // not after a break or a return (continue still steps)
                int64_t go_on = tmp(); set(go_on, 1);
                int64_t b = tmp(); copy(lc.brk, b); if_nonzero(b, [&] { clear(go_on); }); release(b);
                if_nonzero(go_on, [&] { gen_stmt(*step); });
                release(go_on);
            }
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
            int64_t off;
            if ((a.kind == Expr::Index || a.kind == Expr::Field || a.kind == Expr::Deref) && !returned_part(a, off)) {
                LV lv = lvalue(a);
                if (lv.kind == LV::Elem && !lv.index) { go(elem_cell(lv, 0)); out += '.'; continue; }
                if (lv.kind == LV::Cells) { go(lv.cell); out += '.'; continue; }
                if (lv.const_addr()) { go(heap_cell(lv.base, HD)); out += '.'; continue; }
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
            const Type* t = ap->type;
            const char* fn;
            if (t->is_ptr()) fn = "__print_str";
            else if (t->size == 1) fn = t->is_signed ? "__print_sbyte" : "__print_byte";
            else if (t->size == 2) fn = t->is_signed ? "__print_int" : "__print_word";
            else fn = t->is_signed ? "__print_long" : "__print_dword";
            call_user(fn, {ap.get()}, s.pos, -1);
        }
    }

    void pulse_reg(const std::string& name) {
        const bf::RegisterInfo* r = find_reg(name);
        if (!r) internal("no register " + name);
        go(r->addr); out += '+';
    }
    void gen_pulse(const std::string& name, Pos p) {
        if (find_var(name)) fail(p, "'" + name + "' is a variable, not a register");
        if (!find_reg(name)) fail(p, "'" + name + "' is not a register");
        pulse_reg(name);
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
        if (s.e) store_result(*s.e, pc.retval, pc.ret);
        unwind_to(p);
    }
    void store_result(const Expr& e, int64_t cell, const Type* t) {
        int n = std::max(t->size, 1);
        if (n == 1 && t->kind != TK::Struct) { clear(cell); eval8(e, cell); return; }
        clearN(cell, n);
        eval_into(e, cell, t);
    }

    // ---- calls --------------------------------------------------------------------------------------------
    // Inline a procedure with its parameters already bound to cells.  result_cell
    // receives the return value (added; -1 discards it).
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
        const Type* pret = ret_of(&proc);
        int rn = pret ? std::max(pret->size, 1) : 0;
        ExitCtx pc; pc.kind = ExitCtx::Proc; pc.ret = pret; pc.needs = contains_return(proc.body);
        int64_t mark = next_free;
        if (pret) pc.retval = tmpN(rn);
        if (pc.needs) { pc.alive = tmp(); set(pc.alive, 1); }
        ctxs.push_back(pc);
        // lexical scoping: the body sees the globals and its own parameters, not the caller's locals
        std::deque<Scope> hidden_scopes(std::make_move_iterator(scopes.begin() + 1), std::make_move_iterator(scopes.end()));
        scopes.resize(1);
        push_scope();
        for (size_t i = 0; i < bound.size(); i++) { Var v = bound[i]; v.name = proc.params[i].name; declare(v, proc.params[i].pos); }
        gen_stmts(proc.body);
        pop_scope();
        for (auto& sc : hidden_scopes) scopes.push_back(std::move(sc));
        ctxs.pop_back();
        if (pc.needs) release(pc.alive);
        if (pret && result_cell >= 0) {
            if (rn == 1) move(pc.retval, {{result_cell, 1}});
            else { if (!known0N(result_cell, rn)) clearN(result_cell, rn); moveN(pc.retval, result_cell, rn); }
        }
        if (pret) releaseN(pc.retval, rn);
        if (next_free != mark) internal("call frame not released");
        call_stack.pop_back();
        cur_tp = saved_tp;
        ctxs.swap(saved_ctxs);
        cur_proc = saved_proc;
        if (it->second.prelude) prelude_depth--;
    }
    // Inline with value parameters copied from the given cells.
    void call_proc_values(const std::string& name, const std::vector<Var>& vals, Pos p, int64_t result_cell) {
        auto it = procs.find(name);
        if (it == procs.end()) internal("missing library procedure " + name);
        std::vector<Var> bound;
        int64_t mark = next_free;
        for (size_t i = 0; i < vals.size(); i++) {
            const Param& prm = it->second.decl->params[i];
            int n = std::max(prm.type->size, 1);
            Var v; v.type = prm.type; v.cell = alloc(n);
            copyN(vals[i].cell, v.cell, n);
            bound.push_back(v);
        }
        call_proc(name, bound, p, result_cell);
        next_free = mark;
    }

    // Does the body of proc assign its parameter i (or pass it to a ref parameter)?
    std::map<std::pair<const Stmt*, size_t>, bool> written_cache;
    bool param_written(const Stmt& proc, size_t i) {
        auto key = std::make_pair(&proc, i);
        auto it = written_cache.find(key);
        if (it != written_cache.end()) return it->second;
        const std::string& n = proc.params[i].name;
        std::function<bool(const Expr&)> root = [&](const Expr& e) {
            const Expr* x = &e;
            while (x->kind == Expr::Field || x->kind == Expr::Index) x = x->a.get();
            return x->kind == Expr::Name && x->text == n;
        };
        std::function<bool(const Expr&)> in_expr = [&](const Expr& e) -> bool {
            if (e.kind == Expr::Call) {
                ProcInfo* pi = find_proc(e.text);
                for (size_t k = 0; k < e.args.size(); k++) {
                    if (root(*e.args[k]) && (!pi || k >= pi->decl->params.size() || pi->decl->params[k].ref)) return true;
                }
            }
            if (e.kind == Expr::AddrOf && root(*e.a)) return true;
            if (e.a && in_expr(*e.a)) return true;
            if (e.b && in_expr(*e.b)) return true;
            for (auto& x : e.args) if (in_expr(*x)) return true;
            return false;
        };
        std::function<bool(const Stmt&)> in_stmt = [&](const Stmt& st) -> bool {
            if ((st.kind == Stmt::Assign || st.kind == Stmt::IncDec) && st.target && root(*st.target)) return true;
            if (st.kind == Stmt::Call) {
                ProcInfo* pi = find_proc(st.name);
                for (size_t k = 0; k < st.args.size(); k++)
                    if (root(*st.args[k]) && (!pi || k >= pi->decl->params.size() || pi->decl->params[k].ref)) return true;
            }
            if (st.kind == Stmt::Asm) return true;
            if (st.target && in_expr(*st.target)) return true;
            if (st.e && in_expr(*st.e)) return true;
            if (st.init && in_stmt(*st.init)) return true;
            if (st.step && in_stmt(*st.step)) return true;
            for (auto& x : st.args) if (in_expr(*x)) return true;
            for (auto& x : st.body) if (in_stmt(*x)) return true;
            for (auto& x : st.else_body) if (in_stmt(*x)) return true;
            return false;
        };
        bool w = false;
        for (auto& st : proc.body) if (in_stmt(*st)) { w = true; break; }
        written_cache[key] = w;
        return w;
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
        std::vector<std::pair<LV, int64_t>> writeback;     // ref arguments that live outside cells
        std::vector<std::pair<int64_t, int>> pins;          // their runtime indices and addresses
        for (size_t i = 0; i < args.size(); i++) {
            const Param& prm = proc.params[i];
            const Expr& a = *args[i];
            Var v; v.type = prm.type; v.count = -1;
            if (prm.ref) {
                if (prm.array) {
                    Var* sv = a.kind == Expr::Name ? find_var(a.text) : nullptr;
                    if (sv && sv->arr_ptr) { v = *sv; v.owned = false; bound.push_back(v); continue; }
                    if (sv && sv->is_const && sv->count >= 0) {           // a heap array: bind a pointer to it
                        v.type = T->ptr(prm.type); v.arr_ptr = true; v.cell = alloc(2); setN(v.cell, 2, sv->cval);
                        bound.push_back(v);
                        continue;
                    }
                    if (!sv || !sv->cell_array()) fail(a.pos, "'" + prm.name + "' needs a " + prm.type->name + " array");
                    v = *sv; v.owned = false;
                    bound.push_back(v);
                    continue;
                }
                LV lv = lvalue(a);
                if (lv.kind == LV::Cells) { v.cell = lv.cell; v.owned = false; bound.push_back(v); continue; }
                if (lv.kind == LV::Reg) fail(a.pos, "a register cannot be passed by 'ref'");
                // copy in, and back out after the call to the same place: its index or address is computed once
                pins.push_back(pin(lv));
                int n = lv.size();
                v.cell = alloc(n);
                read_lv(lv, v.cell);
                v.owned = false;
                bound.push_back(v);
                writeback.emplace_back(lv, v.cell);
                continue;
            }
            // A constant argument for a parameter the body never writes is bound as a
            // constant: everything computed from it folds, in loops and branches too.
            if (prm.type->is_scalar() && !prm.type->is_ptr() && prm.type->kind != TK::Struct && !param_written(proc, i)) {
                if (auto k = fold(a, true)) {
                    v.is_const = true; v.owned = false; v.cval = wrap(*k, conv_type(prm.type));
                    bound.push_back(v);
                    continue;
                }
            }
            v.cell = alloc(std::max(prm.type->size, 1));
            eval_into(a, v.cell, prm.type);
            bound.push_back(v);
        }
        call_proc(name, bound, p, ret_of(&proc) ? result_cell : -1);
        for (auto& wb : writeback) {
            int n = wb.first.size();
            int64_t t = tmpN(n);
            copyN(wb.second, t, n);
            write_lv(wb.first, t);
            drop(t, n);
            clearN(wb.second, n);
        }
        for (auto& pc : pins) if (pc.second) clearN(pc.first, pc.second);
        next_free = mark;        // value parameters were cleared when the body's scope closed
    }

    void gen_stmt(const Stmt& s) {
        switch (s.kind) {
        case Stmt::Proc:
            if (scopes.size() > 1 || !call_stack.empty()) fail(s.pos, "procedures must be declared at the top level");
            return;                                  // registered up front
        case Stmt::Stack: case Stmt::Heap: case Stmt::Struct: case Stmt::Import:
            return;                                  // handled up front
        case Stmt::Const: {
            Var v; v.name = s.name; v.is_const = true; v.cval = s.cval; v.type = s.ctype; v.owned = false;
            declare(v, s.pos);
            return;
        }
        default: break;
        }
        begin_stmt(s, next_is_cont);
        next_is_cont = false;
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
                set_state_cell(target);
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
        case Stmt::Halt:   pulse_reg("HALT"); break;
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

    // ---- procedures: registration -----------------------------------------------------------------------
    void register_procs(const Program& p, bool is_prelude) {
        for (const StmtP& s : p.items) {
            if (s->kind != Stmt::Proc) continue;
            procs[s->name] = ProcInfo{s.get(), is_prelude};
            if (sr && sr->tramp.count(s->name)) procs[s->name].tramp = true;
        }
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
        for (const Var& f : p->frame) if (f.name == v.name) fail(pos, "'" + v.name + "' is declared twice in " + (p->is_main() ? "the main program" : "'" + p->name + "'") + "; variables of code that calls a rec or shared proc need unique names");
        for (const Var& f : p->consts) if (f.name == v.name) fail(pos, "'" + v.name + "' is already a constant");
        p->frame.push_back(v);
    }
    std::string hidden(TProc* p, const Type* t) {
        Var v; v.name = "__h" + std::to_string(p->hidden_n++); v.type = t;
        p->frame.push_back(v);
        return v.name;
    }

    // Clone e, replacing calls to trampolined procedures by hidden variables that
    // receive their results (each call becomes a Call action and a new resume block).
    ExprP hoist(const Expr& e, LowCtx& L, const Stmt& from) {
        if (e.kind == Expr::Call && is_tramp_call(e.text)) {
            ProcInfo* pi = find_proc(e.text);
            if (!ret_of(pi->decl)) fail(e.pos, "'" + e.text + "' returns nothing and cannot be used in an expression");
            std::vector<ExprP> args;
            for (auto& a : e.args) args.push_back(hoist(*a, L, from));
            std::string h = hidden(L.p, ret_of(pi->decl));
            emit_call(L, pi->tp, std::move(args), e.pos, from, h);
            auto n = std::make_unique<Expr>(); n->kind = Expr::Name; n->pos = e.pos; n->text = h; n->type = e.type;
            return n;
        }
        auto c = std::make_unique<Expr>();
        copy_info(*c, e);
        if (e.a) c->a = hoist(*e.a, L, from);
        if (e.b) c->b = hoist(*e.b, L, from);
        for (auto& x : e.args) c->args.push_back(hoist(*x, L, from));
        return c;
    }

    // A ref argument is copied back after the call to the place it was read from: the indices and
    // pointers that place depends on are computed before the call, into hidden variables of the caller.
    void pin_arg(Expr& e, LowCtx& L, const Stmt& from) {
        auto keep = [&](ExprP& x) {
            if (fold(*x)) return;
            const Type* t = x->type;
            Pos pos = x->pos;
            std::string h = hidden(L.p, t);
            synth_plain(L, mk_assign(from, h, std::move(x), t), from);
            x = std::make_unique<Expr>(); x->kind = Expr::Name; x->pos = pos; x->text = h; x->type = t;
        };
        switch (e.kind) {
        case Expr::Index:
            if (e.a->type && e.a->type->is_ptr()) keep(e.a); else pin_arg(*e.a, L, from);
            keep(e.b);
            return;
        case Expr::Field: if (e.arrow) keep(e.a); else pin_arg(*e.a, L, from); return;
        case Expr::Deref: keep(e.a); return;
        default: return;
        }
    }

    // A call to a trampolined procedure: ends the current block, resumes in a new one.
    void emit_call(LowCtx& L, TProc* callee, std::vector<ExprP> args, Pos pos, const Stmt& from, const std::string& result) {
        const Stmt& decl = *callee->decl;
        if (args.size() != decl.params.size()) fail(pos, "'" + callee->name + "' takes " + std::to_string(decl.params.size()) + " argument(s), " + std::to_string(args.size()) + " given");
        std::vector<std::pair<const Expr*, int>> copy_out;
        for (size_t i = 0; i < args.size(); i++)
            if (decl.params[i].ref && !decl.params[i].array) pin_arg(*args[i], L, from);
        for (size_t i = 0; i < args.size(); i++) {
            if (!decl.params[i].ref || decl.params[i].array) continue;
            ExprP keep = clone(*args[i]);
            copy_out.emplace_back(keep.get(), (int)i);
            synth_exprs.push_back(std::move(keep));
        }
        int resume = new_block(L.p, L);
        Action& a = act(L, Action::Call, &from);
        a.callee = callee; a.args = std::move(args); a.a = resume;
        L.block = resume;
        Action& p = act(L, Action::Pop, &from);
        p.callee = callee; p.result = result; p.copy_out = copy_out;
    }

    void set_state(LowCtx& L, int target, const Stmt* from) { Action& a = act(L, Action::SetState, from); a.a = target; }

    StmtP mk_assign(const Stmt& from, const std::string& name, ExprP value, const Type* t) {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Assign; s->op = "="; s->pos = from.pos; s->end = from.end;
        s->target = std::make_unique<Expr>(); s->target->kind = Expr::Name; s->target->pos = from.pos; s->target->text = name; s->target->type = t;
        s->e = std::move(value);
        return s;
    }

    void plain(LowCtx& L, const Stmt& s) { Action& a = act(L, Action::Plain, &s); a.s = &s; }
    // The rest of a statement whose calls were hoisted before it: when it follows a
    // call (cont), the debugger does not stop on it again.
    void synth_plain(LowCtx& L, StmtP s, const Stmt& from, bool cont = true) {
        Action& a = act(L, Action::Plain, &from); a.s = s.get(); a.cont = cont; synth.push_back(std::move(s));
    }

    // When the rest of a statement list calls nothing compiled as blocks, it runs
    // within one block, so it is generated as ordinary code in one scope: its
    // variables are scoped locals on the temporary stack instead of frame cells.
    // (Not the main program's top level, whose variables are the globals.)
    void lower_stmts(const std::vector<StmtP>& v, LowCtx& L, bool top = false) {
        for (size_t i = 0; i < v.size(); i++) {
            if (!top) {
                bool calls = false, decls = false;
                for (size_t j = i; j < v.size() && !calls; j++) { calls = calls_tramp(*v[j]); decls = decls || v[j]->kind == Stmt::Decl; }
                if (!calls && decls) {
                    Action& a = act(L, Action::PlainRun, v[i].get());
                    a.list = &v; a.from = i;
                    return;
                }
            }
            lower_stmt(*v[i], L);
        }
    }

    void lower_stmt(const Stmt& s, LowCtx& L) {
        TProc* p = L.p;
        switch (s.kind) {
        case Stmt::Proc: case Stmt::Stack: case Stmt::Heap: case Stmt::Struct: case Stmt::Import: return;
        case Stmt::Const: {
            Var v; v.name = s.name; v.is_const = true; v.owned = false; v.cval = s.cval; v.type = s.ctype;
            for (const Var& f : p->consts) if (f.name == v.name) fail(s.pos, "'" + v.name + "' is already declared");
            p->consts.push_back(v);
            return;
        }
        case Stmt::Decl: {
            Var v; v.name = s.name; v.type = s.vtype; v.count = s.count;
            if (s.in_heap) { v.is_const = true; v.cval = s.cval; v.owned = false; p->consts.push_back(v); return; }
            if (s.count >= 0 && !p->is_main() && p->reentrant) {
                // a local array of a recursive procedure lives in its heap frame
                Var a; a.name = s.name; a.type = T->ptr(s.vtype); a.arr_ptr = true; a.arr_off = p->heap_frame;
                p->heap_frame += std::max(s.vtype->size, 1) * s.count;
                frame_add(p, a, s.pos);
                return;
            }
            frame_add(p, v, s.pos);
            if (s.count >= 0) {                            // main: zero from the start; a shared proc's: cleared on every call
                if (!p->is_main()) { Action& a = act(L, Action::Clear, &s); a.result = s.name; }
                return;
            }
            if (!s.e) { Action& a = act(L, Action::Clear, &s); a.result = s.name; return; }
            bool after_call = calls_tramp(*s.e);
            ExprP init = hoist(*s.e, L, s);
            synth_plain(L, mk_assign(s, s.name, std::move(init), s.vtype), s, after_call);
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

    // Lower one trampolined procedure (or main) into blocks.
    void lower_proc(TProc* p, const std::vector<StmtP>& body) {
        LowCtx L{p, 0};
        L.block = new_block(p, L);
        p->entry = L.block;
        if (p->decl) {
            for (const Param& prm : p->decl->params) {
                Var v; v.name = prm.name; v.type = prm.type;
                if (prm.array) { v.type = T->ptr(prm.type); v.arr_ptr = true; }   // passed as a pointer
                frame_add(p, v, prm.pos);
            }
        }
        lower_stmts(body, L, p->is_main());
        act(L, Action::Ret, nullptr);      // implicit return at the end
        if (p->heap_frame > 0) {
            Action fe; fe.kind = Action::FrameEnter;
            B(p->entry).acts.insert(B(p->entry).acts.begin(), std::move(fe));
        }
    }

    // ---- trampoline: generation ------------------------------------------------------------------------------
    void push_frame_scope(TProc* p) {
        push_scope();
        scopes.back().frame = true;
        for (const Var& v : p->consts) scopes.back().vars[v.name] = v;
        for (const Var& v : p->frame) { scopes.back().vars[v.name] = v; scopes.back().order.push_back(v.name); }
    }
    void set_state_cell(int v) { setN(state_cell, state_n, v); }

    // The save stack.  Up to 256 cells it is a cell array indexed by a byte stack
    // pointer (the array idiom); above that a walking region in cells (the heap
    // layout of docs/HEAP.md) indexed by a 16-bit stack pointer.
    void stack_addr(int off, int64_t a) {                 // a (2 cells, zero) = sp + off
        copy(sp_cell, a);
        if (sp_n == 2) copy(sp_cell + 1, a + 1);
        addN_const(a, 2, off);
    }
    int64_t stack_index(int off) { int64_t i = tmp(); copy(sp_cell, i); add(i, off); return i; }
    void stack_store(int off, int64_t cell) {
        if (sp_n == 1) { int64_t i = stack_index(off); int64_t v = tmp(); copy(cell, v); array_write(stack_base, i, v); drop(v); drop(i); return; }
        int64_t a = tmp2(); stack_addr(off, a);
        int64_t v = tmp(); copy(cell, v);
        walk(stack_e0, a, v, -1, true, false);
        drop(v); drop(a, 2);
    }
    void stack_load(int off, int64_t cell) {
        if (sp_n == 1) { int64_t i = stack_index(off); clear(cell); array_read(stack_base, i, cell); drop(i); return; }
        int64_t a = tmp2(); stack_addr(off, a);
        clear(cell);
        walk(stack_e0, a, -1, cell, false, false);
        drop(a, 2);
    }
    // Store or load a run of cells at sp+off, sp+off+1, ...: the 16-bit address is
    // computed once and stepped.
    void stack_run(int off, const std::vector<int64_t>& cells, bool store) {
        if (sp_n == 1) { for (size_t k = 0; k < cells.size(); k++) { if (store) stack_store(off + (int)k, cells[k]); else stack_load(off + (int)k, cells[k]); } return; }
        if (cells.empty()) return;
        int64_t a = tmp2(); stack_addr(off, a);
        for (size_t k = 0; k < cells.size(); k++) {
            int64_t c = tmp2(); copyN(a, c, 2);
            if (store) { int64_t v = tmp(); copy(cells[k], v); walk(stack_e0, c, v, -1, true, false); drop(v); }
            else { clear(cells[k]); walk(stack_e0, c, -1, cells[k], false, false); }
            drop(c, 2);
            if (k + 1 < cells.size()) incN(a, 2);
        }
        release2(a);
    }
    std::vector<int64_t> frame_cells(const TProc* p) const {
        std::vector<int64_t> v;
        if (!p->is_main()) for (const Var& x : p->frame) for (int k = 0; k < x.size(); k++) v.push_back(x.cell + k);
        return v;
    }
    // --check-stack: halt with a message when `need` more cells do not fit
    void stack_check(int need) {
        if (!opt.check_stack) return;
        int64_t a = tmp2(); stack_addr(need, a);
        int64_t lim = tmp2(); setN(lim, 2, stack_size + 1);
        int64_t lt = tmp(); ltN_cells(a, lim, 2, lt, 1);
        int64_t ge = tmp(); not8(lt, ge, 1);
        if_nonzero(ge, [&] { halt_with("\nrecursion stack overflow\n"); });
        release(ge); release(lt); release2(lim); release2(a);
    }
    void halt_with(const std::string& msg) {
        int64_t t = tmp();
        for (unsigned char ch : msg) { set(t, ch); go(t); out += '.'; }
        release(t);
        pulse_reg("HALT");
    }
    // a 16-bit word at a constant heap address, through a 2-cell temporary
    void heap_word_read(int64_t addr, int64_t t) { copy(heap_cell(addr, HD), t); copy(heap_cell(addr + 1, HD), t + 1); }
    void heap_word_write(int64_t addr, int64_t t) {
        for (int k = 0; k < 2; k++) { int64_t c = heap_cell(addr + k, HD); clear(c); move(t + k, {{c, 1}}); }
    }

    // On entry to a procedure with local arrays: take their space from the top of
    // the heap (the allocator's end-of-heap word goes down), point the array
    // variables at it and zero it.
    void frame_enter(TProc* p) {
        int64_t e = tmp2();
        heap_word_read(5, e);
        addN_const(e, 2, -(int64_t)p->heap_frame);
        if (opt.check_stack) {
            int64_t brk = tmp2(); heap_word_read(1, brk);
            int64_t lt = tmp(); ltN_cells(e, brk, 2, lt, 1);
            if_nonzero(lt, [&] { halt_with("\nrecursion stack overflow: no heap left for local arrays\n"); });
            release(lt); release2(brk);
        }
        for (const Var& v : p->frame) {
            if (!v.arr_ptr || v.arr_off < 0) continue;
            clearN(v.cell, 2);
            copyN(e, v.cell, 2);
            addN_const(v.cell, 2, v.arr_off);
        }
        // zero the block
        int64_t a = tmp2(); copyN(e, a, 2);
        int64_t cnt = tmp2(); setN(cnt, 2, p->heap_frame);
        int64_t f = tmp(); nonzeroN(cnt, f, 2);
        open(f);
        {
            clear(f);
            int64_t c = tmp2(); copyN(a, c, 2);
            int64_t z = tmp();
            walk(heap_elem0, c, z, -1, true, false);
            drop(z); drop(c, 2);
            incN(a, 2);
            decN(cnt, 2);
            nonzeroN(cnt, f, 2);
        }
        close();
        release(f); release2(cnt); release2(a);
        heap_word_write(5, e);
        drop(e, 2);
    }
    void frame_leave(TProc* p) {
        if (p->heap_frame <= 0) return;
        int64_t e = tmp2();
        heap_word_read(5, e);
        addN_const(e, 2, p->heap_frame);
        heap_word_write(5, e);
        drop(e, 2);
    }

    // implicit: the end of the body, where a procedure with a result returns 0
    void gen_tramp_return(const Expr* value, Pos pos, bool implicit = false) {
        TProc* p = cur_tp;
        if (p->ret && !value && !implicit) fail(pos, "'" + p->name + "' returns a value");
        if (!p->ret && value) fail(pos, p->is_main() ? "the main program returns nothing" : "'" + p->name + "' returns nothing");
        if (value) store_result(*value, p->retval, p->ret);
        else if (p->ret) clearN(p->retval, std::max(p->ret->size, 1));
        if (p->is_main()) set_state_cell(0);
        else {
            frame_leave(p);
            // Leave the frame zero: frames of procedures that are not running share
            // cells with each other and with the temporaries of the ones that are.
            // ref parameters and the result are taken (and cleared) by the caller.
            for (size_t i = 0; i < p->frame.size(); i++) {
                const Var& v = p->frame[i];
                if (i < p->decl->params.size() && p->decl->params[i].ref && !p->decl->params[i].array) continue;
                if (v.count >= 0) { for (int k = 0; k < v.count; k++) for (int b = 0; b < v.size(); b++) clear(array_cell(v.cell, k, b, v.size())); }
                else clearN(v.cell, v.size());
            }
            if (!p->reentrant) {                    // the resume state its caller left in ret_cell
                clearN(state_cell, state_n);
                moveN(p->ret_cell, state_cell, state_n);
            } else {
                std::vector<int64_t> st;
                for (int k = 0; k < state_n; k++) st.push_back(state_cell + k);
                stack_run(-state_n, st, false);
            }
        }
        size_t b = ctxs.size();
        while (b-- > 0) if (ctxs[b].kind == ExitCtx::Block) break;
        if (b == (size_t)-1) internal("return outside a block");
        unwind_to(b);
    }

    // the pointer an array argument of a recursive procedure stands for
    void array_arg_pointer(const Expr& a, int64_t t, const std::string& pname) {
        Var* sv = a.kind == Expr::Name ? find_var(a.text) : nullptr;
        if (sv && sv->arr_ptr) { copyN(sv->cell, t, 2); return; }
        if (sv && sv->is_const && sv->count >= 0) { setN(t, 2, sv->cval); return; }
        fail(a.pos, "'" + pname + "' needs a heap array: arrays held in cells have no address (declare it with 'heap')");
    }

    void gen_action(Action& a, LBlock& blk) {
        switch (a.kind) {
        case Action::Plain:
            next_is_cont = a.cont;
            gen_stmt(*a.s);
            next_is_cont = false;
            return;
        case Action::PlainRun:
            push_scope();
            gen_stmts(*a.list, a.from);
            pop_scope();
            return;
        case Action::SetState:
            set_state_cell(a.a);
            return;
        case Action::FrameEnter:
            frame_enter(blk.owner);
            return;
        case Action::Clear: {
            Var* v = find_var(a.result);
            if (!v) internal("cleared variable missing");
            if (a.src) begin_stmt(*a.src);
            if (v->count >= 0) { for (int k = 0; k < v->count; k++) for (int b = 0; b < v->size(); b++) clear(array_cell(v->cell, k, b, v->size())); }
            else clearN(v->cell, v->size());
            if (a.src) end_stmt();
            return;
        }
        case Action::IfGoto: {
            if (a.src) begin_stmt(*a.src);
            int64_t c = tmp(); eval_cond(*a.cond, c);
            int64_t f = tmp(); set(f, 1);
            if_nonzero(c, [&] { set_state_cell(a.a); clear(f); });
            if_nonzero(f, [&] { set_state_cell(a.b); });
            release(f); release(c);
            if (a.src) end_stmt();
            return;
        }
        case Action::Call: {
            TProc* callee = a.callee;
            const Stmt& decl = *callee->decl;
            int64_t base = next_free;                   // its arguments must not land in the callee's frame
            next_free = std::max(next_free, callee->frame_hi);
            if (a.src) begin_stmt(*a.src);
            // arguments first (they may read the caller's frame)
            int64_t mark = next_free;
            std::vector<int64_t> vals;
            for (size_t i = 0; i < a.args.size(); i++) {
                const Param& prm = decl.params[i];
                const Var& pv = callee->frame[i];
                int n = pv.size();
                int64_t t = tmpN(n);
                if (prm.array) array_arg_pointer(*a.args[i], t, prm.name);
                else if (prm.ref) read_lv(lvalue(*a.args[i]), t);
                else eval_into(*a.args[i], t, prm.type);
                vals.push_back(t);
            }
            // Save the caller's frame when the callee can come back into the caller
            // (they are in one cycle of calls), and the resume state: on the stack
            // for a re-entrant callee, else in the callee's own ret_cell.  A call that
            // pushes nothing still moves the stack pointer by one, so the debugger's
            // step over can tell when it has returned.
            TProc* me = blk.owner;
            std::vector<int64_t> cells = same_cycle(me, callee) ? frame_cells(me) : std::vector<int64_t>();
            int off = (int)cells.size();
            int pushed = pushed_by(me, callee);
            stack_check(pushed);
            if (!callee->reentrant) {
                setN(callee->ret_cell, state_n, a.a);
                stack_run(0, cells, true);
            } else if (sp_n == 1 && state_n == 1) {
                stack_run(0, cells, true);
                int64_t r = tmp(); set(r, a.a); int64_t i = stack_index(off); array_write(stack_base, i, r); drop(i); drop(r);
            } else {
                int64_t r = tmpN(state_n); setN(r, state_n, a.a);
                for (int k = 0; k < state_n; k++) cells.push_back(r + k);
                stack_run(0, cells, true);
                releaseN(r, state_n);
            }
            addN_const(sp_cell, sp_n, pushed);
            // load the callee's parameters
            for (size_t i = 0; i < vals.size(); i++) {
                const Var& prm = callee->frame[i];
                clearN(prm.cell, prm.size());
                moveN(vals[i], prm.cell, prm.size());
            }
            next_free = mark;
            set_state_cell(callee->entry);
            if (a.src) end_stmt();
            next_free = base;
            return;
        }
        case Action::Pop: {
            TProc* me = blk.owner;
            int64_t base = next_free;                   // above the callee's frame until its results are taken
            next_free = std::max(next_free, a.callee->frame_hi);
            if (a.src) begin_stmt(*a.src, true);
            // ref parameters: take the callee's final values before the pop may overwrite them (self-recursion)
            int64_t mark = next_free;
            std::vector<int64_t> saved;
            for (auto& co : a.copy_out) {
                const Var& prm = a.callee->frame[(size_t)co.second];
                int64_t t = tmpN(prm.size());
                moveN(prm.cell, t, prm.size());
                saved.push_back(t);
            }
            addN_const(sp_cell, sp_n, -pushed_by(me, a.callee));
            if (same_cycle(me, a.callee)) stack_run(0, frame_cells(me), false);
            for (size_t i = a.copy_out.size(); i-- > 0;) {
                LV lv = lvalue(*a.copy_out[i].first);
                const Var& prm = a.callee->frame[(size_t)a.copy_out[i].second];
                int64_t t = tmpN(prm.size());
                moveN(saved[i], t, prm.size());
                write_lv(lv, t);
                drop(t, prm.size());
            }
            next_free = mark;
            if (!a.result.empty()) {
                Var* h = find_var(a.result);
                if (!h) internal("hidden result variable missing");
                clearN(h->cell, h->size());
                moveN(a.callee->retval, h->cell, h->size());
            } else if (a.callee->ret) clearN(a.callee->retval, std::max(a.callee->ret->size, 1));
            if (a.src) end_stmt();
            next_free = base;
            return;
        }
        case Action::Ret: {
            if (a.src) begin_stmt(*a.src);
            gen_tramp_return(a.ret.get(), a.src ? a.src->pos : Pos{}, a.src == nullptr);
            if (a.src) end_stmt();
            return;
        }
        }
    }

    static bool action_may_exit(const Action& a) {
        if (a.kind == Action::Plain) return may_exit(*a.s);
        if (a.kind == Action::PlainRun) { for (size_t i = a.from; i < a.list->size(); i++) if (may_exit(*(*a.list)[i])) return true; return false; }
        return a.kind == Action::Ret;
    }
    void gen_actions(std::vector<Action>& acts, LBlock& blk, size_t from = 0) {
        for (size_t i = from; i < acts.size(); i++) {
            gen_action(acts[i], blk);
            if (i + 1 < acts.size() && action_may_exit(acts[i])) {
                // the test cell lives across the rest, so it must be above the frame of
                // any procedure the rest calls (or takes results from)
                int64_t base = next_free;
                for (size_t j = i + 1; j < acts.size(); j++)
                    if ((acts[j].kind == Action::Call || acts[j].kind == Action::Pop) && acts[j].callee) next_free = std::max(next_free, acts[j].callee->frame_hi);
                int64_t t = tmp();
                copy(alive_cell, t);
                if_nonzero(t, [&] { gen_actions(acts, blk, i + 1); });
                release(t);
                next_free = base;
                return;
            }
        }
    }

    void gen_block(LBlock& blk) {
        TProc* p = blk.owner;
        int64_t saved_free = next_free;
        if (blk.temp_base >= 0) next_free = blk.temp_base;
        int64_t base_free = next_free;
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
        if (next_free != base_free) internal("block temporaries not released");
        next_free = saved_free;
    }

    // Dispatch by countdown: t holds state - (first - 1), and every level, one per
    // block first..last, decrements it and runs its block when it reaches 0.  The
    // levels follow each other instead of nesting, so every block runs with its
    // temporaries at the same height; a block sets the state cell, not t, so one
    // pass runs one block.  (t passes 0 once in up to 256 levels.)
    int64_t disp_scratch = -1;      // a few cells below the frames for the dispatcher's own temporaries
    void gen_dispatch(int64_t t, int first, int last) {
        for (int id = first; id <= last; id++) {
            next_free = disp_scratch;
            add(t, -1);
            int64_t z = tmp();
            iszero8(t, z);
            if_nonzero(z, [&] { gen_block(blocks[(size_t)id - 1]); });
            release(z);
        }
    }
    // 16-bit state: choose the group of 256 by the high byte, then the block by the low byte.
    int64_t disp_t2 = -1, disp_z2 = -1;
    void gen_dispatch16(int64_t h, int groups) {
        add(h, 1);
        for (int g = 0; g < groups; g++) {
            next_free = disp_scratch;
            add(h, -1);
            iszero8(h, disp_z2);
            if_nonzero(disp_z2, [&] {
                int first = std::max(1, g * 256), last = std::min((int)blocks.size(), g * 256 + 255);
                next_free = disp_scratch;
                copy(state_cell, disp_t2);
                add(disp_t2, -(first - g * 256) + 1);
                gen_dispatch(disp_t2, first, last);
                next_free = disp_scratch;
                clear(disp_t2);
            });
        }
    }

    // ---- trampoline: who can reach whom ------------------------------------------------------------------
    std::vector<std::vector<bool>> reach;           // reach[a][b]: a call from tprocs[a] can lead to tprocs[b]
    void tramp_calls(const Expr& e, std::set<int>& found) {
        if (e.kind == Expr::Call) if (ProcInfo* pi = find_proc(e.text); pi && pi->tp) found.insert(pi->tp->idx);
        if (e.a) tramp_calls(*e.a, found);
        if (e.b) tramp_calls(*e.b, found);
        for (auto& x : e.args) tramp_calls(*x, found);
    }
    void tramp_calls(const Stmt& s, std::set<int>& found) {
        if (s.kind == Stmt::Call) if (ProcInfo* pi = find_proc(s.name); pi && pi->tp) found.insert(pi->tp->idx);
        if (s.target) tramp_calls(*s.target, found);
        if (s.e) tramp_calls(*s.e, found);
        if (s.init) tramp_calls(*s.init, found);
        if (s.step) tramp_calls(*s.step, found);
        for (auto& x : s.args) tramp_calls(*x, found);
        for (auto& x : s.body) tramp_calls(*x, found);
        for (auto& x : s.else_body) tramp_calls(*x, found);
    }
    void compute_reach() {
        size_t n = tprocs.size();
        std::vector<std::set<int>> calls(n);
        for (size_t i = 1; i < n; i++) for (auto& st : tprocs[i]->decl->body) tramp_calls(*st, calls[i]);
        reach.assign(n, std::vector<bool>(n, false));
        for (size_t i = 1; i < n; i++) {
            std::vector<int> work(calls[i].begin(), calls[i].end());
            while (!work.empty()) {
                int c = work.back(); work.pop_back();
                if (reach[i][(size_t)c]) continue;
                reach[i][(size_t)c] = true;
                for (int d : calls[(size_t)c]) work.push_back(d);
            }
            tprocs[i]->reentrant = reach[i][i];
        }
    }
    // a call between procedures that can each lead back to the other
    bool same_cycle(const TProc* caller, const TProc* callee) const {
        return !caller->is_main() && reach[(size_t)caller->idx][(size_t)callee->idx] && reach[(size_t)callee->idx][(size_t)caller->idx];
    }
    // cells a call moves the stack pointer by
    int pushed_by(const TProc* caller, const TProc* callee) const {
        if (!callee->reentrant) return 1;
        return (same_cycle(caller, callee) ? caller->save_count() : 0) + state_n;
    }

    // Frames: every variable of a trampolined procedure has a fixed cell.  Two
    // procedures that can never be active at once (neither can lead to the other)
    // share cells; the main program's frame is live throughout.  Within a frame the
    // variables are ordered by references per cell, the densest highest, next to the
    // temporaries (as layout_globals does for inlined code).
    void place_frames(const Program& prog) {
        size_t n = tprocs.size();
        std::vector<std::unordered_map<std::string, double>> refs(n);
        std::unordered_set<std::string> none;
        refs[0] = global_refs(prog);                    // main's frame holds the globals: count every use
        for (size_t i = 1; i < n; i++) {
            std::unordered_map<std::string, double> calls;
            for (auto& st : tprocs[i]->decl->body) count_refs(*st, 1.0, refs[i], calls, none);
        }
        struct Slot { Var* v; int64_t* where; int64_t size; double density; };
        std::vector<std::vector<Slot>> slots(n);
        std::vector<int64_t> size(n, 0);
        for (size_t i = 0; i < n; i++) {
            TProc* tp = tprocs[i].get();
            for (Var& v : tp->frame) {
                int64_t c = v.count >= 0 ? array_total(v) : v.size();
                double r = refs[i].count(v.name) ? refs[i][v.name] : 0.0;
                slots[i].push_back({&v, &v.cell, c, r / (double)std::max<int64_t>(c, 1)});
            }
            if (tp->ret) slots[i].push_back({nullptr, &tp->retval, std::max(tp->ret->size, 1), 1e9});
            if (!tp->is_main() && !tp->reentrant) slots[i].push_back({nullptr, &tp->ret_cell, state_n, 1e9});
            std::stable_sort(slots[i].begin(), slots[i].end(), [](const Slot& a, const Slot& b) { return a.density < b.density; });
            for (const Slot& sl : slots[i]) size[i] += sl.size;
        }
        // Like a call stack: callers before callees (cycles in any order), each frame
        // just above the highest frame of anything that can lead to it.  While a block
        // runs, the frames that hold live values are its own and its callers' (callers
        // in its own cycle have saved theirs on the stack), all at or below its own.
        std::vector<int64_t> off(n, 0);
        std::vector<bool> placed(n, false);
        placed[0] = true;
        int64_t extent = size[0];
        for (size_t done = 1; done < n; done++) {
            size_t pick = 0;
            for (size_t i = 1; i < n && !pick; i++) {
                if (placed[i]) continue;
                bool ready = true;               // every caller outside its cycle is placed
                for (size_t j = 1; j < n && ready; j++)
                    if (!placed[j] && j != i && reach[j][i] && !reach[i][j]) ready = false;
                if (ready) pick = i;
            }
            if (!pick) internal("frame order");
            int64_t o = size[0];
            for (size_t j = 1; j < n; j++) if (placed[j] && reach[j][pick]) o = std::max(o, off[j] + size[j]);
            off[pick] = o;
            placed[pick] = true;
            extent = std::max(extent, o + size[pick]);
        }
        int64_t base = alloc((int)extent);
        for (size_t i = 0; i < n; i++) {
            tprocs[i]->frame_hi = base + off[i] + size[i];
            int64_t c = base + off[i];
            for (const Slot& sl : slots[i]) { *sl.where = c; c += sl.size; }
            TProc* tp = tprocs[i].get();
            for (Var& v : tp->frame)
                if (v.name.rfind("__h", 0) != 0) add_symbol(tp->is_main() ? v.name : tp->name + "." + v.name, v.cell, v.type, v.count, "");
        }
    }

    void gen_trampoline(const Program& prog) {
        tprocs.push_back(std::make_unique<TProc>());          // main is tprocs[0]
        for (auto& kv : procs) {
            if (!kv.second.tramp) continue;
            auto tp = std::make_unique<TProc>();
            tp->name = kv.first; tp->decl = kv.second.decl; tp->ret = ret_of(kv.second.decl);
            tp->idx = (int)tprocs.size();
            kv.second.tp = tp.get();
            tprocs.push_back(std::move(tp));
        }
        compute_reach();
        if (sr && sr->stack_size > 0) stack_size = sr->stack_size;
        lower_proc(tprocs[0].get(), prog.items);               // main first: its consts are visible to the others
        for (size_t i = 1; i < tprocs.size(); i++) lower_proc(tprocs[i].get(), tprocs[i]->decl->body);
        state_n = blocks.size() > 254 ? 2 : 1;
        if (blocks.size() > 65000) fail(Pos{}, "too many trampoline blocks (" + std::to_string(blocks.size()) + ")");

        sp_n = stack_size > 256 ? 2 : 1;
        if (sp_n == 1) stack_base = alloc((int)array_cells(stack_size));   // the save stack, lowest so variables stay close together
        else stack_e0 = alloc((int)((int64_t)HK * (HOP + stack_size))) + (int64_t)HK * HOP;
        state_cell = alloc(state_n); alive_cell = alloc(1); sp_cell = alloc(sp_n);
        int64_t run_cell = state_n == 2 ? alloc(1) : -1;
        int64_t disp_t = alloc(1), disp_h = -1;
        if (state_n == 2) { disp_h = alloc(1); disp_t2 = alloc(1); disp_z2 = alloc(1); }
        disp_scratch = alloc(4);
        res.symbols.push_back(Symbol{"__state", state_cell, state_n, state_n == 1 ? 'b' : 'w', 1, "", state_n, false});
        res.symbols.push_back(Symbol{"__sp", sp_cell, sp_n, sp_n == 1 ? 'b' : 'w', 1, "", sp_n, false});
        res.sp_cell = sp_cell;
        res.sp_bytes = sp_n;
        place_frames(prog);
        // (a call and the code taking its result use temporaries above the callee's frame too:
        // gen_action raises the base for just those)
        for (LBlock& b : blocks) {
            int64_t t = b.owner->frame_hi;
            if (b.owner->reentrant)                     // a suspended caller in its cycle may sit above it
                for (auto& q : tprocs) if (!q->is_main() && reach[(size_t)b.owner->idx][(size_t)q->idx] && reach[(size_t)q->idx][(size_t)b.owner->idx]) t = std::max(t, q->frame_hi);
            b.temp_base = t;
        }
        int64_t after_frames = next_free;

        out += "\n# trampoline: stack of " + std::to_string(stack_size) + " cells and " + std::to_string(blocks.size()) + " blocks\n";
        push_frame_scope(tprocs[0].get());
        scopes.back().top = true;
        set_state_cell(tprocs[0]->entry);
        if (state_n == 1) {
            open(state_cell);
            {
                next_free = disp_scratch;
                copy(state_cell, disp_t);
                gen_dispatch(disp_t, 1, (int)blocks.size());
                next_free = disp_scratch;
                clear(disp_t);
            }
            close();
        } else {
            set(run_cell, 1);
            open(run_cell);
            {
                clear(run_cell);
                next_free = disp_scratch;
                copy(state_cell + 1, disp_h);
                gen_dispatch16(disp_h, (int)blocks.size() / 256 + 1);
                next_free = disp_scratch;
                clear(disp_h);
                nonzeroN(state_cell, run_cell, 2);
            }
            close();
        }
        next_free = after_frames;
        pop_scope();
    }

    // Start-up code: the heap's bookkeeping words and the bytes of every string literal.
    void heap_init() {
        if (heap_lo == INT64_MAX) return;
        auto put = [&](int64_t addr, int v) { if (v) { int64_t c = heap_cell(addr, HD); go(c); add(c, v); } };
        auto put16 = [&](int64_t addr, int64_t v) { put(addr, (int)(v & 255)); put(addr + 1, (int)((v >> 8) & 255)); };
        put16(1, sr->heap_static_end);       // the allocator's break
        put16(5, heap_size);                 // the end of the heap
        for (auto& s : sr->heap_init) for (size_t i = 0; i < s.second.size(); i++) put(s.first + (int64_t)i, (unsigned char)s.second[i]);
    }

    // ---- layout of the global variables ---------------------------------------------------------------
    // Brainfuck reaches a cell by stepping the pointer, and temporaries live just
    // above the last variable, so the cost of a variable is roughly how often code
    // refers to it times its distance from the top.  The globals are laid out before
    // any code is generated: each is counted once per reference, a procedure's body
    // once per inlined call, and they are placed by references per cell, the densest
    // nearest the top (which minimises the total distance: Smith's rule).
    void count_refs(const Expr& e, double w, std::unordered_map<std::string, double>& refs,
                    std::unordered_map<std::string, double>& calls, const std::unordered_set<std::string>& shadow) {
        if ((e.kind == Expr::Name) && !shadow.count(e.text)) refs[e.text] += w;
        if (e.kind == Expr::Call) calls[e.text] += w;
        if (e.a) count_refs(*e.a, w, refs, calls, shadow);
        if (e.b) count_refs(*e.b, w, refs, calls, shadow);
        for (auto& x : e.args) count_refs(*x, w, refs, calls, shadow);
    }
    void count_refs(const Stmt& s, double w, std::unordered_map<std::string, double>& refs,
                    std::unordered_map<std::string, double>& calls, const std::unordered_set<std::string>& shadow) {
        if (s.kind == Stmt::Proc) return;
        if (s.kind == Stmt::Call) calls[s.name] += w;
        if (s.kind == Stmt::Decl && !shadow.count(s.name) && s.e) refs[s.name] += w;
        if (s.target) count_refs(*s.target, w, refs, calls, shadow);
        if (s.e) count_refs(*s.e, w, refs, calls, shadow);
        if (s.init) count_refs(*s.init, w, refs, calls, shadow);
        if (s.step) count_refs(*s.step, w, refs, calls, shadow);
        for (auto& x : s.args) count_refs(*x, w, refs, calls, shadow);
        for (auto& x : s.body) count_refs(*x, w, refs, calls, shadow);
        for (auto& x : s.else_body) count_refs(*x, w, refs, calls, shadow);
    }
    static void declared_names(const std::vector<StmtP>& v, std::unordered_set<std::string>& out) {
        for (auto& s : v) {
            if (s->kind == Stmt::Decl || s->kind == Stmt::Const) out.insert(s->name);
            if (s->init && s->init->kind == Stmt::Decl) out.insert(s->init->name);
            declared_names(s->body, out);
            declared_names(s->else_body, out);
        }
    }
    std::unordered_map<std::string, double> global_refs(const Program& prog) {
        std::unordered_map<std::string, double> refs, calls;
        std::unordered_set<std::string> none;
        for (auto& s : prog.items) count_refs(*s, 1.0, refs, calls, none);
        // each procedure's references, scaled by the number of times it is inlined
        struct PRefs { std::unordered_map<std::string, double> refs, calls; };
        std::unordered_map<std::string, PRefs> per;
        for (auto& kv : procs) {
            if (kv.second.prelude) continue;
            const Stmt& d = *kv.second.decl;
            std::unordered_set<std::string> shadow;
            for (const Param& p : d.params) shadow.insert(p.name);
            declared_names(d.body, shadow);
            PRefs& pr = per[kv.first];
            for (auto& s : d.body) count_refs(*s, 1.0, pr.refs, pr.calls, shadow);
        }
        std::unordered_map<std::string, double> times;          // inlined copies of each procedure
        std::unordered_set<std::string> emitted_once;           // trampolined: one copy, however often called
        std::function<void(const std::string&, double, int)> visit = [&](const std::string& p, double w, int depth) {
            auto it = per.find(p);
            if (it == per.end() || depth > 64) return;
            if (ProcInfo* pi = find_proc(p); pi && pi->tramp) {
                if (!emitted_once.insert(p).second) return;
                w = 1.0;
            }
            times[p] += w;
            for (auto& c : it->second.calls) visit(c.first, w * c.second, depth + 1);
        };
        for (auto& c : calls) visit(c.first, c.second, 0);
        for (auto& kv : per) {
            double t = times.count(kv.first) ? times[kv.first] : 0.0;
            for (auto& r : kv.second.refs) refs[r.first] += t * r.second;
        }
        return refs;
    }
    void layout_globals(const Program& prog) {
        std::unordered_map<std::string, double> refs = global_refs(prog);
        // Groups: runs of declarations with no code between them.  A group is laid out
        // when its first declaration is reached, so code before it keeps its
        // temporaries close to the variables that exist then.
        std::vector<const Stmt*> group;
        auto flush = [&] {
            if (group.size() > 1) {
                auto g = std::make_shared<std::vector<const Stmt*>>(group);
                std::stable_sort(g->begin(), g->end(), [&](const Stmt* a, const Stmt* b) {
                    auto density = [&](const Stmt* s) {
                        Var tv; tv.type = s->vtype; tv.count = s->count;
                        int64_t size = s->count >= 0 ? array_total(tv) : tv.size();
                        double r = refs.count(s->name) ? refs[s->name] : 0.0;
                        return r / (double)std::max<int64_t>(size, 1);
                    };
                    return density(a) < density(b);
                });
                for (const Stmt* d : group) layout_group[d] = g;
            }
            group.clear();
        };
        for (auto& s : prog.items) {
            switch (s->kind) {
            case Stmt::Proc: case Stmt::Const: case Stmt::Struct: case Stmt::Import: case Stmt::Stack: case Stmt::Heap: continue;
            case Stmt::Decl:
                if (s->in_heap) continue;
                group.push_back(s.get());              // an initialiser runs after the whole group is placed
                continue;
            default: flush();
            }
        }
        flush();
    }
    std::unordered_map<const Stmt*, std::shared_ptr<std::vector<const Stmt*>>> layout_group;
    void place_group(const Stmt& s) {
        auto it = layout_group.find(&s);
        if (it == layout_group.end()) return;
        auto g = it->second;
        for (const Stmt* d : *g) {
            layout_group.erase(d);
            if (prealloc.count(d)) continue;
            Var tv; tv.type = d->vtype; tv.count = d->count;
            prealloc[d] = alloc((int)(d->count >= 0 ? array_total(tv) : tv.size()));
        }
    }

    void run(const Program& prog) {
        register_procs(prelude, true);
        register_procs(prog, false);
        out += "# compiled by bfl\n";
        heap_init();
        if (sr && sr->main_tramp) { gen_trampoline(prog); res.cells_used = high - ram; return; }
        if (opt.layout) layout_globals(prog);
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

// Language features enabled so far (docs/ROADMAP.md phases).
static Features current_features() {
    Features f;
    f.wide_ints = true;
    f.heap = true;
    f.alloc = true;
    f.structs = true;
    return f;
}

namespace {

// Loads files for import statements: next to the importing file first, then the
// standard library.  Every file is loaded once; its items replace the first
// import statement that names it.
struct Importer {
    std::vector<std::string>& names;
    std::vector<std::string>& texts;
    std::vector<std::string> dirs;
    std::set<int> libs;
    std::map<std::string, int> loaded;      // key -> file index
    std::vector<ParseError>& errors;

    void expand(Program& prog, int file) {
        std::vector<StmtP> outv;
        for (StmtP& s : prog.items) {
            if (s->kind != Stmt::Import) { outv.push_back(std::move(s)); continue; }
            int idx = load(s->name, file, s->pos);
            if (idx < 0) continue;
            std::vector<ParseError> perr;
            Program sub = parse(texts[(size_t)idx], idx, perr);
            for (const ParseError& e : perr) errors.push_back(e);
            expand(sub, idx);
            for (StmtP& x : sub.items) outv.push_back(std::move(x));
        }
        prog.items = std::move(outv);
    }
    // returns the new file's index, or -1 when it is already loaded or missing
    int load(const std::string& name, int from, Pos pos) {
        namespace fs = std::filesystem;
        std::string key, text;
        bool lib = false;
        std::error_code ec;
        fs::path candidate = dirs[(size_t)from].empty() ? fs::path() : fs::path(dirs[(size_t)from]) / name;
        if (!candidate.empty() && fs::is_regular_file(candidate, ec)) {
            key = fs::weakly_canonical(candidate, ec).string();
            if (loaded.count(key)) return -1;
            std::ifstream f(candidate, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf(); text = ss.str();
        } else if (const char* t = stdlib_text(name)) {
            key = "<std>/" + name;
            if (loaded.count(key)) return -1;
            text = t; lib = true;
        } else {
            errors.push_back(ParseError{pos, "cannot find '" + name + "' next to this file or in the standard library"});
            return -1;
        }
        int idx = (int)texts.size();
        loaded[key] = idx;
        names.push_back(lib ? "<std>/" + name : candidate.string());
        texts.push_back(text);
        dirs.push_back(lib ? std::string() : candidate.parent_path().string());
        if (lib) libs.insert(idx);
        return idx;
    }
};

} // namespace

Result compile(const std::string& source, uint32_t ram_start, const Options& opt) {
    namespace fs = std::filesystem;
    Result res;
    std::vector<std::string> texts{source};
    res.files.push_back(opt.file_name.empty() ? "<source>" : opt.file_name);
    auto err = [&](const Pos& p, const std::string& m) { res.errors.push_back(Error{p.line, p.col, m, p.file}); };
    std::vector<ParseError> perr;
    Program prog = parse(source, 0, perr);
    Importer imp{res.files, texts, {}, {}, {}, perr};
    {
        std::error_code ec;
        fs::path mp(opt.file_name);
        imp.dirs.push_back(!opt.file_name.empty() && fs::is_regular_file(mp, ec) ? mp.parent_path().string() : std::string());
        if (imp.dirs[0].empty() && !opt.file_name.empty() && opt.file_name[0] != '<') imp.dirs[0] = ".";
    }
    imp.expand(prog, 0);
    res.sources = texts;
    for (const ParseError& e : perr) err(e.pos, e.msg);
    if (!res.errors.empty()) return res;
    Types types;
    Program prelude;
    try {
        const char* pt = stdlib_text("prelude.bfl");
        if (!pt) throw ParseError{Pos{}, "prelude.bfl is missing from the standard library"};
        prelude = parse(pt, -1);
    } catch (const ParseError& e) {
        err(Pos{}, "internal compiler error in the prelude: " + e.msg);
        return res;
    }
    SemaResult sr = analyse(prelude, prog, types, current_features(), opt.warnings, imp.libs, res.files);
    auto by_pos = [](const Diag& a, const Diag& b) {
        if (a.pos.file != b.pos.file) return a.pos.file < b.pos.file;
        return a.pos.line != b.pos.line ? a.pos.line < b.pos.line : a.pos.col < b.pos.col;
    };
    std::stable_sort(sr.errors.begin(), sr.errors.end(), by_pos);
    std::stable_sort(sr.warnings.begin(), sr.warnings.end(), by_pos);
    for (const Diag& d : sr.errors) err(d.pos, d.msg);
    for (const Diag& d : sr.warnings) res.warnings.push_back(Error{d.pos.line, d.pos.col, d.msg, d.pos.file});
    if (!res.errors.empty()) return res;

    // One pass without a heap, or two: the first finds how many cells the
    // variables need, the second places the heap just above them.
    auto generate = [&](int64_t heap_lo, bool dry) -> std::unique_ptr<Gen> {
        auto g = std::make_unique<Gen>(texts, ram_start);
        g->T = &types; g->opt = opt; g->sr = &sr; g->dry = dry;
        g->file_names = res.files;
        g->prelude = std::move(prelude);
        if (heap_lo != INT64_MAX) {
            g->heap_lo = heap_lo;
            g->heap_elem0 = heap_lo + (int64_t)HOP * HK;
            g->heap_size = sr.heap_size;
        }
        try {
            g->run(prog);
        } catch (const Fail& f) {
            g->res.errors.push_back(Error{f.pos.line, f.pos.col, f.msg, f.pos.file});
        }
        prelude = std::move(g->prelude);
        return g;
    };
    std::unique_ptr<Gen> g;
    if (sr.uses_heap) {
        auto g1 = generate((int64_t)ram_start + ((int64_t)1 << 30), true);
        if (!g1->res.errors.empty()) g = std::move(g1);
        else {
            int64_t lo = g1->high + 16;
            g = generate(lo, false);
            if (g->res.errors.empty() && g->high != g1->high) g->res.errors.push_back(Error{0, 0, "internal compiler error: heap placement changed the layout", 0});
            g->res.cells_used = g->high - (int64_t)ram_start + 16 + (int64_t)HK * (HOP + sr.heap_size);
            g->res.heap_cell = g->heap_elem0;
            g->res.heap_size = sr.heap_size;
        }
    } else g = generate(INT64_MAX, false);
    Result outr = std::move(g->res);
    outr.files = res.files;
    outr.sources = res.sources;
    outr.warnings = res.warnings;
    if (!outr.errors.empty()) { outr.bf.clear(); outr.stmts.clear(); }
    return outr;
}

std::string format_error(const Result& r, const Error& e, const char* kind) {
    std::string f = e.file >= 0 && e.file < (int)r.files.size() ? r.files[(size_t)e.file] : std::string("<library>");
    std::string s = f + ":" + std::to_string(e.line) + ":" + std::to_string(e.col) + ": ";
    if (kind) s += std::string(kind) + ": ";
    return s + e.msg;
}

std::string describe(const Result& r) {
    std::string s = "# files: F index name\n";
    for (size_t i = 0; i < r.files.size(); i++) s += "F " + std::to_string(i) + " " + r.files[i] + "\n";
    s += "# statement map: S line col end_line start end depth cont file\n";
    char buf[256];
    for (const StmtRange& m : r.stmts) { snprintf(buf, sizeof buf, "S %d %d %d %d %d %d %d %d\n", m.line, m.col, m.end_line, m.start, m.end, m.depth, m.cont ? 1 : 0, m.file); s += buf; }
    s += "# symbols: V name kind cell cells count scope\n";
    for (const Symbol& v : r.symbols) { snprintf(buf, sizeof buf, "V %s %c 0x%llX %d %d %s\n", v.name.c_str(), v.kind, (long long)v.cell, v.cells, v.count, v.scope.c_str()); s += buf; }
    if (r.sp_cell >= 0) { snprintf(buf, sizeof buf, "# trampoline stack pointer cell 0x%llX\n", (long long)r.sp_cell); s += buf; }
    if (r.heap_cell >= 0) { snprintf(buf, sizeof buf, "# heap: %d bytes, byte 0 at cell 0x%llX, %d cells per byte\n", r.heap_size, (long long)r.heap_cell, HK); s += buf; }
    return s;
}

} // namespace bfl
