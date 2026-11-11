# read -t must time out when nothing is typed (interactive wbsh on a pseudoconsole),
# and still take what is typed in time. Scripts that ask "continue? [y/N]" with a
# timeout hung before: the console handle is signalled by any input record.
param([string]$WorkDir, [int]$Scale = 1)
@{
	Feed = @('read -t 1 x; echo "read-rc=$?"\r', 'read -t 8 y; echo "got=[$y]"\r', 'typed in time\r')
	Delay = 1500; Gap = 3000; Settle = 2500; Timeout = 60; Severity = 'high'
	Expect = @('(?m)^read-rc=142\s*$', '(?m)^got=\[typed in time\]\s*$')
}
