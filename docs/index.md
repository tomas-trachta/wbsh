# wbsh documentation

wbsh is a Bash-compatible shell for Windows. The project lives at
[github.com/tomas-trachta/wbsh](https://github.com/tomas-trachta/wbsh); this
site is built from its `docs/` directory.

## Evidence

- [Compatibility ledger](compat-ledger.md): GNU bash 5.3's own test suites
  under wbsh, suite by suite.
- [Real-world corpus](corpus/2026-11-13-local.md): oh-my-bash, bash-it, bash-completion, nvm,
  sdkman, asdf and git's completion scripts sourced under wbsh and Git Bash.
- [Benchmarks](bench/2026-11-12-local.md): same-machine timings against Git Bash.
- [Comparison](comparison.md): wbsh, Git Bash, MSYS2 and WSL, every
  cell sourced and dated.

## Using wbsh

- [Differences from POSIX environments](posix-differences.md): no fork,
  signals, permissions, file system, terminals
- [Command reference](commands.md)
- [Running wbsh under coding agents](agents.md)
- [wbsh in editors and terminals](editors.md): VS Code, Cursor, JetBrains, Windows Terminal, shims, `SHELL`
- [Code signing](signing.md)
- [Stress testing](stress-testing.md) and [stress testing wbshterm](stress-testing-wbshterm.md)
- [C/C++ API reference (Doxygen)](html/index.html)
