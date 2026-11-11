# A CSI with a million parameters and an SGR with a million sub-parameters.
# @stress size=80x24 maxms=5000 maxmem=200
param([int]$Scale = 1)
$e = [char]27
"$e[" + ('1;' * (1000000 * $Scale)) + "m" + "$e[38:2:" + ('1:' * (1000000 * $Scale)) + "mtext"
