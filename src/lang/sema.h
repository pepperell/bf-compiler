// sema.h - the BFL semantic pass: names, types, checks, warnings.
#pragma once
#include "ast.h"
#include <set>
#include <string>
#include <vector>

namespace bfl {

struct Diag {
    Pos         pos;
    std::string msg;
};

// Language features switched on so far (the roadmap phases).
struct Features {
    bool wide_ints = true;      // sbyte, int, dword, long
    bool heap = true;           // pointers, heap arrays, &, *
    bool alloc = true;          // new, alloc, free, string literals as values
    bool structs = true;
};

struct SemaResult {
    std::vector<Diag> errors, warnings;
    std::set<std::string> tramp;          // procedures compiled as trampoline blocks
    bool main_tramp = false;              // the main program calls one of them
    bool uses_heap = false;
    int  heap_size = 8192;                // bytes (heap N;)
    int  stack_size = -1;                 // stack N; (-1 = default)
    // static heap layout: 0 null, 1-2 allocator break, 3-4 free list, 5-6 heap end,
    // then heap arrays and string literals; dynamic allocation starts at heap_static_end
    int  heap_static_end = 7;
    std::vector<std::pair<int64_t, std::string>> heap_init;   // string literals: address, bytes with the NUL
};

// Checks prelude + program (prelude procedures first, unchecked for warnings)
// and annotates both.  `types` owns all types referred to by the annotations.
SemaResult analyse(Program& prelude, Program& prog, Types& types, const Features& f, bool warnings);

} // namespace bfl
