# Long DCS, APC, PM and SOS strings (sixel-like payloads, tmux passthrough).
# @stress size=80x24 maxms=10000 maxmem=300
param([int]$Scale = 1)
$e = [char]27
$body = '#0;2;0;0;0#1;2;100;100;0' + ('~-' * (2MB * $Scale))
"$e" + "Pq$body$e\" + "$e" + "_$('A' * (5MB * $Scale))$e\" + "$e" + "^$('B' * (5MB * $Scale))$e\" + "$e" + "X$('C' * (5MB * $Scale))$e\" + "visible"
