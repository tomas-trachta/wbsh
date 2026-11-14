# Prompts, completion and shell integration

What wbsh does at the prompt beyond reading a line: prompt frameworks,
completion menus and scripts, and the marks it leaves for the terminal.

## Prompt frameworks

wbsh's own prompt, with `\g` for the git branch and status, stays the fast
default: it needs no process per prompt. Frameworks that print the prompt
from a program of their own load the way they do in bash, from `~/.wbshrc`:

```bash
eval "$(starship init bash)"
eval "$(oh-my-posh init bash --config ~/mytheme.omp.json)"
```

They rely on these, all of which wbsh has:

| Feature | Used by |
| --- | --- |
| `PROMPT_COMMAND` as a string or an array, `PROMPT_COMMAND+=(...)` | both |
| `trap '...' DEBUG` with `$_`, `trap -p DEBUG` | starship (preexec) |
| `PS0`, expanded after a command is read and before it runs | oh-my-posh (timer) |
| `$?` and `${PIPESTATUS[@]}` at the start of `PROMPT_COMMAND` | both |
| `\[ \]` around escapes in `PS1`, `PS2` | both |
| `${var@P}`, `shopt -u/-s promptvars`, `trap ... RETURN` | oh-my-posh |
| `jobs -p`, `$RANDOM`, `${var:0:16}`, `shopt -s checkwinsize` | starship |

Checked on 2026-11-14 with stand-ins that reproduce each framework's
`init bash` script line for line, the binary replaced by a shell function
(neither program was installed on the test machine), run under `wbsh -i`
and under bash 4.4 for comparison:

- **starship**: the prompt renders with status, pipe status, job count and
  command duration, `PS2` continues multi-line commands, a `PROMPT_COMMAND`
  set before it keeps running, and `PROMPT_COMMAND+=(...)` after it adds to
  it.
- **oh-my-posh**: the prompt renders, but three gaps remain in the shell
  itself: `shopt -oq posix` is rejected (an error line before every prompt),
  double quotes inside `$(( ))` in its `PS0` make the arithmetic fail (no
  execution time is shown), and `${#DIRSTACK[@]}` is 0 where bash has 1
  (the stack count shows -1).

## Completion

Tab completes as in bash: a first Tab inserts what all matches share, a
second lists them. readline's variables, in `~/.inputrc` or with `bind`,
change that:

| Setting | Effect |
| --- | --- |
| `set show-all-if-ambiguous on` | list on the first Tab |
| `set menu-select on` | (wbsh) an ambiguous Tab opens a menu instead of a list |
| `set menu-complete-display-prefix on` | the menu, and `menu-complete`, start on the common prefix with nothing chosen |
| `"\e[Z": menu-select` | a key that opens the menu whatever `menu-select` says |
| `"\C-n": menu-complete` | readline's cycling through matches in place |

The menu is a grid below the line, the chosen match standing in the line:
Tab and Down go to the next match, Shift-Tab and Up to the previous, Left and
Right move a column, PageUp and PageDown a screenful. Enter takes the match
as a finished word, Esc puts back the word as typed, and any other key keeps
the match and then does its usual job. A grid taller than the window
scrolls, wide (CJK) names keep their columns, and it works the same in vi
insert mode. Command names carry the one-line summary from their manual;
a completion function may describe its own matches by answering
`word<TAB>description` in `COMPREPLY` (a wbsh extension; the word alone is
inserted).

Programmable completion follows bash 5.3: `complete`, `compgen` (with
`-V varname`) and `compopt`; `-o nospace`, `filenames`, `noquote`,
`nosort`, `default`, `bashdefault`, `dirnames` and `plusdirs`; `-D`, `-E`
and `-I` specs, with a `-D` function that returns 124 after defining a
command's spec getting that spec used at once (bash-completion's lazy
loading); and `COMP_WORDS`, `COMP_CWORD`, `COMP_LINE`, `COMP_POINT`,
`COMP_TYPE`, `COMP_KEY` and `COMP_WORDBREAKS`. `COMP_WORDS` is cut at
`COMP_WORDBREAKS` as bash cuts it (`--opt=val` is `--opt`, `=`, `val`), and
a reply replaces the text after the last break character. The default
`COMP_WORDBREAKS` leaves out bash's `:` and `@`, which Windows paths and
`user@host` contain; scripts that read the variable see that.

git's own `git-completion.bash` (as Git for Windows ships it) works
unchanged. The bash-completion project (checked with 2.16.0) does not load
yet: it needs four shell fixes outside the completion code — a line break
inside `[[ ... ]]` after `&&`, a `{` inside a bracket expression in
`${var%%[...]}`, `${arr[@]+"${arr[@]}"}` expanding to separate words, and
`unset` of a caller's local revealing the variable one scope further out
(the "upvar" idiom every one of its helpers uses).

## Shell integration

An interactive wbsh on a terminal that takes escape sequences marks every
prompt and command with OSC 133 — `A` where the prompt starts, `B` where it
ends, `C` where the output starts, `D;status` when the command is done —
sends the command line as VS Code's `OSC 633;E`, and reports the working
directory with OSC 7 before each prompt. An empty line or a line that only
continues a command sends no `C` or `D`.

Windows Terminal and VS Code use the marks to jump between commands and
show their status; wbshterm also copies or re-runs a command, shows its
status and time beside the prompt, opens Ctrl+clicked paths from the shell's
directory, starts split panes there, and can reopen its panes where they
were (see its README, "What the shell tells the terminal").
