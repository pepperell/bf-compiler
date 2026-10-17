// parse.cpp - BFL lexer and recursive-descent parser with error recovery.
#include "ast.h"
#include <cctype>
#include <cstring>

namespace bfl {

namespace {

enum class T { Ident, Num, Str, Char, Punct, End };

struct Token {
    T           type;
    std::string text;       // identifier, punctuator, string contents; Num: "L" for a long literal
    int64_t     value = 0;  // Num / Char
    Pos         pos;
    int32_t     end = 0;    // byte offset just after the token
};

struct Lexer {
    const std::string* s;
    size_t i = 0;
    int line = 1, col = 1, file = 0;
    Lexer(const std::string& src, int f) : s(&src), file(f) {}

    [[noreturn]] void fail(Pos p, const std::string& m) { throw ParseError{p, m}; }
    Pos here() const { return Pos{line, col, (int32_t)i, file}; }
    char peek(size_t k = 0) const { return i + k < s->size() ? (*s)[i + k] : '\0'; }
    char get() {
        char c = (*s)[i++];
        if (c == '\n') { line++; col = 1; } else col++;
        return c;
    }

    void skip_space() {
        for (;;) {
            while (i < s->size() && isspace((unsigned char)(*s)[i])) get();
            if (peek() == '/' && peek(1) == '/') { while (i < s->size() && peek() != '\n') get(); continue; }
            if (peek() == '/' && peek(1) == '*') {
                Pos p = here();
                get(); get();
                while (i < s->size() && !(peek() == '*' && peek(1) == '/')) get();
                if (i >= s->size()) fail(p, "unterminated comment");
                get(); get();
                continue;
            }
            break;
        }
    }

    int escape(Pos p) {          // after the backslash
        char c = get();
        switch (c) {
        case 'n': return 10; case 'r': return 13; case 't': return 9; case '0': return 0;
        case 'e': return 27; case 'a': return 7; case 'b': return 8;
        case '\\': return '\\'; case '\'': return '\''; case '"': return '"';
        case 'x': {
            int v = 0, n = 0;
            while (n < 2 && isxdigit((unsigned char)peek())) { char d = get(); v = v * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10)); n++; }
            if (!n) fail(p, "\\x needs hex digits");
            return v;
        }
        default: fail(p, std::string("unknown escape \\") + c);
        }
    }

    Token next() {
        skip_space();
        Token t; t.pos = here();
        if (i >= s->size()) { t.type = T::End; t.end = (int32_t)i; return t; }
        char c = peek();
        if (isalpha((unsigned char)c) || c == '_') {
            while (isalnum((unsigned char)peek()) || peek() == '_') t.text += get();
            t.type = T::Ident;
        } else if (isdigit((unsigned char)c)) {
            int64_t v = 0;
            const int64_t LIM = 0xFFFFFFFFll;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                get(); get();
                if (!isxdigit((unsigned char)peek())) fail(t.pos, "hex number needs digits");
                while (isxdigit((unsigned char)peek())) { char d = get(); v = v * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10)); if (v > LIM) fail(t.pos, "number too large"); }
            } else if (c == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
                get(); get();
                if (peek() != '0' && peek() != '1') fail(t.pos, "binary number needs digits");
                while (peek() == '0' || peek() == '1') { v = v * 2 + (get() - '0'); if (v > LIM) fail(t.pos, "number too large"); }
            } else {
                while (isdigit((unsigned char)peek())) { v = v * 10 + (get() - '0'); if (v > LIM) fail(t.pos, "number too large"); }
            }
            if (peek() == 'L' || peek() == 'l') { get(); t.text = "L"; }
            if (isalnum((unsigned char)peek()) || peek() == '_') fail(t.pos, "bad character in number");
            t.type = T::Num; t.value = v;
        } else if (c == '\'') {
            get();
            if (peek() == '\0' || peek() == '\n') fail(t.pos, "unterminated character literal");
            int v = peek() == '\\' ? (get(), escape(t.pos)) : (unsigned char)get();
            if (peek() != '\'') fail(t.pos, "character literal must hold one character");
            get();
            t.type = T::Char; t.value = v;
        } else if (c == '"') {
            get();
            for (;;) {
                if (i >= s->size() || peek() == '\n') fail(t.pos, "unterminated string");
                char d = get();
                if (d == '"') break;
                if (d == '\\') t.text += (char)escape(t.pos); else t.text += d;
            }
            t.type = T::Str;
        } else {
            static const char* puncts[] = {"<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--", "->",
                                           "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                                           "+", "-", "*", "/", "%", "<", ">", "=", "!", "&", "|", "^", "~", ".",
                                           "(", ")", "{", "}", "[", "]", ";", ",", nullptr};
            for (const char** p = puncts; *p; p++) {
                size_t n = strlen(*p);
                if (s->compare(i, n, *p) == 0) { for (size_t k = 0; k < n; k++) get(); t.text = *p; t.type = T::Punct; t.end = (int32_t)i; return t; }
            }
            get();
            fail(t.pos, std::string("unexpected character '") + c + "'");
        }
        t.end = (int32_t)i;
        return t;
    }

    // Raw text up to the closing brace of an asm block (the '{' is already consumed).
    std::string raw_until_brace(Pos p) {
        std::string r;
        while (i < s->size() && peek() != '}') r += get();
        if (i >= s->size()) fail(p, "unterminated asm block");
        get();
        return r;
    }
};

bool is_type_keyword(const std::string& w) {
    return w == "byte" || w == "sbyte" || w == "word" || w == "int" || w == "dword" || w == "long" || w == "string";
}

struct Parser {
    Lexer lex;
    Token tok;
    int32_t prev_end = 0;
    std::vector<ParseError>* errors = nullptr;
    int max_errors = 20;

    Parser(const std::string& src, int file) : lex(src, file) { tok = lex.next(); }

    [[noreturn]] void fail(const std::string& m) { throw ParseError{tok.pos, m}; }
    void advance() { prev_end = tok.end; tok = lex.next(); }
    bool is_punct(const char* p) const { return tok.type == T::Punct && tok.text == p; }
    bool is_ident(const char* w) const { return tok.type == T::Ident && tok.text == w; }
    bool accept(const char* p) { if (is_punct(p)) { advance(); return true; } return false; }
    std::string describe() const {
        switch (tok.type) {
        case T::Num: return std::to_string(tok.value);
        case T::Char: return "a character literal";
        case T::Str: return "a string";
        case T::End: return "end of file";
        default: return tok.text;
        }
    }
    void expect(const char* p) { if (!accept(p)) fail(std::string("expected '") + p + "'" + (tok.type == T::End ? " before end of file" : " before '" + describe() + "'")); }
    std::string ident() {
        if (tok.type != T::Ident) fail("expected a name");
        if (is_keyword(tok.text)) fail("'" + tok.text + "' is a keyword");
        std::string n = tok.text; advance(); return n;
    }
    static bool is_keyword(const std::string& w) {
        static const char* kws[] = {"byte", "sbyte", "word", "int", "dword", "long", "string", "const", "proc", "rec", "ref",
                                    "if", "else", "while", "for", "break", "continue", "out", "print", "pulse", "halt", "asm",
                                    "in", "input", "return", "stack", "heap", "struct", "import", "sizeof", "new", "null", nullptr};
        for (const char** k = kws; *k; k++) if (w == *k) return true;
        return false;
    }

    // Look at the token after the current one (k = 1) or further, without consuming.
    Token peek_tok(int k) {
        Lexer save = lex;
        Token t = tok;
        try { for (int n = 0; n < k; n++) t = lex.next(); } catch (const ParseError&) { t.type = T::End; }
        lex = save;
        return t;
    }

    // Does a type start here?  type keyword, or Name followed by a name or '*'s then a name.
    bool type_starts() {
        if (tok.type != T::Ident) return false;
        if (is_type_keyword(tok.text)) return true;
        if (is_keyword(tok.text)) return false;
        int k = 1;
        Token t = peek_tok(k);
        while (t.type == T::Punct && t.text == "*") t = peek_tok(++k);
        return k > 1 ? t.type == T::Ident : (t.type == T::Ident && !is_keyword(t.text));
    }
    TypeSpec type_spec() {
        TypeSpec ts; ts.pos = tok.pos;
        if (tok.type != T::Ident || (is_keyword(tok.text) && !is_type_keyword(tok.text))) fail("expected a type");
        ts.base = tok.text; advance();
        while (accept("*")) ts.ptr++;
        return ts;
    }

    ExprP mk(Expr::Kind k, Pos p) { auto e = std::make_unique<Expr>(); e->kind = k; e->pos = p; return e; }

    std::vector<ExprP> call_args() {
        std::vector<ExprP> v;
        if (!is_punct(")")) do v.push_back(expr()); while (accept(","));
        expect(")");
        return v;
    }

    ExprP primary() {
        Pos p = tok.pos;
        if (tok.type == T::Num || tok.type == T::Char) {
            auto e = mk(Expr::Num, p); e->value = tok.value; if (tok.type == T::Num) e->text = tok.text; advance(); return e;
        }
        if (tok.type == T::Str) { auto e = mk(Expr::Str, p); e->text = tok.text; advance(); return e; }
        if (is_punct("(")) {
            // a cast "(T) e" when a type follows that is not a function-style cast "(byte(x))"
            Token n1 = peek_tok(1);
            bool cast = false;
            if (n1.type == T::Ident && is_type_keyword(n1.text)) {
                Token n2 = peek_tok(2);
                cast = !(n2.type == T::Punct && n2.text == "(");
            } else if (n1.type == T::Ident && !is_keyword(n1.text)) {
                int k = 2; Token t = peek_tok(k);
                while (t.type == T::Punct && t.text == "*") t = peek_tok(++k);
                cast = k > 2 && t.type == T::Punct && t.text == ")";
            }
            advance();
            if (cast) {
                auto e = mk(Expr::Cast, p);
                e->ts = type_spec();
                expect(")");
                e->a = unary();
                return e;
            }
            ExprP e = expr(); expect(")"); return e;
        }
        if (tok.type == T::Ident) {
            if (is_ident("in")) { advance(); return mk(Expr::In, p); }
            if (is_ident("input")) { advance(); return mk(Expr::Input, p); }
            if (is_ident("null")) { advance(); return mk(Expr::Null, p); }
            if (is_ident("sizeof")) {
                advance(); expect("(");
                auto e = mk(Expr::Sizeof, p); e->ts = type_spec(); expect(")");
                return e;
            }
            if (is_ident("new")) {
                advance();
                auto e = mk(Expr::New, p); e->ts = type_spec();
                if (accept("[")) { e->b = expr(); expect("]"); }
                return e;
            }
            if (is_type_keyword(tok.text) && tok.text != "string") {      // function-style cast: int(e)
                auto e = mk(Expr::Cast, p); e->ts.base = tok.text; e->ts.pos = p; advance();
                expect("("); e->a = expr(); expect(")");
                return e;
            }
            if (is_keyword(tok.text)) fail("'" + tok.text + "' cannot be used in an expression");
            std::string n = ident();
            if (accept("(")) { auto e = mk(Expr::Call, p); e->text = n; e->args = call_args(); return e; }
            auto e = mk(Expr::Name, p); e->text = n; return e;
        }
        if (tok.type == T::End) fail("unexpected end of file in expression");
        fail("unexpected '" + describe() + "' in expression");
    }

    ExprP postfix() {
        ExprP e = primary();
        for (;;) {
            Pos p = tok.pos;
            if (accept("[")) { auto x = mk(Expr::Index, p); x->a = std::move(e); x->b = expr(); expect("]"); e = std::move(x); continue; }
            if (is_punct(".") || is_punct("->")) {
                bool arrow = tok.text == "->"; advance();
                auto x = mk(Expr::Field, p); x->arrow = arrow; x->a = std::move(e); x->text = ident(); e = std::move(x);
                continue;
            }
            break;
        }
        return e;
    }

    ExprP unary() {
        Pos p = tok.pos;
        if (is_punct("!") || is_punct("-") || is_punct("~")) {
            std::string op = tok.text; advance();
            auto e = mk(Expr::Unary, p); e->text = op; e->a = unary(); return e;
        }
        if (accept("*")) { auto e = mk(Expr::Deref, p); e->a = unary(); return e; }
        if (accept("&")) { auto e = mk(Expr::AddrOf, p); e->a = unary(); return e; }
        return postfix();
    }

    static int prec(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
        if (op == "<<" || op == ">>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return 0;
    }

    ExprP binary(int min_prec) {
        ExprP lhs = unary();
        for (;;) {
            if (tok.type != T::Punct) break;
            int pr = prec(tok.text);
            if (pr == 0 || pr < min_prec) break;
            std::string op = tok.text; Pos p = tok.pos; advance();
            ExprP rhs = binary(pr + 1);
            auto e = mk(Expr::Binary, p); e->text = op; e->a = std::move(lhs); e->b = std::move(rhs);
            lhs = std::move(e);
        }
        return lhs;
    }
    ExprP expr() { return binary(1); }

    StmtP mks(Stmt::Kind k, Pos p) { auto s = std::make_unique<Stmt>(); s->kind = k; s->pos = p; return s; }
    StmtP finish(StmtP s) { s->end = Pos{0, 0, prev_end, s->pos.file}; return s; }

    // ---- error recovery -------------------------------------------------------------
    void record(const ParseError& e) {
        if (!errors) throw e;
        if ((int)errors->size() < max_errors) errors->push_back(e);
        if ((int)errors->size() >= max_errors) throw e;         // give up
    }
    // Skip to just after the next ';' or a '}' that closes a block opened while skipping,
    // or to (not past) a '}' that closes the enclosing block.
    void synchronise() {
        int depth = 0;
        for (;;) {
            if (tok.type == T::End) return;
            if (is_punct("{")) depth++;
            else if (is_punct("}")) { if (depth == 0) return; if (--depth == 0) { advance(); return; } }
            else if (is_punct(";") && depth == 0) { advance(); return; }
            try { advance(); } catch (const ParseError&) { lex.i++; tok.type = T::Punct; tok.text = "?"; }
        }
    }
    // One statement, recording an error and resynchronising instead of throwing.
    bool guarded(std::vector<StmtP>& out) {
        try { out.push_back(statement()); return true; }
        catch (const ParseError& e) {
            record(e);
            int32_t before = tok.pos.off;
            synchronise();
            if (tok.pos.off == before && !is_punct("}") && tok.type != T::End) {   // guarantee progress
                try { advance(); } catch (const ParseError&) { lex.i++; }
            }
            return false;
        }
    }

    std::vector<StmtP> block() {
        expect("{");
        std::vector<StmtP> v;
        while (!is_punct("}")) {
            if (tok.type == T::End) fail("expected '}' before end of file");
            guarded(v);
        }
        expect("}");
        return v;
    }

    // An assignment, ++/--, call, or (when allow_decl) a declaration.  No ';'.
    StmtP simple(Pos p, bool allow_decl) {
        if (allow_decl && type_starts()) return declaration(p, false);
        if (tok.type == T::Ident && is_keyword(tok.text) && !is_ident("in") && !is_ident("input"))
            fail("unexpected '" + tok.text + "'");
        ExprP target = unary();
        if (target->kind == Expr::Call && !is_punct("=") && !is_punct("++") && !is_punct("--")) {
            auto s = mks(Stmt::Call, p); s->name = target->text; s->args = std::move(target->args);
            return s;
        }
        if (is_punct("++") || is_punct("--")) {
            auto s = mks(Stmt::IncDec, p); s->op = tok.text; advance(); s->target = std::move(target); return s;
        }
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", nullptr};
        for (const char** o = ops; *o; o++) if (is_punct(*o)) {
            auto s = mks(Stmt::Assign, p); s->op = *o; advance(); s->target = std::move(target); s->e = expr(); return s;
        }
        if (tok.type == T::End) fail("unexpected end of file");
        fail("expected '=' or an operator before '" + describe() + "'");
    }

    // [heap] type name [ '[' n ']' ] [= e]   (no ';')
    StmtP declaration(Pos p, bool in_heap) {
        auto s = mks(Stmt::Decl, p);
        s->in_heap = in_heap;
        s->ts = type_spec();
        s->name = ident();
        if (accept("[")) { s->count_e = expr(); expect("]"); }
        if (accept("=")) { if (s->count_e) fail("arrays cannot be initialised"); s->e = expr(); }
        return s;
    }

    StmtP statement() {
        Pos p = tok.pos;
        if (is_punct("{")) { auto s = mks(Stmt::Block, p); s->body = block(); return finish(std::move(s)); }
        if (is_ident("import")) {
            auto s = mks(Stmt::Import, p); advance();
            if (tok.type != T::Str) fail("expected a file name in quotes after 'import'");
            s->name = tok.text; advance(); expect(";");
            return finish(std::move(s));
        }
        if (is_ident("struct")) {
            auto s = mks(Stmt::Struct, p); advance(); s->name = ident(); expect("{");
            while (!is_punct("}")) {
                if (tok.type == T::End) fail("expected '}' before end of file");
                FieldDecl f; f.pos = tok.pos;
                f.ts = type_spec(); f.name = ident();
                if (accept("[")) { f.count = expr(); expect("]"); }
                expect(";");
                s->fields.push_back(std::move(f));
            }
            expect("}");
            accept(";");
            return finish(std::move(s));
        }
        if (is_ident("heap")) {
            advance();
            if (type_starts()) { StmtP s = declaration(p, true); expect(";"); return finish(std::move(s)); }
            auto s = mks(Stmt::Heap, p); s->count_e = expr(); expect(";");
            return finish(std::move(s));
        }
        if (is_ident("stack")) {
            auto s = mks(Stmt::Stack, p); advance(); s->count_e = expr(); expect(";");
            return finish(std::move(s));
        }
        if (is_ident("const")) {
            auto s = mks(Stmt::Const, p); advance(); s->name = ident(); expect("="); s->e = expr(); expect(";");
            return finish(std::move(s));
        }
        if (is_ident("rec") || is_ident("proc")) {
            auto s = mks(Stmt::Proc, p);
            if (is_ident("rec")) { s->rec = true; advance(); if (!is_ident("proc")) fail("expected 'proc' after 'rec'"); }
            advance();
            if (type_starts()) s->ts = type_spec();
            s->name = ident(); expect("(");
            if (!is_punct(")")) do {
                Param prm; prm.pos = tok.pos;
                if (is_ident("ref")) { prm.ref = true; advance(); }
                if (!type_starts() && !(tok.type == T::Ident && !is_keyword(tok.text))) fail("expected a parameter type");
                prm.ts = type_spec();
                prm.name = ident();
                if (accept("[")) { expect("]"); prm.array = true; }
                s->params.push_back(std::move(prm));
            } while (accept(","));
            expect(")");
            s->body = block();
            return finish(std::move(s));
        }
        if (is_ident("if")) {
            auto s = mks(Stmt::If, p); advance(); expect("("); s->e = expr(); expect(")");
            s->body = block();
            if (is_ident("else")) {
                advance();
                if (is_ident("if")) s->else_body.push_back(statement());
                else s->else_body = block();
            }
            return finish(std::move(s));
        }
        if (is_ident("while")) {
            auto s = mks(Stmt::While, p); advance(); expect("("); s->e = expr(); expect(")");
            s->body = block();
            return finish(std::move(s));
        }
        if (is_ident("for")) {
            auto s = mks(Stmt::For, p); advance(); expect("(");
            if (!is_punct(";")) s->init = finish(simple(tok.pos, true));
            expect(";");
            if (!is_punct(";")) s->e = expr();
            expect(";");
            if (!is_punct(")")) s->step = finish(simple(tok.pos, false));
            expect(")");
            s->body = block();
            return finish(std::move(s));
        }
        if (is_ident("return")) {
            auto s = mks(Stmt::Return, p); advance();
            if (!is_punct(";")) s->e = expr();
            expect(";");
            return finish(std::move(s));
        }
        if (is_ident("break"))    { advance(); expect(";"); return finish(mks(Stmt::Break, p)); }
        if (is_ident("continue")) { advance(); expect(";"); return finish(mks(Stmt::Continue, p)); }
        if (is_ident("halt"))     { advance(); expect(";"); return finish(mks(Stmt::Halt, p)); }
        if (is_ident("out") || is_ident("print")) {
            auto s = mks(tok.text == "out" ? Stmt::Out : Stmt::Print, p); advance();
            do s->args.push_back(expr()); while (accept(","));
            expect(";");
            return finish(std::move(s));
        }
        if (is_ident("pulse")) {
            auto s = mks(Stmt::Pulse, p); advance(); s->name = ident(); expect(";");
            return finish(std::move(s));
        }
        if (is_ident("asm")) {
            auto s = mks(Stmt::Asm, p); advance();
            if (!is_punct("{")) fail("expected '{' after asm");
            s->text = lex.raw_until_brace(p);       // the lexer is just past the '{'
            prev_end = (int32_t)lex.i;
            tok = lex.next();
            return finish(std::move(s));
        }
        if (is_ident("else")) fail("'else' without 'if'");
        StmtP s = simple(p, true);
        expect(";");
        return finish(std::move(s));
    }

    Program program() {
        Program prog;
        while (tok.type != T::End) {
            if (is_punct("}")) {
                try { fail("unexpected '}'"); } catch (const ParseError& e) { record(e); advance(); continue; }
            }
            guarded(prog.items);
        }
        return prog;
    }
};

} // namespace

Program parse(const std::string& src, int file, std::vector<ParseError>& errors, int max_errors) {
    Program prog;
    try {
        Parser p(src, file);
        p.errors = &errors;
        p.max_errors = max_errors;
        prog = p.program();
    } catch (const ParseError& e) {
        // the first token could not be read, or the error limit was reached
        if (errors.empty() || errors.back().pos.off != e.pos.off || errors.back().msg != e.msg)
            if ((int)errors.size() < max_errors) errors.push_back(e);
    }
    return prog;
}

Program parse(const std::string& src, int file) {
    Parser p(src, file);
    return p.program();
}

} // namespace bfl
