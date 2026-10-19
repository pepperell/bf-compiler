// sema.cpp - the BFL semantic pass.
//
// Resolves names with lexical scoping, gives every expression a type, folds
// constants, checks every rule the code generator relies on, and collects
// errors (it keeps going after one) and warnings.
//
// Scoping: a procedure body sees its parameters, its own locals and the
// top-level variables and constants declared before the procedure.
// Procedures themselves may be called before they are declared.
#include "sema.h"
#include "../registers.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace bfl {

namespace {

struct Abort {};                     // too many errors

struct Sym {
    enum K { Var, Const } k = Var;
    std::string name;
    Pos         pos;
    const Type* type = nullptr;      // Var: value type (Array for arrays); Const: value type
    int64_t     cval = 0;
    bool        in_heap = false;     // heap array
    int64_t     addr = 0;            // heap array: address of element 0
    bool        param = false, ref = false, global = false;
    int         item = -1;           // top-level position of a global
    bool        used = false, quiet = false;
};

struct ProcSig {
    Stmt*       decl = nullptr;
    const Type* ret = nullptr;       // null: no result
    bool        prelude = false;
    int         item = -1;           // top-level position (user procedures)
    std::vector<std::string> callees;
    std::set<Sym*> globals;          // globals read or written directly
    std::vector<std::pair<std::string, Pos>> locals;   // every local declared (for the trampoline name rule)
    bool has_array_local = false;
};

struct CallSite { std::string callee; Pos pos; int item; };

struct Sema {
    Types& T;
    Features F;
    bool warn;
    SemaResult R;
    std::deque<Sym> syms;
    std::vector<std::map<std::string, Sym*>> scopes;
    std::map<std::string, ProcSig> procs;
    std::map<std::string, StructInfo*> structs;
    ProcSig* cur = nullptr;          // procedure being checked, null for the main program
    bool in_prelude = false;
    int  loop_depth = 0;
    int  item = 0;                   // current top-level item index
    std::vector<CallSite> main_calls;
    std::vector<std::pair<std::string, Pos>> main_locals;
    bool heap_declared = false;      // 'heap N;' seen: keep its size

    Sema(Types& t, const Features& f, bool w) : T(t), F(f), warn(w) {}

    // ---- diagnostics ------------------------------------------------------------------
    void error(Pos p, const std::string& m) {
        for (const Diag& d : R.errors) if (d.pos.off == p.off && d.pos.file == p.file && d.msg == m) return;
        R.errors.push_back(Diag{p, m});
        if (R.errors.size() >= 20) throw Abort{};
    }
    std::set<int> quiet_files;
    std::vector<std::string> file_names;
    // " (first at FILE line N)" when the earlier declaration is in another file
    std::string first_at(Pos first, Pos now) const {
        if (first.file == now.file) return "";
        std::string f = first.file >= 0 && first.file < (int)file_names.size() ? file_names[(size_t)first.file] : std::string("the prelude");
        return " (first at " + f + " line " + std::to_string(first.line) + ")";
    }
    void warning(Pos p, const std::string& m) { if (warn && !in_prelude && !quiet_files.count(p.file)) R.warnings.push_back(Diag{p, m}); }
    void need_heap() { if (!in_prelude) R.uses_heap = true; }
    std::map<std::string, int64_t> strings;     // literal -> heap address
    const std::map<std::string, int64_t>* string_at = nullptr;   // addresses given by the caller (analyse)
    int64_t heap_alloc(int64_t bytes, Pos p) {   // static heap space for arrays and literals
        int64_t a = R.heap_static_end;
        if (a + bytes > 65535) { error(p, "the static heap data does not fit in 64 KB"); return 7; }
        R.heap_static_end = (int)(a + bytes);
        return a;
    }

    const bf::RegisterInfo* find_reg(const std::string& n) const {
        for (const bf::RegisterInfo& r : bf::register_map()) if (n == r.name) return &r;
        return nullptr;
    }

    // ---- scopes ------------------------------------------------------------------------
    Sym* lookup(const std::string& n) {
        for (size_t i = scopes.size(); i-- > 0;) {
            auto it = scopes[i].find(n);
            if (it != scopes[i].end()) return it->second;
        }
        return nullptr;
    }
    Sym* declare(const std::string& n, Pos p, Sym::K k) {
        if (find_reg(n)) { error(p, "'" + n + "' is a register name"); return nullptr; }
        auto& sc = scopes.back();
        if (sc.count(n)) { error(p, "'" + n + "' is already declared in this scope" + first_at(sc[n]->pos, p)); return nullptr; }
        syms.push_back(Sym{});
        Sym* s = &syms.back();
        s->k = k; s->name = n; s->pos = p;
        s->global = scopes.size() == 1 && !cur;
        s->item = s->global ? item : -1;
        s->quiet = in_prelude || n.rfind("__", 0) == 0;
        sc[n] = s;
        if (cur) { if (!s->global) cur->locals.emplace_back(n, p); }
        else if (!in_prelude) main_locals.emplace_back(n, p);
        return s;
    }
    void push() { scopes.emplace_back(); }
    void pop() {
        for (auto& kv : scopes.back()) {
            Sym* s = kv.second;
            if (s->k == Sym::Var && !s->used && !s->param && !s->quiet && !s->global) warning(s->pos, "variable '" + s->name + "' is never read");
        }
        scopes.pop_back();
    }
    void use(Sym* s) {
        s->used = true;
        if (s->global && cur) cur->globals.insert(s);
    }

    // ---- types ---------------------------------------------------------------------------
    const Type* resolve(const TypeSpec& ts) {
        const Type* t = T.by_name(ts.base);
        if (t && t->is_int() && t != T.u8() && t != T.u16() && !F.wide_ints && !in_prelude) {
            error(ts.pos, "type '" + ts.base + "' is not available yet"); return T.error();
        }
        if (ts.base == "string" && !F.heap && !in_prelude) { error(ts.pos, "type 'string' is not available yet"); return T.error(); }
        if (!t) {
            auto it = structs.find(ts.base);
            if (it == structs.end()) { error(ts.pos, "unknown type '" + ts.base + "'"); return T.error(); }
            t = T.struct_type(it->second);
        }
        for (int i = 0; i < ts.ptr; i++) {
            if (!F.heap && !in_prelude) { error(ts.pos, "pointers are not available yet"); return T.error(); }
            t = T.ptr(t);
        }
        return t;
    }
    static const Type* natural(const Types& T, int64_t v, bool is_long = false) {
        if (is_long) return fits(v, T.s32()) ? T.s32() : T.u32();
        if (v >= 0) { if (v <= 255) return T.u8(); if (v <= 65535) return T.u16(); return T.u32(); }
        if (v >= -128) return T.s8(); if (v >= -32768) return T.s16(); return T.s32();
    }
    const Type* const_type(int64_t v, bool is_long = false) {
        const Type* t = natural(T, v, is_long);
        if (t->size > 2 && !F.wide_ints && !in_prelude) return T.u16();
        if (t->is_signed && !F.wide_ints && !in_prelude) return t->size == 1 ? T.u8() : T.u16();
        return t;
    }
    // The type two integer operands are converted to.
    const Type* common(const Expr& a, const Expr& b) {
        const Type *ta = a.type, *tb = b.type;
        if (a.is_const && !b.is_const && fits_either(a.cval, tb)) return tb;
        if (b.is_const && !a.is_const && fits_either(b.cval, ta)) return ta;
        if (ta->size != tb->size) return ta->size > tb->size ? ta : tb;
        if (ta->is_signed && tb->is_signed) return ta;
        if (ta->is_signed != tb->is_signed && !a.is_const && !b.is_const)
            warning(a.pos, "comparing or combining signed '" + (ta->is_signed ? ta : tb)->name + "' with unsigned '" +
                           (ta->is_signed ? tb : ta)->name + "' treats both as unsigned");
        return ta->is_signed ? tb : ta;
    }
    static bool fits_either(int64_t v, const Type* t) {
        if (!t->is_int()) return false;
        int bits = t->size * 8;
        return v >= -(int64_t(1) << (bits - 1)) && v <= (int64_t(1) << bits) - 1;
    }
    // Can a value of expression e be stored in type `to`?  Reports errors and warnings.
    void assignable(const Expr& e, const Type* to, Pos p, const char* what = "value") {
        const Type* from = e.type;
        if (from->is_error() || to->is_error()) return;
        if (to->kind == TK::Array) { error(p, "cannot assign to an array"); return; }
        if (to->is_int()) {
            if (from->is_int()) {
                if (e.is_const) {
                    if (to->is_signed ? !fits(e.cval, to) : !fits_either(e.cval, to)) error(e.pos, what + std::string(" ") + std::to_string(e.cval) + " does not fit " + (to->name[0] == 'i' ? "an " : "a ") + to->name);
                } else if (from->size > to->size) warning(e.pos, "value of type '" + from->name + "' is truncated to '" + to->name + "'");
                return;
            }
            error(e.pos, "cannot convert '" + from->name + "' to '" + to->name + "' without a cast");
            return;
        }
        if (to->is_ptr()) {
            if (e.kind == Expr::Null || (e.is_const && e.cval == 0 && from->is_int())) return;
            if (from->is_ptr()) {
                if (from != to && from->elem != T.u8() && to->elem != T.u8())
                    warning(e.pos, "converting '" + from->name + "' to '" + to->name + "'");
                return;
            }
            error(e.pos, "cannot convert '" + from->name + "' to '" + to->name + "' without a cast");
            return;
        }
        if (to->kind == TK::Struct) {
            if (from != to) error(e.pos, "cannot convert '" + from->name + "' to '" + to->name + "'");
            return;
        }
        error(e.pos, "cannot store a value of type '" + from->name + "' here");
    }

    // ---- constant folding --------------------------------------------------------------------------
    void make_const(Expr& e, int64_t v) {
        e.is_const = true;
        if (e.type && e.type->is_int()) {
            // constant of a fixed type wraps; flexible constants keep their exact value
            e.cval = v;
        } else e.cval = v;
    }
    static std::optional<int64_t> fold_binary(const std::string& o, int64_t a, int64_t b) {
        if (o == "+") return a + b;
        if (o == "-") return a - b;
        if (o == "*") return (int64_t)((uint64_t)a * (uint64_t)b);
        if (o == "/") return b ? a / b : 0;
        if (o == "%") return b ? a % b : a;
        if (o == "<<") return (b >= 32 || b < 0) ? 0 : (int64_t)((uint64_t)a << b);
        if (o == ">>") return (b >= 32 || b < 0) ? (a < 0 ? -1 : 0) : a >> b;
        if (o == "&") return a & b;
        if (o == "|") return a | b;
        if (o == "^") return a ^ b;
        if (o == "==") return a == b;
        if (o == "!=") return a != b;
        if (o == "<") return a < b;
        if (o == "<=") return a <= b;
        if (o == ">") return a > b;
        if (o == ">=") return a >= b;
        if (o == "&&") return (a && b) ? 1 : 0;
        if (o == "||") return (a || b) ? 1 : 0;
        return std::nullopt;
    }

    // ---- lvalues ------------------------------------------------------------------------------------
    // Is the storage behind this lvalue in the heap (so it has an address)?
    bool in_heap(const Expr& e) {
        switch (e.kind) {
        case Expr::Deref: return true;
        case Expr::Index: {
            const Type* bt = e.a->type;
            if (bt && bt->is_ptr()) return true;
            if (e.a->kind == Expr::Name) { Sym* s = lookup(e.a->text); return s && s->in_heap; }
            return in_heap(*e.a);
        }
        case Expr::Field: return e.arrow || in_heap(*e.a);
        default: return false;
        }
    }
    bool is_lvalue(const Expr& e) {
        switch (e.kind) {
        case Expr::Name: {
            Sym* s = lookup(e.text);
            if (s) return s->k == Sym::Var && s->type->kind != TK::Array;
            return find_reg(e.text) != nullptr;
        }
        case Expr::Index: case Expr::Deref: return true;
        case Expr::Field: return e.arrow || is_lvalue(*e.a) || e.a->kind == Expr::Index;
        default: return false;
        }
    }

    // ---- expressions ----------------------------------------------------------------------------------
    // Type-check e.  `cond`: the value is only tested (status registers allowed).
    const Type* expr(Expr& e, bool cond = false) {
        const Type* t = expr_inner(e, cond);
        e.type = t ? t : T.error();
        return e.type;
    }

    const Type* expr_inner(Expr& e, bool cond) {
        switch (e.kind) {
        case Expr::Num: {
            const Type* t = const_type(e.value, e.text == "L");
            if (!F.wide_ints && !in_prelude && e.value > 65535) { error(e.pos, "number too large for a word"); return T.error(); }
            e.is_const = true; e.cval = e.value;
            return t;
        }
        case Expr::Str: {
            if (!F.alloc) { error(e.pos, "a string can only appear in an out statement"); return T.error(); }
            need_heap();
            auto it = strings.find(e.text);
            int64_t a;
            if (it != strings.end()) a = it->second;
            else {
                auto given = string_at ? string_at->find(e.text) : std::map<std::string, int64_t>::const_iterator{};
                a = string_at && given != string_at->end() ? given->second : heap_alloc((int64_t)e.text.size() + 1, e.pos);
                strings[e.text] = a;
                R.heap_init.emplace_back(a, e.text + std::string(1, '\0'));
            }
            e.is_const = true; e.cval = a;
            return T.ptr(T.u8());
        }
        case Expr::Null:
            if (!F.heap) { error(e.pos, "'null' is not available yet"); return T.error(); }
            e.is_const = true; e.cval = 0;
            return T.ptr(T.u8());
        case Expr::In: return T.u8();
        case Expr::Input: return T.u16();
        case Expr::Sizeof: {
            if (!F.structs && !F.heap) { error(e.pos, "'sizeof' is not available yet"); return T.error(); }
            const Type* t = resolve(e.ts);
            if (t->is_error()) return t;
            e.is_const = true; e.cval = t->size;
            return const_type(t->size);
        }
        case Expr::New: {
            if (!F.alloc) { error(e.pos, "'new' is not available yet"); return T.error(); }
            need_heap();
            const Type* t = resolve(e.ts);
            if (e.b) { const Type* n = expr(*e.b); if (!n->is_int() && !n->is_error()) error(e.b->pos, "the element count must be an integer"); else if (e.b->type->size > 2) warning(e.b->pos, "element count is truncated to 'word'"); }
            if (t->is_error()) return t;
            note_call("alloc", e.pos);
            return T.ptr(t);
        }
        case Expr::Name: {
            Sym* s = lookup(e.text);
            if (s) {
                use(s);
                if (s->k == Sym::Const) { e.is_const = true; e.cval = s->cval; return s->type; }
                if (s->type->kind == TK::Array) {
                    if (s->in_heap) { e.is_const = true; e.cval = s->addr; return T.ptr(s->type->elem); }   // a constant pointer
                    error(e.pos, "array '" + e.text + "' needs an index"); return T.error();
                }
                return s->type;
            }
            if (const bf::RegisterInfo* r = find_reg(e.text)) {
                if (r->kind == bf::REG_TRIGGER) { error(e.pos, std::string(r->name) + " is write-only"); return T.error(); }
                (void)cond;
                return T.u8();
            }
            if (procs.count(e.text)) { error(e.pos, "'" + e.text + "' is a procedure; call it with ()"); return T.error(); }
            error(e.pos, "'" + e.text + "' is not declared");
            return T.error();
        }
        case Expr::Index: return index(e);
        case Expr::Field: return field(e);
        case Expr::Deref: {
            if (!F.heap) { error(e.pos, "pointers are not available yet"); return T.error(); }
            const Type* t = expr(*e.a);
            if (t->is_error()) return t;
            if (!t->is_ptr()) { error(e.pos, "'*' needs a pointer, not '" + t->name + "'"); return T.error(); }
            need_heap();
            return t->elem;
        }
        case Expr::AddrOf: {
            if (!F.heap) { error(e.pos, "'&' is not available yet"); return T.error(); }
            const Type* t = expr(*e.a);
            if (t->is_error()) return t;
            if (!in_heap(*e.a)) { error(e.pos, "'&' needs something in the heap; variables held in cells have no address"); return T.error(); }
            return T.ptr(t);
        }
        case Expr::Cast: {
            const Type* to = resolve(e.ts);
            const Type* from = expr(*e.a);
            if (to->is_error() || from->is_error()) return T.error();
            bool ok = (to->is_scalar() && from->is_scalar());
            if (!ok) { error(e.pos, "cannot cast '" + from->name + "' to '" + to->name + "'"); return T.error(); }
            if (e.a->is_const && to->is_int()) { e.is_const = true; e.cval = wrap(e.a->cval, to); }
            return to;
        }
        case Expr::Call: {
            const Type* r = call(e.text, e.args, e.pos, true);
            return r;
        }
        case Expr::Unary: {
            const Type* t = expr(*e.a, e.text == "!");
            if (t->is_error()) return t;
            if (e.text == "!") {
                if (!t->is_scalar()) { error(e.pos, "'!' needs a number or a pointer"); return T.error(); }
                if (e.a->is_const) { e.is_const = true; e.cval = e.a->cval == 0; }
                return T.u8();
            }
            if (!t->is_int()) { error(e.pos, "'" + e.text + "' needs a number, not '" + t->name + "'"); return T.error(); }
            if (e.a->is_const) {
                e.is_const = true;
                e.cval = e.text == "-" ? -e.a->cval : ~e.a->cval;
                if (e.text == "~") e.cval = wrap(e.cval, t->is_signed ? t : T.integer(t->size, false));
                return e.text == "-" ? const_type(e.cval) : t;
            }
            return t;
        }
        case Expr::Binary: return binary(e, cond);
        case Expr::MulShr: return mulshr(e);
        }
        return T.error();
    }

    // mulshr(a, b, k): a and b converted to their common type, multiplied exactly and shifted right by
    // the constant k (rounding down); the result has that type.  Two constants fold exactly.
    const Type* mulshr(Expr& e) {
        const Type* ta = expr(*e.a);
        const Type* tb = expr(*e.b);
        Expr& k = *e.args[0];
        const Type* tk = expr(k);
        if (ta->is_error() || tb->is_error() || tk->is_error()) return T.error();
        if (!ta->is_int() || !tb->is_int()) { error(e.pos, "mulshr needs two integers ('" + ta->name + "' and '" + tb->name + "')"); return T.error(); }
        if (!tk->is_int() || !k.is_const) { error(k.pos, "the shift count of mulshr must be a constant"); return T.error(); }
        if (k.cval < 0) { error(k.pos, "the shift count of mulshr cannot be negative"); return T.error(); }
        e.value = std::min<int64_t>(k.cval, 64);
        const Type* c = common(*e.a, *e.b);
        e.optype = c;
        if (e.a->is_const && e.b->is_const) {
            int64_t x = e.a->cval, y = e.b->cval;           // each in [-2^31, 2^32), so |x * y| < 2^64
            bool negp = (x < 0) != (y < 0) && x && y;
            uint64_t m = (uint64_t)(x < 0 ? -x : x) * (uint64_t)(y < 0 ? -y : y);
            uint64_t qm = e.value >= 64 ? 0 : m >> e.value;
            int64_t v;
            if (!negp) v = qm > 0xFFFFFFFFull ? (int64_t)(qm & 0xFFFFFFFFull) : (int64_t)qm;
            else {
                bool rest = e.value >= 64 ? m != 0 : (m & ((1ull << e.value) - 1)) != 0;
                v = -(int64_t)qm - (rest ? 1 : 0);             // floor of a negative quotient
                if (v < -0x80000000ll) v = wrap(v, T.u32());
            }
            e.is_const = true; e.cval = v;
            return const_type(v);
        }
        return c;
    }

    const Type* binary(Expr& e, bool cond) {
        const std::string& o = e.text;
        bool logic = o == "&&" || o == "||";
        bool cmp = o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=";
        const Type* ta = expr(*e.a, logic && cond);
        const Type* tb = expr(*e.b, logic && cond);
        if (ta->is_error() || tb->is_error()) return T.error();
        if (logic) {
            if (!ta->is_scalar() || !tb->is_scalar()) { error(e.pos, "'" + o + "' needs numbers or pointers"); return T.error(); }
            if (e.a->is_const && e.b->is_const) { e.is_const = true; e.cval = *fold_binary(o, e.a->cval, e.b->cval); }
            e.optype = T.u8();
            return T.u8();
        }
        if (ta->is_ptr() || tb->is_ptr()) {
            if (cmp) {
                bool ok = (ta->is_ptr() && tb->is_ptr()) || (ta->is_ptr() && e.b->is_const && e.b->cval == 0) || (tb->is_ptr() && e.a->is_const && e.a->cval == 0);
                if (!ok) { error(e.pos, "cannot compare '" + ta->name + "' with '" + tb->name + "'"); return T.error(); }
                e.optype = T.u16();
                return T.u8();
            }
            if (o == "+" && ta->is_ptr() && tb->is_int()) { e.optype = T.u16(); return ta; }
            if (o == "+" && tb->is_ptr() && ta->is_int()) { e.optype = T.u16(); return tb; }
            if (o == "-" && ta->is_ptr() && tb->is_int()) { e.optype = T.u16(); return ta; }
            if (o == "-" && ta->is_ptr() && tb->is_ptr()) {
                if (ta != tb) { error(e.pos, "cannot subtract '" + tb->name + "' from '" + ta->name + "'"); return T.error(); }
                e.optype = T.u16(); return T.s16();
            }
            error(e.pos, "invalid operands to '" + o + "' ('" + ta->name + "' and '" + tb->name + "')");
            return T.error();
        }
        if (!ta->is_int() || !tb->is_int()) { error(e.pos, "invalid operands to '" + o + "' ('" + ta->name + "' and '" + tb->name + "')"); return T.error(); }
        bool shift = o == "<<" || o == ">>";
        const Type* c = shift ? ta : common(*e.a, *e.b);       // a shift has its left operand's type
        e.optype = c;
        if (e.a->is_const && e.b->is_const) {
            e.is_const = true;
            e.cval = *fold_binary(o, e.a->cval, e.b->cval);
            if (cmp) return T.u8();
            if (e.cval > 0xFFFFFFFFll || e.cval < -0x80000000ll) e.cval = wrap(e.cval, T.u32());
            return const_type(e.cval);
        }
        return cmp ? T.u8() : c;
    }

    const Type* index(Expr& e) {
        Expr& base = *e.a;
        const Type* elem = nullptr;
        if (base.kind == Expr::Name) {
            Sym* s = lookup(base.text);
            if (!s) {
                if (find_reg(base.text)) { error(e.pos, "'" + base.text + "' is a register, not an array"); }
                else error(e.pos, "'" + base.text + "' is not declared");
                expr(*e.b);
                return T.error();
            }
            use(s);
            if (s->k == Sym::Var && s->type->kind == TK::Array) {
                elem = s->type->elem;
                base.type = s->in_heap ? T.ptr(elem) : s->type;
                if (s->in_heap) { need_heap(); base.is_const = true; base.cval = s->addr; }
                const Type* it = expr(*e.b);
                if (!it->is_int() && !it->is_error()) error(e.b->pos, "an index must be an integer");
                if (e.b->is_const && (e.b->cval < 0 || e.b->cval >= s->type->count))
                    error(e.b->pos, "index " + std::to_string(e.b->cval) + " is outside '" + base.text + "[" + std::to_string(s->type->count) + "]'");
                return elem;
            }
            if (s->k == Sym::Var && s->type->is_ptr()) {
                base.type = s->type;
                need_heap();
                const Type* it = expr(*e.b);
                if (!it->is_int() && !it->is_error()) error(e.b->pos, "an index must be an integer");
                return s->type->elem;
            }
            error(e.pos, "'" + base.text + "' is not an array");
            expr(*e.b);
            return T.error();
        }
        const Type* bt = expr(base);
        const Type* it = expr(*e.b);
        if (bt->is_error()) return bt;
        if (!it->is_int() && !it->is_error()) error(e.b->pos, "an index must be an integer");
        if (bt->is_ptr()) { need_heap(); return bt->elem; }
        if (bt->kind == TK::Array) {
            if (e.b->is_const && (e.b->cval < 0 || e.b->cval >= bt->count))
                error(e.b->pos, "index " + std::to_string(e.b->cval) + " is outside the array of " + std::to_string(bt->count));
            if (!in_heap(base) && !e.b->is_const) {
                const Expr* root = &base;
                while (root->kind == Expr::Field || root->kind == Expr::Index) root = root->a.get();
                Sym* s = root->kind == Expr::Name ? lookup(root->text) : nullptr;
                // An array parameter may be bound to a heap array, and the local arrays of a re-entrant
                // procedure live in the heap: the code generator, which knows, checks those.
                if (root->kind == Expr::Call)
                    error(e.b->pos, "an array field of a returned struct needs a constant index");
                else if (!(s && cur && !s->global && s->type->kind == TK::Array))
                    error(e.b->pos, "an array field of a struct held in cells needs a constant index; put the struct in the heap");
            }
            return bt->elem;
        }
        error(e.pos, "only arrays and pointers can be indexed");
        return T.error();
    }

    const Type* field(Expr& e) {
        if (!F.structs) { error(e.pos, "structs are not available yet"); return T.error(); }
        Expr& base = *e.a;
        const Type* bt;
        if (base.kind == Expr::Name) {
            Sym* s = lookup(base.text);
            if (s && s->k == Sym::Var) { use(s); bt = s->type; base.type = bt; }
            else bt = expr(base);
        } else bt = expr(base);
        if (bt->is_error()) return bt;
        const Type* st = bt;
        if (e.arrow) {
            if (!bt->is_ptr() || bt->elem->kind != TK::Struct) { error(e.pos, "'->' needs a pointer to a struct, not '" + bt->name + "'"); return T.error(); }
            st = bt->elem;
            need_heap();
        } else if (bt->kind != TK::Struct) {
            if (bt->is_ptr() && bt->elem->kind == TK::Struct) error(e.pos, "'" + bt->name + "' is a pointer: use '->'");
            else error(e.pos, "'.' needs a struct, not '" + bt->name + "'");
            return T.error();
        }
        const Field* f = st->st->field(e.text);
        if (!f) { error(e.pos, "'" + st->name + "' has no field '" + e.text + "'"); return T.error(); }
        return f->type;
    }

    // Check a call; returns the result type (void when none).
    const Type* call(const std::string& name, std::vector<ExprP>& args, Pos p, bool as_value) {
        if (name == "alloc" || name == "free") {
            if (!F.alloc && !in_prelude) { error(p, "'" + name + "' is not available yet"); }
            need_heap();
        }
        auto it = procs.find(name);
        if (it == procs.end()) {
            for (auto& a : args) expr(*a);
            if (lookup(name)) error(p, "'" + name + "' is a variable, not a procedure");
            else error(p, "'" + name + "' is not a procedure");
            return T.error();
        }
        ProcSig& ps = it->second;
        const Stmt& d = *ps.decl;
        note_call(name, p);
        if (as_value && !ps.ret) {
            for (auto& a : args) expr(*a);
            error(p, "'" + name + "' returns nothing and cannot be used in an expression");
            return T.error();
        }
        if (args.size() != d.params.size()) {
            for (auto& a : args) expr(*a);
            error(p, "'" + name + "' takes " + std::to_string(d.params.size()) + " argument(s), " + std::to_string(args.size()) + " given");
            return ps.ret ? ps.ret : T.void_t();
        }
        for (size_t i = 0; i < args.size(); i++) {
            const Param& prm = d.params[i];
            Expr& a = *args[i];
            if (prm.ref) {
                if (prm.array) {
                    Sym* s = a.kind == Expr::Name ? lookup(a.text) : nullptr;
                    if (s && s->k == Sym::Var) use(s);
                    if (a.kind != Expr::Name || !s || s->k != Sym::Var) { error(a.pos, "argument for 'ref " + prm.name + "' must be a variable"); continue; }
                    if (s->type->kind != TK::Array || s->type->elem != prm.type) {
                        if (s->type->kind == TK::Array || prm.type == T.u8()) error(a.pos, "'" + prm.name + "' needs a " + prm.type->name + " array");
                        else error(a.pos, "'" + prm.name + "' needs a " + prm.type->name + " array");
                        continue;
                    }
                    a.type = s->type;
                    continue;
                }
                if (a.kind == Expr::Name) {
                    Sym* s = lookup(a.text);
                    if (!s || s->k != Sym::Var) { error(a.pos, "argument for 'ref " + prm.name + "' must be a variable"); if (s) use(s); continue; }
                    use(s);
                    a.type = s->type;
                    if (s->type->kind == TK::Array) { error(a.pos, "'" + prm.name + "' needs a " + prm.type->name + ", not an array"); continue; }
                    if (s->type != prm.type) error(a.pos, "'" + prm.name + "' needs a " + prm.type->name + " variable");
                    continue;
                }
                if (!is_lvalue(a) || a.kind == Expr::Num) { expr(a); error(a.pos, "argument for 'ref " + prm.name + "' must be a variable"); continue; }
                const Type* t = expr(a);
                if (t->is_error()) continue;
                if (t != prm.type) error(a.pos, "'" + prm.name + "' needs a " + prm.type->name + " variable");
                continue;
            }
            const Type* t = expr(a);
            if (t->is_error()) continue;
            if (prm.type && !prm.type->is_error()) assignable(a, prm.type, a.pos, "argument");
        }
        return ps.ret ? ps.ret : T.void_t();
    }
    void note_call(const std::string& callee, Pos p) {
        if (cur) cur->callees.push_back(callee);
        else if (!in_prelude) main_calls.push_back(CallSite{callee, p, item});
    }

    // ---- statements ---------------------------------------------------------------------------------
    void block(std::vector<StmtP>& v) {
        push();
        stmts(v);
        pop();
    }
    void stmts(std::vector<StmtP>& v) {
        bool dead = false, warned = false;
        for (auto& s : v) {
            if (dead && !warned && s->kind != Stmt::Proc && s->kind != Stmt::Struct) { warning(s->pos, "unreachable code"); warned = true; }
            stmt(*s);
            if (s->kind == Stmt::Return || s->kind == Stmt::Break || s->kind == Stmt::Continue) dead = true;
        }
    }
    bool top_level() const { return scopes.size() == 1 && !cur; }

    void stmt(Stmt& s) {
        try { stmt_inner(s); }
        catch (const Abort&) { throw; }
    }

    // Resolve a declaration's type and array length.
    void decl_type(Stmt& s) {
        const Type* t = resolve(s.ts);
        s.vtype = t;
        if (s.count_e) {
            const Type* ct = expr(*s.count_e);
            if (!ct->is_error()) {
                if (!s.count_e->is_const) { error(s.count_e->pos, "array length must be a constant"); s.count = 1; }
                else if (s.count_e->cval < 1 || s.count_e->cval > 65535) { error(s.count_e->pos, "array length out of range"); s.count = 1; }
                else s.count = (int)s.count_e->cval;
            } else s.count = 1;
        }
    }

    void stmt_inner(Stmt& s) {
        switch (s.kind) {
        case Stmt::Import: if (!top_level()) error(s.pos, "'import' must appear at the top level"); return;
        case Stmt::Struct: struct_decl(s); return;
        case Stmt::Proc:
            if (!top_level()) error(s.pos, "procedures must be declared at the top level");
            proc_body(s);
            return;
        case Stmt::Stack: case Stmt::Heap: {
            const char* kw = s.kind == Stmt::Stack ? "stack" : "heap";
            if (!top_level()) { error(s.pos, std::string("'") + kw + "' must appear at the top level"); return; }
            const Type* t = expr(*s.count_e);
            if (t->is_error()) return;
            if (!s.count_e->is_const) { error(s.count_e->pos, std::string("the ") + kw + " size must be a constant"); return; }
            int64_t v = s.count_e->cval;
            if (s.kind == Stmt::Stack) {
                if (v < 4 || v > 32768) { error(s.count_e->pos, "stack size must be 4..32768 cells"); return; }
                R.stack_size = (int)v;
            } else {
                if (!F.heap) { error(s.pos, "the heap is not available yet"); return; }
                if (v < 16 || v > 65535) { error(s.count_e->pos, "heap size must be 16..65535 bytes"); return; }
                R.heap_size = (int)v;
                heap_declared = true;
            }
            s.count = (int)v;
            return;
        }
        case Stmt::Const: {
            const Type* t = expr(*s.e);
            if (t->is_error()) { declare(s.name, s.pos, Sym::Const); return; }
            if (!s.e->is_const) { error(s.e->pos, "constant '" + s.name + "' needs a constant expression"); return; }
            Sym* sym = declare(s.name, s.pos, Sym::Const);
            if (!sym) return;
            sym->cval = s.e->cval; sym->type = t; sym->used = true;
            s.cval = s.e->cval; s.ctype = t;
            return;
        }
        case Stmt::Decl: {
            decl_type(s);
            const Type* t = s.vtype;
            int64_t haddr = 0;
            if (s.in_heap) {
                if (!F.heap) error(s.pos, "heap variables are not available yet");
                if (!top_level()) error(s.pos, "heap arrays must be declared at the top level");
                if (!s.count_e) error(s.pos, "'heap' declares an array: give it a length");
                need_heap();
                if (!t->is_error() && s.count > 0) haddr = heap_alloc((int64_t)t->size * s.count, s.pos);
                s.cval = haddr;
            }
            if (s.count_e) {
                if (!t->is_error() && t != T.u8() && !F.structs && !s.in_heap) error(s.pos, std::string(t->is_int() ? "word" : t->name.c_str()) + " arrays are not supported; use two byte arrays");
                if (!t->is_error() && t->kind == TK::Void) error(s.pos, "an array needs an element type");
            }
            if (s.e) {
                const Type* it = expr(*s.e);
                if (!it->is_error() && !t->is_error()) assignable(*s.e, t, s.e->pos);
            }
            if (cur && s.count_e) cur->has_array_local = true;
            Sym* sym = declare(s.name, s.pos, Sym::Var);
            if (!sym) return;
            sym->type = s.count_e && !t->is_error() ? T.array(t, std::max(s.count, 1)) : t;
            sym->in_heap = s.in_heap;
            sym->addr = haddr;
            if (sym->global) sym->used = sym->used || sym->quiet;
            return;
        }
        case Stmt::Assign: assign(s); return;
        case Stmt::IncDec: {
            const Type* t = lvalue(*s.target, "++");
            if (!t->is_error() && !t->is_scalar()) error(s.pos, "'" + s.op + "' needs a number or a pointer");
            return;
        }
        case Stmt::If: {
            cond(*s.e);
            block(s.body);
            if (!s.else_body.empty()) {
                if (s.else_body.size() == 1 && s.else_body[0]->kind == Stmt::If) stmt(*s.else_body[0]);
                else block(s.else_body);
            }
            return;
        }
        case Stmt::While: {
            cond(*s.e);
            loop_depth++; block(s.body); loop_depth--;
            return;
        }
        case Stmt::For: {
            push();
            if (s.init) stmt(*s.init);
            if (s.e) cond(*s.e);
            loop_depth++; block(s.body); loop_depth--;
            if (s.step) stmt(*s.step);
            pop();
            return;
        }
        case Stmt::Break: case Stmt::Continue:
            if (!loop_depth) error(s.pos, std::string(s.kind == Stmt::Break ? "'break'" : "'continue'") + " outside a loop");
            return;
        case Stmt::Return: {
            if (s.e) expr(*s.e);
            if (!cur) {
                if (s.e) error(s.pos, "the main program returns nothing");
                return;
            }
            if (cur->ret && !s.e) error(s.pos, "this procedure returns a value");
            else if (!cur->ret && s.e) error(s.pos, "this procedure returns nothing");
            else if (s.e && !s.e->type->is_error()) assignable(*s.e, cur->ret, s.e->pos, "return value");
            return;
        }
        case Stmt::Out:
            for (auto& a : s.args) {
                if (a->kind == Expr::Str) { a->type = T.u8(); continue; }
                const Type* t = expr(*a);
                if (!t->is_error() && !t->is_int()) error(a->pos, "'out' writes bytes; use print for '" + t->name + "'");
                else if (!t->is_error() && t->size > 1 && !a->is_const) warning(a->pos, "'out' writes the low byte of '" + t->name + "'");
            }
            return;
        case Stmt::Print:
            for (auto& a : s.args) {
                if (a->kind == Expr::Str) { a->type = T.u8(); continue; }
                const Type* t = expr(*a);
                if (!t->is_error() && !t->is_int() && !(t->is_ptr() && t->elem == T.u8()))
                    error(a->pos, "print takes numbers and strings, not '" + t->name + "'");
            }
            return;
        case Stmt::Pulse: case Stmt::Halt: {
            if (s.kind == Stmt::Halt) return;
            Sym* v = lookup(s.name);
            if (v) error(s.pos, "'" + s.name + "' is a variable, not a register");
            else if (!find_reg(s.name)) error(s.pos, "'" + s.name + "' is not a register");
            return;
        }
        case Stmt::Asm: return;
        case Stmt::Call: call(s.name, s.args, s.pos, false); return;
        case Stmt::Block: block(s.body); return;
        }
    }

    void cond(Expr& e) {
        const Type* t = expr(e, true);
        if (!t->is_error() && !t->is_scalar()) error(e.pos, "a condition must be a number or a pointer");
    }

    // Check an assignment target; returns its type.
    const Type* lvalue(Expr& t, const std::string& op) {
        if (t.kind == Expr::Name) {
            Sym* s = lookup(t.text);
            if (s) {
                if (s->k == Sym::Const) { error(t.pos, "'" + t.text + "' is a constant"); return T.error(); }
                if (s->type->kind == TK::Array) { error(t.pos, s->in_heap ? "a heap array cannot be assigned" : "array '" + t.text + "' needs an index"); return T.error(); }
                if (op != "=") use(s); else if (s->global && cur) cur->globals.insert(s);
                t.type = s->type;
                return s->type;
            }
            if (find_reg(t.text)) { t.type = T.u8(); return T.u8(); }
            error(t.pos, "'" + t.text + "' is not declared");
            return T.error();
        }
        if (t.kind == Expr::Index || t.kind == Expr::Field || t.kind == Expr::Deref) {
            const Type* ty = expr(t);
            if (ty->kind == TK::Array) { error(t.pos, "cannot assign to an array"); return T.error(); }
            return ty;
        }
        error(t.pos, "this cannot be assigned to");
        return T.error();
    }

    void assign(Stmt& s) {
        Expr& tg = *s.target;
        const bf::RegisterInfo* reg = (tg.kind == Expr::Name && !lookup(tg.text)) ? find_reg(tg.text) : nullptr;
        const Type* lt = lvalue(tg, s.op);
        const Type* rt = expr(*s.e);
        if (lt->is_error() || rt->is_error()) return;
        if (reg && reg->kind == bf::REG_SWITCH) {
            if (!s.e->is_const) error(s.e->pos, std::string(reg->name) + " only accepts a constant");
            return;
        }
        if (reg && reg->kind != bf::REG_LATCH) {
            if (s.op != "+=" && s.op != "-=") error(s.pos, std::string(reg->name) + " cannot be assigned: write it with 'pulse " + reg->name + "' or '" + reg->name + " += 1'");
            else if (!s.e->is_const) error(s.e->pos, std::string(reg->name) + " only accepts a constant with += or -=");
            return;
        }
        if (s.op == "=") { assignable(*s.e, lt, s.e->pos); return; }
        if (lt->kind == TK::Struct) { error(s.pos, "'" + s.op + "' does not apply to a struct"); return; }
        if (lt->is_ptr()) {
            if ((s.op == "+=" || s.op == "-=") && rt->is_int()) return;
            error(s.pos, "'" + s.op + "' does not apply to a pointer");
            return;
        }
        if (!rt->is_int()) { error(s.e->pos, "'" + s.op + "' needs a number"); return; }
    }

    void struct_decl(Stmt& s) {
        if (!F.structs) { error(s.pos, "structs are not available yet"); return; }
        if (!top_level()) { error(s.pos, "structs must be declared at the top level"); return; }
        if (structs.count(s.name) || T.by_name(s.name)) { error(s.pos, "type '" + s.name + "' is already defined"); return; }
        StructInfo* st = T.new_struct(s.name);
        structs[s.name] = st;            // visible to its own fields (only as a pointer)
        int off = 0;
        for (FieldDecl& f : s.fields) {
            const Type* ft = resolve(f.ts);
            if (ft->is_error()) continue;
            if (ft->kind == TK::Struct && ft->st == st) { error(f.pos, "a struct cannot contain itself; use a pointer"); continue; }
            if (ft->kind == TK::Void) { error(f.pos, "a field needs a type"); continue; }
            if (f.count) {
                const Type* ct = expr(*f.count);
                if (ct->is_error()) continue;
                if (!f.count->is_const || f.count->cval < 1 || f.count->cval > 4096) { error(f.count->pos, "an array field needs a constant length of 1..4096"); continue; }
                ft = T.array(ft, (int)f.count->cval);
            }
            if (st->field(f.name)) { error(f.pos, "duplicate field '" + f.name + "'"); continue; }
            st->fields.push_back(Field{f.name, ft, off});
            off += ft->size;
        }
        if (s.fields.empty()) error(s.pos, "struct '" + s.name + "' has no fields");
        st->size = std::max(off, 1);
        T.finish_struct(st);
    }

    // ---- procedures ----------------------------------------------------------------------------------
    void signature(Stmt& s, bool prelude) {
        if (procs.count(s.name)) { error(s.pos, "procedure '" + s.name + "' is already defined" + first_at(procs[s.name].decl->pos, s.pos)); return; }
        if (find_reg(s.name)) { error(s.pos, "'" + s.name + "' is a register name"); return; }
        ProcSig ps;
        ps.decl = &s; ps.prelude = prelude; ps.item = prelude ? -1 : item;
        if (!s.ts.empty()) {
            ps.ret = resolve(s.ts);
            if (ps.ret->kind == TK::Struct && !F.structs) ps.ret = T.error();
        }
        s.vtype = ps.ret;
        for (size_t i = 0; i < s.params.size(); i++) {
            Param& prm = s.params[i];
            for (size_t j = 0; j < i; j++) if (s.params[j].name == prm.name) error(prm.pos, "duplicate parameter '" + prm.name + "'");
            prm.type = resolve(prm.ts);
            if (prm.array && !prm.ref) error(prm.pos, "array parameters must be 'ref'");
            if (prm.array && prm.type != T.u8() && !F.structs && !prm.type->is_error()) error(prm.pos, "word arrays are not supported");
        }
        procs[s.name] = std::move(ps);
    }

    void proc_body(Stmt& s) {
        auto it = procs.find(s.name);
        if (it == procs.end() || it->second.decl != &s) return;       // duplicate: already reported
        ProcSig& ps = it->second;
        auto saved_scopes = std::move(scopes);
        scopes.clear();
        scopes.push_back(saved_scopes.front());                       // globals declared so far
        ProcSig* saved_cur = cur; cur = &ps;
        int saved_loop = loop_depth; loop_depth = 0;
        bool saved_prelude = in_prelude; in_prelude = ps.prelude;
        push();
        for (Param& prm : s.params) {
            Sym* sym = declare(prm.name, prm.pos, Sym::Var);
            if (!sym) continue;
            sym->type = prm.array ? T.array(prm.type, 256) : prm.type;
            sym->param = true; sym->ref = prm.ref;
        }
        try { stmts(s.body); } catch (const Abort&) { scopes = std::move(saved_scopes); throw; }
        pop();
        in_prelude = saved_prelude;
        loop_depth = saved_loop;
        cur = saved_cur;
        scopes = std::move(saved_scopes);
    }

    // ---- whole program -----------------------------------------------------------------------------------
    void run(Program& prelude, Program& prog) {
        scopes.clear();
        push();
        // structs first, so signatures can name them; then every signature
        in_prelude = true;
        for (auto& s : prelude.items) if (s->kind == Stmt::Proc) signature(*s, true);
        in_prelude = false;
        for (size_t i = 0; i < prog.items.size(); i++) {
            item = (int)i;
            Stmt& s = *prog.items[i];
            if (s.kind == Stmt::Struct) struct_decl(s);
        }
        for (size_t i = 0; i < prog.items.size(); i++) {
            item = (int)i;
            if (prog.items[i]->kind == Stmt::Proc) signature(*prog.items[i], false);
        }
        in_prelude = true;
        for (auto& s : prelude.items) if (s->kind == Stmt::Proc) proc_body(*s);
        in_prelude = false;
        for (size_t i = 0; i < prog.items.size(); i++) {
            item = (int)i;
            Stmt& s = *prog.items[i];
            if (s.kind == Stmt::Struct) continue;     // done above
            stmt(s);
        }
        // unused globals
        for (auto& kv : scopes.front()) {
            Sym* g = kv.second;
            if (g->k == Sym::Var && !g->used && !g->quiet) warning(g->pos, "variable '" + g->name + "' is never read");
        }
        call_graph();                    // also sizes a heap that was not declared
        if (R.uses_heap && R.heap_static_end > R.heap_size)
            error(Pos{}, "heap arrays and string literals need " + std::to_string(R.heap_static_end) + " bytes of heap but the heap has " +
                         std::to_string(R.heap_size) + "; declare a larger one with 'heap N;'");
    }

    void call_graph() {
        // non-rec recursion is an error
        for (auto& kv : procs) {
            if (kv.second.decl->rec) continue;
            std::vector<std::string> stack{kv.first};
            std::set<std::string> seen;
            bool cyc = false;
            while (!stack.empty() && !cyc) {
                std::string n = stack.back(); stack.pop_back();
                auto it = procs.find(n);
                if (it == procs.end()) continue;
                for (const std::string& c : it->second.callees) {
                    if (c == kv.first) { cyc = true; break; }
                    auto ci = procs.find(c);
                    if (ci != procs.end() && ci->second.decl->rec) continue;   // the cycle goes through a rec proc
                    if (seen.insert(c).second) stack.push_back(c);
                }
            }
            if (cyc) error(kv.second.decl->pos, "'" + kv.first + "' is recursive; declare it 'rec proc'");
        }
        // trampolined: rec and shared procs and everything that calls one
        for (auto& kv : procs) if (kv.second.decl->rec || kv.second.decl->shared) R.tramp.insert(kv.first);
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& kv : procs) {
                if (R.tramp.count(kv.first)) continue;
                for (const std::string& c : kv.second.callees) if (R.tramp.count(c)) { R.tramp.insert(kv.first); changed = true; break; }
            }
        }
        for (const CallSite& c : main_calls) if (R.tramp.count(c.callee)) R.main_tramp = true;
        // trampoline rules.  A trampolined procedure that can come back to itself through calls is
        // re-entrant (as codegen's compute_reach decides): only its local arrays live in the heap
        auto reaches = [&](const std::string& from, const std::string& to) {
            std::vector<std::string> work{from};
            std::set<std::string> seen;
            while (!work.empty()) {
                std::string n = work.back(); work.pop_back();
                auto it = procs.find(n);
                if (it == procs.end()) continue;
                for (const std::string& c : it->second.callees) {
                    if (c == to) return true;
                    if (R.tramp.count(c) && seen.insert(c).second) work.push_back(c);
                }
            }
            return false;
        };
        bool heap_frames = false;
        for (const std::string& n : R.tramp) {
            ProcSig& ps = procs[n];
            bool reentrant = reaches(n, n);
            check_tramp_names(ps.locals, "'" + n + "'", ps.decl, reentrant);
            if (reentrant && ps.has_array_local) heap_frames = true;
        }
        if (R.main_tramp) check_tramp_names(main_locals, "the main program", nullptr, false);
        // Without 'heap N;', a program that never takes heap space at run time (no alloc or free in
        // anything it can call, which covers new, and no re-entrant local arrays, which come off the
        // top of the heap) and does not ask how much is left (heap_unused) gets a heap just big
        // enough for its static data instead of 8 KB.
        if (!heap_declared && R.uses_heap && !heap_frames) {
            std::set<std::string> called;
            std::vector<std::string> work;
            for (const CallSite& c : main_calls) work.push_back(c.callee);
            while (!work.empty()) {
                std::string n = work.back(); work.pop_back();
                if (!called.insert(n).second) continue;
                auto it = procs.find(n);
                if (it != procs.end()) for (const std::string& c : it->second.callees) work.push_back(c);
            }
            if (!called.count("alloc") && !called.count("free") && !called.count("heap_unused"))   // it asks: keep the room
                R.heap_size = std::max(R.heap_static_end, 16);
        }
        // a procedure must not run before the globals it uses exist
        std::map<std::string, std::set<Sym*>> closure;
        std::function<const std::set<Sym*>&(const std::string&, std::set<std::string>&)> globals_of =
            [&](const std::string& n, std::set<std::string>& visiting) -> const std::set<Sym*>& {
                auto c = closure.find(n);
                if (c != closure.end()) return c->second;
                std::set<Sym*> out;
                auto it = procs.find(n);
                if (it != procs.end() && visiting.insert(n).second) {
                    out = it->second.globals;
                    for (const std::string& callee : it->second.callees) {
                        const std::set<Sym*>& sub = globals_of(callee, visiting);
                        out.insert(sub.begin(), sub.end());
                    }
                    visiting.erase(n);
                }
                return closure[n] = out;
            };
        for (const CallSite& c : main_calls) {
            std::set<std::string> visiting;
            for (Sym* g : globals_of(c.callee, visiting))
                if (g->item > c.item) error(c.pos, "'" + c.callee + "' uses '" + g->name + "', which is declared after this call");
        }
    }
    void check_tramp_names(const std::vector<std::pair<std::string, Pos>>& locals, const std::string& where, const Stmt* proc, bool reentrant) {
        std::map<std::string, Pos> seen;
        for (auto& l : locals) {
            if (seen.count(l.first)) error(l.second, "'" + l.first + "' is declared twice in " + where + "; variables of code that calls a rec or shared proc need unique names");
            else seen[l.first] = l.second;
        }
        if (!proc) return;
        for (const Param& prm : proc->params) if (prm.array) R.uses_heap = true;      // arrays are passed as heap pointers
        if (reentrant && procs[proc->name].has_array_local) R.uses_heap = true;      // its local arrays live in the heap
    }
};

} // namespace

SemaResult analyse(Program& prelude, Program& prog, Types& types, const Features& f, bool warnings, const std::set<int>& quiet_files,
                   const std::vector<std::string>& file_names, const StringPlaces* places) {
    Sema s(types, f, warnings);
    s.quiet_files = quiet_files;
    s.file_names = file_names;
    if (places) { s.string_at = &places->at; s.R.heap_static_end = (int)places->arrays_from; }
    try { s.run(prelude, prog); } catch (const Abort&) {}
    return std::move(s.R);
}

} // namespace bfl
