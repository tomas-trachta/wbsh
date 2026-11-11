# A pager on the alternate screen: q leaves it and the shell output is back.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('seq 1 300 | less\r', ' ', 'q', 'echo after-less\r'); Gap = 500; Expect = @('(?m)^after-less\s*$') }
