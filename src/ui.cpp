// ui.cpp - Win32 front end:  bfui.exe [program.b]
//
// Layout:
//   +----------------------------------------------+------------------+
//   | device panel: lamps, number display, status  |                  |
//   | toolbar: Open Save Run Debug Step Pause Stop |  memory viewer   |
//   +----------------------------------------------+  (hex dump of    |
//   | source editor (RichEdit; breakpoints in red) |   any tape       |
//   +----------------------------------------------+   region)        |
//   | VT100 terminal (keys typed here feed ',')    |                  |
//   +----------------------------------------------+------------------+
//   | status bar                                                      |
//   +-----------------------------------------------------------------+
// Normal runs use the JIT on a worker thread.  Debug sessions use the
// interpreter (bf::Debugger) so the pointer and source position are known
// at every step.  The UI thread only touches the machine through
// request_stop(), bus_read(), the Debugger API and the thread-safe devices.
#include "bf.h"
#include "devices.h"
#include "hw.h"
#include "help.h"
#include "lang/lang.h"
#include "terminal.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace bf;

namespace {

enum : UINT { WM_APP_OUTPUT = WM_APP + 1, WM_APP_DONE, WM_APP_DEVICE, WM_APP_DEBUG };
enum : int {
    ID_OPEN = 101, ID_RELOAD, ID_SAVE, ID_SAVEAS, ID_EXIT,
    ID_RUN, ID_DEBUG, ID_STEP, ID_STEP_OVER, ID_RUN_TO_CURSOR, ID_PAUSE, ID_STOP, ID_CLEAR, ID_INTERP, ID_TOGGLE_BP, ID_CLEAR_BP, ID_MEMMAP,
    ID_EDITOR, ID_CONSOLE, ID_MEMVIEW, ID_MEM_ADDR, ID_MEM_GO, ID_MEM_PREV, ID_MEM_NEXT, ID_MEM_PTR, ID_MEM_START, ID_MEM_DEV, ID_MEM_AUTO,
    ID_MEM_FOCUS, ID_BAUD0, ID_BAUD1, ID_BAUD2, ID_BAUD3, ID_BAUD4,
    ID_TB_SPEED, ID_TB_INTERP,
    ID_WATCHLIST, ID_WATCH_ADD, ID_WATCH_PTR, ID_WATCH_REMOVE, ID_WATCH_DEV,
    ID_VIEW_FB, ID_LANG, ID_VIEW_GEN, ID_EXPORT_BF, ID_GENEDIT,
    ID_SER_NONE, ID_SER_LOOP, ID_SER_COM_BASE = 400,  // ID_SER_COM_BASE + n = COMn
    ID_HELP_BASE = 700                                             // ID_HELP_BASE + (int)help::Topic
};
enum : UINT { WM_APP_FBSHOW = WM_APP + 10 };
const int BAUDS[5] = {0, 115200, 38400, 9600, 2400};
enum : int { TIMER_MEM = 1 };

constexpr int PANEL_H = 72;
constexpr int MEM_ROWS = 32, MEM_COLS = 16;
constexpr COLORREF BP_COLOUR = RGB(255, 150, 150);

enum class Mode { Idle, Running, DebugPaused, DebugRunning };

struct UiConsole : ConsoleIo {
    HWND hwnd = nullptr;
    KeyQueue* keys = nullptr;
    std::mutex mu;
    std::string pending;
    std::atomic<bool> posted{false};
    std::atomic<int> baud{0};                 // 0 = unlimited; otherwise pace output like a serial line
    std::chrono::steady_clock::time_point next{};
    void out(uint8_t c) override {
        int b = baud.load(std::memory_order_relaxed);
        if (b > 0) {
            auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;
            next += std::chrono::nanoseconds(10'000'000'000LL / b);   // 10 bits per byte
            if (next - now > std::chrono::milliseconds(1)) std::this_thread::sleep_until(next);
        }
        { std::lock_guard<std::mutex> g(mu); pending.push_back((char)c); }
        if (!posted.exchange(true)) PostMessageW(hwnd, WM_APP_OUTPUT, 0, 0);
    }
    int in() override { return keys ? keys->wait_pop() : -1; }
    void cancel() override { if (keys) keys->cancel(); }
    std::string take() { std::lock_guard<std::mutex> g(mu); posted = false; std::string s; s.swap(pending); return s; }
};

struct App {
    HWND  wnd = nullptr, editor = nullptr, console = nullptr, status = nullptr;
    HWND  memview = nullptr, mem_addr = nullptr, mem_auto = nullptr;
    HWND  mem_buttons[6] = {};
    HWND  tb_buttons[10] = {}, tb_speed = nullptr, tb_interp = nullptr;
    HWND  watchlist = nullptr, watch_add = nullptr, watch_ptr = nullptr, watch_remove = nullptr, watch_dev = nullptr;
    HFONT mono = nullptr, ui_font = nullptr, big = nullptr, memfont = nullptr;
    HMENU menu = nullptr;
    HACCEL accel = nullptr;
    UINT  dpi = 96;
    Machine*   machine = nullptr;
    StdDevices dev;
    SerialDevice      ser;
    FramebufferDevice fb;
    HWND  fbwnd = nullptr;
    std::vector<std::string> com_ports;
    UiConsole  con;
    Program    prog;
    std::wstring path;
    std::thread worker;
    Mode  mode = Mode::Idle;
    std::atomic<bool> running{false};        // a program is executing or paused (keys go to it)
    RunResult  last{};
    bool have_last = false;
    bool use_interp = false;
    bool modified = false;
    int  suppress_change = 0;
    int64_t mem_base = 256;
    std::atomic<bool> device_posted{false};
    int  console_h = 0;
    bool dragging = false;
    RECT splitter{};
    // debugging
    std::unique_ptr<Debugger> dbg;
    std::set<int32_t> bps;                   // source offsets with a breakpoint
    std::unordered_multimap<int32_t, int32_t> pc_of_src;
    // BFL (the compiled language, src/lang): the editor holds .bfl source, which
    // is compiled to brainfuck first; the statement map drives the debugger.
    bool lang = false;
    bfl::Result lr;
    HWND genwnd = nullptr, genedit = nullptr;     // "Generated brainfuck" window
    std::vector<std::pair<int32_t, int32_t>> src_pc;   // (source offset, pc) of the debug program, sorted
    std::set<int64_t> auto_watch;                 // watch rows added from the symbol table
} app;

int px(int v) { return MulDiv(v, (int)app.dpi, 96); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(n, 0);
    if (n) GetWindowTextW(h, w.data(), n + 1);
    return w;
}

// Editor text with CR-only line breaks: character offsets equal RichEdit
// positions, so IR source offsets map straight onto the editor.
std::wstring editor_text() {
    GETTEXTLENGTHEX gl{GTL_DEFAULT | GTL_NUMCHARS, 1200};
    LRESULT n = SendMessageW(app.editor, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    std::wstring w((size_t)n + 1, 0);
    GETTEXTEX gt{};
    gt.cb = (DWORD)((n + 1) * sizeof(wchar_t));
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    LRESULT got = SendMessageW(app.editor, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)w.data());
    w.resize((size_t)got);
    return w;
}
// One byte per editor character, so compile offsets match editor positions.
std::string latin1(const std::wstring& w) {
    std::string s(w.size(), 0);
    for (size_t i = 0; i < w.size(); i++) s[i] = w[i] < 256 ? (char)w[i] : '?';
    return s;
}
bool is_cmd(wchar_t c) { return c == '+' || c == '-' || c == '<' || c == '>' || c == '.' || c == ',' || c == '[' || c == ']'; }
// In BFL mode a breakpoint sits on the first non-blank character of a line.
bool is_bp_char(wchar_t c) { return app.lang ? !(c == L' ' || c == L'\t' || c == L'\r') : is_cmd(c); }
bool is_bfl_path(const std::wstring& p) { size_t n = p.size(); return n > 4 && _wcsicmp(p.c_str() + n - 4, L".bfl") == 0; }
// 1-based line/column -> character offset in CR-separated editor text.
int32_t offset_of(const std::wstring& text, int line, int col) {
    int l = 1; int32_t i = 0;
    while (l < line && i < (int32_t)text.size()) { if (text[i] == L'\r') l++; i++; }
    return std::min(i + std::max(col, 1) - 1, (int32_t)text.size());
}
int32_t line_end(const std::wstring& text, int32_t pos) { while (pos < (int32_t)text.size() && text[pos] != L'\r') pos++; return pos; }
int32_t line_start(const std::wstring& text, int32_t pos) { while (pos > 0 && text[pos - 1] != L'\r') pos--; return pos; }
int line_of_offset(const std::wstring& text, int32_t pos) { int l = 1; for (int32_t i = 0; i < pos && i < (int32_t)text.size(); i++) if (text[i] == L'\r') l++; return l; }

void set_status(const std::wstring& s) { SendMessageW(app.status, SB_SETTEXTW, 0, (LPARAM)s.c_str()); }
void update_title();
void set_lang(bool on) {
    app.lang = on;
    CheckMenuItem(app.menu, ID_LANG, on ? MF_CHECKED : MF_UNCHECKED);
    EnableMenuItem(app.menu, ID_VIEW_GEN, on ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(app.menu, ID_EXPORT_BF, on ? MF_ENABLED : MF_GRAYED);
    update_title();
}

void set_baud_index(int i) {
    app.con.baud = BAUDS[i];
    CheckMenuRadioItem(app.menu, ID_BAUD0, ID_BAUD4, ID_BAUD0 + i, MF_BYCOMMAND);
    SendMessageW(app.tb_speed, CB_SETCURSEL, i, 0);
}
void set_interp(bool on) {
    app.use_interp = on;
    CheckMenuItem(app.menu, ID_INTERP, on ? MF_CHECKED : MF_UNCHECKED);
    if (app.tb_interp) SendMessageW(app.tb_interp, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// toolbar button order
enum { TB_OPEN, TB_SAVE, TB_RUN, TB_DEBUG, TB_STEP, TB_OVER, TB_CURSOR, TB_PAUSE, TB_STOP, TB_CLEAR, TB_COUNT };

void set_mode(Mode m) {
    app.mode = m;
    bool idle = m == Mode::Idle, paused = m == Mode::DebugPaused, drun = m == Mode::DebugRunning, run = m == Mode::Running;
    auto en = [](int id, int btn, bool on) {
        EnableMenuItem(app.menu, id, on ? MF_ENABLED : MF_GRAYED);
        if (btn >= 0) EnableWindow(app.tb_buttons[btn], on);
    };
    en(ID_RUN, TB_RUN, idle || paused);
    en(ID_DEBUG, TB_DEBUG, idle);
    en(ID_STEP, TB_STEP, paused);
    en(ID_STEP_OVER, TB_OVER, paused);
    en(ID_RUN_TO_CURSOR, TB_CURSOR, paused);
    en(ID_PAUSE, TB_PAUSE, drun);
    en(ID_STOP, TB_STOP, !idle);
    en(ID_OPEN, TB_OPEN, idle);
    en(ID_RELOAD, -1, idle);
    SetWindowTextW(app.tb_buttons[TB_RUN], paused ? L"▶ Continue" : L"▶ Run");
    SendMessageW(app.editor, EM_SETREADONLY, !idle, 0);
    app.running = !idle;
    if (run || drun) { if (SendMessageW(app.mem_auto, BM_GETCHECK, 0, 0) == BST_CHECKED) SetTimer(app.wnd, TIMER_MEM, 150, nullptr); }
    else KillTimer(app.wnd, TIMER_MEM);
    RECT rc; GetClientRect(app.wnd, &rc); rc.bottom = px(PANEL_H);
    InvalidateRect(app.wnd, &rc, FALSE);
}

void notify_device() {
    if (!app.device_posted.exchange(true)) PostMessageW(app.wnd, WM_APP_DEVICE, 0, 0);
}

// ---- memory viewer ---------------------------------------------------------------
int64_t current_ptr() {
    if (app.dbg) return app.dbg->ptr();
    if (app.mode == Mode::Idle && app.have_last) return app.last.ptr;
    return -1;
}

// ---- watch list: cells shown live; a checked row pauses the program after a write ----
int watch_count() { return (int)SendMessageW(app.watchlist, LVM_GETITEMCOUNT, 0, 0); }
int64_t watch_addr(int i) {
    LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = i;
    SendMessageW(app.watchlist, LVM_GETITEMW, 0, (LPARAM)&it);
    return (int64_t)it.lParam;
}
bool watch_checked(int i) { return ((SendMessageW(app.watchlist, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK) >> 12) - 1) == 1; }
int  watch_find(int64_t a) { for (int i = 0; i < watch_count(); i++) if (watch_addr(i) == a) return i; return -1; }

void refresh_watches() {
    wchar_t buf[32];
    for (int i = 0; i < watch_count(); i++) {
        uint8_t v = app.machine->bus_read(watch_addr(i));
        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = buf;
        it.iSubItem = 1; swprintf(buf, 32, L"%02X", v);  SendMessageW(app.watchlist, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; swprintf(buf, 32, L"%u", v);    SendMessageW(app.watchlist, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; swprintf(buf, 32, L"%lc", (v >= 32 && v < 127) ? (wchar_t)v : L'.'); SendMessageW(app.watchlist, LVM_SETITEMW, 0, (LPARAM)&it);
    }
}
void watch_add(int64_t a) {
    if (a < 0 || (uint64_t)a >= app.machine->tape_size() || watch_find(a) >= 0) return;
    wchar_t buf[32]; swprintf(buf, 32, L"%06llX", (long long)a);
    LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = watch_count(); it.pszText = buf; it.lParam = (LPARAM)a;
    SendMessageW(app.watchlist, LVM_INSERTITEMW, 0, (LPARAM)&it);
    refresh_watches();
}
void watch_remove_selected() {
    int i = (int)SendMessageW(app.watchlist, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i < 0) return;
    if (app.dbg) app.dbg->set_watch(watch_addr(i), false);
    SendMessageW(app.watchlist, LVM_DELETEITEM, i, 0);
}
bool watch_dev_checked() { return SendMessageW(app.watch_dev, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void watches_to_session() {
    if (!app.dbg) return;
    app.dbg->clear_watches();
    for (int i = 0; i < watch_count(); i++) if (watch_checked(i)) app.dbg->set_watch(watch_addr(i), true);
    app.dbg->set_watch_device_window(watch_dev_checked());
}
void watch_add_named(int64_t a, const std::wstring& label) {
    if (a < 0 || (uint64_t)a >= app.machine->tape_size() || watch_find(a) >= 0) return;
    LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = watch_count(); it.pszText = (LPWSTR)label.c_str(); it.lParam = (LPARAM)a;
    SendMessageW(app.watchlist, LVM_INSERTITEMW, 0, (LPARAM)&it);
}
// Replace the rows that came from the BFL symbol table with the current globals.
void auto_watches() {
    for (int i = watch_count() - 1; i >= 0; i--) if (app.auto_watch.count(watch_addr(i))) SendMessageW(app.watchlist, LVM_DELETEITEM, i, 0);
    app.auto_watch.clear();
    wchar_t buf[96];
    for (const bfl::Symbol& v : app.lr.symbols) {
        if (!v.scope.empty()) continue;
        std::wstring name = widen(v.name);
        if (v.kind == 'b') { swprintf(buf, 96, L"%s  %llX", name.c_str(), (long long)v.cell); watch_add_named(v.cell, buf); app.auto_watch.insert(v.cell); }
        else if (v.kind == 'w') {
            swprintf(buf, 96, L"%s lo  %llX", name.c_str(), (long long)v.cell); watch_add_named(v.cell, buf); app.auto_watch.insert(v.cell);
            swprintf(buf, 96, L"%s hi  %llX", name.c_str(), (long long)v.cell + 1); watch_add_named(v.cell + 1, buf); app.auto_watch.insert(v.cell + 1);
        } else {
            for (int k = 0; k < v.count && k < 16; k++) {
                int64_t c = bfl::array_cell(v.cell, k);
                swprintf(buf, 96, L"%s[%d]  %llX", name.c_str(), k, (long long)c); watch_add_named(c, buf); app.auto_watch.insert(c);
            }
        }
    }
    refresh_watches();
}

void refresh_memory() {
    refresh_watches();
    Machine& m = *app.machine;
    int64_t size = (int64_t)m.tape_size();
    int64_t page = MEM_ROWS * MEM_COLS;
    if (app.mem_base > size - page) app.mem_base = size - page;
    if (app.mem_base < 0) app.mem_base = 0;
    app.mem_base &= ~(int64_t)(MEM_COLS - 1);
    int64_t ptr = current_ptr();
    int64_t mmio = m.mmio_size();

    std::wstring s;
    s.reserve(MEM_ROWS * 90);
    wchar_t line[128];
    for (int r = 0; r < MEM_ROWS; r++) {
        int64_t a = app.mem_base + r * MEM_COLS;
        int n = swprintf(line, 128, L"%c%06llX ", a < mmio ? L'D' : L' ', (long long)a);
        uint8_t row[MEM_COLS];
        for (int c = 0; c < MEM_COLS; c++) row[c] = m.bus_read(a + c);
        for (int c = 0; c < MEM_COLS; c++) {
            bool at = (a + c == ptr);
            n += swprintf(line + n, 128 - n, at ? L"[%02X]" : L" %02X ", row[c]);
            if (c == 7) line[n++] = L' ';
        }
        line[n++] = L' ';
        for (int c = 0; c < MEM_COLS; c++) line[n++] = (row[c] >= 32 && row[c] < 127) ? (wchar_t)row[c] : L'.';
        line[n++] = L'\r'; line[n++] = L'\n'; line[n] = 0;
        s += line;
    }
    SetWindowTextW(app.memview, s.c_str());
    swprintf(line, 128, L"%llX", (long long)app.mem_base);
    if (GetFocus() != app.mem_addr) SetWindowTextW(app.mem_addr, line);
}

void mem_goto(int64_t a) { app.mem_base = a; refresh_memory(); }
void mem_show_ptr() {
    int64_t p = current_ptr();
    if (p < 0) return;
    if (p < app.mem_base || p >= app.mem_base + MEM_ROWS * MEM_COLS) mem_goto((p - MEM_ROWS / 2 * MEM_COLS) & ~(int64_t)(MEM_COLS - 1));
    else refresh_memory();
}
bool mem_auto() { return SendMessageW(app.mem_auto, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// ---- editor helpers ------------------------------------------------------------------
void editor_select(int32_t a, int32_t b, bool scroll) {
    CHARRANGE cr{a, b};
    SendMessageW(app.editor, EM_EXSETSEL, 0, (LPARAM)&cr);
    if (scroll) SendMessageW(app.editor, EM_SCROLLCARET, 0, 0);
}
void editor_get_sel(CHARRANGE& cr) { SendMessageW(app.editor, EM_EXGETSEL, 0, (LPARAM)&cr); }

void mark_bp(int32_t pos, bool on) {
    app.suppress_change++;
    CHARRANGE save; editor_get_sel(save);
    SendMessageW(app.editor, WM_SETREDRAW, FALSE, 0);
    editor_select(pos, pos + 1, false);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BACKCOLOR;
    if (on) cf.crBackColor = BP_COLOUR; else cf.dwEffects = CFE_AUTOBACKCOLOR;
    SendMessageW(app.editor, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(app.editor, EM_EXSETSEL, 0, (LPARAM)&save);
    SendMessageW(app.editor, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.editor, nullptr, FALSE);
    app.suppress_change--;
}

// Recover breakpoints from the red background marks (they move with edits).
void collect_bps(const std::wstring& text, int32_t a, int32_t b, std::set<int32_t>& out) {
    if (a >= b) return;
    editor_select(a, b, false);
    CHARFORMAT2W cf{}; cf.cbSize = sizeof cf;
    SendMessageW(app.editor, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    if (cf.dwMask & CFM_BACKCOLOR) {                     // uniform over the range
        if (!(cf.dwEffects & CFE_AUTOBACKCOLOR) && cf.crBackColor == BP_COLOUR)
            for (int32_t i = a; i < b; i++) if (is_bp_char(text[i])) out.insert(i);
        return;
    }
    int32_t mid = a + (b - a) / 2;
    collect_bps(text, a, mid, out);
    collect_bps(text, mid, b, out);
}
void sync_bps_from_editor(const std::wstring& text) {
    app.suppress_change++;
    CHARRANGE save; editor_get_sel(save);
    SendMessageW(app.editor, WM_SETREDRAW, FALSE, 0);
    app.bps.clear();
    collect_bps(text, 0, (int32_t)text.size(), app.bps);
    SendMessageW(app.editor, EM_EXSETSEL, 0, (LPARAM)&save);
    SendMessageW(app.editor, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.editor, nullptr, FALSE);
    app.suppress_change--;
}

// ---- statement map helpers (BFL mode) ----------------------------------------------------------
// First pc whose source offset lies in [lo, hi), or -1.
int32_t first_pc_in(int32_t lo, int32_t hi) {
    auto it = std::lower_bound(app.src_pc.begin(), app.src_pc.end(), std::make_pair(lo, INT32_MIN));
    if (it == app.src_pc.end() || it->first >= hi) return -1;
    return it->second;
}
// Innermost statement whose generated range contains the brainfuck offset.
int stmt_at(int32_t src) {
    int best = -1;
    for (size_t i = 0; i < app.lr.stmts.size(); i++) {
        const bfl::StmtRange& r = app.lr.stmts[i];
        if (src >= r.start && src < r.end && (best < 0 || r.depth > app.lr.stmts[best].depth)) best = (int)i;
    }
    return best;
}
// Statements starting on a source line, outermost first.
std::vector<int> stmts_on_line(int line) {
    std::vector<int> v;
    for (size_t i = 0; i < app.lr.stmts.size(); i++) if (app.lr.stmts[i].line == line) v.push_back((int)i);
    std::sort(v.begin(), v.end(), [](int a, int b) { return app.lr.stmts[a].depth < app.lr.stmts[b].depth; });
    return v;
}
// The debug-program pcs a breakpoint at editor position pos stands for.
std::vector<int32_t> pcs_for_bp(int32_t pos, const std::wstring& text) {
    std::vector<int32_t> v;
    if (!app.lang) { auto r = app.pc_of_src.equal_range(pos); for (auto it = r.first; it != r.second; ++it) v.push_back(it->second); return v; }
    for (int i : stmts_on_line(line_of_offset(text, pos))) {
        int32_t pc = first_pc_in(app.lr.stmts[i].start, app.lr.stmts[i].end);
        if (pc >= 0) v.push_back(pc);
    }
    return v;
}
void select_stmt(int i, const std::wstring& text) {
    const bfl::StmtRange& r = app.lr.stmts[i];
    int32_t a = offset_of(text, r.line, r.col);
    editor_select(a, line_end(text, a), true);
}

void toggle_bp_at_caret() {
    std::wstring text = editor_text();
    CHARRANGE cr; editor_get_sel(cr);
    int32_t pos = cr.cpMin;
    if (app.lang) pos = line_start(text, pos);
    // the command at the caret, else the next one on the same line (BFL: the first non-blank character of the line)
    while (pos < (int32_t)text.size() && !is_bp_char(text[pos]) && text[pos] != L'\r') pos++;
    if (pos >= (int32_t)text.size() || !is_bp_char(text[pos])) { set_status(app.lang ? L"Empty line." : L"No command at or after the caret on this line."); return; }
    bool on = !app.bps.count(pos);
    if (on) app.bps.insert(pos); else app.bps.erase(pos);
    mark_bp(pos, on);
    if (app.dbg) for (int32_t pc : pcs_for_bp(pos, text)) app.dbg->set_breakpoint(pc, on);
    wchar_t buf[96]; swprintf(buf, 96, L"Breakpoint %s at offset %d (%zu total).", on ? L"set" : L"cleared", pos, app.bps.size());
    set_status(buf);
}
void clear_all_bps() {
    for (int32_t p : app.bps) mark_bp(p, false);
    app.bps.clear();
    if (app.dbg) app.dbg->clear_breakpoints();
    set_status(L"Breakpoints cleared.");
}

void update_title() {
    std::wstring name = app.path.empty() ? L"untitled" : app.path.substr(app.path.find_last_of(L"\\/") + 1);
    SetWindowTextW(app.wnd, (name + (app.modified ? L" *" : L"") + (app.lang ? L"  [BFL]" : L"") + L" - Brainfuck Machine").c_str());
}

void append_output(const std::string& bytes) { term::feed(app.console, bytes.data(), bytes.size()); }

void gen_update() {
    if (!app.genedit) return;
    std::wstring w; w.reserve(app.lr.bf.size() + 256);
    for (char c : app.lr.bf) { if (c == '\n') w += L"\r\n"; else w += (wchar_t)(unsigned char)c; }
    SetWindowTextW(app.genedit, w.c_str());
}

// Compile the editor contents: BFL -> brainfuck -> IR, or brainfuck -> IR.
bool compile_editor(bool optimise) {
    std::wstring text = editor_text();
    std::string s = latin1(text);
    if (app.lang) {
        for (char& c : s) if (c == '\r') c = '\n';
        app.lr = bfl::compile(s, app.machine->mmio_size());
        if (!app.lr.ok()) {
            const bfl::Error& e = app.lr.errors[0];
            wchar_t buf[512];
            swprintf(buf, 512, L"BFL error, line %d col %d: %s", e.line, e.col, widen(e.msg).c_str());
            set_status(buf);
            int32_t p = offset_of(text, e.line, e.col);
            editor_select(p, line_end(text, p), true);
            SetFocus(app.editor);
            gen_update();
            return false;
        }
        gen_update();
        s = app.lr.bf;
    }
    app.prog = compile(s, optimise);
    if (!app.prog.error.empty()) { set_status(L"Compile error: " + widen(app.prog.error)); return false; }
    return true;
}

// ---- files --------------------------------------------------------------------------
bool load_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { MessageBoxW(app.wnd, L"Cannot open file.", L"bf", MB_ICONERROR); return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    std::string crlf; crlf.reserve(src.size() + src.size() / 20);
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\r') continue;
        if (src[i] == '\n') crlf += "\r\n"; else crlf += src[i];
    }
    app.suppress_change++;
    SetWindowTextW(app.editor, widen(crlf).c_str());
    SendMessageW(app.editor, EM_EMPTYUNDOBUFFER, 0, 0);
    app.suppress_change--;
    app.bps.clear();
    app.path = path;
    app.modified = false;
    set_lang(is_bfl_path(path));
    if (!compile_editor(true)) return false;
    wchar_t buf[256];
    if (app.lang) swprintf(buf, 256, L"Loaded BFL: %zu bytes of brainfuck (%lld cells), %zu IR instructions. F5 runs, Ctrl+F5 debugs, Ctrl+B shows the brainfuck.", app.lr.bf.size(), (long long)app.lr.cells_used, app.prog.code.size());
    else swprintf(buf, 256, L"Loaded: %zu commands, %zu IR instructions. F5 runs, Ctrl+F5 debugs.", app.prog.raw_ops, app.prog.code.size());
    set_status(buf);
    return true;
}

bool save_file(bool ask) {
    if (ask || app.path.empty()) {
        wchar_t file[MAX_PATH] = L"";
        if (!app.path.empty()) wcsncpy(file, app.path.c_str(), MAX_PATH - 1);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = app.wnd;
        ofn.lpstrFilter = L"Programs (*.b;*.bf;*.bfl)\0*.b;*.bf;*.bfl\0Brainfuck (*.b;*.bf)\0*.b;*.bf\0BFL source (*.bfl)\0*.bfl\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = app.lang ? L"bfl" : L"b";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
        if (!GetSaveFileNameW(&ofn)) return false;
        app.path = file;
        set_lang(is_bfl_path(app.path));
    }
    std::wstring w = editor_text();
    std::string lf; lf.reserve(w.size());
    for (wchar_t c : w) {
        if (c == L'\r') lf += '\n';
        else if (c < 128) lf += (char)c;
        else { char u[8]; int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, 8, nullptr, nullptr); lf.append(u, n); }
    }
    std::ofstream f(app.path, std::ios::binary);
    if (!f) { MessageBoxW(app.wnd, L"Cannot write file.", L"bf", MB_ICONERROR); return false; }
    f.write(lf.data(), (std::streamsize)lf.size());
    app.modified = false;
    update_title();
    set_status(L"Saved " + app.path);
    return true;
}

void open_dialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = app.wnd;
    ofn.lpstrFilter = L"Programs (*.b;*.bf;*.bfl)\0*.b;*.bf;*.bfl\0Brainfuck (*.b;*.bf)\0*.b;*.bf\0BFL source (*.bfl)\0*.bfl\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) load_file(file);
}

// Write the brainfuck generated from the BFL source to a .b file.
void export_bf() {
    if (!app.lang) { set_status(L"The editor holds brainfuck already; use Save."); return; }
    if (!compile_editor(true)) return;
    wchar_t file[MAX_PATH] = L"";
    if (!app.path.empty()) { std::wstring n = app.path.substr(0, app.path.size() - 4) + L".b"; wcsncpy(file, n.c_str(), MAX_PATH - 1); }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = app.wnd;
    ofn.lpstrFilter = L"Brainfuck (*.b)\0*.b\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"b";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;
    std::ofstream f(file, std::ios::binary);
    if (!f) { MessageBoxW(app.wnd, L"Cannot write file.", L"bf", MB_ICONERROR); return; }
    f.write(app.lr.bf.data(), (std::streamsize)app.lr.bf.size());
    set_status(std::wstring(L"Exported ") + file);
}

// ---- running (JIT) ------------------------------------------------------------------
void start_run() {
    if (app.mode != Mode::Idle) return;
    if (app.worker.joinable()) app.worker.join();
    if (!compile_editor(true)) return;
    term::clear(app.console);
    app.dev.reset();
    app.fb.clear();
    app.machine->reset();
    app.have_last = false;
    set_mode(Mode::Running);
    set_status(app.use_interp ? L"Running (interpreter)..." : L"Running (JIT)...");
    notify_device();
    bool jit = !app.use_interp;
    app.worker = std::thread([jit] {
        app.last = app.machine->run(app.prog, jit);
        PostMessageW(app.wnd, WM_APP_DONE, 0, 0);
    });
}

std::wstring result_text(const RunResult& r) {
    wchar_t buf[512];
    std::wstring msg = widen(r.message);
    swprintf(buf, 512, L"%s%s%s  -  %.3f s, final cell 0x%llX (%lld)", widen(status_name(r.status)).c_str(),
             msg.empty() ? L"" : L": ", msg.c_str(), r.seconds, (long long)r.ptr, (long long)r.ptr);
    return buf;
}

void finish_run() {
    if (app.worker.joinable()) app.worker.join();
    app.have_last = true;
    set_mode(Mode::Idle);
    append_output(app.con.take());
    set_status(result_text(app.last));
    notify_device();
    refresh_memory();
}

// ---- debugging ----------------------------------------------------------------------------
void show_paused() {
    int32_t pc = app.dbg->pc();
    const Instr& x = app.prog.code[pc];
    int line, col;
    if (app.lang) {
        std::wstring text = editor_text();
        int stmt = x.op == END ? -1 : stmt_at(x.src);
        if (stmt >= 0) { select_stmt(stmt, text); line = app.lr.stmts[stmt].line - 1; col = app.lr.stmts[stmt].col - 1; }
        else { editor_select(0, 0, false); line = col = -1; }
    } else {
        editor_select(x.src, x.src + (x.op == END ? 0 : 1), true);
        line = (int)SendMessageW(app.editor, EM_EXLINEFROMCHAR, 0, x.src);
        col = x.src - (int)SendMessageW(app.editor, EM_LINEINDEX, line, 0);
    }
    int64_t p = app.dbg->ptr();
    uint8_t v = app.machine->bus_read(p);
    Debugger::Break br = app.dbg->last_break();
    wchar_t why[96] = L"Paused";
    switch (br.reason) {
    case Debugger::Reason::Breakpoint:   wcscpy(why, L"Breakpoint"); break;
    case Debugger::Reason::RunTo:        wcscpy(why, L"Reached target"); break;
    case Debugger::Reason::Watch:        swprintf(why, 96, L"Cell 0x%llX written (now %u)", (long long)br.addr, app.machine->bus_read(br.addr)); break;
    case Debugger::Reason::DeviceWindow: wcscpy(why, L"Pointer entered the device window"); break;
    case Debugger::Reason::Pause:        wcscpy(why, L"Paused by request"); break;
    default: break;
    }
    wchar_t buf[320];
    if (x.op == END) swprintf(buf, 320, L"%s - at end of program.  cell 0x%llX = %u", why, (long long)p, v);
    else if (line < 0) swprintf(buf, 320, L"%s - between statements (brainfuck offset %d, IR %d)   cell 0x%llX = %u (0x%02X)", why, x.src, pc, (long long)p, v, v);
    else swprintf(buf, 320, L"%s - line %d col %d (IR %d)   cell 0x%llX = %u (0x%02X)", why, line + 1, col + 1, pc, (long long)p, v, v);
    set_status(buf);
    mem_show_ptr();
}

void start_debug() {
    if (app.mode != Mode::Idle) return;
    if (app.worker.joinable()) app.worker.join();
    std::wstring text = editor_text();
    if (!compile_editor(false)) return;
    sync_bps_from_editor(text);
    app.pc_of_src.clear();
    app.src_pc.clear();
    for (size_t i = 0; i < app.prog.code.size(); i++) {
        app.pc_of_src.emplace(app.prog.code[i].src, (int32_t)i);
        app.src_pc.emplace_back(app.prog.code[i].src, (int32_t)i);
    }
    std::sort(app.src_pc.begin(), app.src_pc.end());
    if (app.lang) auto_watches();
    term::clear(app.console);
    app.dev.reset();
    app.fb.clear();
    app.machine->reset();
    app.have_last = false;
    app.dbg = std::make_unique<Debugger>(*app.machine, app.prog, [](Debugger::Event e) {
        PostMessageW(app.wnd, WM_APP_DEBUG, (WPARAM)e, 0);
    });
    for (int32_t s : app.bps) for (int32_t pc : pcs_for_bp(s, text)) app.dbg->set_breakpoint(pc, true);
    if (app.lang) {
        std::vector<uint8_t> marks(app.prog.code.size(), 0);
        for (const bfl::StmtRange& r : app.lr.stmts) { int32_t pc = first_pc_in(r.start, r.end); if (pc >= 0) marks[pc] = 1; }
        if (!marks.empty()) marks.back() = 1;          // END
        app.dbg->set_statement_marks(marks);
    }
    watches_to_session();
    set_mode(Mode::DebugPaused);
    notify_device();
    show_paused();
}

void debug_event(Debugger::Event e) {
    if (!app.dbg) return;
    append_output(app.con.take());
    if (e == Debugger::Event::Paused) {
        set_mode(Mode::DebugPaused);
        show_paused();
        return;
    }
    app.last = app.dbg->result();
    app.have_last = true;
    app.dbg.reset();                       // joins the session thread
    set_mode(Mode::Idle);
    editor_select(0, 0, false);
    set_status(result_text(app.last));
    notify_device();
    refresh_memory();
}

void debug_step()  {
    if (app.mode != Mode::DebugPaused) return;
    set_mode(Mode::DebugRunning);
    if (app.lang) app.dbg->step_statement(); else app.dbg->step();
}
void debug_cont()  { if (app.mode == Mode::DebugPaused) { set_mode(Mode::DebugRunning); set_status(L"Running (debugger)..."); app.dbg->cont(); } }
void debug_pause() { if (app.mode == Mode::DebugRunning) app.dbg->pause(); }

// Step over: a '[' runs its whole loop (to the instruction after the matching ']').
void debug_step_over() {
    if (app.mode != Mode::DebugPaused) return;
    const Instr& x = app.prog.code[app.dbg->pc()];
    if (app.lang) {
        int s = x.op == END ? -1 : stmt_at(x.src);
        set_mode(Mode::DebugRunning);
        if (s >= 0) app.dbg->step_statement(app.lr.stmts[s].start, app.lr.stmts[s].end); else app.dbg->step_statement();
        return;
    }
    if (x.op != JZ) { debug_step(); return; }
    set_mode(Mode::DebugRunning);
    set_status(L"Running to the end of the loop...");
    app.dbg->run_to(x.target);
}

void debug_run_to_cursor() {
    if (app.mode != Mode::DebugPaused) return;
    std::wstring text = editor_text();
    CHARRANGE cr; editor_get_sel(cr);
    int32_t pos = cr.cpMin;
    if (app.lang) {
        std::vector<int32_t> pcs = pcs_for_bp(pos, text);
        if (pcs.empty()) { set_status(L"No statement starts on this line."); return; }
        set_mode(Mode::DebugRunning);
        set_status(L"Running to the caret...");
        app.dbg->run_to(pcs.front());
        return;
    }
    while (pos < (int32_t)text.size() && !is_cmd(text[pos])) pos++;
    auto r = app.pc_of_src.equal_range(pos);
    if (r.first == r.second) { set_status(L"No command at or after the caret."); return; }
    set_mode(Mode::DebugRunning);
    set_status(L"Running to the caret...");
    app.dbg->run_to(r.first->second);
}

void stop_any() {
    if (app.mode == Mode::Running) app.machine->request_stop();
    else if (app.dbg) app.dbg->stop();
}

// ---- framebuffer window -------------------------------------------------------------------
constexpr int FB_SCALE = 5;

LRESULT CALLBACK fb_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: SetTimer(h, 1, 30, nullptr); return 0;
    case WM_TIMER: if (app.fb.dirty.exchange(false)) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        static uint32_t pix[FramebufferDevice::W * FramebufferDevice::H];
        for (int i = 0; i < FramebufferDevice::W * FramebufferDevice::H; i++) {
            COLORREF c = FramebufferDevice::colour(app.fb.pixels[i]);
            pix[i] = (uint32_t)(GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c));
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = FramebufferDevice::W;
        bi.bmiHeader.biHeight = -FramebufferDevice::H;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        RECT rc; GetClientRect(h, &rc);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, FramebufferDevice::W, FramebufferDevice::H, pix, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void fb_create(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = fb_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BfFramebuffer";
    RegisterClassW(&wc);
    RECT r{0, 0, px(FramebufferDevice::W * FB_SCALE), px(FramebufferDevice::H * FB_SCALE)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE);
    app.fbwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Framebuffer 128x64", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, app.wnd, nullptr, inst, nullptr);
}
void fb_show(bool show) {
    ShowWindow(app.fbwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    CheckMenuItem(app.menu, ID_VIEW_FB, show ? MF_CHECKED : MF_UNCHECKED);
}

// ---- generated brainfuck window (BFL mode) ------------------------------------------------------
LRESULT CALLBACK gen_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: if (app.genedit) MoveWindow(app.genedit, 0, 0, LOWORD(l), HIWORD(l), TRUE); return 0;
    case WM_SETFOCUS: SetFocus(app.genedit); return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); CheckMenuItem(app.menu, ID_VIEW_GEN, MF_UNCHECKED); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void gen_create(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = gen_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BfGenerated";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    app.genwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Generated brainfuck", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, px(700), px(600), app.wnd, nullptr, inst, nullptr);
    app.genedit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY,
                                  0, 0, 10, 10, app.genwnd, (HMENU)(INT_PTR)ID_GENEDIT, inst, nullptr);
    SendMessageW(app.genedit, WM_SETFONT, (WPARAM)app.mono, TRUE);
    SendMessageW(app.genedit, EM_SETLIMITTEXT, 0, 0);
}
void gen_show(bool show) {
    if (show && app.lang) compile_editor(true);
    ShowWindow(app.genwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    CheckMenuItem(app.menu, ID_VIEW_GEN, show ? MF_CHECKED : MF_UNCHECKED);
}

// ---- serial port selection ------------------------------------------------------------------
void serial_select(const std::string& port) {
    app.ser.configure(port);
    CheckMenuRadioItem(app.menu, ID_SER_NONE, ID_SER_COM_BASE + 255,
                       port.empty() ? ID_SER_NONE : port == "loopback" ? ID_SER_LOOP : ID_SER_COM_BASE + atoi(port.c_str() + 3), MF_BYCOMMAND);
    if (!port.empty() && !app.ser.is_open()) set_status(L"Could not open " + widen(port) + L".");
    else set_status(port.empty() ? L"Serial device detached." : L"Serial device attached to " + widen(port) + L".");
    notify_device();
}
std::wstring serial_text() {
    if (app.ser.port.empty()) return L"Serial: none";
    wchar_t buf[96];
    swprintf(buf, 96, L"Serial: %s %d%s", widen(app.ser.port).c_str(), SER_BAUDS[app.ser.baud_idx & 7],
             app.ser.error ? L" ERROR" : app.ser.is_open() ? L" open" : L" closed");
    return buf;
}

// ---- painting / layout -------------------------------------------------------------
void paint_panel(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    int d = px(32), gap = px(12), x = px(16), y = (rc.bottom - d) / 2;
    uint8_t leds = app.dev.leds.value;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 96));
    HBRUSH on = CreateSolidBrush(RGB(70, 230, 90)), off = CreateSolidBrush(RGB(40, 60, 42));
    SelectObject(dc, pen);
    for (int b = 7; b >= 0; b--) {
        SelectObject(dc, (leds >> b) & 1 ? on : off);
        Ellipse(dc, x, y, x + d, y + d);
        x += d + gap;
    }
    DeleteObject(on); DeleteObject(off); DeleteObject(pen);

    RECT box{x + px(16), y - 2, x + px(16) + px(150), y + d + 2};
    HBRUSH dark = CreateSolidBrush(RGB(12, 12, 14));
    FillRect(dc, &box, dark);
    DeleteObject(dark);
    wchar_t num[16];
    swprintf(num, 16, L"%5u", (unsigned)app.dev.number.value.load());
    SelectObject(dc, app.big);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 80, 60));
    DrawTextW(dc, num, -1, &box, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, app.ui_font);
    SetTextColor(dc, RGB(160, 160, 170));
    RECT l1{px(16), rc.bottom - px(18), x, rc.bottom - 2};
    DrawTextW(dc, L"LEDS 0xFF (<1)", -1, &l1, DT_LEFT | DT_SINGLELINE);
    RECT l2{box.left, rc.bottom - px(18), box.right, rc.bottom - 2};
    DrawTextW(dc, L"NUMBER 0xF6..F7 (<9,<10)", -1, &l2, DT_LEFT | DT_SINGLELINE);
    RECT right{box.right + px(24), rc.top, rc.right - 8, rc.bottom};
    const wchar_t* text = L"IDLE\r\nF5 run   Ctrl+F5 debug   F9 breakpoint   Ctrl+O open   Ctrl+S save   Ctrl+G go to address";
    switch (app.mode) {
    case Mode::Running:      text = L"RUNNING\r\nKeys typed in the terminal feed KEY_DATA and ','"; break;
    case Mode::DebugPaused:  text = L"PAUSED\r\nF11 step   F10 step over   Ctrl+F10 to cursor   F5 continue   F9 breakpoint   Shift+F5 stop"; break;
    case Mode::DebugRunning: text = L"DEBUGGING\r\nF6 pause   Shift+F5 stop"; break;
    default: break;
    }
    SetTextColor(dc, app.mode == Mode::DebugPaused ? RGB(255, 210, 90) : RGB(200, 200, 210));
    std::wstring all = std::wstring(text) + L"\r\n" + serial_text() + L"     Framebuffer: Ctrl+F";
    DrawTextW(dc, all.c_str(), -1, &right, DT_LEFT | DT_VCENTER);
}

void layout() {
    RECT rc; GetClientRect(app.wnd, &rc);
    SendMessageW(app.status, WM_SIZE, 0, 0);
    RECT sb; GetWindowRect(app.status, &sb);
    int sbh = sb.bottom - sb.top;
    int panel = px(PANEL_H);
    int memw = px(620);
    int left_w = rc.right - memw;
    int body_top = panel, body_h = rc.bottom - sbh - panel;
    int gap = px(6);
    if (app.console_h == 0) { int cw, ch; term::get_cell_size(app.console, cw, ch); app.console_h = 25 * ch + px(8); }

    // toolbar row
    int tbh = px(26), tx = px(6), ty = body_top + px(5);
    const int tbw[TB_COUNT] = {46, 46, 84, 52, 44, 44, 66, 50, 54, 46};
    for (int i = 0; i < TB_COUNT; i++) { MoveWindow(app.tb_buttons[i], tx, ty, px(tbw[i]), tbh, TRUE); tx += px(tbw[i]) + px(4); }
    tx += px(6);
    MoveWindow(app.tb_speed, tx, ty, px(140), tbh * 8, TRUE);
    body_top += tbh + px(10);
    body_h -= tbh + px(10);

    int min_edit = px(60);
    app.console_h = std::clamp(app.console_h, px(40), std::max(px(40), body_h - gap - min_edit));
    int edit_h = body_h - gap - app.console_h;
    MoveWindow(app.editor, 0, body_top, left_w, edit_h, TRUE);
    app.splitter = RECT{0, body_top + edit_h, left_w, body_top + edit_h + gap};
    MoveWindow(app.console, 0, app.splitter.bottom, left_w, app.console_h, TRUE);

    int x = left_w + px(6), y = body_top + px(6), bh = px(24);
    MoveWindow(app.mem_addr, x, y, px(80), bh, TRUE); x += px(84);
    const int bw[6] = {36, 36, 36, 44, 44, 44};
    for (int i = 0; i < 6; i++) { MoveWindow(app.mem_buttons[i], x, y, px(bw[i]), bh, TRUE); x += px(bw[i]) + px(4); }
    MoveWindow(app.mem_auto, x, y, px(110), bh, TRUE);
    int watch_h = px(150);
    int mem_h = body_h - bh - px(18) - watch_h - bh - px(12);
    MoveWindow(app.memview, left_w + px(6), y + bh + px(6), memw - px(12), mem_h, TRUE);
    int wy = y + bh + px(6) + mem_h + px(6);
    int wx = left_w + px(6);
    MoveWindow(app.watch_add, wx, wy, px(74), bh, TRUE);    wx += px(78);
    MoveWindow(app.watch_ptr, wx, wy, px(74), bh, TRUE);    wx += px(78);
    MoveWindow(app.watch_remove, wx, wy, px(64), bh, TRUE); wx += px(72);
    MoveWindow(app.watch_dev, wx, wy, px(230), bh, TRUE);
    MoveWindow(app.watchlist, left_w + px(6), wy + bh + px(6), memw - px(12), watch_h, TRUE);
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, app.wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

LRESULT CALLBACK addr_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && w == '\r') { PostMessageW(app.wnd, WM_COMMAND, ID_MEM_GO, 0); return 0; }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, addr_proc, 1);
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        app.wnd = h;
        app.dpi = GetDpiForWindow(h);
        app.menu = CreateMenu();
        HMENU file = CreatePopupMenu(), run = CreatePopupMenu(), debug = CreatePopupMenu(), help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, ID_OPEN, L"&Open...\tCtrl+O");
        AppendMenuW(file, MF_STRING, ID_RELOAD, L"&Reload from disk\tCtrl+R");
        AppendMenuW(file, MF_STRING, ID_SAVE, L"&Save\tCtrl+S");
        AppendMenuW(file, MF_STRING, ID_SAVEAS, L"Save &As...");
        AppendMenuW(file, MF_STRING, ID_EXPORT_BF, L"&Export generated brainfuck...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
        AppendMenuW(run, MF_STRING, ID_RUN, L"&Run editor contents\tF5");
        AppendMenuW(run, MF_STRING | MF_GRAYED, ID_STOP, L"&Stop\tShift+F5");
        AppendMenuW(run, MF_STRING, ID_CLEAR, L"&Clear terminal\tCtrl+L");
        AppendMenuW(run, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(run, MF_STRING, ID_LANG, L"Source is &BFL (compile to brainfuck first)");
        AppendMenuW(run, MF_STRING, ID_VIEW_GEN, L"Show &generated brainfuck\tCtrl+B");
        AppendMenuW(run, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(run, MF_STRING, ID_INTERP, L"Use &interpreter instead of JIT");
        HMENU baud = CreatePopupMenu();
        AppendMenuW(baud, MF_STRING | MF_CHECKED, ID_BAUD0, L"&Unlimited");
        AppendMenuW(baud, MF_STRING, ID_BAUD1, L"&115200 baud");
        AppendMenuW(baud, MF_STRING, ID_BAUD2, L"&38400 baud");
        AppendMenuW(baud, MF_STRING, ID_BAUD3, L"&9600 baud");
        AppendMenuW(baud, MF_STRING, ID_BAUD4, L"&2400 baud");
        AppendMenuW(run, MF_POPUP, (UINT_PTR)baud, L"&Terminal speed");
        AppendMenuW(debug, MF_STRING, ID_DEBUG, L"Start &debugging\tCtrl+F5");
        AppendMenuW(debug, MF_STRING | MF_GRAYED, ID_STEP, L"Step &into\tF11");
        AppendMenuW(debug, MF_STRING | MF_GRAYED, ID_STEP_OVER, L"Step &over loop\tF10");
        AppendMenuW(debug, MF_STRING | MF_GRAYED, ID_RUN_TO_CURSOR, L"Run to &cursor\tCtrl+F10");
        AppendMenuW(debug, MF_STRING | MF_GRAYED, ID_PAUSE, L"&Pause\tF6");
        AppendMenuW(debug, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(debug, MF_STRING, ID_TOGGLE_BP, L"Toggle &breakpoint\tF9");
        AppendMenuW(debug, MF_STRING, ID_CLEAR_BP, L"&Clear all breakpoints");
        AppendMenuW(debug, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(debug, MF_STRING, ID_WATCH_ADD, L"&Watch cell at viewer address");
        AppendMenuW(debug, MF_STRING, ID_WATCH_PTR, L"Watch cell at &pointer");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::START, L"&Contents\tF1");
        AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::SYMBOLS, L"&Brainfuck symbols");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::MEMORY_MAP, L"&Memory map");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::DEVICES, L"&Devices");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::DEBUGGER, L"De&bugger");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::TERMINAL, L"&Terminal");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::COMMAND_LINE, L"Command &line");
        AppendMenuW(help, MF_STRING, ID_HELP_BASE + (int)help::LANGUAGE, L"BFL &language");
        HMENU devices = CreatePopupMenu(), serial = CreatePopupMenu();
        AppendMenuW(serial, MF_STRING | MF_CHECKED, ID_SER_NONE, L"&None");
        AppendMenuW(serial, MF_STRING, ID_SER_LOOP, L"&Loopback (TX wired to RX)");
        app.com_ports = list_com_ports();
        if (!app.com_ports.empty()) AppendMenuW(serial, MF_SEPARATOR, 0, nullptr);
        for (const std::string& p : app.com_ports)
            AppendMenuW(serial, MF_STRING, ID_SER_COM_BASE + atoi(p.c_str() + 3), widen(p).c_str());
        AppendMenuW(devices, MF_POPUP, (UINT_PTR)serial, L"&Serial port");
        AppendMenuW(devices, MF_STRING, ID_VIEW_FB, L"&Framebuffer window\tCtrl+F");
        AppendMenuW(app.menu, MF_POPUP, (UINT_PTR)file, L"&File");
        AppendMenuW(app.menu, MF_POPUP, (UINT_PTR)run, L"&Run");
        AppendMenuW(app.menu, MF_POPUP, (UINT_PTR)debug, L"&Debug");
        AppendMenuW(app.menu, MF_POPUP, (UINT_PTR)devices, L"De&vices");
        AppendMenuW(app.menu, MF_POPUP, (UINT_PTR)help, L"&Help");
        SetMenu(h, app.menu);

        app.mono = CreateFontW(-MulDiv(11, (int)app.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        app.ui_font = CreateFontW(-MulDiv(9, (int)app.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app.big = CreateFontW(-MulDiv(18, (int)app.dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        app.memfont = CreateFontW(-MulDiv(9, (int)app.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        app.editor = make(MSFTEDIT_CLASS, L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL, ID_EDITOR, app.mono);
        SendMessageW(app.editor, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
        SendMessageW(app.editor, EM_SETEVENTMASK, 0, ENM_CHANGE);
        SendMessageW(app.editor, EM_SETTARGETDEVICE, 0, 1);         // no word wrap
        term::Config tc;
        tc.font = app.mono;
        tc.on_key = [](uint8_t k) { if (app.running) app.dev.keys.push(k); };
        app.console = term::create(h, ID_CONSOLE, tc);
        { int cw, ch; term::get_cell_size(app.console, cw, ch); app.console_h = 25 * ch + px(8); }

        app.mem_addr = make(L"EDIT", L"100", WS_BORDER | ES_AUTOHSCROLL | ES_UPPERCASE, ID_MEM_ADDR, app.mono);
        SetWindowSubclass(app.mem_addr, addr_proc, 1, 0);
        const wchar_t* names[6] = {L"Go", L"<<", L">>", L"Start", L"Dev", L"Ptr"};
        const int ids[6] = {ID_MEM_GO, ID_MEM_PREV, ID_MEM_NEXT, ID_MEM_START, ID_MEM_DEV, ID_MEM_PTR};
        for (int i = 0; i < 6; i++) app.mem_buttons[i] = make(L"BUTTON", names[i], BS_PUSHBUTTON, ids[i], app.ui_font);
        app.mem_auto = make(L"BUTTON", L"Live refresh", BS_AUTOCHECKBOX, ID_MEM_AUTO, app.ui_font);
        SendMessageW(app.mem_auto, BM_SETCHECK, BST_CHECKED, 0);
        app.memview = make(L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY, ID_MEMVIEW, app.memfont);

        const wchar_t* tb_names[TB_COUNT] = {L"Open", L"Save", L"▶ Run", L"Debug", L"Step", L"Over", L"To cursor", L"Pause", L"■ Stop", L"Clear"};
        const int tb_ids[TB_COUNT] = {ID_OPEN, ID_SAVE, ID_RUN, ID_DEBUG, ID_STEP, ID_STEP_OVER, ID_RUN_TO_CURSOR, ID_PAUSE, ID_STOP, ID_CLEAR};
        for (int i = 0; i < TB_COUNT; i++) app.tb_buttons[i] = make(L"BUTTON", tb_names[i], BS_PUSHBUTTON, tb_ids[i], app.ui_font);
        app.tb_speed = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_TB_SPEED, app.ui_font);
        for (const wchar_t* s : {L"Terminal: unlimited", L"115200 baud", L"38400 baud", L"9600 baud", L"2400 baud"})
            SendMessageW(app.tb_speed, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(app.tb_speed, CB_SETCURSEL, 0, 0);

        app.watchlist = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                        0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_WATCHLIST, nullptr, nullptr);
        SendMessageW(app.watchlist, WM_SETFONT, (WPARAM)app.memfont, TRUE);
        SendMessageW(app.watchlist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        {
            const wchar_t* cols[4] = {L"Break / cell", L"Hex", L"Dec", L"Chr"};
            const int widths[4] = {110, 50, 50, 40};
            for (int i = 0; i < 4; i++) {
                LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = px(widths[i]);
                SendMessageW(app.watchlist, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
            }
        }
        app.watch_add = make(L"BUTTON", L"+ Address", BS_PUSHBUTTON, ID_WATCH_ADD, app.ui_font);
        app.watch_ptr = make(L"BUTTON", L"+ Pointer", BS_PUSHBUTTON, ID_WATCH_PTR, app.ui_font);
        app.watch_remove = make(L"BUTTON", L"Remove", BS_PUSHBUTTON, ID_WATCH_REMOVE, app.ui_font);
        app.watch_dev = make(L"BUTTON", L"Break on entering device window", BS_AUTOCHECKBOX, ID_WATCH_DEV, app.ui_font);

        app.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, nullptr, nullptr, nullptr);
        set_status(L"Type a program, open one (Ctrl+O) or drop a .b / .bfl file onto the window. F5 runs, Ctrl+F5 debugs.");
        DragAcceptFiles(h, TRUE);
        update_title();
        set_mode(Mode::Idle);
        refresh_memory();
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc); rc.bottom = px(PANEL_H);
        paint_panel(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        rc.top = px(PANEL_H);
        FillRect((HDC)w, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_SETFOCUS: SetFocus(app.editor); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&app.splitter, pt)) { app.dragging = true; SetCapture(h); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (app.dragging) {
            RECT rc; GetClientRect(h, &rc);
            RECT sb; GetWindowRect(app.status, &sb);
            int body_bottom = rc.bottom - (sb.bottom - sb.top);
            app.console_h = body_bottom - GET_Y_LPARAM(l) - px(3);
            layout();
        }
        return 0;
    case WM_LBUTTONUP: if (app.dragging) { app.dragging = false; ReleaseCapture(); } return 0;
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        if ((HWND)w == h && PtInRect(&app.splitter, pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE; }
        break;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == app.memview) { SetBkColor((HDC)w, RGB(255, 255, 255)); return (LRESULT)GetStockObject(WHITE_BRUSH); }
        break;
    case WM_TIMER: if (w == TIMER_MEM) refresh_memory(); return 0;
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, file, MAX_PATH)) { if (app.mode == Mode::Idle && load_file(file)) start_run(); }
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_EDITOR && code == EN_CHANGE) {
            if (!app.suppress_change && !app.modified) { app.modified = true; update_title(); }
            return 0;
        }
        if (id == ID_MEM_AUTO && code == BN_CLICKED) {
            if (mem_auto() && app.mode != Mode::Idle) SetTimer(h, TIMER_MEM, 150, nullptr); else KillTimer(h, TIMER_MEM);
            return 0;
        }
        if (id == ID_VIEW_FB) { fb_show(!IsWindowVisible(app.fbwnd)); return 0; }
        if (id == ID_VIEW_GEN) { gen_show(!IsWindowVisible(app.genwnd)); return 0; }
        if (id == ID_LANG) { if (app.mode == Mode::Idle) set_lang(!app.lang); return 0; }
        if (id == ID_EXPORT_BF) { export_bf(); return 0; }
        if (id == ID_SER_NONE) { serial_select(""); return 0; }
        if (id == ID_SER_LOOP) { serial_select("loopback"); return 0; }
        if (id > ID_SER_COM_BASE && id <= ID_SER_COM_BASE + 255) { serial_select("COM" + std::to_string(id - ID_SER_COM_BASE)); return 0; }
        switch (id) {
        case ID_OPEN:   if (app.mode == Mode::Idle) open_dialog(); break;
        case ID_RELOAD: if (app.mode == Mode::Idle && !app.path.empty()) load_file(app.path); break;
        case ID_SAVE:   save_file(false); break;
        case ID_SAVEAS: save_file(true); break;
        case ID_EXIT:   SendMessageW(h, WM_CLOSE, 0, 0); break;
        case ID_RUN:    if (app.mode == Mode::DebugPaused) debug_cont(); else start_run(); break;
        case ID_DEBUG:  start_debug(); break;
        case ID_STEP:   debug_step(); break;
        case ID_STEP_OVER: debug_step_over(); break;
        case ID_RUN_TO_CURSOR: debug_run_to_cursor(); break;
        case ID_PAUSE:  debug_pause(); break;
        case ID_WATCH_ADD: {
            std::wstring t = window_text(app.mem_addr);
            wchar_t* end = nullptr;
            long long a = wcstoll(t.c_str(), &end, 16);
            if (end && end != t.c_str()) watch_add(a);
            break;
        }
        case ID_WATCH_PTR:    { int64_t p = current_ptr(); if (p >= 0) watch_add(p); break; }
        case ID_WATCH_REMOVE: watch_remove_selected(); break;
        case ID_WATCH_DEV:    if (code == BN_CLICKED && app.dbg) app.dbg->set_watch_device_window(watch_dev_checked()); break;
        case ID_STOP:   stop_any(); break;
        case ID_CLEAR:  term::clear(app.console); break;
        case ID_TOGGLE_BP: toggle_bp_at_caret(); break;
        case ID_CLEAR_BP:  clear_all_bps(); break;
        case ID_BAUD0: case ID_BAUD1: case ID_BAUD2: case ID_BAUD3: case ID_BAUD4:
            set_baud_index(id - ID_BAUD0);
            break;
        case ID_TB_SPEED:
            if (code == CBN_SELCHANGE) { set_baud_index((int)SendMessageW(app.tb_speed, CB_GETCURSEL, 0, 0)); SetFocus(app.editor); }
            break;
        case ID_INTERP:    set_interp(!app.use_interp); break;
        case ID_TB_INTERP: if (code == BN_CLICKED) set_interp(SendMessageW(app.tb_interp, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case ID_MEM_GO: {
            std::wstring t = window_text(app.mem_addr);
            wchar_t* end = nullptr;
            long long a = wcstoll(t.c_str(), &end, 16);
            if (end && end != t.c_str()) mem_goto(a);
            break;
        }
        case ID_MEM_PREV:  mem_goto(app.mem_base - MEM_ROWS * MEM_COLS); break;
        case ID_MEM_NEXT:  mem_goto(app.mem_base + MEM_ROWS * MEM_COLS); break;
        case ID_MEM_START: mem_goto(app.machine->mmio_size()); break;
        case ID_MEM_DEV:   mem_goto(0); break;
        case ID_MEM_PTR:   { int64_t p = current_ptr(); if (p >= 0) mem_goto((p - MEM_ROWS / 2 * MEM_COLS) & ~(int64_t)(MEM_COLS - 1)); break; }
        case ID_MEM_FOCUS: SetFocus(app.mem_addr); SendMessageW(app.mem_addr, EM_SETSEL, 0, -1); break;
        default:
            if (id >= ID_HELP_BASE && id < ID_HELP_BASE + (int)help::TOPIC_COUNT)
                help::show(h, GetModuleHandleW(nullptr), app.mono, app.ui_font, (help::Topic)(id - ID_HELP_BASE));
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)l;
        if (nm->idFrom == ID_WATCHLIST && nm->code == LVN_ITEMCHANGED) {
            auto* lv = (NMLISTVIEW*)l;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) && app.dbg)
                app.dbg->set_watch(watch_addr(lv->iItem), watch_checked(lv->iItem));
        }
        return 0;
    }
    case WM_APP_OUTPUT: append_output(app.con.take()); return 0;
    case WM_APP_DEVICE: {
        app.device_posted = false;
        RECT rc; GetClientRect(h, &rc); rc.bottom = px(PANEL_H);
        InvalidateRect(h, &rc, FALSE);
        return 0;
    }
    case WM_APP_DONE:  finish_run(); return 0;
    case WM_APP_FBSHOW: if (!IsWindowVisible(app.fbwnd)) fb_show(true); return 0;
    case WM_APP_DEBUG: debug_event((Debugger::Event)w); return 0;
    case WM_CLOSE:
        if (app.modified && MessageBoxW(h, L"The editor has unsaved changes. Discard them?", L"bf", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        stop_any();
        if (app.worker.joinable()) app.worker.join();
        app.dbg.reset();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    LoadLibraryW(L"Msftedit.dll");
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    Machine::Config cfg;
    Machine machine(cfg);
    app.machine = &machine;
    app.con.keys = &app.dev.keys;
    machine.set_console(&app.con);
    app.dev.install(machine, &app.con);
    app.dev.leds.on_change = [](uint8_t) { notify_device(); };
    app.dev.number.on_change = [](uint16_t) { notify_device(); };
    install_hw(machine, app.ser, app.fb);
    app.ser.on_change = [] { notify_device(); };
    app.fb.on_first_plot = [] { PostMessageW(app.wnd, WM_APP_FBSHOW, 0, 0); };

    ACCEL acc[] = {
        {FVIRTKEY | FCONTROL, 'F', ID_VIEW_FB}, {FVIRTKEY | FCONTROL, 'B', ID_VIEW_GEN}, {FVIRTKEY, VK_F1, ID_HELP_BASE + (int)help::START},
        {FVIRTKEY, VK_F5, ID_RUN}, {FVIRTKEY | FSHIFT, VK_F5, ID_STOP}, {FVIRTKEY | FCONTROL, VK_F5, ID_DEBUG},
        {FVIRTKEY, VK_F11, ID_STEP}, {FVIRTKEY, VK_F10, ID_STEP_OVER}, {FVIRTKEY | FCONTROL, VK_F10, ID_RUN_TO_CURSOR},
        {FVIRTKEY, VK_F6, ID_PAUSE}, {FVIRTKEY, VK_F9, ID_TOGGLE_BP},
        {FVIRTKEY | FCONTROL, 'O', ID_OPEN}, {FVIRTKEY | FCONTROL, 'S', ID_SAVE}, {FVIRTKEY | FCONTROL, 'R', ID_RELOAD},
        {FVIRTKEY | FCONTROL, 'L', ID_CLEAR}, {FVIRTKEY | FCONTROL, 'G', ID_MEM_FOCUS},
    };
    app.accel = CreateAcceleratorTableW(acc, (int)(sizeof acc / sizeof acc[0]));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BfMachineWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Brainfuck Machine", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1400, 860, nullptr, nullptr, inst, nullptr);
    app.con.hwnd = h;
    ShowWindow(h, SW_SHOW);
    fb_create(inst);
    gen_create(inst);
    set_lang(false);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline, &argc);
    if (argv && argc >= 1 && cmdline[0]) { if (load_file(argv[0])) start_run(); }
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(h, app.accel, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (app.worker.joinable()) app.worker.join();
    app.dbg.reset();
    return 0;
}
