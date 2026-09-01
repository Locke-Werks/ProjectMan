<div align="center">

<img src="assets/projectman.ico" width="96" alt="ProjectMan">

# ProjectMan

**Every project you have, what state it is in, and Claude Code one keystroke away.**

[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2011-d6262a?style=flat-square)](#requirements)

</div>

---

A projects directory grows past the point where you can hold it in your head.
ProjectMan indexes it, shows the git state and outstanding work for every
project in it, and launches Claude Code into the one you pick.

Two front ends over one core.

**`pm`** is a console picker. Selecting a project hands it *this* terminal, so
Claude Code takes over the window you are already in. When it exits you are back
at the list.

**`ProjectMan.exe`** is the same data as a desktop dashboard. Launching opens a
new Windows Terminal window in the project directory.

## Dispatch

When work is outstanding in several repositories at once, `pm dispatch` (or
Ctrl+D in either front end) lists every outstanding item as a checklist. Tick
what you want worked and hit GO. One Claude Code session starts with each
selected repository passed via `--add-dir` and a briefing of what is outstanding
where.

Selection is per item, not per repository, so one repo can contribute some of
its work and not the rest. The session is interactive: it asks when something is
unclear rather than guessing. By default it commits but never pushes, so every
result is reviewable with `git log` and reversible with `git reset`.

## Commands

```
pm                          browse and launch
pm ls [--dirty] [--repos] [--sort recent|name|dirty|open] [--json]
pm status <name>            one project in detail
pm go <name> [-c|-r]        hand this terminal to Claude Code there
pm open <name>              open a new terminal window there
pm items [--json]           every outstanding item across the tree
pm dispatch [--all] [--dry-run]
pm refresh                  refetch open pull requests and issues
pm doctor                   resolve git, claude and wt, and time a sweep
pm config [--path|--init]
```

`<name>` matches on exact name, then unique prefix, then unique substring. A
repository inside a container directory is qualified by it, so
`pm go "HHS Matrix/ocio-ato-modernization-site"` picks that checkout rather than
the one at the root.

Exit codes: 0 ok, 1 error, 2 usage, 3 environment, 4 could not launch, 5 no
match or ambiguous. When a child ran, `pm` returns the child's code unchanged.

## Keys

Every action is Ctrl-modified, because bare letters go to the filter.

| Key | |
|---|---|
| type | filter |
| Enter | launch Claude Code |
| Ctrl+R | continue the last conversation there |
| Ctrl+E | pick a session to resume |
| Ctrl+T | open a terminal window |
| Ctrl+D | dispatch |
| F5 | rescan |
| Ctrl+Q | quit |

In the dispatch view: Space toggles, Ctrl+A all, Ctrl+N none, Enter go, Esc back.

## What counts as outstanding

- Uncommitted changes, unpushed commits, stashes, and branches with no upstream
- Open pull requests and issues, fetched with `gh` and cached
- Unchecked `- [ ]` boxes in markdown
- The title and last prompt of an unfinished Claude Code session

## Configuration

`%LOCALAPPDATA%\ProjectMan\projectman.toml`, written on first run. A copy beside
the executable takes precedence, so a portable checkout can carry its own.

Unknown keys are rejected rather than ignored, and a file that fails to parse is
reported and left alone rather than overwritten.

```toml
root = 'C:\Users\you\projects'

[launch]
claude = 'C:\Users\you\.local\bin\claude.exe'
args   = ["--dangerously-skip-permissions", "--effort", "max"]

[dispatch]
commit    = true
push      = false
max_repos = 8
```

`scan.threads = 0` picks three quarters of the logical CPUs.

## Requirements

Windows 11. `git` on PATH. `claude` for launching, `gh` for pull requests and
issues, and Windows Terminal for the desktop front end; each is optional and
only that feature is unavailable without it.

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
