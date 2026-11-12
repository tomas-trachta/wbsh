# Scrollback beyond the in-memory limit goes to a file in the temp folder.
# The file must be gone when the terminal ends, so the output (which can hold
# passwords and tokens) does not stay on disk.
param([string]$WorkDir, [int]$Scale = 1)
@{
	Feed = @('for i in $(seq 1 30000); do echo "secret-line-$i"; done\r'); Settle = 1500; Timeout = 180
	Check = { param($r)
		$left = @(Get-ChildItem $env:TEMP -Filter 'wbh*.tmp' -File -ErrorAction SilentlyContinue |
			Where-Object { $_.CreationTime -ge $r.Started })
		@{ Ok = ($left.Count -eq 0); What = "$($left.Count) scrollback file(s) left in the temp folder after the terminal ended"; Severity = 'high' }
		@{ Ok = $false; Severity = 'review'; What = 'scrollback beyond 10 000 lines is written unencrypted to %TEMP%\wbh*.tmp while the terminal runs (deleted on close); decide if that needs a setting or a note' }
	}
}
