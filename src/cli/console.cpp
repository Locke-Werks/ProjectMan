#include "console.h"

#include "launcher.h"
#include "strutil.h"

#include <windows.h>

namespace pm::cli {
namespace {

// The single live session, so the console control handler can restore state
// when the window is closed out from under us.
ConsoleSession* g_session = nullptr;

BOOL WINAPI ctrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        // The console delivers its own copy of this event to Claude Code
        // independently: it is attached to the same console and, because the
        // handoff passes no CREATE_NEW_PROCESS_GROUP, it is in the same process
        // group. Returning TRUE marks the event handled here so pm survives,
        // and takes nothing away from the child.
        //
        // Note this is a real handler, never SetConsoleCtrlHandler(nullptr,
        // TRUE). That form sets an "ignore Ctrl+C" process attribute which
        // CreateProcessW INHERITS, leaving the child structurally incapable of
        // ever receiving Ctrl+C.
        return childLive() ? TRUE : FALSE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_session)
            g_session->leaveTui();
        return FALSE;   // let the default action proceed; the console is going

    default:
        return FALSE;
    }
}

} // namespace

ConsoleSession::ConsoleSession() { g_session = this; }

ConsoleSession::~ConsoleSession()
{
    leaveTui();
    if (g_session == this)
        g_session = nullptr;
}

bool ConsoleSession::acquire()
{
    in_  = GetStdHandle(STD_INPUT_HANDLE);
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD inMode = 0, outMode = 0;
    if (!GetConsoleMode(static_cast<HANDLE>(in_), &inMode)
        || !GetConsoleMode(static_cast<HANDLE>(out_), &outMode)) {
        return false;   // redirected or no console at all
    }

    savedIn_    = inMode;
    savedOut_   = outMode;
    savedCpIn_  = GetConsoleCP();
    savedCpOut_ = GetConsoleOutputCP();

    CONSOLE_CURSOR_INFO ci{};
    if (GetConsoleCursorInfo(static_cast<HANDLE>(out_), &ci)) {
        savedCursorVisible_ = ci.bVisible != FALSE;
        savedCursorSize_    = ci.dwSize;
    }

    SetConsoleCtrlHandler(&ctrlHandler, TRUE);
    haveState_ = true;
    return true;
}

bool ConsoleSession::enterTui()
{
    if (!haveState_ || inTui_)
        return haveState_;

    SetConsoleOutputCP(CP_UTF8);

    DWORD outMode = savedOut_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                  | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(static_cast<HANDLE>(out_), outMode)) {
        SetConsoleOutputCP(savedCpOut_);
        return false;   // no VT support: conhost older than Windows 10 1703
    }

    DWORD inMode = savedIn_;
    // ENABLE_EXTENDED_FLAGS must be set for the QUICK_EDIT clear to take
    // effect. Without it the clear is silently ignored, and a stray click then
    // selects text and freezes all output until the selection is dismissed.
    inMode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT
                | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE);
    inMode |= (ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    // ENABLE_VIRTUAL_TERMINAL_INPUT stays off deliberately: it turns keystrokes
    // into ANSI byte sequences, which fights ReadConsoleInputW's KEY_EVENT
    // records. Virtual key codes are decoded directly instead.
    SetConsoleMode(static_cast<HANDLE>(in_), inMode);

    // The alternate screen buffer, rather than CreateConsoleScreenBuffer plus
    // SetConsoleActiveScreenBuffer. With a second buffer the child would
    // inherit STD_OUTPUT_HANDLE, which is the ORIGINAL buffer, while the active
    // one is ours. 1049 keeps a single buffer throughout and restores the
    // primary's contents and scrollback on the way out, so a finished Claude
    // Code session stays readable above the prompt.
    write(L"\x1b[?1049h");
    write(L"\x1b[?25l");

    inTui_ = true;
    return true;
}

void ConsoleSession::leaveTui(bool restoreCodepage)
{
    if (!haveState_)
        return;

    if (inTui_) {
        write(L"\x1b[0m");
        write(L"\x1b[?25h");
        write(L"\x1b[?1049l");
        inTui_ = false;
    }

    SetConsoleMode(static_cast<HANDLE>(in_), savedIn_);
    SetConsoleMode(static_cast<HANDLE>(out_), savedOut_);

    CONSOLE_CURSOR_INFO ci{};
    ci.dwSize   = savedCursorSize_ ? savedCursorSize_ : 25;
    ci.bVisible = savedCursorVisible_ ? TRUE : FALSE;
    SetConsoleCursorInfo(static_cast<HANDLE>(out_), &ci);

    if (restoreCodepage) {
        SetConsoleOutputCP(savedCpOut_);
        SetConsoleCP(savedCpIn_);
    }
}

void ConsoleSession::write(std::wstring_view s) const
{
    if (!out_ || s.empty())
        return;
    DWORD written = 0;
    WriteConsoleW(static_cast<HANDLE>(out_), s.data(), static_cast<DWORD>(s.size()),
                  &written, nullptr);
}

void ConsoleSession::write(std::string_view utf8) const { write(widen(utf8)); }

void ConsoleSession::size(int* cols, int* rows) const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (out_ && GetConsoleScreenBufferInfo(static_cast<HANDLE>(out_), &csbi)) {
        if (cols)
            *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (rows)
            *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
    if (cols)
        *cols = 80;
    if (rows)
        *rows = 25;
}

} // namespace pm::cli
