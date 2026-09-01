A control surface over the projects tree.

`pm` lists every project with its git state and outstanding work, and hands the
current terminal to Claude Code in the one you pick. `ProjectMan.exe` shows the
same data as a desktop dashboard and launches into a new Windows Terminal
window.

`pm dispatch`, or Ctrl+D in either front end, lists every outstanding item
across the tree as a checklist. Tick what you want worked and one Claude Code
session starts with each selected repository attached and a briefing of what is
outstanding where. It commits but does not push.

Installing puts the install directory on PATH so `pm` works from any terminal.
