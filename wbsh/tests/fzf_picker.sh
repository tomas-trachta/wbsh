#!/bin/bash
# The fzf picker driven by `__fzf`, which types keys into it without a
# console: `accept [LINE]` or `abort`, and with -o what it drew.

echo "=== the selection scrolls past the rows on screen"
seq 100 | __fzf '<DOWN>' '<DOWN>' '<ENTER>'
seq 100 | __fzf $(for i in $(seq 20); do printf '<DOWN> '; done) '<ENTER>'
seq 100 | __fzf -r 5 '<PGDN>' '<PGDN>' '<ENTER>'
seq 100 | __fzf -r 5 '<PGDN>' '<PGDN>' '<PGUP>' '<UP>' '<ENTER>'
seq 100 | __fzf $(for i in $(seq 150); do printf '<DOWN> '; done) '<ENTER>'
seq 5 | __fzf '<UP>' '<ENTER>'

echo "=== the list window follows the selection"
seq 10 | __fzf -o -r 3 '<DOWN>' '<DOWN>' '<DOWN>' '<ENTER>' | tail -n 6

echo "=== rows wider than the screen are cut, so nothing wraps"
printf '%0200d\n' 1 2 3 | __fzf -o -c 30 '3' '<ENTER>'
printf 'žluťoučký kůň úpěl ďábelské ódy\n' | __fzf -o -c 20 'k' '<ENTER>'
printf '日本語のファイル名.txt\n' | __fzf -o -c 12 '<ENTER>'

echo "=== a long query shows its end"
printf 'abcdefghijklmnopqrstuvwxyz\n' | __fzf -o -c 12 'abcdefghijklm' '<ENTER>' | tail -n 6

echo "=== control characters take a cell instead of moving the cursor"
printf 'a\tb\n' | __fzf -o '<ENTER>'

echo "=== typing narrows, Backspace and Ctrl-U widen again"
printf 'apple\nbanana\ncherry\napricot\n' | __fzf 'ap' '<ENTER>'
printf 'apple\nbanana\ncherry\napricot\n' | __fzf 'apr' '<BS>' '<BS>' '<DOWN>' '<ENTER>'
printf 'apple\nbanana\ncherry\napricot\n' | __fzf 'zz' '<C-u>' 'ch' '<ENTER>'
printf 'apple\nbanana\n' | __fzf 'zz' '<ENTER>'
printf 'apple\nbanana\n' | __fzf '<ESC>'

echo "=== non-ASCII queries"
printf 'kůň\nkun\n' | __fzf 'ů' '<ENTER>'
printf 'a\n😀 smile\n' | __fzf '😀' '<ENTER>'

echo "=== the directory walk skips .git and node_modules"
dir=$(mktemp -d)
mkdir -p "$dir/src" "$dir/.git/objects" "$dir/node_modules/pkg" "$dir/web/node_modules/x"
touch "$dir/src/main.c" "$dir/.git/HEAD" "$dir/node_modules/pkg/index.js" "$dir/README"
touch "$dir/web/node_modules/x/y.js" "$dir/web/app.js"
__fzf -w "$dir" -o '<ENTER>' | tail -n 9
__fzf -w "$dir" 'main' '<ENTER>'
__fzf -w "$dir" 'index' '<ENTER>'
rm -rf "$dir"

echo "=== empty input still ends at once"
printf '' | fzf
echo "rc=$?"
