# wbsh SDK

Write a util, drop in the DLL, restart. Nothing gets rebuilt, and nothing
gets linked.

A util is an ordinary Windows DLL that exports three C functions. It can add
commands to the shell, features to the terminal, or both from the same file.

## Getting the SDK

Every release on GitHub carries `wbsh-sdk-<version>.zip`:

```
include\wbshsdk.h     the whole contract
wbshutil.props        one property sheet for MSBuild projects
template\             a util to rename: myutil.c, myutil.vcxproj, CMakeLists.txt
samples\              hello, pick, procs, tally as source
wbshsdk.pdb           symbols for the SDK DLL, for a readable stack
README.md, CHANGELOG.md
```

There is no import library to link. The host hands a util a table of
functions when it loads, so any compiler that can build a Windows DLL will
do: MSVC, clang, MinGW.

## The shortest useful util

```c
#include "wbshsdk.h"

static const WbshUtilInfo kInfo = {
    WBSH_SDK_ABI, "greet", "1.0.0", "Says hello."
};

static const WbshApi* wbsh;

static int greet(void* user, int argc, const char* const* argv) {
    (void)user; (void)argc; (void)argv;
    wbsh->print("hello\n");
    return 0;
}

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) { return &kInfo; }
WBSH_UTIL_API void wbshUtilUnload(void) { }

WBSH_UTIL_API int wbshUtilLoad(const WbshApi* api) {
    wbsh = api;
    if (api->host == WBSH_HOST_SHELL) {
        WbshCommand command = { sizeof(command), "greet", "Says hello.", "greet", greet };
        api->register_command(&command);
    }
    return WBSH_OK;
}
```

## Building one

**With MSBuild.** Copy `template\` anywhere, rename what you like, and tell
the project where the SDK is:

```
msbuild myutil.vcxproj -p:Configuration=Release -p:Platform=x64 -p:WbshSdkDir=C:\sdk\wbsh-sdk-1.0.24\
```

`WbshSdkDir` is the folder holding `wbshutil.props`; a project left inside
the zip's own `template\` folder finds it without being told. The DLL lands
in `bin\x64\Release\`.

**With CMake.** The same folder has a `CMakeLists.txt`:

```
cmake -S . -B build -DWBSH_SDK_DIR=C:\sdk\wbsh-sdk-1.0.24
cmake --build build --config Release
```

**With anything else.** Add `include\` to the include path, define
`WIN32_LEAN_AND_MEAN`, and build a DLL. That is the whole recipe.

The samples build the same way. `sdk/samples/hello` is the one to read
first: one command with usage text and Tab completion, one status segment,
and a look at Ctrl+C and shell variables. `pick` is a fuzzy picker built on
the terminal helpers, `procs` a live process monitor that also puts a count
in the status bar, and `tally` shows the two halves of a util agreeing
across the process boundary.

## Installing one

Drop the DLL in any of these. They are scanned in this order:

| Folder | For |
| --- | --- |
| `<install>\plugins\` | Utils that ship with an install |
| `%APPDATA%\wbsh\plugins\` | A user's own, no administrator needed |
| each folder in `WBSH_PLUGINS` | Your build output, while you work on it |

`WBSH_PLUGINS` is a list of folders separated by `;`, read by both hosts.

`utils` lists what loaded and, under **not loaded**, every DLL that was
refused and why. A refused DLL is skipped, so one broken util never costs
you the others.

## Working on one

The edit-run loop does not need a restart:

```
$ utils load C:\work\myutil\bin\x64\Release\myutil.dll
$ myutil
$ utils reload
```

`utils load` brings one DLL into the running shell. `utils reload` drops
every util, lets go of every DLL, and loads everything again, so a rebuilt
DLL is picked up in place. Files loaded by hand are reloaded too.

`help NAME` prints the summary and usage a command registered, `help`
lists every util command under **added by utils**, and Tab on a command's
argument asks the util for candidates.

## Two hosts, one DLL

| Host | Is | Offers |
| --- | --- | --- |
| `WBSH_HOST_SHELL` | wbsh.exe | Commands, stdout/stderr, shell variables, Ctrl+C |
| `WBSH_HOST_TERMINAL` | wbshterm.exe | Status-bar segments |

The hosts are **separate processes**. When wbshterm runs a shell, your DLL
is loaded twice — once in each — so `wbshUtilLoad` runs twice with a
different `api->host` each time, and each copy has its own globals. A
counter bumped by your command does not move the segment your terminal is
drawing; if the two halves need to agree, they have to agree through
something outside the process. `sdk/samples/tally` does it through a named
piece of shared memory, which is the smallest thing that works: both copies
open `Local\wbsh-sample-tally` in `wbshUtilLoad`, the command writes a word
into it, the segment reads the word back.

Register what the host offers and return `WBSH_OK` regardless. A host that
does not offer something answers `WBSH_ERR_UNSUPPORTED`, which is an
answer, not a failure. Returning anything but `WBSH_OK` from
`wbshUtilLoad` unloads the util.

## Rules the boundary keeps

**It is C.** No exceptions, no C++ objects, no allocations freed on the
other side. That is what lets a util built with one compiler load into a
host built with another.

**`WBSH_SDK_ABI` must match exactly.** A util built against a different
number is refused by name rather than crashing later. The number only
changes when something already in the header changes shape; a function
added at the end of `WbshApi` is not such a change, and `api->size` says
how far the host's table reaches.

**Strings are UTF-8.** Anything the host hands you is valid until your
function returns; copy it if you want to keep it.

**A segment is on the paint path.** `WbshSegmentFn` is called while the
terminal draws, so return something you already have. Do not read a file,
take a lock, or wait on anything — the window is not repainting while you
think. A segment that has to measure something does it on a thread of its
own and leaves an atomic behind for the segment to read; `sdk/samples/procs`
does exactly that, and stops the thread in `wbshUtilUnload`.

**Names are checked.** Empty, over 64 bytes, or carrying a space, a slash
or a control byte is refused, and a command may not take the name of a
bundled one: a DLL in a folder does not get to quietly become `ls`. Two
utils with the same name do not load twice either.

## A command, in full

```c
static void complete(void* user, int argc, const char* const* argv,
                     WbshCompletion* completion) {
    if (argc == 2) wbsh->complete_add(completion, "--verbose");
}

WbshCommand command;
memset(&command, 0, sizeof(command));
command.size     = sizeof(command);
command.name     = "greet";
command.summary  = "Says hello.";                /* `help` */
command.usage    = "greet [--verbose] [name]";   /* `help greet` */
command.fn       = greet;
command.complete = complete;                     /* Tab */
wbsh->register_command(&command);
```

`argv[0]` is the command's name and the exit status is the return value,
the way `main` works. The completion callback sees the words typed so far
with the one being completed last; it adds candidates and the host keeps
the ones that match.

A command that runs for a while asks `wbsh->cancelled()` now and then and
returns 130 when it answers 1; that is how Ctrl+C reaches a util that is
not reading the keyboard itself. `wbsh->set_variable("NAME", "value")`
sets a shell variable in the shell that ran the command, and NULL unsets
it.

## Interactive utils

A command that wants the keyboard, not a line of stdin, opens the terminal:

```c
WbshTerminal* term = wbsh->terminal_open();
if (term == NULL) { wbsh->print_error("no terminal\n"); return 2; }

WbshKey key;
while (wbsh->terminal_read_key(term, &key, -1) > 0) {
    if (key.kind == WBSH_KEY_ESCAPE) break;
    if (key.kind == WBSH_KEY_CHAR) wbsh->terminal_print(term, key.text);
}

wbsh->terminal_close(term);
```

Opening it puts the keyboard in raw mode and turns on escape-sequence
processing for what you write back; closing it undoes both. Keys come
decoded: arrows, function keys, Enter, Escape and the rest are a
`WbshKeyKind`, a typed character is `WBSH_KEY_CHAR` with its UTF-8 in
`text`, and Ctrl and Alt are bits in `modifiers`. Ctrl+C is a key like
any other and does not end the process while the terminal is open. A
resize arrives as `WBSH_KEY_RESIZE`; `terminal_size` then has the new
numbers. The timeout is how a util that watches something stays
responsive: `procs` waits a second for a key and takes a fresh snapshot
when none comes.

The terminal is the **console device**, not stdin or stdout. In
`ls | pick | xargs rm` the picker still gets keys and still draws, and
the line it settles on still goes down the pipe through `wbsh->print`.
`wbsh->is_terminal(fd)` says whether stdin, stdout or stderr is the console
or a pipe, which is how a util decides whether to read candidates or to
walk a directory.

Draw with escape sequences. wbshterm runs the shell on a pseudoconsole,
so the same bytes render the same way there and in a plain console
window. The alternate screen is the one thing a pseudoconsole drops:
draw in place and erase what you drew on the way out, the way `pick`
does, rather than expecting a clean slate to come back on its own.

There is no terminal inside wbshterm.exe itself: `terminal_open` returns
NULL in the terminal host, where a status segment is the way to say things.

## What the SDK gives you

Everything is a member of `WbshApi`, documented where it is declared in
`include/wbshsdk.h`: `version`, `register_command`, `register_segment`,
`write_out`, `write_err`, `print`, `print_error`, `working_directory`,
`variable`, `set_variable`, `cancelled`, `complete_add`, and for the
terminal `is_terminal`, `terminal_open`, `terminal_close`, `terminal_size`,
`terminal_write`, `terminal_print`, `terminal_read_key`.

The terminal has no stdout of its own — a pane's output belongs to the
shell running in it — so `print` writes nowhere there. Say things in the
terminal with a segment.

## Compatibility

The ABI is **2**. A util built against ABI 2 keeps loading in every wbsh
release until a bump is announced in [CHANGELOG.md](CHANGELOG.md), which
also records what each number meant. Additions arrive at the end of
`WbshApi` without a bump; a util that wants one checks `api->size` first.

`wbshsdk.dll` carries the repo's own version number, which moves
independently; `api->version()` reports it. Its symbols ship in the SDK
zip as `wbshsdk.pdb`.
