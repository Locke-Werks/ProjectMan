#pragma once

#include <string>
#include <string_view>

namespace pm::cli {

// Owns the console's mode, codepage and cursor state, and puts it all back.
//
// Restoration is idempotent and happens on every exit path: the destructor, the
// console control handler, and explicitly before Claude Code is handed the
// terminal. Getting this wrong leaves a person with an invisible cursor and no
// echo in a shell they did not break.
class ConsoleSession {
public:
    ConsoleSession();
    ~ConsoleSession();

    ConsoleSession(const ConsoleSession&)            = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

    // False when there is no real console, because output is redirected or this
    // is running under CI. The caller falls back to plain list output.
    bool acquire();

    // False when the console cannot do virtual terminal sequences, which means
    // a conhost older than Windows 10 1703. No TUI there either.
    bool enterTui();

    // Safe to call twice. Pass false while handing the terminal to a child, so
    // the UTF-8 output codepage stays in force for its lifetime.
    void leaveTui(bool restoreCodepage = true);

    bool inTui() const { return inTui_; }
    bool hasConsole() const { return haveState_; }

    void write(std::wstring_view s) const;
    void write(std::string_view utf8) const;

    void size(int* cols, int* rows) const;

    void* inputHandle() const { return in_; }
    void* outputHandle() const { return out_; }

private:
    void* in_  = nullptr;
    void* out_ = nullptr;

    unsigned long savedIn_    = 0;
    unsigned long savedOut_   = 0;
    unsigned int  savedCpIn_  = 0;
    unsigned int  savedCpOut_ = 0;
    bool          savedCursorVisible_ = true;
    unsigned long savedCursorSize_    = 25;

    bool haveState_ = false;
    bool inTui_     = false;
};

} // namespace pm::cli
