// parse.cpp - BFL lexer and recursive-descent parser.
#include "ast.h"
#include <cctype>
#include <cstring>

namespace bfl {

namespace {

enum class T { Ident, Num, Str, Char, Punct, End };

struct Token {
    T           type;
    std::string text;       // identifier, punctuator, string contents
    int32_t     value = 0;  // Num / Char
    Pos         pos;
    int32_t     end = 0;    // byte offset just after the token
};

struct Lexer {
    const std::string& s;
    size_t i = 0;
    int line = 1, col = 1;
    explicit Lexer(const std::string& src) : s(src) {}

    [[noreturn]] void fail(Pos p, const std::string& m) { throw ParseError{p, m}; }
    Pos here() const { return Pos{line, col, (int32_t)i}; }
    char peek(size_t k = 0) const { return i + k < s.size() ? s[i + k] : '\0'; }
    char get() {
        char c = s[i++];
        if (c == '\n') { line++; col = 1; } else col++;
        return c;
    }

    void skip_space() {
        for (;;) {
            while (i < s.size() && isspace((unsigned char)s[i])) get();
            if (peek() == '/' && peek(1) == '/') { while (i < s.size() && peek() != '\n') get(); continue; }
            if (peek() == '/' && peek(1) == '*') {
                Pos p = here();
                get(); get();
                while (i < s.size() && !(peek() == '*' && peek(1) == '/')) get();
                if (i >= s.size()) fail(p, "unterminated comment");
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
        if (i >= s.size()) { t.type = T::End; t.end = (int32_t)i; return t; }
        char c = peek();
        if (isalpha((unsigned char)c) || c == '_') {
            while (isalnum((unsigned char)peek()) || peek() == '_') t.text += get();
            t.type = T::Ident;
        } else if (isdigit((unsigned char)c)) {
            int64_t v = 0;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                get(); get();
                if (!isxdigit((unsigned char)peek())) fail(t.pos, "hex number needs digits");
                while (isxdigit((unsigned char)peek())) { char d = get(); v = v * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10)); if (v > 0xFFFFFFFF) fail(t.pos, "number too large"); }
            } else if (c == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
                get(); get();
                if (peek() != '0' && peek() != '1') fail(t.pos, "binary number needs digits");
                while (peek() == '0' || peek() == '1') { v = v * 2 + (get() - '0'); if (v > 0xFFFFFFFF) fail(t.pos, "number too large"); }
            } else {
                while (isdigit((unsigned char)peek())) { v = v * 10 + (get() - '0'); if (v > 0xFFFFFFFF) fail(t.pos, "number too large"); }
            }
            if (isalnum((unsigned char)peek()) || peek() == '_') fail(t.pos, "bad character in number");
            t.type = T::Num; t.value = (int32_t)v;
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
                if (i >= s.size() || peek() == '\n') fail(t.pos, "unterminated string");
                char d = get();
                if (d == '"') break;
                if (d == '\\') t.text += (char)escape(t.pos); else t.text += d;
            }
            t.type = T::Str;
        } else {
            static const char* puncts[] = {"<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--",
                                           "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                                           "+", "-", "*", "/", "%", "<", ">", "=", "!", "&", "|", "^", "~",
                                           "(", ")", "{", "}", "[", "]", ";", ",", nullptr};
            for (const char** p = puncts; *p; p++) {
                size_t n = strlen(*p);
                if (s.compare(i, n, *p) == 0) { for (size_t k = 0; k < n; k++) get(); t.text = *p; t.type = T::Punct; t.end = (int32_t)i; return t; }
            }
            fail(t.pos, std::string("unexpected character '") + c + "'");
        }
        t.end = (int32_t)i;
        return t;
    }

    // Raw text up to the closing brace of an asm block; the pointer is on the '{' already consumed.
    std::string raw_until_brace(Pos p) {
        std::string r;
        while (i < s.size() && peek() != '}') r += get();
        if (i >= s.size()) fail(p, "unterminated asm block");
        get();
        return r;
    }
};

struct Parser {
    Lexer lex;
    Token tok;
    int32_t prev_end = 0;
    explicit Parser(const std::string& src) : lex(src) { tok = lex.next(); }

    [[noreturn]] void fail(const std::string& m) { throw ParseError{tok.pos, m}; }
    void advance() { prev_end = tok.end; tok = lex.next(); }
    bool is_punct(const char* p) const { return tok.type == T::Punct && tok.text == p; }
    bool is_ident(const char* w) const { return tok.type == T::Ident && tok.text == w; }
    bool accept(const char* p) { if (is_punct(p)) { advance(); return true; } return false; }
    void expect(const char* p) { if (!accept(p)) fail(std::string("expected '") + p + "'" + (tok.type == T::End ? " before end of file" : " before '" + tok.text + "'")); }
    std::string ident() {
        if (tok.type != T::Ident) fail("expected a name");
        std::string n = tok.text; advance(); return n;
    }
    static bool is_keyword(const std::string& w) {
        static const char* kws[] = {"byte", "word", "const", "proc", "rec", "ref", "if", "else", "while", "for", "break", "continue",
                                    "out", "print", "pulse", "halt", "asm", "in", "input", "return", "stack", nullptr};
        for (const char** k = kws; *k; k++) if (w == *k) return true;
        return false;
    }

    ExprP mk(Expr::Kind k, Pos p) { auto e = std::make_unique<Expr>(); e->kind = k; e->pos = p; return e; }

    ExprP primary() {
        Pos p = tok.pos;
        if (tok.type == T::Num || tok.type == T::Char) { auto e = mk(Expr::Num, p); e->value = tok.value; advance(); return e; }
        if (tok.type == T::Str) { auto e = mk(Expr::Str, p); e->text = tok.text; advance(); return e; }
        if (accept("(")) { ExprP e = expr(); expect(")"); return e; }
        if (tok.type == T::Ident) {
            if (is_ident("in")) { advance(); return mk(Expr::In, p); }
            if (is_ident("input")) { advance(); return mk(Expr::Input, p); }
            if (is_ident("byte") || is_ident("word")) {           // cast: byte(e), word(e)
                auto e = mk(Expr::Unary, p); e->text = tok.text; advance();
                expect("("); e->a = expr(); expect(")");
                return e;
            }
            if (is_keyword(tok.text)) fail("'" + tok.text + "' cannot be used in an expression");
            std::string n = ident();
            if (accept("[")) { auto e = mk(Expr::Index, p); e->text = n; e->a = expr(); expect("]"); return e; }
            if (accept("(")) {
                auto e = mk(Expr::Call, p); e->text = n;
                if (!is_punct(")")) do e->args.push_back(expr()); while (accept(","));
                expect(")");
                return e;
            }
            auto e = mk(Expr::Name, p); e->text = n; return e;
        }
        if (tok.type == T::End) fail("unexpected end of file in expression");
        fail("unexpected '" + tok.text + "' in expression");
    }

    ExprP unary() {
        Pos p = tok.pos;
        if (is_punct("!") || is_punct("-") || is_punct("~")) {
            std::string op = tok.text; advance();
            auto e = mk(Expr::Unary, p); e->text = op; e->a = unary(); return e;
        }
        return primary();
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
    StmtP finish(StmtP s) { s->end = Pos{0, 0, prev_end}; return s; }

    std::vector<StmtP> block() {
        expect("{");
        std::vector<StmtP> v;
        while (!is_punct("}")) {
            if (tok.type == T::End) fail("expected '}' before end of file");
            v.push_back(statement());
        }
        expect("}");
        return v;
    }

    // Statement that may appear as a for-loop init/step: assignment, ++/--, or a call.
    StmtP simple(Pos p) {
        if (tok.type != T::Ident || is_keyword(tok.text)) fail("expected an assignment");
        std::string n = ident();
        if (accept("(")) {
            auto s = mks(Stmt::Call, p); s->name = n;
            if (!is_punct(")")) do s->args.push_back(expr()); while (accept(","));
            expect(")");
            return s;
        }
        ExprP target;
        if (accept("[")) { target = mk(Expr::Index, p); target->text = n; target->a = expr(); expect("]"); }
        else { target = mk(Expr::Name, p); target->text = n; }
        if (is_punct("++") || is_punct("--")) {
            auto s = mks(Stmt::IncDec, p); s->op = tok.text; advance(); s->target = std::move(target); return s;
        }
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", nullptr};
        for (const char** o = ops; *o; o++) if (is_punct(*o)) {
            auto s = mks(Stmt::Assign, p); s->op = *o; advance(); s->target = std::move(target); s->e = expr(); return s;
        }
        fail("expected '=' or an operator after '" + n + "'");
    }

    StmtP statement() {
        Pos p = tok.pos;
        if (is_punct("{")) { auto s = mks(Stmt::Block, p); s->body = block(); return finish(std::move(s)); }
        if (is_ident("byte") || is_ident("word")) {
            auto s = mks(Stmt::Decl, p); s->word = tok.text == "word"; advance();
            s->name = ident();
            if (accept("[")) {
                if (tok.type != T::Num) fail("array length must be a number");
                s->count = tok.value; advance(); expect("]");
                if (s->count < 1 || s->count > 65535) fail("array length out of range");
            }
            if (accept("=")) { if (s->count >= 0) fail("arrays cannot be initialised"); s->e = expr(); }
            expect(";");
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
            if (is_ident("byte")) { s->ret = 1; advance(); } else if (is_ident("word")) { s->ret = 2; advance(); }
            s->name = ident(); expect("(");
            if (!is_punct(")")) do {
                Param prm; prm.pos = tok.pos;
                if (is_ident("ref")) { prm.ref = true; advance(); }
                if (is_ident("byte")) advance(); else if (is_ident("word")) { prm.word = true; advance(); } else fail("expected 'byte' or 'word' in parameter list");
                prm.name = ident();
                if (accept("[")) { expect("]"); prm.array = true; if (!prm.ref) fail("array parameters must be 'ref'"); if (prm.word) fail("word arrays are not supported"); }
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
            if (!is_punct(";")) s->init = simple(tok.pos);
            expect(";");
            if (!is_punct(";")) s->e = expr();
            expect(";");
            if (!is_punct(")")) s->step = simple(tok.pos);
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
        if (is_ident("stack")) {
            auto s = mks(Stmt::Stack, p); advance();
            if (tok.type != T::Num) fail("expected the stack size in cells after 'stack'");
            s->count = tok.value; advance(); expect(";");
            if (s->count < 4 || s->count > 256) fail("stack size must be 4..256 cells");
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
            // the current token is '{'; the lexer has consumed it, so read raw from here
            s->text = lex.raw_until_brace(p);
            prev_end = (int32_t)lex.i;
            tok = lex.next();
            return finish(std::move(s));
        }
        if (is_ident("else")) fail("'else' without 'if'");
        if (tok.type == T::Ident && is_keyword(tok.text) && !is_ident("in") && !is_ident("input")) fail("unexpected '" + tok.text + "'");
        StmtP s = simple(p);
        expect(";");
        return finish(std::move(s));
    }

    Program program() {
        Program prog;
        while (tok.type != T::End) {
            if (is_punct("}")) fail("unexpected '}'");
            prog.items.push_back(statement());
        }
        return prog;
    }
};

} // namespace

Program parse(const std::string& src) {
    Parser p(src);
    return p.program();
}

} // namespace bfl
