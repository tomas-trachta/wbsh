# Real-world corpus

`run.py` sources the bash code people actually load into their shells
(frameworks, version managers, completion scripts, prompt helpers) under
wbsh and, as a reference, under another bash, and records how each file
behaves. Snapshots are committed under [`docs/corpus/`](../../docs/corpus/).

```sh
python -I tools/corpus/run.py \
    --shell wbsh=x64/Release/wbsh.exe \
    --shell "git-bash=C:/Program Files/Git/bin/bash.exe" \
    --clone-root C:/scratch/corpus-src \
    --markdown docs/corpus/<date>-local.md --json corpus.json --low-priority
```

| option | effect |
| --- | --- |
| `--shell NAME=PATH` | a shell to run; the first is the one under test, later ones are references |
| `--clone-root DIR` | where the assets are fetched; reused when the pinned commit is already checked out |
| `--filter GLOB` | only assets whose name matches; repeatable |
| `--timeout S` | seconds per run before the process tree is killed (default 20) |
| `--slow S` | a clean run longer than this is `slow` (default 1) |
| `--jobs N` | runs in parallel (default 2) |
| `--markdown FILE`, `--json FILE` | the snapshot, and every run with its stderr |
| `--low-priority` | run below normal priority |

## The manifest

[`manifest.json`](manifest.json) pins every asset to a commit:

```json
{"name": "nvm", "repo": "https://github.com/nvm-sh/nvm", "ref": "v0.41.0",
 "commit": "4e8758a801eea9479b32a904f31093e410ddaba4",
 "entries": [{"name": "nvm.sh", "file": "nvm.sh",
              "setup": ["export NVM_DIR=\"$HOME/.nvm\"; mkdir -p \"$NVM_DIR\""],
              "check": "declare -F nvm >/dev/null && nvm --version >/dev/null"}]}
```

- `commit` is what is fetched (`git fetch --depth 1 origin <commit>` into a
  fresh `git init`); `ref` is the human name. `sparse` limits the checkout
  to some paths (git's own repository is large; only `contrib/completion/`
  is needed).
- An entry is one `file`, or a `glob` list that expands to one entry per
  matching file (every oh-my-bash plugin, every bash-completion completion,
  ...).
- `setup` lines run before the file is sourced; `$ASSET` is the checkout.
  They do what a user's `~/.bashrc` would do (set `OSH`, `BASH_IT`,
  `NVM_DIR`, source the framework before one of its plugins).
- `check` runs after sourcing and must succeed; it proves the file defined
  what it is supposed to (a function, a completion spec), so a shell that
  silently skips the file does not count as clean.
- `modes` is `script`, `interactive` or both (default both). Glob entries
  run interactively only, since that is how plugins are loaded; so does
  oh-my-bash's main file, which returns at once in a non-interactive shell.
- `writable: true` gives every run a private copy of the checkout. bash-it
  creates symlinks under `enabled/` and oh-my-bash writes a cache inside its
  own tree; without the copy one run would change what the next one sees.

`generated` assets are init scripts printed by tools (`starship init bash`,
`zoxide init bash`, `direnv hook bash`, `fzf --bash`). They are included
only when the tool is on `PATH`; otherwise the runner says it skipped them.

To add an asset, resolve the commit of a release tag with
`git ls-remote <repo> 'refs/tags/<tag>^{}'` (or `refs/tags/<tag>` for a
lightweight tag) and add a block; keep checks cheap and network-free.

## How a run works

Each run gets a fresh working directory with an empty `HOME`, stdin closed,
`TERM=xterm-256color`, `LANG=C.UTF-8`, `PS1`/`PS2` empty, and `WBSH_*`,
`BASH_ENV`, `PROMPT_COMMAND` and the frameworks' own variables removed. The
runner writes a small driver script there (the setup lines, `. <file>`,
the check, all inside one `{ ... }` group) and runs it:

- `script` mode: `<shell> driver.sh`.
- `interactive` mode: `<shell> --norc --noediting -i < driver.sh`, an
  interactive shell reading the driver from a pipe, which is how a
  `~/.bashrc` is executed. bash's own notices for that situation
  (`cannot set terminal process group`, `no job control in this shell`,
  `bind: warning: line editing not enabled`, the `exit` echo) are removed
  from stderr before judging. The `{ ... }` group matters here: bash prints
  its prompt to stderr before reading each line, so once a theme has set
  `PS1`, a driver spread over several commands would fill stderr with
  prompts.

Nothing in the cloned repositories is executed except the files sourced
this way: no installers, no `make`, no hooks.

| result | meaning |
| --- | --- |
| `ok` | exit status 0, nothing on stderr, at most `--slow` seconds |
| `slow` | like ok, but slower than `--slow` |
| `fails` | non-zero exit status (including a failed `check`, status 97) or anything on stderr |
| `hangs` | killed at `--timeout` |

"Sourced cleanly" in the snapshot is ok + slow. A file that fails under the
reference bash too (for example because it needs a Linux tool) is shown side
by side, so wbsh is judged against what bash does on the same machine, not
against a perfect score.

Paths in the recorded stderr are shortened to `$ASSET` (the checkout or its
private copy) and `$WORK` (the run's directory), so snapshots carry no
machine-specific paths.
