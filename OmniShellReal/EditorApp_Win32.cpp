//EditorApp_Win32.cpp

#if !defined(_WIN32)
#error "EditorApp_Win32.cpp must only be compiled on Windows."
#endif

#define NOMINMAX // keep std::min/std::max intact
#include "EditorApp.h"
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#undef min
#undef max

// Fallbacks if windowsx.h isn't present (kept harmless if already defined)
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

namespace {
    struct State {
        EditorLaunchOptions opts;
        HWND hwnd = nullptr;
        HFONT hFont = nullptr;
        int lineHeight = 16, charWidth = 8;
        int gutterPx = 0;
        int scrollY = 0; // first visible line
        int scrollX = 0; // horizontal columns
        int caretLine = 0;
        int caretCol = 0;
        bool insertMode = true;
        bool dirty = false;
        bool running = true;
        std::optional<std::vector<std::string>> result;
    };

    static State* g = nullptr;

    static void RecalcMetrics(HDC hdc) {
        TEXTMETRIC tm{};
        GetTextMetrics(hdc, &tm);
        g->lineHeight = tm.tmHeight + tm.tmExternalLeading;
        g->charWidth = tm.tmAveCharWidth;

        if (!g->opts.showLineNumbers) {
            g->gutterPx = 0;
            return;
        }

        int maxLineNum = std::max(1, (int)g->opts.lines.size());
        int digits = 1; while (maxLineNum >= 10) { maxLineNum /= 10; ++digits; }
        g->gutterPx = (digits + 2) * g->charWidth;
    }

    static void EnsureCaretVisible(RECT rc) {
        int viewCols = (rc.right - rc.left - g->gutterPx) / g->charWidth;
        if (viewCols < 8) viewCols = 8;
        if (g->caretCol < g->scrollX) g->scrollX = g->caretCol;
        else if (g->caretCol >= g->scrollX + viewCols) g->scrollX = g->caretCol - viewCols + 1;

        int viewRows = (rc.bottom - rc.top) / g->lineHeight;
        if (viewRows < 3) viewRows = 3;
        if (g->caretLine < g->scrollY) g->scrollY = g->caretLine;
        else if (g->caretLine >= g->scrollY + viewRows) g->scrollY = g->caretLine - viewRows + 1;
    }

    static void ClampCaret() {
        if (g->opts.lines.empty()) { g->caretLine = 0; g->caretCol = 0; return; }
        if (g->caretLine >= (int)g->opts.lines.size()) g->caretLine = (int)g->opts.lines.size() - 1;
        int len = (int)g->opts.lines[g->caretLine].size();
        if (g->caretCol > len) g->caretCol = len;
        if (g->caretCol < 0) g->caretCol = 0;
    }

    static void MoveCaret(HWND hwnd) {
        RECT rc; GetClientRect(hwnd, &rc);
        EnsureCaretVisible(rc);
        int x = g->gutterPx + (g->caretCol - g->scrollX) * g->charWidth;
        int y = (g->caretLine - g->scrollY) * g->lineHeight;
        SetCaretPos(x, y);
    }

    static void Paint(HWND hwnd) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        HGDIOBJ old = SelectObject(hdc, g->hFont);
        SetBkMode(hdc, TRANSPARENT);

        RECT rc; GetClientRect(hwnd, &rc);
        RecalcMetrics(hdc);

        // Fill background
        HBRUSH bg = (HBRUSH)(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);

        // Draw gutter (optional)
        if (g->opts.showLineNumbers && g->gutterPx > 0) {
            RECT gutter{ rc.left, rc.top, rc.left + g->gutterPx, rc.bottom };
            FillRect(hdc, &gutter, (HBRUSH)(COLOR_BTNFACE + 1));
        }

        int y = rc.top;
        int first = g->scrollY;
        int viewRows = (rc.bottom - rc.top) / g->lineHeight;
        int last = std::min((int)g->opts.lines.size(), first + viewRows);

        for (int i = first; i < last; ++i) {
            if (g->opts.showLineNumbers) {
                char num[32]; wsprintfA(num, "%d", i + 1);
                TextOutA(hdc, rc.left + 2, y, num, (int)strlen(num));
            }

            const std::string& s = g->opts.lines[i];
            int colStart = std::min((int)s.size(), g->scrollX);
            const char* text = s.c_str() + colStart;
            int maxCols = (rc.right - rc.left - g->gutterPx) / g->charWidth;
            int count = (int)strlen(text);
            if (count > maxCols) count = maxCols;
            TextOutA(hdc, rc.left + g->gutterPx, y, text, count);

            y += g->lineHeight;
        }

        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
    }

    static void InsertChar(char c) {
        if (g->opts.lines.empty()) g->opts.lines.emplace_back();
        std::string& s = g->opts.lines[g->caretLine];
        if (g->insertMode || g->caretCol >= (int)s.size()) s.insert(s.begin() + g->caretCol, c);
        else s[g->caretCol] = c;
        g->caretCol++;
        g->dirty = true;
    }

    static void Backspace() {
        if (g->opts.lines.empty()) return;
        if (g->caretCol > 0) {
            std::string& s = g->opts.lines[g->caretLine];
            s.erase(s.begin() + g->caretCol - 1);
            g->caretCol--;
            g->dirty = true;
        }
    }

    static void DeleteAt() {
        if (g->opts.lines.empty()) return;
        std::string& s = g->opts.lines[g->caretLine];
        if (g->caretCol < (int)s.size()) {
            s.erase(s.begin() + g->caretCol);
            g->dirty = true;
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE:
            CreateCaret(hwnd, nullptr, 2, g->lineHeight);
            ShowCaret(hwnd);
            return 0;
        case WM_SIZE:
            MoveCaret(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_SETFOCUS:
            ShowCaret(hwnd); MoveCaret(hwnd); return 0;
        case WM_KILLFOCUS:
            HideCaret(hwnd); return 0;
        case WM_PAINT:
            Paint(hwnd); return 0;
        case WM_CHAR:
            if (wParam >= 32 && wParam < 127) { InsertChar((char)wParam); InvalidateRect(hwnd, nullptr, FALSE); MoveCaret(hwnd); }
            else if (wParam == 8) { Backspace(); InvalidateRect(hwnd, nullptr, FALSE); MoveCaret(hwnd); } // backspace
            return 0;
        case WM_KEYDOWN:
            switch (wParam) {
            case VK_LEFT:  if (g->caretCol > 0) g->caretCol--; ClampCaret(); MoveCaret(hwnd); return 0;
            case VK_RIGHT: if (!g->opts.lines.empty() && g->caretCol < (int)g->opts.lines[g->caretLine].size()) g->caretCol++; MoveCaret(hwnd); return 0;
            case VK_UP:    if (g->caretLine > 0) g->caretLine--; ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
            case VK_DOWN:  if (g->caretLine + 1 < (int)g->opts.lines.size()) g->caretLine++; ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
            case VK_HOME:  g->caretLine = 0; ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
            case VK_END:   if (!g->opts.lines.empty()) g->caretLine = (int)g->opts.lines.size() - 1; ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
            case VK_PRIOR: g->caretLine = std::max(0, g->caretLine - 20); ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0; // PageUp
            case VK_NEXT:  g->caretLine = std::min((int)g->opts.lines.size() - 1, g->caretLine + 20); ClampCaret(); MoveCaret(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0; // PageDown
            case VK_DELETE: DeleteAt(); InvalidateRect(hwnd, nullptr, FALSE); MoveCaret(hwnd); return 0;
            case VK_INSERT: g->insertMode = !g->insertMode; return 0;
            case 'S': if (GetKeyState(VK_CONTROL) & 0x8000) { g->dirty = false; g->result = g->opts.lines; DestroyWindow(hwnd); } return 0;
            case 'Q': if (GetKeyState(VK_CONTROL) & 0x8000) { g->result.reset(); DestroyWindow(hwnd); } return 0;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            short dz = GET_WHEEL_DELTA_WPARAM(wParam);
            g->scrollY = std::max(0, g->scrollY - dz / WHEEL_DELTA * 3);
            InvalidateRect(hwnd, nullptr, FALSE);
            MoveCaret(hwnd);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT rc; GetClientRect(hwnd, &rc);
            int row = g->scrollY + y / g->lineHeight;
            row = std::max(0, std::min(row, (int)g->opts.lines.size() - 1));
            int col = (x - g->gutterPx) / g->charWidth + g->scrollX;
            col = std::max(0, (int)std::min<size_t>(col, g->opts.lines[row].size()));
            g->caretLine = row;
            g->caretCol = col;
            SetFocus(hwnd);
            MoveCaret(hwnd);
            return 0;
        }
        case WM_CLOSE:
            if (g->dirty) {
                int r = MessageBoxA(hwnd, "Save changes?", g->opts.title.c_str(),
                    MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) return 0;
                if (r == IDYES) g->result = g->opts.lines;
                else g->result.reset();
            }
            else {
                g->result = g->opts.lines; // return last state (no changes still OK)
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

} // namespace

std::optional<std::vector<std::string>>
LaunchEditorWindow_Win32(const EditorLaunchOptions& opts)
{
    State state; g = &state; state.opts = opts;

    // Seed caret from opts
    state.caretLine = std::max(0, opts.initialLine);
    state.caretCol = std::max(0, opts.initialCol);

    HINSTANCE hInst = GetModuleHandle(nullptr);
    const char* cls = "OmniEditorWin";
    WNDCLASSA wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = cls; wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, cls, state.opts.title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 800, nullptr, nullptr, hInst, nullptr);
    state.hwnd = hwnd;

    // Choose font by preference
    LOGFONTA lf{}; lf.lfHeight = -16;
    if (state.opts.monospace) {
        strcpy_s(lf.lfFaceName, "Consolas");
    }
    else {
        strcpy_s(lf.lfFaceName, "Segoe UI");
    }
    state.hFont = CreateFontIndirectA(&lf);

    // Prime metrics and initial scroll/caret so first paint is correct
    if (state.hFont && hwnd) {
        HDC hdc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(hdc, state.hFont);
        RecalcMetrics(hdc);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);

        RECT rc; GetClientRect(hwnd, &rc);
        ClampCaret();
        EnsureCaretVisible(rc);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!state.running) break;
    }

    if (state.hFont) DeleteObject(state.hFont);
    return state.result;
}