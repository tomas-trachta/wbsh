#!/bin/bash
# shopt lists options as bash 5.3 does: names padded to 20 columns, and
# to 15 for `shopt -o` with no names (set -o's format); an unknown option
# letter is an invalid option with the usage line.

exec 2>&1
shopt -s extglob
shopt extglob nullglob
shopt -o noglob
shopt -o | grep -E '^(noglob|xtrace) '
shopt -z
echo "status $?"
