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
    int file = 0;           // index into Result::files
};

struct Options {
    bool warnings = false;      // collect warnings (Result::warnings)
    bool check_heap = false;    // null and bounds checks on heap accesses
    bool check_stack = false;   // overflow check on rec proc calls
    bool layout = true;         // place global variables by how often they are used (else in declaration order)
    int share = 2;              // run a multiply or division used this many times in a block through one copy (0: never)
    std::string file_name;      // main file name, for messages and relative imports
};

// One source statement and the byte range of brainfuck generated for it.
// Ranges nest: a while statement's range contains its body's ranges.
struct StmtRange {
    int     line, col;      // where the statement starts in the source
    int     end_line;       // last source line of the statement
    int32_t start, end;     // [start, end) in the output text
    int     depth;          // nesting level (0 = top level)
    bool    cont;           // continuation of a statement split by a rec call (no step mark)
    int     file = 0;       // index into Result::files
};

struct Symbol {
    std::string name;
    int64_t     cell;       // absolute tape cell (first cell for wide values and arrays)
    int         cells;      // cells occupied
    char        kind;       // 'b' 1 byte, 'w' 2 bytes, 'l' 4 bytes, 'p' pointer, 'a' array
    int         count;      // elements for an array, else 1
    std::string scope;      // "" for globals, else the procedure name
    int         elem = 1;   // bytes per value (per element for arrays)
    bool        is_signed = false;
};

struct Result {
    std::string             bf;          // the program
    std::vector<StmtRange>  stmts;
    std::vector<Symbol>     symbols;
    std::vector<Error>      errors;      // empty on success
    std::vector<Error>      warnings;
    std::vector<std::string> files;      // file names by index (0 = the main file)
    std::vector<std::string> sources;    // file texts by index (imports included, for debuggers)
    int64_t                 cells_used = 0;
    int64_t                 sp_cell = -1;    // trampoline stack pointer cell, -1 if no rec procs are used
    int                     sp_bytes = 1;    // 1, or 2 for a stack of more than 256 cells
    int64_t                 heap_cell = -1;  // first cell of heap byte 0, -1 without a heap; byte a is stored in cell heap_cell + 5*a + 4
    int                     heap_size = 0;   // bytes
    bool ok() const { return errors.empty(); }
};

// ram_start is the first RAM cell (the machine's mmio_size); the pointer starts there.
Result compile(const std::string& source, uint32_t ram_start = 256, const Options& opt = Options());

// "file:line:col: message" for an error or warning.
std::string format_error(const Result& r, const Error& e, const char* kind = nullptr);

// Text form of the statement map and symbols, written next to --emit output.
std::string describe(const Result& r);

// The cell holding byte `byte` of element `index` of an array in cells declared at
// `base` with `size`-byte elements (docs/LANG.md; for debuggers).
inline int64_t array_cell(int64_t base, int index, int byte = 0, int size = 1) {
    return base + (1 + 2 * (int64_t)size) * ((int64_t)index + 1) + 1 + size + byte;
}

} // namespace bfl
