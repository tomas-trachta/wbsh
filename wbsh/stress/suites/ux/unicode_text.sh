#!/bin/bash
# UTF-8 text, file names and widths: what non-English users meet first.
. "$STRESS_ROOT/lib/check.sh"

severity high
expect_eq "character count of UTF-8 text" "$(printf 'čřž€😀' | wc -m)" 5
x='Příliš žluťoučký kůň'
expect_eq "\${#x} counts characters" "${#x}" 20
expect_eq "substring by characters" "${x:7:9}" "žluťoučký"
expect_eq "upper-case of non-ASCII" "${x^^}" "PŘÍLIŠ ŽLUŤOUČKÝ KŮŇ"
: > 'soubor-čřž.txt'
: > '😀 emoji.txt'
expect_eq "glob finds non-ASCII file names" "$(ls *čřž* 2>&1)" "soubor-čřž.txt"
expect_eq "file name with emoji survives" "$(echo 😀*)" "😀 emoji.txt"
printf 'ž\na\nč\n' > u.txt
expect_eq "sort handles UTF-8 bytes" "$(sort u.txt | head -n 1)" "a"
expect_eq "tr on UTF-8" "$(echo čau | tr a A)" "čAu"
expect_eq "sed on UTF-8" "$(echo žluť | sed 's/./X/')" "Xluť"
expect_eq "grep -o on UTF-8" "$(echo kůň | grep -o 'ů.')" "ůň"

severity medium
expect_eq "awk length() counts characters, as wc -m does" "$(echo žluť | awk '{ print length($0) }')" 4
out=$(cmd /c 'echo čřž' 2>/dev/null | tr -d '\r')
case "$out" in
    *čřž*) ok "non-ASCII output of cmd.exe arrives as UTF-8" ;;
    *) review "non-ASCII output of cmd.exe arrives in the OEM code page (Git Bash does the same): got '$out'" ;;
esac
finish
