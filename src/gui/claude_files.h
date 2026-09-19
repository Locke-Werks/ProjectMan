#pragma once

#include "json.h"
#include "model.h"

#include <cstdint>
#include <string>

// Reading files somebody else is still writing.
//
// Every source the board and the node explorer read belongs to Claude Code, is
// undocumented, and has a live writer on the other end of it. That shapes all
// of this: opens share, sizes are capped, a missing key is information rather
// than an error, and nothing here throws or reports. A source that has changed
// shape under us must cost a field, never a refresh.
//
// Extracted from agent_board.cpp when claude_state.cpp needed the same six
// functions. Nothing here is specific to a card, a node or a session.
namespace pm::gui::cf {

// FILETIME counts 100ns ticks from 1601; Unix time counts from 1970.
inline constexpr std::int64_t kFileTimeUnixEpoch = 116444736000000000LL;

// The two known folders everything here hangs off, empty when the shell cannot
// resolve them. Named rather than taking a KNOWNFOLDERID, so this header stays
// free of windows.h: it is included by translation units that want Qt parsed
// first, and windows.h is a wall of macros that does not co-operate.
fs::path userProfile();
fs::path localAppData();

// %USERPROFILE%\.claude. Empty when the profile cannot be resolved. Every
// Claude Code state directory this program reads hangs off it.
fs::path claudeHome();

// The tail of a file, read without taking it away from whoever is writing it.
//
// Sessions append to the event log continuously and Claude Code rewrites a
// registry record on every status change. An exclusive open would fail against
// a live writer, or worse, block a hook that sits on the critical path of
// someone's tool call. FILE_SHARE_DELETE is in the set for the rollover, which
// renames the live log out from under any reader holding it open.
//
// Past `maxBytes` the head of the file goes rather than the tail, and the first
// partial line with it: what matters in an append-only file is its end.
std::string readShared(const fs::path& file, std::int64_t maxBytes);

// The same, from the other end: the FIRST `maxBytes` of a file, with no line
// trimming.
//
// Everything else here is a log or a record that grows, so readShared keeps the
// end and that is the right default. A document is the opposite: its title is
// the first line of it, and reading a 15KB plan through readShared with an 8KB
// budget returns the last 8KB, which has no heading in it and silently falls
// back to naming the plan after its own filename.
std::string readSharedHead(const fs::path& file, std::int64_t maxBytes);

// Whether somebody has this file open for writing, asked by trying to open it
// with sharing denied.
//
// This answers "is it still running" for a SHELL and for nothing else. Claude
// Code holds a write handle on a background shell's output file for exactly as
// long as that shell runs, so the open fails with a sharing violation while it
// lives and succeeds once it ends. That is the only liveness signal the file
// carries: a foreground command's file is deleted when it returns, but a
// background shell's lingers at its final size, looking exactly like one that
// has gone quiet.
//
// A MONITOR never holds the file, even mid-event, so this is false throughout
// its life and says nothing about it. Measured on CLI 2.1.278 by sampling four
// files 60 times over nine seconds: a live background shell locked 60/60, two
// monitors emitting events throughout locked 0/60, a finished shell 0/60. An
// earlier single sample caught a monitor mid-write and read as locked, which is
// how this came to be documented the wrong way round to begin with. See
// BackgroundTask::live for what to ask instead.
//
// False for a file that is not there at all: nothing is writing to a file that
// does not exist. A failure that is not a sharing violation is also false,
// because it is a file this process cannot judge either way and claiming a task
// is running for ever is the worse of the two errors.
bool writerHoldsOpen(const fs::path& file);

// The write time of a file, as Unix milliseconds. Zero when it cannot be read.
std::int64_t fileWriteTimeMs(const fs::path& file);

// A cwd as Claude Code spells the directory it keeps that session's files in:
// every character that is not a letter or a digit becomes a dash, and runs are
// not collapsed, so "C:\p\My App" is "C--p-My-App".
//
// Checked against all 28 project directories on this machine by reading each
// transcript's own cwd back and recomputing the name: 28 matches, 0 misses.
// It is not invertible, since a project whose name contains a dash spells the
// same as one containing a space, so this only ever goes cwd to slug.
//
// Two directories are named this way and both are read here: the session's own
// files under ~/.claude/projects, and its scratchpad and task output under
// %TEMP%\claude. If a later Claude Code changes the rule, the cost is the
// subagent lines on a card and the shells on the canvas; everything else is
// read from somewhere that does not depend on it.
std::string projectSlug(const fs::path& cwd);

// FILETIME ticks to Unix milliseconds. Zero for anything before 1970.
std::int64_t ticksToUnixMs(unsigned long long ticks);

// An ISO-8601 instant of the shape Claude Code writes into its job files,
// "2026-09-17T18:18:53.979Z", as Unix milliseconds. Zero when it will not
// parse. Deliberately narrow: this is not a date library, it reads the one
// format this program has seen, and anything else is a source that changed.
std::int64_t iso8601Ms(const std::string& text);

// ------------------------------------------------------------------- fields

// A string member, or empty when absent or not a string.
std::string stringField(const json::Value* object, const char* key);

// For a key whose shape is not pinned down. These files are undocumented and
// carry the CLI version that wrote them, so a value that is a string today need
// not be on the next one. A number or an object comes back as its JSON rather
// than as nothing, which puts an unrecognised value on screen instead of
// letting it read as absent. Keys read as structure rather than as text, a
// session id or a path, still go through stringField.
std::string anyString(const json::Value* object, const char* key);

std::int64_t intField(const json::Value* object, const char* key);

// A process creation FILETIME, written as a decimal string because the value is
// past what a double holds exactly. Read it either way: a later writer emitting
// it as a number would still be reporting the same thing.
unsigned long long ticksField(const json::Value* object, const char* key);

// ------------------------------------------------------------- still running

// Whether `pid` still belongs to the process that recorded it.
//
// Two separate questions, and the second is the one that bites: these files
// outlive the processes that wrote them, and Windows hands a pid straight back
// out, so a stale file can name a pid that something unrelated now owns. The
// recorded creation time is what tells the two apart; `startedAtMs` is the
// weaker fallback for a record that carries no ticks.
bool processStillOurs(unsigned long pid, unsigned long long recordedTicks,
                      std::int64_t startedAtMs);

} // namespace pm::gui::cf
