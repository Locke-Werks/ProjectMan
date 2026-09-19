A control surface over the projects tree.

`pm` lists every project with its git state and outstanding work, and hands the current terminal to Claude Code in the one you pick. `ProjectMan.exe` shows the same data as a desktop dashboard and launches into a new Windows Terminal window.

New in 1.0.1: the node explorer fills the canvas. The view fits whatever is on it to within about a hundred pixels of every edge at any size, so one session on its own is a large circle rather than a small one adrift in a black field, and the zoom eases out and re-centres as work arrives and leaves. The fit reads each node at the size it is settling to rather than the size it is mid-pop, so nothing lunges at the camera on its way out.

New in 1.0.0: **a node explorer.** A third tab draws the same sessions as what they are: a live session is a large node, every workflow run and subagent under it is a smaller one tied to it, and the whole thing settles under repulsion and link tension. Work pops in when it starts and pops out when it ends, the rest slides out of the way, and the view zooms itself to keep the structure in frame. It is a view and nothing else: no panning, no zoom control, no selection. One watcher feeds it and the board, so the two cannot disagree about the same instant, and it stops repainting entirely once the graph has settled.

Also new in 1.0.0: a workflow run's line counts the way Claude Code counts it, done out of started rather than running out of started, and the agents inside a run are listed under it by the label their script gave them. Both come from the run's own journal, which is the only file that says what a run is doing while it is doing it, so neither needs hooks installed.

Fixed in 1.0.0: a card went on asking in red after its question had been answered. The flag that decided the column and the text the card showed were cleared by different things, so an approved permission prompt left its line behind on a card that had gone back to work, hiding the tool line underneath it.

New in 0.8.0: the agent board is a tab rather than a window. `Ctrl+B` cycles the three tabs, and the count of sessions waiting on you rides on the AGENTS tab itself, so the answer to "is anything asking for me" does not require being on that tab to see.

New in 0.7.0: **an agent board.** A board of every Claude Code session on the machine, one card each, in NEEDS YOU, WORKING, IDLE or DONE, moving as the sessions move rather than when you rescan. Desktop only.

The column comes from the session registry Claude Code keeps for itself at `~/.claude/sessions/<pid>.json`. That is the only thing that knows a session is alive, and the only thing still true after one is killed without warning; it is also where a session waiting on a permission prompt says so and says what for, so NEEDS YOU is read rather than inferred from a transcript. The rest of the card, the prompt it was given and the tool call in flight, comes from Claude Code's hooks and needs `pm hook install` once. Without that the board still works from the registry alone: who is alive and who needs you, but not what any of them is doing.

`pm hook install` writes eight entries into `~/.claude/settings.json` and tags each one, so `pm hook uninstall` takes back exactly its own and leaves every other hook alone. That file is hand-maintained, so being careful with it is most of the work: a failure to read it refuses rather than treating it as absent and replacing it, a duplicate key refuses rather than guessing which copy wins, a symlinked file is written through rather than replaced, and a change made while `pm` was working aborts the write instead of reverting it. Install then uninstall returns the file byte for byte.

`pm hook` itself runs on the critical path of every tool call in every session, so it loads no config, prints nothing and always exits 0. Exit code 2 in particular is what Claude Code reads from a `PreToolUse` hook as a refusal, so even the usage path returns 0 once stdin is not a console.

The board only reports. ENGAGE, CONTINUE and DISPATCH are the launches the project list already offers, aimed at the session's repository. FOCUS raises the window a session is showing through, which is best effort: `claude.exe` usually has no window of its own, so it raises the terminal or the dock hosting the session, and a dock column shares one window across every pane in it. Nothing writes to Claude Code's own files.

New in 0.6.0: **dispatch runs inside ProjectMan.** GO no longer opens a terminal with an interactive session in it; it runs `claude -p` with the briefing on stdin and shows the session's event stream as it arrives, in a run window on the desktop and as plain lines on the console. Each tool call is one line, errors are shown, the run ends with its duration, turn count and cost (labelled as an API-rate equivalent when the session runs on a subscription), and STOP (or Ctrl+C) takes down the session and everything it started. CONTINUE resumes the same session interactively, in the dock when there is one, with the same repositories.

Fixed in 0.6.0: a dispatched session started idle, with nothing to do. Claude Code's `--add-dir` takes every following bare argument as another directory, and the briefing came right after the last one, so the session read it as a path and waited for a prompt that never came. The briefing no longer travels on the command line at all, and an interactive launch that does carry a prompt now puts `--` in front of it.

New in 0.5.0: **ProjectMan MCP**, a second product with its own installer, `ProjectMan-MCP-Setup.exe`. It is an stdio MCP server exposing exactly one tool, `get-projects`, which returns every project under the projects root with its git state and outstanding work as one JSON object. It reports and does nothing else: no launching, no dispatching, no writing, and it never creates a config.

That is why it requires ProjectMan. The server reads `projectman.toml` for the projects root and the scan settings and will not invent one, so its installer checks for ProjectMan and refuses rather than guessing. Register it with `claude mcp add projectman -- pm-mcp`.

ProjectMan neither installs nor offers it. The two ship from one tag and share a version number; installing either has no effect on the other.

New in 0.4.0: if [Docked Console](https://github.com/Locke-Werks/dockedconsole) is installed, that is where launches go. It is no longer a separate action: DOCK and `Ctrl+K` are gone, and ENGAGE, TERMINAL, `Enter`, `Ctrl+T`, `pm go` and `pm open` all land in a column. Without it they open an ordinary window exactly as before, and the button legend in each front end says which it will be. Hold Shift over a launch, or use `--window`, for a loose window on one launch; `dock.use_dock = false` turns it off entirely.

A dock that is full now falls back to a window instead of refusing. As an explicit action a refusal was right, but as the default path it would leave a button doing nothing.

The desktop detail pane grew a view of what actually changed: every modified file with its status and line counts, the diffstat total, and the last eight commits. It is fetched off the GUI thread, one repository at a time, so arrowing down the list does not spawn a git process per row.

New in 0.3.0: general instructions on a dispatch. A free-text box above the item list scopes the run to one job across the selected repositories rather than working the items as found: "ensure every default branch is named main, and rename it where it is not", or "update all docs to match the current code state". It goes into the briefing above the items and governs them, and the rule that otherwise forbids unlisted work is relaxed to permit exactly what the instruction asks, since without that the session would be right to refuse. `Ctrl+G` in the console, the box in the desktop dialog, or `pm dispatch -i "<text>"`.

PREVIEW, `Ctrl+P`, shows the whole briefing before anything launches, exactly as the session receives it. The console preview scrolls and wraps to the window; the desktop one copies to the clipboard. `pm dispatch --dry-run` prints the same text.

New in 0.2.0: DOCK, `Ctrl+K`, or `pm dock <name>` puts the session in a [Docked Console](https://github.com/Locke-Werks/dockedconsole) column instead of a window of its own. No dock running starts one, a column with room takes a horizontal split, a full column gets a new column, and three columns at their pane cap refuses rather than falling back to a loose window. Needs Docked Console 0.4.0 or newer; if it is not installed, ProjectMan offers the signed installer.

ProjectMan does not run `wt` for this. Windows Terminal keeps elevated and unelevated windows in separate monarch worlds, and an unelevated `wt` aimed at an elevated one does not fail: it creates a new window that permanently owns the name, so every later pane meant for that column lands in the stray window. ProjectMan sends the dock a request and the dock splits itself.

Also fixed in 0.2.0: a session launched from inside another Claude Code session inherited that session's `CLAUDE_CODE_CHILD_SESSION` marker and started with its transcript turned off. Every launch path now builds its own environment. And `pm open`, which is meant to open a plain shell, was opening Claude Code.

`pm dispatch`, or Ctrl+D in either front end, lists every outstanding item across the tree as a checklist. Tick what you want worked and one Claude Code session starts with each selected repository attached and a briefing of what is outstanding where. It commits but does not push.

Installing puts the install directory on PATH so `pm` works from any terminal.
