# wbsh SDK changelog

The SDK's ABI number and the wbsh version move independently. A util is
tied to the ABI number alone: every wbsh release keeps loading utils built
against the current ABI until a bump is announced here, one release ahead
where at all possible.

## ABI 2 — wbsh 1.0.25

Utils are no longer linked against `wbshsdk.lib`. The host hands
`wbshUtilLoad` a `WbshApi` table and the util calls through it, so a DLL
from any compiler loads, and the SDK DLL only has to be beside the host.

- `wbshUtilLoad(WbshHostKind)` became `wbshUtilLoad(const WbshApi*)`; the
  host is `api->host`.
- Every `wbshX()` function became the member `api->x`: `wbshPrint` is
  `api->print`, `wbshTerminalOpen` is `api->terminal_open`, and so on.
- `register_command` takes a `WbshCommand` with `summary`, `usage` and a
  Tab-completion callback, instead of a name and a function.
- New: `set_variable`, `cancelled`, `complete_add`, `version`.
- New on the host side: `wbshSdkLoadFile`, `wbshSdkPathAt`; refusals name
  the reason (no describe, no load, ABI mismatch with both numbers, bad
  name, name already loaded, load returned non-zero).
- The shell gained `utils load <dll>`, `utils reload`, the **not loaded**
  section of `utils`, `help` for util commands, Tab completion through the
  util, and the `WBSH_PLUGINS` folder list. The terminal reads
  `WBSH_PLUGINS` too.
- The SDK ships as `wbsh-sdk-<version>.zip` with the header, a property
  sheet, a template project for MSBuild and CMake, the samples, and
  `wbshsdk.pdb`.

Moving a util from ABI 1: keep the `WbshApi*` from `wbshUtilLoad` in a
global, replace each `wbshX(` with `api->x(`, and build the `WbshCommand`
for each command. `sdk/samples/hello` shows every piece.

## ABI 1 — wbsh 1.0.16

The first published contract: `wbshUtilDescribe`, `wbshUtilLoad`,
`wbshUtilUnload`; commands, status segments, stdout/stderr, working
directory, variables, and the terminal helpers, all linked from
`wbshsdk.lib`.
