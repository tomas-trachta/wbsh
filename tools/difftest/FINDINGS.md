# Difftest findings: the compatibility backlog

Divergences between wbsh and GNU bash found with the differential corpus,
with the corpus case that covers each. Last local run, 2026-11-14, against Git
Bash 4.4.23 as the reference: **432 / 436 pass** (31 more cases need bash 5.x
and are judged in CI against bash 5.3). The first run on 2026-11-12 passed 363.

## Still open

1. **`globstar` is on by default in wbsh** (a deliberate choice), so a script
   that relies on bash's default of off sees `**` recurse.
   (glob/globstar_off)
2. Cases that differ only because bash 4.4 is older than the bash 5.3 wbsh
   follows; CI's bash 5.3 reference should accept them:
   - `declare -p` prints control characters as `$'..'`
     (builtins/declare_p_special_chars).
   - `globskipdots` (glob/dot_patterns).
   - An `eval` syntax error returns 2 (builtins/eval_syntax_error).

Cases whose expected output changed in bash 5.2 / 5.3 carry a `requires:`
line, so a 4.4 reference skips them: `unset 'a[@]'` empties the array
(arrays/array_unset_whole_by_star, 5.2), arithmetic errors say "arithmetic
syntax error" (arith/syntax_error_in_arith, 5.3) and `${u?}` says "parameter
not set" (paramexp/error_if_unset, 5.3).

## Known gaps outside the corpus

- `$LINENO` inside a multi-line `$( )` is one lower than bash's.
- A procsubst path (`\\.\pipe\...`) handed to `eval` loses its backslashes
  (bash passes `/dev/fd/63`).
- `echo a | nosuchcmd` can give PIPESTATUS `141 127` where bash usually
  gives `0 127`: the first element runs in this shell after the later ones
  are started, so a reader that fails to start may be gone before it
  writes. bash has the same race but its writer is almost always first.
- `TZ` is ignored: `%(...)T`, `date` and friends always use the system
  time zone.

## Fixed on 2026-11-14

- `@` and `*` as associative-array keys: `m[*]=x` and `m[@]=x` fail with
  "bad array subscript" and drop the rest of the line; `${m[$k]}` with
  `k='*'` looks up the literal key; `declare -p` quotes `["@"]`; `unset
  'm[$k]'` expands the key, and bash 5.2+'s `unset 'm[@]'` removes only the
  key `@` (arrays/assoc_special_keys, arrays/assoc_assign_at_star_rejected,
  arrays/assoc_lookup_key_from_variable, arrays/unset_assoc_key_from_variable,
  arrays/unset_assoc_at_key, arrays/indexed_assign_at_rejected).
- A readonly variable in `(( ))`, `let` and `$(( ))` fails the expression as
  bash does (arith/readonly_assign_fails, arith/readonly_assign_in_expansion).
- `R=8 true 2>/dev/null` reports the readonly error before the redirection and
  runs the command without the assignment (redirect/readonly_prefix_before_redirect).
- `a[-9]=z` with too few elements: status 1 (it already was; the entry was stale).
- `GLOBIGNORE` drives the `dotglob` shopt like bash: a pattern turns it on,
  `GLOBIGNORE=` keeps it, `unset GLOBIGNORE` turns it off
  (glob/globignore_empty_keeps_dotglob).
- `[[ 1x -eq 1 ]]` prints bash's `[[:` arithmetic message
  (builtins/dbracket_arith_error_message).
- Line numbers in multi-line `eval` strings count from the line the `eval`
  command ends on, as bash does (builtins/eval_multiline_line_numbers).
- `kill` errors carry the `SCRIPT: line N:` prefix (builtins/kill_error_has_line).
- Parser messages use bash 5.3's wording: `syntax error: unexpected end of file
  from \`while' command on line 2`, `syntax error near unexpected token`,
  `unexpected EOF while looking for matching \`"'` (syntax/*).
- An unterminated here-document in a pipeline stage is warned about
  (heredoc/unterminated_in_pipeline).
- `lastpipe` in an interactive shell: bash applies it only with job control
  off, and so does wbsh (`set +m`); not a bug.
- `$-` in a pipeline stage of a `-c` shell carries the `c`.
- Dynamic variables (`LINENO`, `RANDOM`, `EPOCHSECONDS`, ...) read as 0 inside
  arithmetic (arith/dynamic_vars_in_arith).
- `declare -a c` with no value prints as `declare -a c`, as bash 5.3 does
  (`c=()` still prints `declare -a c=()`).

## Fixed on 2026-11-12

`set -e` leaking into `$( )` (inherit_errexit); aliases in scripts,
`\name`, trailing-space chaining, `alias -p`; `;&` / `;;&` fall-through;
readonly assignment errors; `lastpipe`; globs with directory components and
trailing `/`; `failglob`, `GLOBIGNORE`, `shopt -u globstar`, sorted globstar
results, `nocasematch` in `case`; negative array indices, `s+=(..)`,
`unset 'a[@]'`, spaced subscripts, `read` into `m[k]`; `mapfile -d`;
`${a[@]/#/x}` and `${a[@]/%/x}`; bash's `SCRIPT: line N:` error prefixes,
arithmetic error spacing, `cd`/`source`/`getopts` messages; unterminated
here-documents; `$'..'` inside double quotes, `\u`/`\U`, `printf %q`,
`xpg_echo`, invalid printf conversions; `shopt -p`, `umask -S`, `$_`,
`DIRSTACK`, `caller`, `local -`, `declare -f` / `type` formatting, `eval`
syntax errors; `$-` carrying `c` / `s`.
