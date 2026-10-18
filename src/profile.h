// profile.h - where the instructions of a profiled run went (Machine::profile).
// Shared by bf --profile (cli.cpp) and Run > Profile in the UI (ui.cpp), so the
// two always attribute the same counts to the same lines.
#pragma once
#include "bf.h"
#include "lang/lang.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bf {

// One row of a profile: the instructions executed in a BFL statement line, or
// at a brainfuck byte offset.  file >= 0: BFL file index and 1-based line.
// file -1: line is the brainfuck byte offset, or -1 for code outside statements.
struct ProfileRow { uint64_t count; int file, line; };

// The instruction counts[i] belongs to: Machine::profile holds the optimised code's
// counts, then the literal program's (run while the pointer can reach the device window).
inline const Instr* profile_instr(const Program& prog, size_t i) {
    if (i < prog.code.size()) return &prog.code[i];
    i -= prog.code.size();
    return i < prog.literal.size() ? &prog.literal[i] : nullptr;
}

inline uint64_t profile_total(const std::vector<uint64_t>& counts) {
    uint64_t total = 0;
    for (uint64_t n : counts) total += n;
    return total;
}

// "ADD 30.1% SET 12.0% ...": the share of each kind of instruction.
inline std::string profile_by_kind(const Program& prog, const std::vector<uint64_t>& counts) {
    static const char* names[] = {"ADD", "SET", "MUL", "MOVE", "OUT", "IN", "JZ", "JNZ", "ENDIF", "SCAN", "IDIOM", "LOOPX", "END"};
    uint64_t total = profile_total(counts);
    uint64_t per[16] = {};
    for (size_t i = 0; i < counts.size(); i++)
        if (const Instr* x = profile_instr(prog, i)) per[x->op] += counts[i];
    std::string s;
    char buf[48];
    for (int k = 0; k <= END; k++)
        if (per[k]) { snprintf(buf, sizeof buf, "%s%s %.1f%%", s.empty() ? "" : " ", names[k], 100.0 * (double)per[k] / (double)total); s += buf; }
    return s;
}

// Each instruction's count goes to the innermost statement whose generated range
// holds its source offset (lr given), or to that offset (plain brainfuck).  The
// literal program's instructions carry their brainfuck offsets too, so its counts
// go to the same statements.
// Rows come most executed first.
inline std::vector<ProfileRow> profile_rows(const Program& prog, const std::vector<uint64_t>& counts, const bfl::Result* lr) {
    std::map<std::pair<int, int>, uint64_t> by;          // (file, line) or (-1, source offset)
    std::vector<int> parent;
    if (lr) {
        std::vector<int> st;
        for (size_t i = 0; i < lr->stmts.size(); i++) {
            while (!st.empty() && lr->stmts[(size_t)st.back()].end <= lr->stmts[i].start) st.pop_back();
            parent.push_back(st.empty() ? -1 : st.back());
            st.push_back((int)i);
        }
    }
    for (size_t pc = 0; pc < counts.size(); pc++) {
        const Instr* x = profile_instr(prog, pc);
        if (!counts[pc] || !x) continue;
        int32_t off = x->src;
        if (!lr) { by[{-1, off}] += counts[pc]; continue; }
        auto it = std::upper_bound(lr->stmts.begin(), lr->stmts.end(), off,
                                   [](int32_t o, const bfl::StmtRange& r) { return o < r.start; });
        int k = (int)(it - lr->stmts.begin()) - 1;
        while (k >= 0 && lr->stmts[(size_t)k].end <= off) k = parent[(size_t)k];
        if (k < 0) { by[{-1, -1}] += counts[pc]; continue; }
        by[{lr->stmts[(size_t)k].file, lr->stmts[(size_t)k].line}] += counts[pc];
    }
    std::vector<std::pair<uint64_t, std::pair<int, int>>> sorted;
    for (auto& kv : by) sorted.push_back({kv.second, kv.first});
    std::sort(sorted.rbegin(), sorted.rend());
    std::vector<ProfileRow> rows;
    for (auto& [n, key] : sorted) rows.push_back({n, key.first, key.second});
    return rows;
}

// The text of a source line, trimmed, for listing (empty if there is no such line).
inline std::string profile_line_text(const bfl::Result& lr, int file, int line, size_t max = 70) {
    if (file < 0 || (size_t)file >= lr.sources.size()) return "";
    const std::string& s = lr.sources[(size_t)file];
    size_t a = 0;
    for (int l = 1; l < line && a != std::string::npos; l++) { a = s.find('\n', a); if (a != std::string::npos) a++; }
    if (a == std::string::npos) return "";
    size_t b = s.find('\n', a);
    std::string text = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    size_t f = text.find_first_not_of(" \t");
    text = f == std::string::npos ? "" : text.substr(f);
    if (!text.empty() && text.back() == '\r') text.pop_back();
    if (text.size() > max) text = text.substr(0, max - 3) + "...";
    return text;
}

} // namespace bf
