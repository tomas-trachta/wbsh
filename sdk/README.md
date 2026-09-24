# wbsh SDK

Write a util, drop in the DLL, restart. Nothing gets rebuilt.

A util is an ordinary Windows DLL that exports three C functions. It can add
commands to the shell, features to the terminal, or both from the same file.

## The shortest useful util

```c
#include "wbshsdk.h"

static const WbshUtilInfo kInfo = {
    WBSH_SDK_ABI, "greet", "1.0.0", "Says hello."
};

static int greet(void* user, int argc, const char* const* argv) {
    (void)user; (void)argc; (void)argv;
    wbshPrint("hello\n");
    return 0;
}

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) { return &kInfo; }
WBSH_UTIL_API void wbshUtilUnload(void) { }

WBSH_UTIL_API int wbshUtilLoad(WbshHostKind host) {
    if (host == WBSH_HOST_SHELL) wbshRegisterCommand("greet", greet, NULL);
    return WBSH_OK;
}
```

Include `sdk/include`, link `wbshsdk.lib`, build a DLL. `sdk/samples/hello`
is a working project that does this and adds a status segment too; copying
it is the fastest start.

## Installing one

Drop the DLL in either folder. Both are scanned, the first one first:

| Folder | For |
| --- | --- |
| `<install>\plugins\` | Utils that ship with an install |
| `%APPDATA%\wbsh\plugins\` | A user's own, no administrator needed |

`utils` lists what loaded. A DLL that will not load says why on stderr and
is skipped, so one broken util never costs you the others.

## Two hosts, one DLL

| Host | Is | Offers |
| --- | --- | --- |
| `WBSH_HOST_SHELL` | wbsh.exe | Commands, stdout/stderr, shell variables |
| `WBSH_HOST_TERMINAL` | wbshterm.exe | Status-bar segments |

The hosts are **separate processes**. When wbshterm runs a shell, your DLL
is loaded twice — once in each — so `wbshUtilLoad` runs twice with a
different `host` each time, and each copy has its own globals. A counter
bumped by your command does not move the segment your terminal is drawing;
if the two halves need to agree, they have to agree through something
outside the process.

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
changes when something already in the header changes shape.

**Strings are UTF-8.** Anything the host hands you is valid until your
function returns; copy it if you want to keep it.

**A segment is on the paint path.** `WbshSegmentFn` is called while the
terminal draws, so return something you already have. Do not read a file,
take a lock, or wait on anything — the window is not repainting while you
think.

**Names are checked.** Empty, over 64 bytes, or carrying a space, a slash
or a control byte is refused, and a command may not take the name of a
bundled one: a DLL in a folder does not get to quietly become `ls`.

## What the SDK gives you

`wbshHost`, `wbshSdkVersion`, `wbshRegisterCommand`,
`wbshRegisterStatusSegment`, `wbshWriteOut`, `wbshWriteErr`, `wbshPrint`,
`wbshPrintError`, `wbshWorkingDirectory`, `wbshVariable`. Every one is
documented where it is declared, in `include/wbshsdk.h`.

The terminal has no stdout of its own — a pane's output belongs to the
shell running in it — so `wbshPrint` writes nowhere there. Say things in
the terminal with a segment.

## Version

The ABI is **1**. `wbshsdk.dll` carries the repo's own version number,
which moves independently; `wbshSdkVersion()` reports it.
