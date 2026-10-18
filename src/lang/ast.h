// ast.h - BFL syntax tree shared by the parser, the semantic pass and the code generator.
//
// The parser fills in the syntax.  The semantic pass (sema.cpp) resolves type
// names, checks the program and annotates it: every expression gets a type,
// constant expressions get their value, declarations get their resolved type.
// The code generator only ever sees programs the semantic pass accepted.
#pragma once
#include "types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bfl {

struct Pos { int line = 1, col = 1; int32_t off = 0; int file = 0; };

struct ParseError {
    Pos pos;
    std::string msg;
};

// A type as written: base name ("byte", "string", a struct name) plus pointer depth.
struct TypeSpec {
    std::string base;           // "" = none (procedure without a result)
    int         ptr = 0;
    Pos         pos;
    bool empty() const { return base.empty(); }
};

struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
    enum Kind {
        Num, Str, Name, Index, Field, Deref, AddrOf, In, Input, Unary, Binary, Call, Cast, Sizeof, New, Null
    };
    Kind        kind;
    Pos         pos;
    int64_t     value = 0;      // Num
    std::string text;           // Str: contents; Name/Call: identifier; Field: field name; Unary/Binary: operator
    bool        arrow = false;  // Field: p->f (true) or s.f (false)
    ExprP       a, b;           // Unary/Deref/AddrOf/Cast: a; Binary: a op b; Index: a[b]; Field: a.f; New: b = count
    std::vector<ExprP> args;    // Call
    TypeSpec    ts;             // Cast, Sizeof, New

    // ---- filled in by the semantic pass ----
    const Type* type = nullptr;     // result type
    const Type* optype = nullptr;   // Binary: the type both operands are converted to
    bool        is_const = false;   // compile-time constant (cval valid)
    int64_t     cval = 0;
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Param {
    bool        ref = false, array = false;
    TypeSpec    ts;
    std::string name;
    Pos         pos;
    const Type* type = nullptr;     // sema: the element type for array parameters
};

struct FieldDecl {
    TypeSpec    ts;
    std::string name;
    ExprP       count;              // array field length, or null
    Pos         pos;
};

struct Stmt {
    enum Kind {
        Decl, Const, Assign, IncDec, If, While, For, Break, Continue, Out, Print, Pulse, Halt, Asm, Call, Block,
        Proc, Return, Stack, Heap, Struct, Import
    };
    Kind        kind;
    Pos         pos, end;           // end: just after the statement's text
    std::string name;               // Decl/Const/Proc/Call/Pulse/Struct: identifier; Import: path
    std::string op;                 // Assign: "=", "+=", ...; IncDec: "++" / "--"
    std::string text;               // Asm: raw brainfuck
    bool        rec = false;        // Proc: declared "rec proc" (compiled as a trampoline)
    bool        shared = false;     // Proc: declared "shared proc" (one copy, called through the trampoline)
    bool        in_heap = false;    // Decl: "heap T name[n]"
    TypeSpec    ts;                 // Decl: variable type; Proc: result type (empty = none)
    ExprP       count_e;            // Decl: array length; Stack/Heap: size
    ExprP       target;             // Assign/IncDec: an lvalue expression
    ExprP       e;                  // Decl init, Const value, Assign rhs, If/While/For condition, Return value
    StmtP       init, step;         // For
    std::vector<ExprP> args;        // Out items, Call arguments
    std::vector<StmtP> body, else_body;
    std::vector<Param> params;      // Proc
    std::vector<FieldDecl> fields;  // Struct

    // ---- filled in by the semantic pass ----
    const Type* vtype = nullptr;    // Decl: variable (or array element) type; Proc: result type or null
    int         count = -1;         // Decl: array length (-1 scalar); Stack/Heap: size
    int64_t     cval = 0;           // Const: value
    const Type* ctype = nullptr;    // Const: type of the value
};

struct Program {
    std::vector<StmtP> items;
};

// One source file of a compilation.
struct SourceFile {
    std::string name;               // as given (path or library name)
    std::string text;
};

// Parse one file.  Errors are recorded (up to `max_errors`) and parsing resumes
// at the next statement; `file` is stored in every Pos.
Program parse(const std::string& src, int file, std::vector<ParseError>& errors, int max_errors = 20);
// Convenience for internal sources that must parse cleanly (throws ParseError).
Program parse(const std::string& src, int file = 0);

} // namespace bfl
