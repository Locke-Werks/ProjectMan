A control surface over the projects tree.

`pm` lists every project with its git state and outstanding work, and hands the
current terminal to Claude Code in the one you pick. `ProjectMan.exe` shows the
same data as a desktop dashboard and launches into a new Windows Terminal
window.

New in 0.3.0: general instructions on a dispatch. A free-text box above the item
list scopes the run to one job across the selected repositories rather than
working the items as found: "ensure every default branch is named main, and
rename it where it is not", or "update all docs to match the current code
state". It goes into the briefing above the items and governs them, and the rule
that otherwise forbids unlisted work is relaxed to permit exactly what the
instruction asks, since without that the session would be right to refuse.
`Ctrl+G` in the console, the box in the desktop dialog, or
`pm dispatch -i "<text>"`.

PREVIEW, `Ctrl+P`, shows the whole briefing before anything launches, exactly as
the session receives it. The console preview scrolls and wraps to the window;
the desktop one copies to the clipboard. `pm dispatch --dry-run` prints the same
text.

New in 0.2.0: DOCK, `Ctrl+K`, or `pm dock <name>` puts the session in a [Docked
Console](https://github.com/Locke-Werks/dockedconsole) column instead of a
window of its own. No dock running starts one, a column with room takes a
horizontal split, a full column gets a new column, and three columns at their
pane cap refuses rather than falling back to a loose window. Needs Docked
Console 0.4.0 or newer; if it is not installed, ProjectMan offers the signed
installer.

ProjectMan does not run `wt` for this. Windows Terminal keeps elevated and
unelevated windows in separate monarch worlds, and an unelevated `wt` aimed at
an elevated one does not fail: it creates a new window that permanently owns the
name, so every later pane meant for that column lands in the stray window.
ProjectMan sends the dock a request and the dock splits itself.

Also fixed in 0.2.0: a session launched from inside another Claude Code session
inherited that session's `CLAUDE_CODE_CHILD_SESSION` marker and started with its
transcript turned off. Every launch path now builds its own environment. And
`pm open`, which is meant to open a plain shell, was opening Claude Code.

`pm dispatch`, or Ctrl+D in either front end, lists every outstanding item
across the tree as a checklist. Tick what you want worked and one Claude Code
session starts with each selected repository attached and a briefing of what is
outstanding where. It commits but does not push.

Installing puts the install directory on PATH so `pm` works from any terminal.
