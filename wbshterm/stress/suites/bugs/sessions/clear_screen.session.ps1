# clear empties the visible grid.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('seq 1 60\r', 'clear\r', 'echo after-clear\r'); Gap = 500
	Expect = @('(?m)^after-clear\s*$'); Absent = @('(?m)^5[0-9]\s*$') }
