# Up arrow recalls the previous command.
param([string]$WorkDir, [int]$Scale = 1)
@{
	Feed = @('echo recalled-line\r', '\e[A\r'); Gap = 500
	Check = { param($r)
		$n = ([regex]::Matches($r.Dump, '(?m)^recalled-line\s*$')).Count
		@{ Ok = ($n -ge 2); What = "the recalled command ran $($n - 1) time(s) instead of once" } }
}
