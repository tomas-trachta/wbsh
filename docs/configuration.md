# Configuration

Startup files, the variables wbsh reads, and the prompt.

## Startup files

`~/.wbshrc` is sourced at the start of every interactive session.
A login shell (`wbsh -l`, `wbsh -lc <command>`) first sources
`~/.wbsh_profile` and sets `shopt login_shell`. wbsh does not read
`~/.bash_profile` or `~/.profile`, because those files are written for
Git Bash or Linux. Unlike bash, an interactive login shell also reads
`~/.wbshrc` after the profile.

An example `~/.wbshrc`:

```sh
export EDITOR=code
alias ll='ls -lah'
alias gs='git status'
PS1='\[\e[35;1m\]\w\[\e[0m\]\g \$ '
```

## Variables

| Variable | Effect |
| --- | --- |
| `PS1`, `PS2` | Primary and continuation prompts. |
| `PATH` | Stored in POSIX form, auto-translated for spawns. |
| `HOME` | Defaults to `%USERPROFILE%` in POSIX form. |
| `HISTFILE` | History file, default `$HOME/.wbsh_history`. Each command is appended as it finishes. Empty: not saved. |
| `HISTSIZE`, `HISTFILESIZE` | Entries kept in memory / in the file. Default 5000; negative: no limit. |
| `HISTCONTROL` | `ignorespace`, `ignoredups`, `ignoreboth`, `erasedups`. |
| `HISTIGNORE` | Colon-separated patterns left out of history (`&` = previous entry). |
| `COLUMNS`, `LINES` | Updated on resize; fires the `WINCH` trap. |
| `WBSH_GIT_NO_DIRTY` | Skip the working-tree check in the `\g` prompt, for huge repos. |
| `WBSH_GIT_STATUS_WAIT` | Milliseconds the prompt waits for `\g`'s background `git status`, default 50. After that it shows the last known state and redraws when the new one arrives. |
| `WBSH_NO_PATHCONV`, `MSYS_NO_PATHCONV` | Suppress POSIX→Win32 argument translation for spawned commands. |
| `MSYS2_ARG_CONV_EXCL` | `*`, or `;`-separated prefixes of arguments to pass on untranslated, as in Git Bash. See [argument translation](commands.md#arguments-for-native-programs). |
| `MSYSTEM` | Exported as `MINGW64` when not set; picks the `uname -s` prefix. |
| `WBSH_UNAME_S` | Replaces what `uname -s` prints (default `MINGW64_NT-10.0-<build>`). |
| `WBSH_PAGER` | `builtin` makes `less` use the bundled pager even when a `less` is on PATH. |

## Prompt escapes

| Escape | Meaning |
| --- | --- |
| `\u` | user |
| `\h`, `\H` | short / long hostname |
| `\w`, `\W` | working directory / its basename |
| `\g` | ` (branch)` inside a git repo; ` (branch \| state)` during a merge, rebase, cherry-pick, revert or bisect, or when the tree has unstaged, staged or untracked changes |
| `\d` | date (`Tue May 26`) |
| `\t`, `\T`, `\@`, `\A` | time: 24-hour, 12-hour, 12-hour am/pm, 24-hour `HH:MM` |
| `\D{format}` | `strftime` format |
| `\s` | `wbsh` |
| `\v`, `\V` | version: short / long |
| `\j` | number of jobs |
| `\!`, `\#` | history number / command number |
| `\l` | terminal name |
| `\n` `\r` `\a` `\e` `\\` `\$` `\nnn` | the usual characters; `\nnn` is octal |
| `\[`, `\]` | non-printing markers |

Default `PS1`:

```
\[\e[32;1m\]\u@\h\[\e[0m\] \[\e[36;1m\]\w\[\e[0m\]\g\$
```

[prompts.md](prompts.md) covers prompt frameworks, completion menus and shell integration.
