# Ctrl+C reaches a foreground command through the pseudoconsole and the prompt comes back.
# On 2026-11-11 --snapshot sessions do not deliver Ctrl+C (byte 0x03 or a
# win32-input-mode key event), not even to `cat`, while the self-test's live
# session (check "Ctrl-C interrupts the running command") does with the same
# byte. So this is a review item about snapshot mode until that is fixed;
# Ctrl+C in the real window is on the manual checklist. Make it
# Severity = 'high' once snapshot sessions pass Ctrl+C on.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('sleep 30\r', '\x03', 'echo interrupted-ok\r'); Gap = 800; Settle = 1500; Timeout = 25
	Expect = @('(?m)^interrupted-ok\s*$'); Severity = 'review' }
