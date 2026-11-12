# Ctrl+C reaches a foreground command through the pseudoconsole and the prompt
# comes back: a builtin (sleep) and an external program (ping).
param([string]$WorkDir, [int]$Scale = 1)
@{
	Feed = @('sleep 30\r', '\x03', 'echo after-sleep-$((2+2))\r', 'ping -n 30 127.0.0.1 > /dev/null\r', '\x03',
		'echo after-ping-$((3+3))\r')
	Gap = 800; Settle = 1500; Timeout = 40; Severity = 'high'
	Expect = @('(?m)^after-sleep-4\s*$', '(?m)^after-ping-6\s*$')
}
