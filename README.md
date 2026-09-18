<div align="center">

<img src="assets/projectman.ico" width="96" alt="ProjectMan">

# ProjectMan

**Every project you have, what state it is in, and Claude Code one keystroke away.**

[![release](https://img.shields.io/github/v/release/Locke-Werks/ProjectMan?style=flat-square&color=d6262a)](https://github.com/Locke-Werks/ProjectMan/releases)
[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2011-d6262a?style=flat-square)](#requirements)

</div>

---

A projects directory grows past the point where you can hold it in your head.
ProjectMan indexes it, shows the git state and outstanding work for every
project in it, and launches Claude Code into the one you pick. Once sessions are
running it shows what each of them is doing, and which one stopped to ask you
something.

Two front ends over one core.

**`pm`** is a console picker. **`ProjectMan.exe`** is the same data as a desktop
dashboard.

Where a launch lands depends on one thing: whether Docked Console is installed.
With it, sessions go into a column of the dock and the list stays up. Without
it, the console hands over *this* terminal and the desktop app opens a new
Windows Terminal window.

## Docked

If [Docked Console](https://github.com/Locke-Werks/dockedconsole) is installed,
launching puts the session in one of its columns instead of a window of its own.
There is no separate action for it: ENGAGE, TERMINAL and `pm go` all land there,
and the button legend says where a launch is going. Without it they open an
ordinary window, exactly as before.

No dock running starts one; a column with room takes a horizontal split; a full
column gets a new column. Three columns at their pane cap falls back to a
window rather than refusing, because the dock being full is not a reason for a
launch to do nothing.

Hold **Shift** over ENGAGE or TERMINAL, or press `Ctrl+Shift+T`, for a loose
window on one launch. `pm go --window` and `pm open --window` are the same thing
from a shell, and `dock.use_dock = false` turns the whole behaviour off.

ProjectMan never runs `wt` for this: it sends the dock a request and the dock
splits itself. Windows Terminal keeps elevated and unelevated windows in
separate worlds, and aiming `wt` at the wrong one does not fail, it makes a new
window that permanently steals the name. The dock always splits from the token
that owns the column.

Needs Docked Console 0.4.0 or newer, and 0.4.1 for TERMINAL, which asks for a
plain shell rather than a command. An older one cannot take a pane and says so.
If it is not installed at all, ProjectMan offers the download.

## Agents

The desktop app has two tabs, `// PROJECTS` and `// AGENTS`, and the second one
is a board of every Claude Code session on the machine: one card each, sorted
into NEEDS YOU, WORKING, IDLE and DONE, and moving as the sessions move rather
than when you rescan. `Ctrl+B` switches between the two. Desktop only.

It is a tab rather than a window because the board is watching whether or not
you are looking at it, and the number of sessions waiting on you rides on the
AGENTS tab itself. From the project list you can see that something is asking
for you without going to look.

The column comes from the session registry Claude Code keeps for itself at
`~/.claude/sessions/<pid>.json`, one file per running process. That is the only
thing that knows a session is alive, and the only thing still true after one is
killed without warning. It is also where a session waiting on a permission
prompt says so, and what it is waiting for, so NEEDS YOU is read rather than
inferred from a transcript.

The rest of the card is what the session is actually doing: the prompt it was
given, the tool call in flight, the question it asked. That comes from Claude
Code's hooks, and needs registering once:

```
pm hook install
```

That writes eight entries into `~/.claude/settings.json`, merging with whatever
is already there and tagging each one, so `pm hook uninstall` takes back exactly
its own and leaves every other hook alone. `pm hook status` says which state you
are in, and names any event a newer ProjectMan registers that your install is
missing. Without it the board still works, from the registry alone: you see who
is alive and who needs you, but not what any of them is doing.

## Subagents and workflows

A card also carries what the session is running underneath itself. A subagent is
not a card of its own: it has no process, no window and no prompt to answer, so
there is nothing FOCUS, ENGAGE or DISPATCH could aim at. It belongs to the
session that can be acted on.

A subagent keeps its parent's session id and reports its tool calls through the
same hooks, so telling the two apart needs `agent_id`, which every event fired
inside one carries. Without reading it a card shows whichever of five subagents
called a tool last as what the session itself is doing. The card now shows the
session's own work on its own line and each subagent on one of its own,
brightest while it is running.

A Workflow tool run appears as one line: its name, the phase it has reached, and
how many of its agents are running out of how many it has started. Those come
from two files Claude Code writes beside the session, and the split between them
decides what can be known. The script lands when the run starts, so the name and
the run are visible immediately. The summary, which is the only thing carrying a
status, is written when the run ends, so `completed` or `killed` arrives at the
end and not before.

Only `SubagentStop` says a subagent has finished. Nothing on disk does: the file
Claude Code writes when one spawns is never rewritten. So with no hooks
registered a subagent line is dimmed and says on hover that it was seen to start
and nothing more, rather than implying it is still going.

The board only reports. ENGAGE, CONTINUE and DISPATCH are the same launches the
project list offers, aimed at the session's repository. FOCUS raises the window a
session is showing through, which is best effort and says so: `claude.exe`
usually has no window of its own, so it raises the terminal or the dock hosting
the session, and a dock column shares one window across every pane in it.

Nothing here writes to Claude Code's own files. The registry is read, never
touched, and the only file ProjectMan writes for this is its own event log under
`%LOCALAPPDATA%\ProjectMan\agents`.

A session's name comes from Claude Code and changes as the work changes topic, so
a card renames itself over a long session. The registry is undocumented and
carries the version that wrote it, so an unrecognised value is shown rather than
dropped, and a Claude Code that stops publishing it costs the columns, not the
board.

## ProjectMan MCP

A second product in this repository, shipped as its own installer: an stdio
[MCP](https://modelcontextprotocol.io) server exposing exactly one tool.

```
get-projects   every project under the root, with its git state and open work
```

It reports and nothing else. No launching, no dispatching, no writing, and it
never creates a config. That last part is why it requires ProjectMan: it reads
`projectman.toml` to learn the projects root and the scan settings, and without
one there is nothing to report on. Its installer checks for ProjectMan and
refuses rather than guessing a root. Asked for projects with no config present,
the tool answers with an error saying so.

The result is one JSON object: the root, a scan timestamp, totals, and a
`projects` array carrying each project's path, kind, origin, full git state
(branch, upstream, ahead/behind, staged, unstaged, untracked, conflicted,
stashes, last commit) and everything the front ends count as outstanding.

Registering it with Claude Code, once the installer has put it on PATH:

```
claude mcp add projectman -- pm-mcp
```

ProjectMan itself neither installs nor offers this. The two are released
together and versioned together, but installing one has no effect on the other.

## Dispatch

When work is outstanding in several repositories at once, `pm dispatch` (or
Ctrl+D in either front end) lists every outstanding item as a checklist. Tick
what you want worked and hit GO. One Claude Code session runs with each selected
repository passed via `--add-dir` and a briefing of what is outstanding where.

The run happens inside ProjectMan, not in a terminal. It is `claude -p`, one
prompt run to completion, with the briefing fed in on stdin and the session's
event stream rendered as it arrives: what the session says, one line per tool
call, and any error a tool returned. The desktop app opens a run window with
STOP, COPY and CONTINUE; the console prints the same lines and Ctrl+C stops it.
STOP takes down the session and everything it started. The run ends with a
summary: how long it took, how many turns, what it cost, and at `suggest`, how
many tool calls were refused. A session signed in on a subscription draws on
that subscription's usage rather than an API bill, and the summary says so: the
dollar figure is then what the tokens would have cost at API rates. CONTINUE resumes the same session interactively,
in the dock when there is one, with the same repositories, which is how a run
that stopped to ask something gets its answer.

Selection is per item, not per repository, so one repo can contribute some of
its work and not the rest. By default the session commits but never pushes, so
every result is reviewable with `git log` and reversible with `git reset`.

**General instructions** scope the run to one job across the selected
repositories rather than working the items as found: "ensure every default
branch is named main, and rename it where it is not", or "update all docs to
match the current code state". The text goes in above the item list and governs
it, and the rule that otherwise forbids unlisted work is relaxed to permit
exactly what the instruction asks. The item list stays as context. `Ctrl+G` in
the console, the box in the desktop dialog, or `pm dispatch -i "<text>"`.

**Preview** shows the whole briefing before anything launches, exactly as the
session receives it. `Ctrl+P` in the console, the PREVIEW button in the desktop
dialog, or `pm dispatch --dry-run`.

## Commands

```
pm                          browse and launch
pm ls [--dirty] [--repos] [--sort recent|name|dirty|open] [--json]
pm status <name>            one project in detail
pm go <name> [-c|-r] [--window]      launch Claude Code there
pm open <name> [--window]            a plain shell there
pm dock <name>                       the dock explicitly, never a window
pm items [--json]           every outstanding item across the tree
pm dispatch [--all] [--dry-run] [-i "<instructions>"]
pm refresh                  refetch open pull requests and issues
pm doctor                   resolve git, claude and wt, and time a sweep
pm hook <install|uninstall|status>   the hooks the agent board reads
pm config [show|get <key>|set <key> <value>] [--path|--init]
```

`<name>` matches on exact name, then unique prefix, then unique substring. A
repository inside a container directory is qualified by it, so
`pm go "HHS Matrix/ocio-ato-modernization-site"` picks that checkout rather than
the one at the root.

Exit codes: 0 ok, 1 error, 2 usage, 3 environment, 4 could not launch, 5 no
match or ambiguous, 6 the dock is full. When a child ran, `pm` returns the
child's code unchanged.

## Keys

Every action is Ctrl-modified, because bare letters go to the filter.

| Key | |
|---|---|
| type | filter |
| Enter | launch Claude Code (into the dock when one is installed) |
| Ctrl+R | continue the last conversation there |
| Ctrl+E | pick a session to resume |
| Ctrl+T | open a plain shell |
| Shift+Enter, Ctrl+Shift+T | the same, in a loose window rather than the dock |
| Ctrl+D | dispatch |
| Ctrl+B | switch between the PROJECTS and AGENTS tabs (desktop only) |
| F2 | settings (Ctrl+, in the desktop app) |
| F5 | rescan |
| Ctrl+Q | quit |

In the dispatch view: Space toggles, Ctrl+A all, Ctrl+N none, Ctrl+G general
instructions, Ctrl+P preview the briefing, Enter go, Esc back.

## What counts as outstanding

- Uncommitted changes, unpushed commits, stashes, and branches with no upstream
- Open pull requests and issues, fetched with `gh` and cached
- Unchecked `- [ ]` boxes in markdown
- The title and last prompt of an unfinished Claude Code session

## Settings

Both front ends read and write the same file, from one shared description of
the editable surface, so adding a setting adds it to both.

- Console: `F2` opens the settings view. Left and right change a value, Enter
  edits a text field, Escape saves and returns. It is `F2` rather than `Ctrl+,`
  because Windows Terminal binds that to its own settings and the key never
  reaches the application.
- Desktop: `Ctrl+,` or the SETTINGS button.
- Shell: `pm config`, `pm config get <key>`, `pm config set <key> <value>`.

### Autonomy

One ladder governs how far Claude Code may go on its own. Each rung contains
the ones below it, and it drives both the flags a session launches with and
what a dispatch briefing permits, so the two can never contradict each other.

| Rung | |
|---|---|
| `suggest` | Read and report. Changes nothing, and asks before every tool. |
| `write` | Edit the working tree. Every change stays visible in `git diff`. |
| `commit` | Edit and commit. Nothing leaves the machine; `git reset` undoes it. |
| `push` | Commit and push a branch. Work leaves the machine. |
| `full` | Push and open pull requests. Reviewable, but public. |

`commit` is the default. At `suggest`, `--dangerously-skip-permissions` is not
passed, because asking before every edit is the whole of that rung.

### The file

`%LOCALAPPDATA%\ProjectMan\projectman.toml`, written on first run. A copy beside
the executable takes precedence, so a portable checkout can carry its own.

Unknown keys are rejected rather than ignored, and a file that fails to parse is
reported with its line number and left alone rather than overwritten.

```toml
root     = 'C:\Users\you\projects'
autonomy = "commit"

[launch]
claude = 'C:\Users\you\.local\bin\claude.exe'
effort = "max"
# skip_permissions follows the autonomy ladder until you set it here.

[dispatch]
max_repos = 8
max_items = 40

[dock]
use_dock = true   # false keeps every launch in a window of its own
```

`scan.threads = 0` picks three quarters of the logical CPUs.

## Requirements

Windows 11. `git` on PATH. `claude` for launching, `gh` for pull requests and
issues, Windows Terminal for the desktop front end, and Docked Console 0.4.0 or
newer for DOCK; each is optional and only that feature is unavailable without
it.

The agent board needs a Claude Code that publishes its session registry, which
2.1.270 does. An older one that does not leaves the board empty until
`pm hook install`, after which it fills from the hooks instead, without the
columns the registry decides.

Subagent lines need a Claude Code that puts `agent_id` on a hook payload and
fires `SubagentStart` and `SubagentStop`, which 2.1.276 does. Workflow lines
need no hooks at all, and neither costs anything else on the board when it is
absent.

## Building

```
cmake --preset vs -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build --preset vs
```

Qt is needed only for the desktop application. `-DPM_BUILD_GUI=OFF` builds just
`pm.exe`, which links no Qt at all and ships as a single file.

The presets deliberately name no generator: pinning a Visual Studio year breaks
the moment a machine moves on.

## License

GPLv3. See [LICENSE](LICENSE).

Chakra Petch and Outfit are bundled under the SIL Open Font License 1.1; see
[assets/fonts](assets/fonts/README.md).
