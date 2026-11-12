# The command line: bad input gives an error exit, never a crash, and the
# headless modes (replay, snapshot, selftest, fetch) never wait for a click.
param($Context)

$term = $Context.Terminal
$work = $Context.Work
$raw = Join-Path $work 'small.raw'
[IO.File]::WriteAllBytes($raw, [Text.Encoding]::ASCII.GetBytes("hello`r`n"))
$log = Join-Path $Context.OutDir 'logs\ux-cli.log'

function Test-Run([string]$what, [string[]]$argv, [scriptblock]$expect, [string]$sev = 'medium') {
	$r = Invoke-Probe -Argv (@($term) + $argv) -WorkDir $work -Output "$log.tmp" -TimeoutSec 8 -Env @{ HOME = $work }
	$shown = ($argv -join ' ')
	if ($r.Crashed) {
		return [pscustomobject]@{ Ok = $false; Severity = 'high'; Artifact = $log; Text = "'$shown' crashes wbshterm ($($r.CrashName))" }
	}
	if ($r.TimedOut) {
		return [pscustomobject]@{ Ok = $false; Severity = 'high'; Artifact = $log
			Text = "'$shown' does not end within 8 s: a modal message box or an endless wait blocks scripts and CI" }
	}
	if ($r.ExitCode -eq 3 -or $r.ExitCode -eq 0xC0000409) {
		return [pscustomobject]@{ Ok = $false; Severity = 'high'; Artifact = $log; Text = "'$shown' aborts (exit $($r.ExitCode)): an uncaught exception" }
	}
	$ok = & $expect $r
	return [pscustomobject]@{ Ok = $ok; Severity = $sev; Artifact = $log; Text = "${what}: '$shown' (exit $($r.ExitCode))" }
}

Test-Run 'replay of a small file works' @('--replay', $raw, '--dump', (Join-Path $work 'g.txt'), '--size', '20x2') { param($r) $r.ExitCode -eq 0 }
Test-Run 'fetch prints the panel and ends' @('--fetch', '--config', (Join-Path $work 'c.conf')) { param($r) $r.ExitCode -eq 0 }
Test-Run 'an unknown option is an error' @('--no-such-option') { param($r) $r.ExitCode -ne 0 }
Test-Run 'a missing option value is an error' @('--replay', $raw, '--size') { param($r) $r.ExitCode -ne 0 }
Test-Run 'a bad --size is an error' @('--replay', $raw, '--size', 'abc') { param($r) $r.ExitCode -ne 0 }
Test-Run 'a zero --size is an error or clamps' @('--replay', $raw, '--size', '0x0') { param($r) $true }
Test-Run 'a huge --size is an error' @('--replay', $raw, '--size', '100000x100000') { param($r) $r.ExitCode -ne 0 } 'high'
Test-Run 'a bad --select is an error or ignored' @('--snapshot', (Join-Path $work 's3.png'), '--select', 'abc', '--settle', '100', '--delay', '0') { param($r) $true } 'high'
Test-Run 'a bad --scroll is an error' @('--replay', $raw, '--scroll', 'many') { param($r) $r.ExitCode -ne 0 }
Test-Run 'a bad --delay is an error' @('--snapshot', (Join-Path $work 's.png'), '--delay', '-5') { param($r) $r.ExitCode -ne 0 }
Test-Run 'a missing recording is an error' @('--replay', (Join-Path $work 'nope.raw'), '--dump', (Join-Path $work 'g2.txt')) { param($r) $r.ExitCode -ne 0 }
Test-Run 'an unwritable dump path is an error' @('--replay', $raw, '--dump', (Join-Path $work 'no\such\dir\g.txt')) { param($r) $r.ExitCode -ne 0 }
Test-Run 'a missing --config file falls back to defaults' @('--replay', $raw, '--dump', (Join-Path $work 'g3.txt'), '--config', (Join-Path $work 'missing.conf')) { param($r) $r.ExitCode -eq 0 }
Test-Run 'a shell that does not exist is an error' @('--snapshot', (Join-Path $work 's2.png'), '--shell', (Join-Path $work 'nope.exe'), '--settle', '100') { param($r) $r.ExitCode -ne 0 }
Remove-Item "$log.tmp" -ErrorAction SilentlyContinue

# --record starts a fresh file: a second run must not add to the first.
$rec = Join-Path $work 'twice.raw'
foreach ($n in 1, 2) {
	$null = Invoke-Probe -Argv @($term, '--snapshot', (Join-Path $work "rec$n.png"), '--record', $rec, '--feed', "echo run-$n\r",
		'--delay', '300', '--settle', '400', '--size', '60x10') -WorkDir $work -Output "$log.rec" -TimeoutSec 20 -Env @{ HOME = $work }
}
$recorded = if (Test-Path $rec) { [IO.File]::ReadAllText($rec) } else { '' }
[pscustomobject]@{ Ok = ($recorded -match 'run-2' -and $recorded -notmatch 'run-1'); Severity = 'low'; Artifact = $rec
	Text = '--record appends to an existing recording instead of starting a new one' }
