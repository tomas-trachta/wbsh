# read -t must time out when nothing is typed (interactive wbsh on a pseudoconsole).
# Scripts that ask "continue? [y/N]" with a timeout hang otherwise.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('read -t 1 x; echo "read-rc=$?"\r'); Settle = 3000; Severity = 'high'
	Expect = @('(?m)^read-rc=\d+\s*$') }
