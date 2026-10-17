// lang.h - BFL, a small low-level language that compiles to brainfuck for this machine.
//
//   bfl::compile(source) -> brainfuck text + statement map + symbol table
//
// The compiler always knows where the pointer is: every variable has a fixed
// cell, procedures are inlined, and register names from registers.h address
// the device window directly.  See docs/LANG.md for the language.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bfl {

struct Error {
    int line, col;          // 1-based; line 0 = no position
    std::string msg;
};

// One source statement and the byte range of brainfuck generated for it.
// Ranges nest: a while statement's range contains its body's ranges.
struct StmtRange {
    int     line, col;      // where the statement starts in the source
    int     end_line;       // last source line of the statement
    int32_t start, end;     // [start, end) in the output text
    int     depth;          // nesting level (0 = top level)
};

struct Symbol {
    std::string name;
    int64_t     cell;       // absolute tape cell (first cell for words and arrays)
    int         cells;      // cells occupied
    char        kind;       // 'b' byte, 'w' word, 'a' byte array
    int         count;      // elements for an array, else 1
    std::string scope;      // "" for globals, else the procedure name
};

struct Result {
    std::string             bf;          // the program
    std::vector<StmtRange>  stmts;
    std::vector<Symbol>     symbols;
    std::vector<Error>      errors;      // empty on success
    int64_t                 cells_used = 0;
    bool ok() const { return errors.empty(); }
};

// ram_start is the first RAM cell (the machine's mmio_size); the pointer starts there.
Result compile(const std::string& source, uint32_t ram_start = 256);

// Text form of the statement map and symbols, written next to --emit output.
std::string describe(const Result& r);

// The element data cell of a byte array declared at `base` (for debuggers).
inline int64_t array_cell(int64_t base, int index) { return base + 3 + 3 * index + 2; }

} // namespace bfl
