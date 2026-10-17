// terminal.cpp - VT100/ANSI terminal emulator (see terminal.h).
#include "terminal.h"
#include <windowsx.h>
#include <algorithm>
#include <deque>
#include <vector>

namespace term {

namespace {

struct Cell {
    wchar_t ch = L' ';
    uint8_t fg = 7, bg = 0;
    uint8_t attr = 0;          // 1 bold, 2 underline, 4 reverse
    bool same_style(const Cell& o) const { return fg == o.fg && bg == o.bg && attr == o.attr; }
};
using Line = std::vector<Cell>;

struct Term {
    HWND  hwnd = nullptr;
    HFONT font = nullptr;
    int   cw = 8, chh = 16;                   // cell size in pixels
    int   cols = 80, rows = 25;
    std::vector<Line> screen;
    std::deque<Line>  scrollback;
    size_t max_scrollback = 5000;
    int   cx = 0, cy = 0;
    bool  wrap_pending = false;
    int   top = 0, bot = 24;                  // scroll region
    Cell  cur;                                // current attributes (ch unused)
    int   saved_cx = 0, saved_cy = 0;
    Cell  saved_attr;
    bool  cursor_visible = true, host_cursor = true;
    bool  lnm = true, host_lnm = true;         // newline mode: LF also does CR
    enum State { GROUND, ESC, CSI, OSC, CHARSET } st = GROUND;
    std::string params;
    bool  priv = false;
    int   view = 0;                           // lines scrolled back into history
    std::function<void(uint8_t)> on_key;

    Line blank_line() const { Line l(cols); for (auto& c : l) { c.bg = cur.bg; } return l; }

    void reset_all() {
        screen.assign(rows, Line(cols));
        scrollback.clear();
        cx = cy = 0; wrap_pending = false;
        top = 0; bot = rows - 1;
        cur = Cell(); saved_attr = Cell(); saved_cx = saved_cy = 0;
        cursor_visible = true;
        lnm = host_lnm;
        st = GROUND; params.clear(); priv = false;
        view = 0;
    }

    void scroll_up(int n) {
        for (int i = 0; i < n; i++) {
            if (top == 0) {
                scrollback.push_back(std::move(screen[0]));
                if (scrollback.size() > max_scrollback) scrollback.pop_front();
            }
            screen.erase(screen.begin() + top);
            screen.insert(screen.begin() + bot, blank_line());
        }
    }
    void scroll_down(int n) {
        for (int i = 0; i < n; i++) {
            screen.erase(screen.begin() + bot);
            screen.insert(screen.begin() + top, blank_line());
        }
    }
    void linefeed() {
        if (cy == bot) scroll_up(1);
        else if (cy < rows - 1) cy++;
    }
    void put(wchar_t ch) {
        if (wrap_pending) { wrap_pending = false; cx = 0; linefeed(); }
        Cell& c = screen[cy][cx];
        c.ch = ch; c.fg = cur.fg; c.bg = cur.bg; c.attr = cur.attr;
        if (cx == cols - 1) wrap_pending = true; else cx++;
    }
    void erase_cells(int row, int from, int to) {       // [from, to)
        for (int x = std::max(0, from); x < std::min(cols, to); x++) { Cell& c = screen[row][x]; c = Cell(); c.bg = cur.bg; }
    }
    void move(int x, int y) {
        cx = std::clamp(x, 0, cols - 1);
        cy = std::clamp(y, 0, rows - 1);
        wrap_pending = false;
    }
    void respond(const char* s) { if (on_key) for (; *s; s++) on_key((uint8_t)*s); }

    int param(size_t i, int def) const {
        size_t k = 0, start = 0;
        for (size_t p = 0; p <= params.size(); p++) {
            if (p == params.size() || params[p] == ';') {
                if (k == i) {
                    if (p == start) return def;
                    return atoi(params.substr(start, p - start).c_str());
                }
                k++; start = p + 1;
            }
        }
        return def;
    }
    size_t nparams() const { if (params.empty()) return 0; return (size_t)std::count(params.begin(), params.end(), ';') + 1; }

    void sgr() {
        size_t n = nparams();
        if (n == 0) { cur = Cell(); return; }
        for (size_t i = 0; i < n; i++) {
            int p = param(i, 0);
            if (p == 0) cur = Cell();
            else if (p == 1) cur.attr |= 1;
            else if (p == 4) cur.attr |= 2;
            else if (p == 7) cur.attr |= 4;
            else if (p == 22) cur.attr &= ~1;
            else if (p == 24) cur.attr &= ~2;
            else if (p == 27) cur.attr &= ~4;
            else if (p >= 30 && p <= 37) cur.fg = (uint8_t)(p - 30);
            else if (p == 39) cur.fg = 7;
            else if (p >= 40 && p <= 47) cur.bg = (uint8_t)(p - 40);
            else if (p == 49) cur.bg = 0;
            else if (p >= 90 && p <= 97) cur.fg = (uint8_t)(p - 90 + 8);
            else if (p >= 100 && p <= 107) cur.bg = (uint8_t)(p - 100 + 8);
            else if ((p == 38 || p == 48) && i + 1 < n) {
                int mode = param(i + 1, 0);
                uint8_t idx = 7;
                if (mode == 5 && i + 2 < n) { idx = (uint8_t)param(i + 2, 0); i += 2; }
                else if (mode == 2 && i + 4 < n) {
                    int r = param(i + 2, 0), g = param(i + 3, 0), b = param(i + 4, 0);
                    idx = (uint8_t)(16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255));
                    i += 4;
                }
                if (p == 38) cur.fg = idx; else cur.bg = idx;
            }
        }
    }

    void csi(char final) {
        int n = param(0, 1); if (n < 1) n = 1;
        switch (final) {
        case 'A': move(cx, std::max(cy - n, cy >= top ? top : 0)); break;
        case 'B': move(cx, std::min(cy + n, cy <= bot ? bot : rows - 1)); break;
        case 'C': move(cx + n, cy); break;
        case 'D': move(cx - n, cy); break;
        case 'E': move(0, std::min(cy + n, rows - 1)); break;
        case 'F': move(0, std::max(cy - n, 0)); break;
        case 'G': case '`': move(n - 1, cy); break;
        case 'd': move(cx, n - 1); break;
        case 'H': case 'f': move(param(1, 1) - 1, param(0, 1) - 1); break;
        case 'J': {
            int m = param(0, 0);
            if (m == 0) { erase_cells(cy, cx, cols); for (int y = cy + 1; y < rows; y++) erase_cells(y, 0, cols); }
            else if (m == 1) { erase_cells(cy, 0, cx + 1); for (int y = 0; y < cy; y++) erase_cells(y, 0, cols); }
            else { for (int y = 0; y < rows; y++) erase_cells(y, 0, cols); if (m == 3) scrollback.clear(); }
            wrap_pending = false;
            break;
        }
        case 'K': {
            int m = param(0, 0);
            if (m == 0) erase_cells(cy, cx, cols); else if (m == 1) erase_cells(cy, 0, cx + 1); else erase_cells(cy, 0, cols);
            wrap_pending = false;
            break;
        }
        case 'X': erase_cells(cy, cx, cx + n); break;
        case '@': { Line& l = screen[cy]; l.insert(l.begin() + cx, (size_t)std::min(n, cols - cx), Cell()); l.resize(cols); break; }
        case 'P': { Line& l = screen[cy]; int k = std::min(n, cols - cx); l.erase(l.begin() + cx, l.begin() + cx + k); l.resize(cols); break; }
        case 'L': if (cy >= top && cy <= bot) { int save = top; top = cy; scroll_down(std::min(n, bot - cy + 1)); top = save; } break;
        case 'M': if (cy >= top && cy <= bot) { int save = top; top = cy; scroll_up(std::min(n, bot - cy + 1)); top = save; } break;
        case 'S': scroll_up(n); break;
        case 'T': scroll_down(n); break;
        case 'm': sgr(); break;
        case 's': saved_cx = cx; saved_cy = cy; break;
        case 'u': move(saved_cx, saved_cy); break;
        case 'r': {
            int t = param(0, 1) - 1, b = param(1, rows) - 1;
            if (t < 0) t = 0; if (b > rows - 1) b = rows - 1;
            if (t < b) { top = t; bot = b; move(0, 0); }
            break;
        }
        case 'h': case 'l':
            if (priv && param(0, 0) == 25) cursor_visible = (final == 'h');
            if (!priv && param(0, 0) == 20) lnm = (final == 'h');        // LNM
            break;
        case 'n':
            if (param(0, 0) == 6) { char buf[32]; snprintf(buf, sizeof buf, "\x1b[%d;%dR", cy + 1, cx + 1); respond(buf); }
            break;
        case 'c': respond("\x1b[?1;2c"); break;
        default: break;
        }
    }

    void esc(char c) {
        st = GROUND;
        switch (c) {
        case '[': st = CSI; params.clear(); priv = false; break;
        case ']': st = OSC; break;
        case '(': case ')': case '*': case '+': st = CHARSET; break;
        case '7': saved_cx = cx; saved_cy = cy; saved_attr = cur; break;
        case '8': move(saved_cx, saved_cy); cur = saved_attr; break;
        case 'D': linefeed(); break;
        case 'M': if (cy == top) scroll_down(1); else if (cy > 0) cy--; break;
        case 'E': cx = 0; linefeed(); break;
        case 'c': { auto k = on_key; reset_all(); on_key = k; break; }
        default: break;
        }
    }

    void feed_byte(uint8_t b) {
        switch (st) {
        case GROUND:
            switch (b) {
            case 0x1B: st = ESC; break;
            case 0x07: break;
            case 0x08: if (cx > 0) cx--; wrap_pending = false; break;
            case 0x09: cx = std::min((cx / 8 + 1) * 8, cols - 1); wrap_pending = false; break;
            case 0x0A: case 0x0B: case 0x0C:
                linefeed();
                if (lnm) { cx = 0; wrap_pending = false; }
                break;
            case 0x0D: cx = 0; wrap_pending = false; break;
            default: if (b >= 0x20) put((wchar_t)b); break;
            }
            break;
        case ESC: esc((char)b); break;
        case CSI:
            if (b == 0x1B) { st = ESC; }
            else if (b >= 0x30 && b <= 0x3F) { if (b == '?' || b == '>' || b == '<' || b == '=') priv = true; else params.push_back((char)b); }
            else if (b >= 0x20 && b <= 0x2F) { /* intermediates ignored */ }
            else if (b >= 0x40 && b <= 0x7E) { st = GROUND; csi((char)b); }
            else if (b < 0x20) { st = GROUND; feed_byte(b); }   // control inside CSI: execute it
            else st = GROUND;
            break;
        case OSC:
            if (b == 0x07) st = GROUND;
            else if (b == 0x1B) st = ESC;      // ESC \ terminator; the '\' is then ignored by esc()
            break;
        case CHARSET: st = GROUND; break;
        }
    }

    // ---- resizing / scrolling ------------------------------------------------------
    void resize_to(int w, int h) {
        int nc = std::max(2, w / cw), nr = std::max(2, h / chh);
        if (nc == cols && nr == rows) return;
        while ((int)screen.size() > nr) {                 // push surplus top rows into history
            if (cy > 0) { scrollback.push_back(std::move(screen[0])); screen.erase(screen.begin()); cy--; }
            else screen.pop_back();
        }
        while ((int)screen.size() < nr) screen.push_back(Line(nc));
        for (auto& l : screen) l.resize(nc);
        cols = nc; rows = nr;
        top = 0; bot = rows - 1;
        cx = std::min(cx, cols - 1); cy = std::min(cy, rows - 1);
        wrap_pending = false;
        update_scrollbar();
    }
    void update_scrollbar() {
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};
        si.nMin = 0; si.nMax = (int)scrollback.size() + rows - 1; si.nPage = rows;
        si.nPos = (int)scrollback.size() - view;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    }
    void set_view(int v) {
        view = std::clamp(v, 0, (int)scrollback.size());
        update_scrollbar();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // ---- painting -------------------------------------------------------------------
    static COLORREF colour(uint8_t i) {
        static const COLORREF base[16] = {
            RGB(0, 0, 0), RGB(205, 49, 49), RGB(13, 188, 121), RGB(229, 229, 16), RGB(36, 114, 200), RGB(188, 63, 188), RGB(17, 168, 205), RGB(204, 204, 204),
            RGB(102, 102, 102), RGB(241, 76, 76), RGB(35, 209, 139), RGB(245, 245, 67), RGB(59, 142, 234), RGB(214, 112, 214), RGB(41, 184, 219), RGB(255, 255, 255)};
        if (i < 16) return base[i];
        if (i < 232) { int v = i - 16; int r = v / 36, g = (v / 6) % 6, b = v % 6; auto s = [](int x) { return x ? 55 + x * 40 : 0; }; return RGB(s(r), s(g), s(b)); }
        int g = 8 + (i - 232) * 10; return RGB(g, g, g);
    }
    const Line& visible_line(int r) const {
        int first = (int)scrollback.size() - view;       // index in (scrollback + screen) space
        int idx = first + r;
        if (idx < (int)scrollback.size()) return scrollback[idx];
        return screen[idx - (int)scrollback.size()];
    }
    void paint(HDC dc, const RECT& rc) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldb = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(colour(0));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SelectObject(mem, font);
        SetBkMode(mem, OPAQUE);
        std::wstring run;
        for (int r = 0; r < rows; r++) {
            const Line& l = visible_line(r);
            int x = 0;
            while (x < (int)l.size() && x < cols) {
                int s = x;
                run.clear();
                while (x < (int)l.size() && x < cols && l[x].same_style(l[s])) run.push_back(l[x++].ch);
                const Cell& c = l[s];
                uint8_t fg = c.fg, bgc = c.bg;
                if ((c.attr & 1) && fg < 8) fg += 8;
                if (c.attr & 4) std::swap(fg, bgc);
                SetTextColor(mem, colour(fg));
                SetBkColor(mem, colour(bgc));
                RECT cell{s * cw, r * chh, x * cw, (r + 1) * chh};
                ExtTextOutW(mem, s * cw, r * chh, ETO_OPAQUE | ETO_CLIPPED, &cell, run.c_str(), (UINT)run.size(), nullptr);
                if (c.attr & 2) { RECT ul{cell.left, cell.bottom - 2, cell.right, cell.bottom - 1}; HBRUSH b = CreateSolidBrush(colour(fg)); FillRect(mem, &ul, b); DeleteObject(b); }
            }
        }
        if (cursor_visible && host_cursor && view == 0) {
            RECT cr{cx * cw, cy * chh, (cx + 1) * cw, (cy + 1) * chh};
            if (GetFocus() == hwnd) InvertRect(mem, &cr);
            else FrameRect(mem, &cr, (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldb);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void key_down(WPARAM vk) {
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (shift && vk == VK_PRIOR) { set_view(view + rows); return; }
        if (shift && vk == VK_NEXT)  { set_view(view - rows); return; }
        const char* s = nullptr;
        switch (vk) {
        case VK_UP: s = "\x1b[A"; break;    case VK_DOWN: s = "\x1b[B"; break;
        case VK_RIGHT: s = "\x1b[C"; break; case VK_LEFT: s = "\x1b[D"; break;
        case VK_HOME: s = "\x1b[H"; break;  case VK_END: s = "\x1b[F"; break;
        case VK_INSERT: s = "\x1b[2~"; break; case VK_DELETE: s = "\x1b[3~"; break;
        case VK_PRIOR: s = "\x1b[5~"; break; case VK_NEXT: s = "\x1b[6~"; break;
        case VK_F1: s = "\x1bOP"; break; case VK_F2: s = "\x1bOQ"; break; case VK_F3: s = "\x1bOR"; break; case VK_F4: s = "\x1bOS"; break;
        default: return;
        }
        respond(s);
    }
};

Term* get(HWND h) { return (Term*)GetWindowLongPtrW(h, GWLP_USERDATA); }

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Term* t = get(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = (CREATESTRUCTW*)l;
        t = new Term();
        t->hwnd = h;
        auto* cfg = (const Config*)cs->lpCreateParams;
        t->font = cfg->font;
        t->on_key = cfg->on_key;
        t->host_lnm = t->lnm = cfg->newline_mode;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)t);
        HDC dc = GetDC(h);
        SelectObject(dc, t->font);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        t->cw = tm.tmAveCharWidth; t->chh = tm.tmHeight;
        ReleaseDC(h, dc);
        t->reset_all();
        return TRUE;
    }
    case WM_NCDESTROY: delete t; SetWindowLongPtrW(h, GWLP_USERDATA, 0); return 0;
    case WM_SIZE: t->resize_to(LOWORD(l), HIWORD(l)); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc); t->paint(dc, rc); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: SetFocus(h); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (c == '\r') c = '\n';
        if (c < 256 && t->on_key) t->on_key((uint8_t)c);
        if (t->view) t->set_view(0);
        return 0;
    }
    case WM_KEYDOWN: t->key_down(w); return 0;
    case WM_MOUSEWHEEL: t->set_view(t->view + 3 * GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA); return 0;
    case WM_VSCROLL: {
        int pos = (int)t->scrollback.size() - t->view;
        switch (LOWORD(w)) {
        case SB_LINEUP: pos--; break;         case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= t->rows; break; case SB_PAGEDOWN: pos += t->rows; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: { SCROLLINFO si{sizeof si, SIF_TRACKPOS}; GetScrollInfo(h, SB_VERT, &si); pos = si.nTrackPos; break; }
        case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = (int)t->scrollback.size(); break;
        }
        t->set_view((int)t->scrollback.size() - pos);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

HWND create(HWND parent, int id, const Config& cfg) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
        wc.lpszClassName = L"BfTerminal";
        wc.style = CS_DBLCLKS;
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"BfTerminal", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), (LPVOID)&cfg);
}

void feed(HWND h, const char* bytes, size_t n) {
    Term* t = get(h);
    if (!t) return;
    for (size_t i = 0; i < n; i++) t->feed_byte((uint8_t)bytes[i]);
    if (t->view) t->view = 0;
    t->update_scrollbar();
    InvalidateRect(h, nullptr, FALSE);
}

void clear(HWND h) { if (Term* t = get(h)) { auto k = t->on_key; t->reset_all(); t->on_key = k; t->update_scrollbar(); InvalidateRect(h, nullptr, FALSE); } }
void set_cursor_visible(HWND h, bool v) { if (Term* t = get(h)) { t->host_cursor = v; InvalidateRect(h, nullptr, FALSE); } }
void set_newline_mode(HWND h, bool on) { if (Term* t = get(h)) { t->host_lnm = t->lnm = on; } }
bool newline_mode(HWND h) { Term* t = get(h); return t ? t->host_lnm : true; }
void get_size(HWND h, int& cols, int& rows) { Term* t = get(h); cols = t ? t->cols : 0; rows = t ? t->rows : 0; }
void get_cell_size(HWND h, int& cw, int& ch) { Term* t = get(h); cw = t ? t->cw : 8; ch = t ? t->chh : 16; }

} // namespace term
