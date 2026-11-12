# DEC special graphics through the pseudoconsole: ncurses, dialog and mc draw
# boxes with ESC ( 0 when they do not trust UTF-8.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('printf "\033(0lqqk\033(B\n"\r'); Expect = @("(?m)^$([char]0x250C)$([char]0x2500)$([char]0x2500)$([char]0x2510)\s*$") }
