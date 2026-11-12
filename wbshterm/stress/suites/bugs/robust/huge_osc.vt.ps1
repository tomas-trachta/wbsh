# A 20 MB window title and a 20 MB unterminated OSC: the terminal must cap what it keeps.
# @stress size=80x24 maxms=10000 maxmem=300
param([int]$Scale = 1)
$e = [char]27
"$e]2;" + ('T' * (20MB * $Scale)) + [char]7 + "after title`r`n$e]0;" + ('U' * (20MB * $Scale))
