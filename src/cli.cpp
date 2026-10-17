// cli.cpp - console runner:  bf [options] program.b
#include "bf.h"
#include "devices.h"
#include "hw.h"
#include "registers.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace bf;

namespace {

struct StdioConsole : ConsoleIo {
    std::vector<uint8_t> buf;
    bool tty_in = _isatty(_fileno(stdin)) != 0;
    StdioConsole() { buf.reserve(1 << 16); }
    void out(uint8_t c) override { buf.push_back(c); if (buf.size() >= (1 << 16)) flush(); }
    void flush() override { if (!buf.empty()) { fwrite(buf.data(), 1, buf.size(), stdout); buf.clear(); } fflush(stdout); }
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
        "usage: bf [options] program.b\n"
        "       bf [options] -e 'code'\n"
        "  --interp        run the IR interpreter instead of the x64 JIT\n"
        "  --no-opt        disable IR optimisation\n"
        "  --dump          print the IR and exit\n"
        "  --regs          print the device register map as a Markdown table and exit\n"
        "  --time          print run time and status to stderr\n"
        "  --eof MODE      ',' at end of input: nochange (default), 0, 255\n"
        "  --ram N         RAM cells (default 1048576)\n"
        "  --mmio N        device window cells (default 256, 0 disables devices)\n"
        "  --quiet-devices do not report LED / number display changes on stderr\n"
        "  --serial PORT   attach the serial device to COMn, or 'loopback' for testing\n",
        stderr);
}

} // namespace

int main(int argc, char** argv) {
    bool use_jit = true, optimise = true, dump = false, show_time = false, quiet = false;
    Machine::Config cfg;
    std::string src, path, serial_port;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", what); exit(2); } return argv[++i]; };
        if (a == "--regs") { fputs(register_map_markdown().c_str(), stdout); return 0; }
        if (a == "--interp") use_jit = false;
        else if (a == "--no-opt") optimise = false;
        else if (a == "--dump") dump = true;
        else if (a == "--time") show_time = true;
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
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else path = a;
    }
    if (src.empty()) {
        if (path.empty()) { usage(); return 2; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
        std::stringstream ss; ss << f.rdbuf(); src = ss.str();
    }

    Program prog = compile(src, optimise);
    if (!prog.error.empty()) { fprintf(stderr, "compile error: %s\n", prog.error.c_str()); return 1; }
    if (dump) { fputs(disassemble(prog).c_str(), stdout); fprintf(stderr, "%zu commands -> %zu IR instructions\n", prog.raw_ops, prog.code.size()); return 0; }

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
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

    RunResult r = m.run(prog, use_jit);
    con.flush();
    if (show_time || (r.status != Status::Ok && r.status != Status::Halted))
        fprintf(stderr, "[%s%s%s] %.3f s, final cell %lld\n", status_name(r.status),
                r.message.empty() ? "" : ": ", r.message.c_str(), r.seconds, (long long)r.ptr);
    return r.status == Status::Ok || r.status == Status::Halted ? 0 : 3;
}
