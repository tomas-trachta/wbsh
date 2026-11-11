# Ctrl+C reaches a foreground command through the pseudoconsole and the prompt comes back.
# Headless snapshot sessions do not deliver Ctrl+C on 2026-11-11, neither as the byte
# 0x03 nor as a win32-input-mode key event, so this is a review item until the real
# window is checked by hand (docs/stress-testing.md, wbshterm checklist). Make it
# Severity = 'high' once Ctrl+C works here.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('sleep 30\r', '\x03', 'echo interrupted-ok\r'); Gap = 800; Settle = 1500; Timeout = 25
	Expect = @('(?m)^interrupted-ok\s*$'); Severity = 'review' }
