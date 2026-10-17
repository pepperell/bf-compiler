// ast.h - BFL syntax tree shared by the parser and the code generator.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bfl {

struct Pos { int line = 1, col = 1; int32_t off = 0; };

struct ParseError {
    Pos pos;
    std::string msg;
};

struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
    enum Kind { Num, Str, Name, Index, In, Input, Unary, Binary, Call };
    Kind        kind;
    Pos         pos;
    int32_t     value = 0;      // Num
    std::string text;           // Str: contents; Name/Index/Call: identifier; Unary/Binary: operator
    ExprP       a, b;           // Unary: a; Binary: a op b; Index: a = index expression
    std::vector<ExprP> args;    // Call
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Param {
    bool        ref = false, word = false, array = false;
    std::string name;
    Pos         pos;
};

struct Stmt {
    enum Kind { Decl, Const, Assign, IncDec, If, While, For, Break, Continue, Out, Print, Pulse, Halt, Asm, Call, Block, Proc, Return, Stack };
    Kind        kind;
    Pos         pos, end;           // end: just after the statement's text
    std::string name;               // Decl/Const/Proc/Call/Pulse: identifier
    std::string op;                 // Assign: "=", "+=", ...; IncDec: "++" / "--"
    std::string text;               // Asm: raw brainfuck
    bool        word = false;       // Decl: word type
    bool        rec = false;        // Proc: declared "rec proc" (compiled as a trampoline)
    int         ret = 0;            // Proc: return type 0 none, 1 byte, 2 word
    int         count = -1;         // Decl: array length, -1 scalar; Stack: size
    ExprP       target;             // Assign/IncDec: Name or Index expression
    ExprP       e;                  // Decl init, Const value, Assign rhs, If/While/For condition, Return value
    StmtP       init, step;         // For
    std::vector<ExprP> args;        // Out items, Call arguments
    std::vector<StmtP> body, else_body;
    std::vector<Param> params;      // Proc
};

struct Program {
    std::vector<StmtP> items;
};

// Throws ParseError.  line_base offsets reported line numbers (the prelude uses 0 lines).
Program parse(const std::string& src);

} // namespace bfl
