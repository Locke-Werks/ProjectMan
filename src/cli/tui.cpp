#include "tui.h"

#include "launcher.h"
#include "strutil.h"
#include "theme.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace pm::cli {
namespace {

using namespace pm::theme;

// ------------------------------------------------------------------ styling

std::string fg(Rgb c)
{
    return "\x1b[38;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";"
         + std::to_string(c.b) + "m";
}

std::string bg(Rgb c)
{
    return "\x1b[48;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";"
         + std::to_string(c.b) + "m";
}

const char* kReset = "\x1b[0m";

// End a line by erasing the rest of it first.
//
// The frame is redrawn in place from the home position rather than by clearing
// the screen, which avoids flicker, but it means a short line leaves whatever
// the previous frame had to the right of it. Switching views is where that
// shows: the project view's wider columns survive behind the dispatch view's
// narrower ones.
const char* kEol = "\x1b[0m\x1b[K\r\n";

// The design language's tracked all-caps. A terminal cell grid cannot do
// fractional letter spacing, so tracking is one space between characters,
// which is the closest honest equivalent.
std::string tracked(std::string_view s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        if (i)
            out.push_back(' ');
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// Truncate to a column budget and pad to it. Everything here is ASCII in
// practice; a multi-byte name is clipped conservatively by bytes rather than
// risking a split code point on screen.
std::string cell(std::string_view s, size_t width)
{
    std::string out(s.substr(0, width));
    if (out.size() < width)
        out.append(width - out.size(), ' ');
    return out;
}

std::string relTime(std::int64_t unix)
{
    if (unix <= 0)
        return "-";
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::int64_t d = now - unix;
    if (d < 0)
        d = 0;
    if (d < 3600)        return std::to_string(d / 60) + "m";
    if (d < 86400)       return std::to_string(d / 3600) + "h";
    if (d < 86400 * 30)  return std::to_string(d / 86400) + "d";
    if (d < 86400 * 365) return std::to_string(d / (86400 * 30)) + "mo";
    return std::to_string(d / (86400 * 365)) + "y";
}

// ------------------------------------------------------------------- state

enum class View { Projects, Dispatch, Settings };

struct Ui {
    ProjectList* projects = nullptr;
    Config*      cfg      = nullptr;

    View        view = View::Projects;
    std::string filter;

    std::vector<size_t> visible;    // indices into *projects
    size_t              cursor = 0;
    size_t              top    = 0;

    WorkList            items;
    std::vector<size_t> itemVisible;
    size_t              itemCursor = 0;
    size_t              itemTop    = 0;

    size_t      settingCursor = 0;
    size_t      settingTop    = 0;
    bool        editing       = false;
    std::string editBuffer;
    bool        settingsDirty = false;

    int cols = 100;
    int rows = 30;

    std::string flash;   // a transient message on the footer line
};

void refilter(Ui& u)
{
    u.visible.clear();
    for (size_t i = 0; i < u.projects->size(); ++i) {
        const Project& p = (*u.projects)[i];
        if (u.filter.empty() || icontains(p.displayName(), u.filter)
            || icontains(p.path.string(), u.filter)) {
            u.visible.push_back(i);
        }
    }
    if (u.cursor >= u.visible.size())
        u.cursor = u.visible.empty() ? 0 : u.visible.size() - 1;
}

void refilterItems(Ui& u)
{
    u.itemVisible.clear();
    for (size_t i = 0; i < u.items.size(); ++i) {
        const WorkItem& w = u.items[i];
        if (u.filter.empty() || icontains(w.project, u.filter)
            || icontains(w.summary, u.filter)) {
            u.itemVisible.push_back(i);
        }
    }
    if (u.itemCursor >= u.itemVisible.size())
        u.itemCursor = u.itemVisible.empty() ? 0 : u.itemVisible.size() - 1;
}

void clampScroll(size_t cursor, size_t& top, size_t count, int windowRows)
{
    const size_t win = static_cast<size_t>(std::max(1, windowRows));
    if (cursor < top)
        top = cursor;
    else if (cursor >= top + win)
        top = cursor - win + 1;
    if (count <= win)
        top = 0;
    else if (top + win > count)
        top = count - win;
}

// ------------------------------------------------------------------ drawing

void rule(std::string& out, int cols)
{
    out += fg(kBorder);
    out.append(static_cast<size_t>(std::max(0, cols)), '-');
    out += kReset;
    out += kEol;
}

void header(std::string& out, const Ui& u, std::string_view eyebrow,
            std::string_view right)
{
    // The single red scanline at the top of every screen.
    out += bg(kRed);
    out.append(static_cast<size_t>(std::max(0, u.cols)), ' ');
    out += kReset;
    out += kEol;

    // "// EYEBROW", the recurring tactical mark.
    out += fg(kRed);
    out += "// ";
    out += tracked(eyebrow);
    out += kReset;

    const std::string r(right);
    const int used = static_cast<int>(3 + tracked(eyebrow).size() + r.size());
    if (used < u.cols)
        out.append(static_cast<size_t>(u.cols - used), ' ');
    out += fg(kFg4);
    out += r;
    out += kReset;
    out += kEol;
}

void drawFilter(std::string& out, const Ui& u)
{
    out += fg(kFg4);
    out += " / ";
    out += kReset;
    if (u.filter.empty()) {
        out += fg(kFg4);
        out += "type to filter";
    } else {
        out += fg(kFg1);
        out += u.filter;
        out += fg(kRed);
        out += "_";
    }
    out += kReset;
    out += kEol;
}

std::string changesCell(const GitStatus& g)
{
    switch (g.state) {
    case GitState::Dirty:    return std::to_string(g.dirtyCount());
    case GitState::Clean:    return "clean";
    case GitState::Bare:     return "bare";
    case GitState::Error:    return "error";
    case GitState::Unknown:  return "...";
    case GitState::NotARepo: return "";
    }
    return "";
}

Rgb changesColour(const GitStatus& g)
{
    switch (g.state) {
    case GitState::Dirty: return kWarning;
    case GitState::Clean: return kSuccess;
    case GitState::Error: return kRedDark;
    default:              return kFg4;
    }
}

std::string syncCell(const GitStatus& g)
{
    if (g.state == GitState::NotARepo || g.state == GitState::Bare)
        return "";
    if (!g.hasUpstream)
        return "no upstream";
    std::string s;
    if (g.ahead)
        s += "+" + std::to_string(g.ahead);
    if (g.behind)
        s += (s.empty() ? "" : " ") + std::string("-") + std::to_string(g.behind);
    return s.empty() ? "synced" : s;
}

void drawProjects(std::string& out, const Ui& u, int listRows)
{
    const size_t nameW   = static_cast<size_t>(std::max(16, u.cols * 32 / 100));
    const size_t branchW = static_cast<size_t>(std::max(10, u.cols * 22 / 100));

    out += fg(kFg4);
    out += "  " + cell("NAME", nameW) + " " + cell("BRANCH", branchW) + " ";
    out += cell("CHANGES", 9) + " " + cell("SYNC", 12) + " LAST";
    out += kReset;
    out += kEol;

    for (int r = 0; r < listRows; ++r) {
        const size_t vi = u.top + static_cast<size_t>(r);
        if (vi >= u.visible.size()) {
            out += kEol;
            continue;
        }

        const Project& p        = (*u.projects)[u.visible[vi]];
        const bool     selected = (vi == u.cursor);

        // A 2px red left edge on the selected row, which is how "red is the
        // only accent" survives a list view.
        if (selected) {
            out += fg(kRed);
            out += "\xe2\x96\x8c";   // U+258C left half block
            out += bg(kElevated);
            out += " ";
        } else {
            out += "  ";
        }

        out += fg(selected ? kFg1 : kFg2);
        out += cell(p.displayName(), nameW);
        out += " ";
        out += fg(kFg3);
        out += cell(p.git.branch, branchW);
        out += " ";
        out += fg(changesColour(p.git));
        out += cell(changesCell(p.git), 9);
        out += " ";
        out += fg(kFg3);
        out += cell(syncCell(p.git), 12);
        out += " ";
        out += fg(kFg4);
        out += relTime(p.activityUnix());
        out += kReset;
        out += kEol;
    }
}

void drawItems(std::string& out, const Ui& u, int listRows)
{
    const size_t projW = static_cast<size_t>(std::max(14, u.cols * 26 / 100));

    out += fg(kFg4);
    out += "   " + cell("KIND", 13) + " " + cell("PROJECT", projW) + " WHAT";
    out += kReset;
    out += kEol;

    for (int r = 0; r < listRows; ++r) {
        const size_t vi = u.itemTop + static_cast<size_t>(r);
        if (vi >= u.itemVisible.size()) {
            out += kEol;
            continue;
        }

        const WorkItem& w        = u.items[u.itemVisible[vi]];
        const bool      selected = (vi == u.itemCursor);

        if (selected) {
            out += fg(kRed);
            out += "\xe2\x96\x8c";
            out += bg(kElevated);
        } else {
            out += " ";
        }

        out += w.selected ? fg(kRed) : fg(kFg4);
        out += w.selected ? " [x] " : " [ ] ";

        out += fg(kFg4);
        out += cell(kindLabel(w.kind), 13);
        out += " ";
        out += fg(selected ? kFg1 : kFg2);
        out += cell(w.project, projW);
        out += " ";
        out += fg(kFg3);

        const size_t used = 1 + 5 + 13 + 1 + projW + 1;
        out += cell(w.summary, static_cast<size_t>(std::max<int>(
                                  0, u.cols - static_cast<int>(used))));
        out += kReset;
        out += kEol;
    }
}

void drawSettings(std::string& out, const Ui& u, int listRows)
{
    const std::vector<Setting>& all = settings();

    out += fg(kFg4);
    out += "   " + cell("SETTING", 26) + " " + cell("VALUE", 30) + " ";
    out += "WHAT IT DOES";
    out += kReset;
    out += kEol;

    for (int r = 0; r < listRows; ++r) {
        const size_t i = u.settingTop + static_cast<size_t>(r);
        if (i >= all.size()) {
            out += kEol;
            continue;
        }

        const Setting& s        = all[i];
        const bool     selected = (i == u.settingCursor);

        if (selected) {
            out += fg(kRed);
            out += "\xe2\x96\x8c";
            out += bg(kElevated);
            out += " ";
        } else {
            out += "  ";
        }

        out += fg(selected ? kFg1 : kFg2);
        out += cell(s.label, 26);
        out += " ";

        const bool editingThis = selected && u.editing;
        if (editingThis) {
            out += fg(kFg1);
            out += cell(u.editBuffer + "_", 30);
        } else {
            // The one setting whose displayed value is not simply its own: it
            // follows the autonomy ladder until somebody pins it.
            out += fg(selected ? kRed : kFg3);
            out += cell(readSetting(*u.cfg, s.key), 30);
        }

        out += " ";
        out += fg(kFg4);

        const size_t used = 2 + 26 + 1 + 30 + 1;
        out += cell(s.help, static_cast<size_t>(
                                std::max<int>(0, u.cols - static_cast<int>(used))));
        out += kReset;
        out += kEol;
    }
}

void drawDetail(std::string& out, const Ui& u)
{
    if (u.view == View::Settings) {
        // The autonomy rung governs both the flags a launch carries and what a
        // dispatch briefing permits, so it is worth spelling out under the list
        // rather than leaving it to the one-line help column.
        out += fg(kRed);
        out += " " + std::string(autonomyLabel(u.cfg->autonomy)) + "  ";
        out += fg(kFg2);
        out += autonomySummary(u.cfg->autonomy);
        out += kReset;
        out += kEol;

        out += fg(kFg4);
        out += " claude ";
        LaunchSpec spec;
        for (const std::string& a : claudeArgs(spec, *u.cfg))
            out += a + " ";
        if (u.settingsDirty) {
            out += fg(kRed);
            out += "  UNSAVED";
        }
        out += kReset;
        out += kEol;
        return;
    }

    if (u.view == View::Dispatch) {
        int sel = 0;
        std::vector<std::string> repos;
        for (const WorkItem& w : u.items) {
            if (!w.selected)
                continue;
            ++sel;
            const std::string k = w.path.string();
            if (std::find(repos.begin(), repos.end(), k) == repos.end())
                repos.push_back(k);
        }
        out += fg(kFg3);
        out += " " + std::to_string(sel) + " item" + (sel == 1 ? "" : "s")
             + " selected across " + std::to_string(repos.size())
             + (repos.size() == 1 ? " repository" : " repositories") + ". ";
        out += fg(kFg4);
        out += dispatchSummary(u.cfg->autonomy);
        out += kReset;
        out += kEol;
        out += fg(kFg4);
        out += " Selection is per item, so one repository can contribute some of "
               "its work and not the rest.";
        out += kReset;
        out += kEol;
        return;
    }

    if (u.visible.empty()) {
        out += kEol;
        out += kEol;
        return;
    }

    const Project& p = (*u.projects)[u.visible[u.cursor]];

    out += fg(kFg4);
    out += " " + p.path.string();
    if (!p.ownerRepo.empty()) {
        out += "   ";
        out += fg(kFg3);
        out += p.ownerRepo;
    }
    out += kReset;
    out += kEol;

    out += fg(kFg3);
    out += " ";
    if (p.git.lastCommitUnix) {
        out += relTime(p.git.lastCommitUnix) + " ago  "
             + cell(p.git.lastCommitSubject,
                    static_cast<size_t>(std::max(0, u.cols - 14)));
    } else if (!p.git.error.empty()) {
        out += p.git.error;
    }
    out += kReset;
    out += kEol;
}

void drawFooter(std::string& out, const Ui& u)
{
    // The last line of the frame, so it never ends with a newline: one more
    // would scroll the whole thing and take the red scanline with it.
    if (!u.flash.empty()) {
        out += fg(kRed);
        out += " " + u.flash;
    } else {
        out += fg(kFg4);
        if (u.view == View::Projects) {
            out += " ENTER launch   ^R continue   ^E resume   ^T terminal   "
                   "^D dispatch   F2 settings   ^Q quit";
        } else if (u.view == View::Settings) {
            out += u.editing
                     ? " type to edit   ENTER commit   ESC cancel"
                     : " LEFT/RIGHT change   ENTER edit   ESC back and save   ^Q quit";
        } else {
            out += " SPACE toggle   ^A all   ^N none   ENTER go   ESC back   ^Q quit";
        }
    }
    out += kReset;
    out += "\x1b[K";
}

void render(ConsoleSession& con, Ui& u)
{
    con.size(&u.cols, &u.rows);

    // Everything that is not the list: scanline, eyebrow, rule, filter, rule,
    // column head, rule, two detail lines, rule, footer. The frame has to come
    // to exactly u.rows lines. One line over and the terminal scrolls, which
    // eats the red scanline off the top.
    const int chrome   = 11;
    const int listRows = std::max(1, u.rows - chrome);

    if (u.view == View::Projects)
        clampScroll(u.cursor, u.top, u.visible.size(), listRows);
    else if (u.view == View::Settings)
        clampScroll(u.settingCursor, u.settingTop, settings().size(), listRows);
    else
        clampScroll(u.itemCursor, u.itemTop, u.itemVisible.size(), listRows);

    std::string out;
    out.reserve(static_cast<size_t>(u.cols * u.rows) * 2);
    out += "\x1b[H";   // home, then overwrite everything

    if (u.view == View::Projects) {
        int dirty = 0, open = 0;
        for (const Project& p : *u.projects) {
            if (p.git.state == GitState::Dirty)
                ++dirty;
            open += p.open.total();
        }
        header(out, u, "projects",
               std::to_string(u.visible.size()) + " shown  " + std::to_string(dirty)
                   + " dirty  " + std::to_string(open) + " open");
    } else if (u.view == View::Settings) {
        header(out, u, "settings", Config::filePath().string());
    } else {
        header(out, u, "dispatch",
               std::to_string(u.itemVisible.size()) + " outstanding");
    }

    rule(out, u.cols);
    if (u.view == View::Settings) {
        out += fg(kFg4);
        out += " Changes are written when you leave this view.";
        out += kReset;
        out += kEol;
    } else {
        drawFilter(out, u);
    }
    rule(out, u.cols);

    if (u.view == View::Projects)
        drawProjects(out, u, listRows);
    else if (u.view == View::Settings)
        drawSettings(out, u, listRows);
    else
        drawItems(out, u, listRows);

    rule(out, u.cols);
    drawDetail(out, u);
    rule(out, u.cols);
    drawFooter(out, u);

    out += "\x1b[0J";   // clear anything left below
    con.write(out);
}

// ------------------------------------------------------------------- input

bool isCtrl(const KEY_EVENT_RECORD& k)
{
    return (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
}

} // namespace

BrowseResult browse(ConsoleSession& con, ProjectList& projects, Config& cfg)
{
    BrowseResult result;

    Ui u;
    u.projects = &projects;
    u.cfg      = &cfg;
    refilter(u);

    const HANDLE in = static_cast<HANDLE>(con.inputHandle());

    bool dirty = true;
    for (;;) {
        if (dirty) {
            render(con, u);
            dirty = false;
        }

        INPUT_RECORD rec[64];
        DWORD        n = 0;
        if (!ReadConsoleInputW(in, rec, 64, &n))
            break;

        for (DWORD i = 0; i < n; ++i) {
            if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                dirty = true;
                continue;
            }
            if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown)
                continue;

            const KEY_EVENT_RECORD& k  = rec[i].Event.KeyEvent;
            const wchar_t           ch = k.uChar.UnicodeChar;
            u.flash.clear();
            dirty = true;

            const bool onItems    = (u.view == View::Dispatch);
            const bool onSettings = (u.view == View::Settings);

            // The settings view owns every keystroke while a text field is
            // open, including the ones that are shortcuts everywhere else. A
            // Ctrl+D that fired mid-edit would drop the half-typed value and
            // jump to dispatch.
            if (onSettings && u.editing) {
                if (k.wVirtualKeyCode == VK_RETURN) {
                    const Setting& s = settings()[u.settingCursor];
                    std::string    err;
                    if (applySetting(*u.cfg, s.key, u.editBuffer, &err)) {
                        u.settingsDirty = true;
                        u.editing       = false;
                    } else {
                        u.flash = err;
                    }
                    continue;
                }
                if (k.wVirtualKeyCode == VK_ESCAPE) {
                    u.editing = false;
                    continue;
                }
                if (k.wVirtualKeyCode == VK_BACK) {
                    if (!u.editBuffer.empty())
                        u.editBuffer.pop_back();
                    continue;
                }
                if (ch >= 0x20 && ch < 0x7F) {
                    u.editBuffer.push_back(static_cast<char>(ch));
                    continue;
                }
                continue;
            }

            size_t& cur = onSettings ? u.settingCursor
                        : onItems    ? u.itemCursor
                                     : u.cursor;
            const size_t count = onSettings ? settings().size()
                               : onItems    ? u.itemVisible.size()
                                            : u.visible.size();

            // Navigation
            if (k.wVirtualKeyCode == VK_UP) {
                if (cur > 0) --cur;
                continue;
            }
            if (k.wVirtualKeyCode == VK_DOWN) {
                if (cur + 1 < count) ++cur;
                continue;
            }
            if (k.wVirtualKeyCode == VK_PRIOR) {   // PageUp
                cur = (cur > 10) ? cur - 10 : 0;
                continue;
            }
            if (k.wVirtualKeyCode == VK_NEXT) {    // PageDown
                cur = std::min(count ? count - 1 : 0, cur + 10);
                continue;
            }
            if (k.wVirtualKeyCode == VK_HOME) { cur = 0; continue; }
            if (k.wVirtualKeyCode == VK_END)  { cur = count ? count - 1 : 0; continue; }

            // F2 rather than Ctrl+comma, which is the console front end's one
            // unavoidable divergence from the desktop one: Windows Terminal
            // binds Ctrl+comma to its own settings and swallows it before the
            // application sees a key event at all. Function keys pass through.
            if (k.wVirtualKeyCode == VK_F2) {
                if (u.view == View::Projects) {
                    u.settingCursor = 0;
                    u.settingTop    = 0;
                    u.editing       = false;
                    u.settingsDirty = false;
                    u.view          = View::Settings;
                }
                continue;
            }

            if (onSettings
                && (k.wVirtualKeyCode == VK_LEFT || k.wVirtualKeyCode == VK_RIGHT)) {
                const Setting& s = settings()[u.settingCursor];
                const int dir = k.wVirtualKeyCode == VK_RIGHT ? 1 : -1;
                const std::string next = cycleSetting(*u.cfg, s.key, dir);
                // Text and path settings have nothing to cycle through; Enter
                // opens them instead.
                if (!next.empty() || s.kind == SettingKind::Choice) {
                    std::string err;
                    if (applySetting(*u.cfg, s.key, next, &err))
                        u.settingsDirty = true;
                    else
                        u.flash = err;
                }
                continue;
            }

            // Every action is Ctrl-modified or a named key, because a bare
            // letter has to remain available for the filter.
            if (isCtrl(k)) {
                switch (k.wVirtualKeyCode) {
                case 'Q':
                    result.action = Action::Quit;
                    return result;
                case VK_OEM_COMMA:
                    if (u.view == View::Projects) {
                        u.settingCursor = 0;
                        u.settingTop    = 0;
                        u.editing       = false;
                        u.settingsDirty = false;
                        u.view          = View::Settings;
                    }
                    continue;
                case 'D':
                    if (!onItems && !onSettings) {
                        u.items = collectWorkItems(projects);
                        preselect(u.items, cfg.dispatchMaxRepos);
                        u.filter.clear();
                        refilterItems(u);
                        u.itemCursor = 0;
                        u.itemTop    = 0;
                        u.view       = View::Dispatch;
                    }
                    continue;
                case 'A':
                    if (onItems) {
                        for (WorkItem& w : u.items)
                            w.selected = true;
                    }
                    continue;
                case 'N':
                    if (onItems) {
                        for (WorkItem& w : u.items)
                            w.selected = false;
                    }
                    continue;
                case 'R':
                    if (!onItems && !onSettings && !u.visible.empty()) {
                        result.action  = Action::LaunchContinue;
                        result.project = &projects[u.visible[u.cursor]];
                        return result;
                    }
                    continue;
                case 'E':
                    if (!onItems && !onSettings && !u.visible.empty()) {
                        result.action  = Action::LaunchResume;
                        result.project = &projects[u.visible[u.cursor]];
                        return result;
                    }
                    continue;
                case 'T':
                    if (!onItems && !onSettings && !u.visible.empty()) {
                        result.action  = Action::OpenTerminal;
                        result.project = &projects[u.visible[u.cursor]];
                        return result;
                    }
                    continue;
                default:
                    continue;
                }
            }

            if (k.wVirtualKeyCode == VK_ESCAPE) {
                if (onSettings) {
                    // Written on the way out rather than on every keystroke, so
                    // a half-finished pass through the list is one file write
                    // instead of twenty.
                    if (u.settingsDirty) {
                        std::string err;
                        if (!u.cfg->save(&err)) {
                            u.flash = err;
                            continue;
                        }
                        u.settingsDirty = false;
                    }
                    u.view = View::Projects;
                    refilter(u);
                } else if (onItems) {
                    u.view = View::Projects;
                    u.filter.clear();
                    refilter(u);
                } else if (!u.filter.empty()) {
                    u.filter.clear();
                    refilter(u);
                } else {
                    result.action = Action::Quit;
                    return result;
                }
                continue;
            }

            if (k.wVirtualKeyCode == VK_RETURN) {
                if (onSettings) {
                    const Setting& st = settings()[u.settingCursor];
                    if (st.kind == SettingKind::Text || st.kind == SettingKind::Path
                        || st.kind == SettingKind::StringList) {
                        u.editBuffer = readSetting(*u.cfg, st.key);
                        u.editing    = true;
                    } else {
                        // Choice, Bool and Int have nothing to type, so Enter
                        // means the same as Right.
                        const std::string next = cycleSetting(*u.cfg, st.key, 1);
                        std::string       err;
                        if (applySetting(*u.cfg, st.key, next, &err))
                            u.settingsDirty = true;
                        else
                            u.flash = err;
                    }
                    continue;
                }
                if (onItems) {
                    DispatchOptions opt;
                    opt.autonomy = cfg.autonomy;
                    opt.maxRepos = cfg.dispatchMaxRepos;
                    opt.maxItems = cfg.dispatchMaxItems;

                    result.plan = buildDispatchPlan(u.items, opt);
                    if (result.plan.items.empty()) {
                        u.flash = "nothing selected";
                        continue;
                    }
                    result.action = Action::Dispatch;
                    return result;
                }
                if (!u.visible.empty()) {
                    result.action  = Action::LaunchNew;
                    result.project = &projects[u.visible[u.cursor]];
                    return result;
                }
                continue;
            }

            if (onItems && ch == L' ') {
                if (!u.itemVisible.empty()) {
                    WorkItem& w = u.items[u.itemVisible[u.itemCursor]];
                    w.selected  = !w.selected;
                }
                continue;
            }

            if (k.wVirtualKeyCode == VK_BACK && !onSettings) {
                if (!u.filter.empty()) {
                    u.filter.pop_back();
                    onItems ? refilterItems(u) : refilter(u);
                }
                continue;
            }

            if (ch >= 0x20 && ch < 0x7F && !onSettings) {
                u.filter.push_back(static_cast<char>(ch));
                onItems ? refilterItems(u) : refilter(u);
                continue;
            }
        }
    }

    return result;
}

} // namespace pm::cli
