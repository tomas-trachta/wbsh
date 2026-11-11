# Requests to resize the window (DECSLPP, DECCOLM, CSI 8 t) to absurd sizes.
# @stress size=80x24 maxms=5000 maxmem=200
param([int]$Scale = 1)
$e = [char]27
("$e[8;99999;99999t$e[8;0;0t$e[?3h$e[?3l$e[999999t$e[?40h$e[?3hxx" * (100 * $Scale))
