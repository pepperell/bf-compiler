// cli.cpp - console runner:  bf [options] program.b
#include "bf.h"
#include "devices.h"
#include "hw.h"
#include "registers.h"
#include "lang/lang.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace bf;

namespace {

struct StdioConsole : ConsoleIo {
    std::vector<uint8_t> buf;
    bool tty_in = _isatty(_fileno(stdin)) != 0;
    bool tty_out = _isatty(_fileno(stdout)) != 0;
    ULONGLONG last_flush = GetTickCount64();
    StdioConsole() { buf.reserve(1 << 16); }
    // Buffered for speed, but never held back long: a slow streaming program
    // (digits of e, say) shows output within 50 ms, and a terminal gets whole lines.
    void out(uint8_t c) override {
        buf.push_back(c);
        if (buf.size() >= (1 << 16) || (tty_out && c == '\n')) { flush(); return; }
        ULONGLONG now = GetTickCount64();
        if (now - last_flush >= 50) flush();
    }
    void flush() override {
        if (!buf.empty()) { fwrite(buf.data(), 1, buf.size(), stdout); buf.clear(); }
        fflush(stdout);
        last_flush = GetTickCount64();
    }
    int in() override {
        flush();
        if (tty_in) { int c = _getch(); if (c == '\r') c = '\n'; if (c == 26 || c == 4) return -1; putchar(c); fflush(stdout); return c & 255; }
        int c = getchar();
        return c == EOF ? -1 : (c & 255);
    }
};

// Keyboard registers fed from stdin: console => non-blocking, pipe/file => bytes.
// rel 0 = data latch, rel 1 = status (read: key waiting, write: pop into latch).
struct CliKeyboard : Device {
    StdioConsole* con = nullptr;
    int pending = -1;          // -1 none buffered, -2 EOF
    uint8_t data = 0;
    bool waiting() {
        if (con->tty_in) return _kbhit() != 0;
        if (pending == -1) { int c = getchar(); pending = (c == EOF) ? -2 : c; }
        return pending >= 0;
    }
    int pop() {
        if (con->tty_in) { if (!_kbhit()) return 0; int c = _getch(); return c == '\r' ? '\n' : c; }
        if (!waiting()) return 0;
        int c = pending; pending = -1; return c;
    }
    uint8_t read(uint32_t rel) override { return rel == 1 ? (waiting() ? 1 : 0) : data; }
    void write(uint32_t rel, uint8_t v) override { if (rel == 1) data = (uint8_t)pop(); else data = v; }
};

Machine* g_machine = nullptr;
BOOL WINAPI ctrl_handler(DWORD) { if (g_machine) g_machine->request_stop(); return TRUE; }

void usage() {
    fputs(
        "usage: bf [options] program.b | program.bfl\n"
        "       bf [options] -e 'code'\n"
        "  A .bfl file (or --lang) is compiled from BFL to brainfuck first.\n"
        "  --lang          treat the input as BFL whatever its name\n"
        "  --emit FILE     write the brainfuck produced from a .bfl file to FILE (and FILE.map), then exit\n"
        "  --warn          report BFL warnings (unused variables, truncation, unreachable code)\n"
        "  --check-heap    BFL: halt with a message on a null or out-of-range heap access\n"
        "  --check-stack   BFL: halt with a message when the recursion stack overflows\n"
        "  --interp        run the IR interpreter instead of the x64 JIT\n"
        "  --no-opt        disable IR optimisation\n"
        "  --dump          print the IR and exit\n"
        "  --regs          print the device register map as a Markdown table and exit\n"
        "  --time          print run time and status to stderr\n"
        "  --profile       run in the interpreter, then report where the executed instructions went\n"
        "                  (by BFL statement for a .bfl file, by brainfuck byte offset otherwise)\n"
        "  --eof MODE      ',' at end of input: nochange (default), 0, 255\n"
        "  --ram N         RAM cells (default 1048576)\n"
        "  --mmio N        device window cells (default 256, 0 disables devices)\n"
        "  --quiet-devices do not report LED / number display changes on stderr\n"
        "  --serial PORT   attach the serial device to COMn, or 'loopback' for testing\n",
        stderr);
}

// --profile: instructions executed per BFL statement (innermost statement only), or
// per brainfuck byte offset of the instruction for plain brainfuck.
void report_profile(const Program& prog, const std::vector<uint64_t>& counts, const bfl::Result* lr) {
    uint64_t total = 0;
    for (uint64_t n : counts) total += n;
    fprintf(stderr, "\nprofile: %llu IR instructions executed\n", (unsigned long long)total);
    if (!total) return;
    {
        static const char* names[] = {"ADD", "SET", "MUL", "MOVE", "OUT", "IN", "JZ", "JNZ", "ENDIF", "SCAN", "IDIOM", "LOOPX", "END"};
        uint64_t per[16] = {};
        for (size_t pc = 0; pc < counts.size(); pc++) per[prog.code[pc].op] += counts[pc];
        fprintf(stderr, "by kind:");
        for (int k = 0; k <= END; k++) if (per[k]) fprintf(stderr, " %s %.1f%%", names[k], 100.0 * (double)per[k] / (double)total);
        fprintf(stderr, "\n");
    }
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
        if (!counts[pc]) continue;
        int32_t off = prog.code[pc].src;
        if (!lr) { by[{-1, off}] += counts[pc]; continue; }
        auto it = std::upper_bound(lr->stmts.begin(), lr->stmts.end(), off,
                                   [](int32_t o, const bfl::StmtRange& r) { return o < r.start; });
        int k = (int)(it - lr->stmts.begin()) - 1;
        while (k >= 0 && lr->stmts[(size_t)k].end <= off) k = parent[(size_t)k];
        if (k < 0) { by[{-1, -1}] += counts[pc]; continue; }
        by[{lr->stmts[(size_t)k].file, lr->stmts[(size_t)k].line}] += counts[pc];
    }
    std::vector<std::pair<uint64_t, std::pair<int, int>>> rows;
    for (auto& kv : by) rows.push_back({kv.second, kv.first});
    std::sort(rows.rbegin(), rows.rend());
    for (size_t i = 0; i < rows.size() && i < 30; i++) {
        auto [n, key] = rows[i];
        std::string where, text;
        if (key.first < 0) where = key.second < 0 ? "(outside statements)" : "byte " + std::to_string(key.second);
        else {
            where = (key.first > 0 ? lr->files[(size_t)key.first] + ":" : std::string()) + std::to_string(key.second);
            const std::string& s = lr->sources[(size_t)key.first];
            size_t a = 0;
            for (int l = 1; l < key.second && a != std::string::npos; l++) { a = s.find('\n', a); if (a != std::string::npos) a++; }
            if (a != std::string::npos) {
                size_t b = s.find('\n', a);
                text = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
                size_t f = text.find_first_not_of(" \t");
                text = f == std::string::npos ? "" : text.substr(f);
                if (!text.empty() && text.back() == '\r') text.pop_back();
                if (text.size() > 70) text = text.substr(0, 67) + "...";
            }
        }
        fprintf(stderr, "%14llu %5.1f%%  %-12s %s\n", (unsigned long long)n, 100.0 * (double)n / (double)total, where.c_str(), text.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    bool use_jit = true, optimise = true, dump = false, show_time = false, quiet = false, profile = false;
    Machine::Config cfg;
    std::string src, path, serial_port, emit_path;
    bool lang = false, warn = false, check_heap = false, check_stack = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", what); exit(2); } return argv[++i]; };
        if (a == "--regs") { fputs(register_map_markdown().c_str(), stdout); return 0; }
        if (a == "--interp") use_jit = false;
        else if (a == "--no-opt") optimise = false;
        else if (a == "--dump") dump = true;
        else if (a == "--time") show_time = true;
        else if (a == "--profile") profile = true;
        else if (a == "--quiet-devices") quiet = true;
        else if (a == "--ram") cfg.ram_size = strtoull(need("--ram"), nullptr, 0);
        else if (a == "--mmio") cfg.mmio_size = (uint32_t)strtoul(need("--mmio"), nullptr, 0);
        else if (a == "--eof") {
            std::string m = need("--eof");
            if (m == "0") cfg.eof = EofMode::Zero; else if (m == "255" || m == "-1") cfg.eof = EofMode::MinusOne;
            else if (m == "nochange") cfg.eof = EofMode::NoChange; else { fprintf(stderr, "bad --eof\n"); return 2; }
        }
        else if (a == "-e") src = need("-e");
        else if (a == "--serial") serial_port = need("--serial");
        else if (a == "--emit") emit_path = need("--emit");
        else if (a == "--lang") lang = true;
        else if (a == "--warn") warn = true;
        else if (a == "--check-heap") check_heap = true;
        else if (a == "--check-stack") check_stack = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else path = a;
    }
    if (src.empty()) {
        if (path.empty()) { usage(); return 2; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
        std::stringstream ss; ss << f.rdbuf(); src = ss.str();
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".bfl") == 0) lang = true;
    }
    bfl::Result lr;
    if (lang) {
        bfl::Options lo;
        lo.warnings = warn;
        lo.check_heap = check_heap;
        lo.check_stack = check_stack;
        if (getenv("BFL_NO_LAYOUT")) lo.layout = false;            // for measuring
        lo.file_name = path.empty() ? "<code>" : path;
        lr = bfl::compile(src, cfg.mmio_size, lo);
        for (const bfl::Error& e : lr.warnings) fprintf(stderr, "%s\n", bfl::format_error(lr, e, "warning").c_str());
        if (!lr.ok()) {
            for (const bfl::Error& e : lr.errors) fprintf(stderr, "%s\n", bfl::format_error(lr, e).c_str());
            return 1;
        }
        if (!emit_path.empty()) {
            std::ofstream o(emit_path, std::ios::binary), m(emit_path + ".map", std::ios::binary);
            if (!o || !m) { fprintf(stderr, "cannot write %s\n", emit_path.c_str()); return 2; }
            o << lr.bf; m << bfl::describe(lr);
            Program lp = compile(lr.bf, true);
            fprintf(stderr, "%zu commands (%zu bytes annotated), %lld cells, %zu IR instructions, %zu bytes of x64\n",
                    lp.raw_ops, lr.bf.size(), (long long)lr.cells_used, lp.code.size(), jit_code_size(lp));
            return 0;
        }
        src = lr.bf;
    }

    Program prog = compile(src, optimise);
    if (!prog.error.empty()) { fprintf(stderr, "compile error: %s\n", prog.error.c_str()); return 1; }
    if (dump) { fputs(disassemble(prog).c_str(), stdout); fprintf(stderr, "%zu commands -> %zu IR instructions\n", prog.raw_ops, prog.code.size()); return 0; }

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    {   // let a Windows console interpret VT100 / ANSI escape sequences in the program's output
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    StdioConsole con;
    Machine m(cfg);
    m.set_console(&con);
    StdDevices dev;
    CliKeyboard kb; kb.con = &con;
    SerialDevice ser;
    FramebufferDevice fb;
    if (cfg.mmio_size >= 256) {
        dev.install(m, &con);
        m.map(A_KEY_DATA, 2, &kb);
        install_hw(m, ser, fb);
        if (!serial_port.empty()) {
            ser.configure(serial_port);
            if (!ser.is_open()) { fprintf(stderr, "cannot open serial port %s\n", serial_port.c_str()); return 2; }
        }
        if (!quiet) {
            dev.leds.on_change = [&](uint8_t v) {
                con.flush();
                char s[64]; int n = snprintf(s, sizeof s, "[LEDS ");
                for (int b = 7; b >= 0; b--) s[n++] = (v >> b) & 1 ? '#' : '.';
                snprintf(s + n, sizeof s - n, "]\n"); fputs(s, stderr);
            };
            dev.number.on_change = [&](uint16_t v) { con.flush(); fprintf(stderr, "[NUMBER %u]\n", v); };
        }
    }
    g_machine = &m;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    std::vector<uint64_t> counts;
    if (profile) m.profile = &counts;
    RunResult r = m.run(prog, use_jit);
    con.flush();
    if (profile) report_profile(prog, counts, lang ? &lr : nullptr);
    if (show_time || (r.status != Status::Ok && r.status != Status::Halted))
        fprintf(stderr, "[%s%s%s] %.3f s, final cell %lld\n", status_name(r.status),
                r.message.empty() ? "" : ": ", r.message.c_str(), r.seconds, (long long)r.ptr);
    return r.status == Status::Ok || r.status == Status::Halted ? 0 : 3;
}
