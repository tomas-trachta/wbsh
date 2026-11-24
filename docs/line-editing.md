# Line editing

The interactive line editor of wbsh: history, predictions, completion,
highlighting and key bindings.

- **History:** persistent, with reverse-incremental search.
- **Predictions:** the rest of the best matching history entry appears as dim
  ghost text. Entries whose last run failed are never suggested.
- **Completion:** programmable, plus per-tool completion for git, docker, npm,
  cargo and kubectl.
- **Kill ring:** kills are kept, and yanking cycles through them.
- **Syntax highlighting:** the line is coloured as you type (see below).

## Syntax highlighting

Command words are green when they name a builtin, function, alias or program
on `PATH`, and red when they name nothing. Keywords, strings, expansions,
comments and options get colours of their own, and paths that exist are
underlined. Lines that continue an unfinished command are coloured in context.
Command lookups come from memory (`PATH` is listed once, off the typing thread)
and paths on network drives are never probed, so a key costs about 20 µs more.

Highlighting is on by default and off under `NO_COLOR` or `TERM=dumb`. In
`~/.inputrc`:

```
set syntax-highlight off
set syntax-highlight-colors command=1;32:unknown=31:keyword=35
```

`WBSH_HIGHLIGHT` takes the same `role=SGR` list and wins over `.inputrc`. The
roles are `command` (32), `unknown` (31), `keyword` (35), `string` (33),
`variable` (36), `comment` (90), `operator` (none), `option` (94) and `path` (4).
`0` turns a role's colour off. The colours are the 16 ANSI ones, so wbshterm
draws them in its theme's palette.

## Keys

Bindings follow readline's emacs mode. `Esc` followed by a key works as Alt
with it, and AltGr characters type as usual. For vi keys, use `set -o vi` (or
`set editing-mode vi` in `~/.inputrc`). `set -o emacs` sets the emacs keys
again.

**Moving**

| Keys | Action |
|------|--------|
| `← / →`, `Ctrl-B / Ctrl-F`           | move by one character |
| `Alt-B / Alt-F`, `Ctrl-← / Ctrl-→`   | move by one word |
| `Home / End`, `Ctrl-A / Ctrl-E`      | start / end of line |

**Completion and history**

| Keys | Action |
|------|--------|
| `Tab`                                | complete; second `Tab` lists candidates |
| `→` at end of line                   | accept the inline prediction |
| `↑ / ↓`, `Ctrl-P / Ctrl-N`           | walk history |
| `Ctrl-R` / `Ctrl-S`                  | search history backward / forward |
| `Esc` / `Ctrl-G`                     | cancel the search |
| `Alt-.` / `Alt-_`                    | insert the last word of the previous command (repeat: older ones) |

**Editing**

| Keys | Action |
|------|--------|
| `Ctrl-U / Ctrl-K`                    | kill to start / end of line |
| `Ctrl-W`                             | kill the previous whitespace-separated word |
| `Alt-Backspace / Alt-D`, `Ctrl-Del`  | kill the previous / next word |
| `Ctrl-Y`, `Alt-Y`                    | yank the last kill, then cycle older kills |
| `Ctrl-T` / `Alt-T`                   | swap characters / words |
| `Alt-U / Alt-L / Alt-C`              | upcase / downcase / capitalize the word |
| `Ctrl-_`, `Ctrl-X Ctrl-U` / `Alt-R`  | undo / undo every change to the line |
| `Ctrl-V`                             | paste from the clipboard |

**Everything else**

| Keys | Action |
|------|--------|
| `Ctrl-X Ctrl-E`                      | edit the line in `$VISUAL` / `$EDITOR` (default notepad), then run it |
| `Ctrl-O`                             | run the line, then show the next history entry |
| `Alt-#`                              | comment out the line and keep it in history |
| `Ctrl-L`                             | clear the screen |
| `Ctrl-C`                             | abandon the line |
| `Ctrl-D` (empty line)                | exit |

## Completing file names

- Names are quoted the way the word was typed (`"My Doc` → `"My Documents/`) or
  get backslashes (`My\ Documents/`).
- The value after `=` completes on its own (`--file=./sr`).
- Names match regardless of case. Put `set completion-ignore-case off` in
  `~/.inputrc` to change that.

## Changing bindings

The `bind` builtin changes them for the session:

- `bind -p` lists the bindings.
- `bind -l` lists the commands.
- `bind -x` runs a shell command with `READLINE_LINE` / `READLINE_POINT`.

`~/.inputrc` (or `$INPUTRC`) is read before `.wbshrc` and takes readline's
syntax, including `$if mode=emacs` ... `$endif` and `$include`:

```
"\C-t": transpose-chars
Meta-q: upcase-word
"\ez": "macro text"
set bell-style none
```
