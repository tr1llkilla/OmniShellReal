Copyright © 2025 Cadell Richard Anderson

//EditorApp_X11.cpp

#include "EditorApp.h"
#include <optional>
#include <vector>
#include <string>

#if !defined(__linux__)

// Non-Linux stub: lets you keep this file in the project without breaking Windows builds
std::optional<std::vector<std::string>>
LaunchEditorWindow_X11(const EditorLaunchOptions& opts)
{
    // Not supported on this platform — return no result
    return std::nullopt;
}

#else // __linux__ real implementation

#include <X11/Xlib.h>
#include <X11/Xutil.h>

std::optional<std::vector<std::string>>
LaunchEditorWindow_X11(const EditorLaunchOptions& opts)
{
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return std::nullopt;

    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(
        dpy,
        RootWindow(dpy, scr),
        50, 50,
        1000, 800,
        0,
        BlackPixel(dpy, scr),
        WhitePixel(dpy, scr));

    XStoreName(dpy, win, opts.title.c_str());

    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);

    XSelectInput(dpy, win,
        ExposureMask |
        KeyPressMask | KeyReleaseMask |
        ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask |
        StructureNotifyMask);

    XMapWindow(dpy, win);

    // TODO: Setup font (Xft) and input contexts (XIM/XIC) for robust typing

    std::vector<std::string> lines = opts.lines;
    int caretLine = opts.initialLine;
    int caretCol = opts.initialCol;
    bool insertMode = true;
    bool dirty = false;
    bool running = true;

    auto repaint = [&]() {
        // TODO: draw gutter + visible lines, caret
        // Use opts.showLineNumbers, opts.wordWrap, opts.monospace
        };

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            repaint();
            break;

        case ConfigureNotify:
            repaint();
            break;

        case KeyPress: {
            // TODO: translate keys (arrows, pgup/dn, home/end, backspace/delete, printable)
            // update caret/lines, set dirty, repaint()
            break;
        }

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wmDelete) {
                // TODO: prompt to save; for now, accept and return edited or discard
                running = false;
            }
            break;
        }
    }

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return lines; // or std::nullopt if discard
}

#endif // __linux__
