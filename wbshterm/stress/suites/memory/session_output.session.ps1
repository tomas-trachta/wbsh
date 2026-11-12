# A live session that prints ten times more lines: wbshterm's memory must stay flat
# (scrollback beyond the cap goes to the disk store or away).
# @stress tolerance=16
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @("seq 1 $(50000 * $Scale)\r"); Delay = 600; Settle = 1000; Size = '120x40'; Timeout = 300 }
