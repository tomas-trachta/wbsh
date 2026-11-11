#Requires -Version 7
<#
.SYNOPSIS
    wbshterm stress-test driver: performance, memory, bugs, security and UX suites.

.DESCRIPTION
    Drives the headless modes of wbshterm.exe:
      --replay <raw> --dump <txt>   bytes in, grid text out, no shell (parser + grid)
      --snapshot <png> --record     a live pseudoconsole session with wbsh, typed input
      --selftest <report>           the built-in checks
    Each run is measured through the shared Job-object probe
    (tools/stress/ProcessProbe.cs), so a crash, a hang, memory, handles and
    GDI/USER objects are all seen. Results go to results/<timestamp>/.

    Exit code: 0 when no case failed and nothing regressed, 1 otherwise.

.EXAMPLE
    .\wbshterm\stress\stress.ps1                      # quick profile, all suites
    .\wbshterm\stress\stress.ps1 -Mode full           # release gate
    .\wbshterm\stress\stress.ps1 -Suite bugs -FuzzCount 5000 -Seed 9
    .\wbshterm\stress\stress.ps1 -Suite bugs -Filter cursor* -Record   # write missing .expect files
    .\wbshterm\stress\stress.ps1 -Suite memory -Gui   # also the window soak (opens a window)
#>
[CmdletBinding()]
param(
	[ValidateSet('quick', 'full')]
	[string]$Mode = 'quick',
	[ValidateSet('perf', 'memory', 'bugs', 'security', 'ux')]
	[string[]]$Suite = @('perf', 'memory', 'bugs', 'security', 'ux'),
	[string]$Filter = '*',
	[string]$Exe,
	[string]$Shell,
	[int]$FuzzCount = -1,
	[int]$Seed = -1,
	[int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount / 2),
	[switch]$Record,
	[switch]$Gui,
	[switch]$UpdateBaseline,
	[switch]$KeepWork,
	[switch]$List
)

$ErrorActionPreference = 'Stop'
$StressRoot = $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $StressRoot)
$SuitesDir = Join-Path $StressRoot 'suites'
$Product = 'wbshterm'

. (Join-Path $RepoRoot 'tools\stress\Core.ps1')
. (Join-Path $StressRoot 'lib\Vt.ps1')

$Profiles = @{
	quick = @{ Runs = 3; Warmup = 1; Scale = 1; Fuzz = 400; SessionRuns = 2; MemRuns = 1; WindowSeconds = 20 }
	full  = @{ Runs = 7; Warmup = 2; Scale = 4; Fuzz = 4000; SessionRuns = 5; MemRuns = 2; WindowSeconds = 120 }
}
$P = $Profiles[$Mode]
if ($FuzzCount -ge 0) { $P.Fuzz = $FuzzCount }
if ($Seed -lt 0) { $Seed = Get-Random -Maximum 1000000 }

# ---------------------------------------------------------------- setup

function Resolve-Terminal {
	if ($Exe) { return (Resolve-Path $Exe).Path }
	foreach ($c in 'x64\Release\wbshterm.exe', 'wbshterm\build\wbshterm.exe', 'x64\Debug\wbshterm.exe') {
		$p = Join-Path $RepoRoot $c
		if (Test-Path $p) { return $p }
	}
	throw "wbshterm.exe not found; build it (msbuild wbsh.sln /t:wbshterm) or pass -Exe."
}

$TermExe = Resolve-Terminal
$ShellExe = if ($Shell) { (Resolve-Path $Shell).Path } else { Join-Path (Split-Path $TermExe) 'wbsh.exe' }
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$OutDir = Join-Path $StressRoot "results\$Stamp"
$BaselinePath = Get-MachineBaselinePath $StressRoot
$Baseline = Read-Baseline $BaselinePath
New-StressState

# ---------------------------------------------------------------- runners

function Get-Size($meta, [string]$default = '120x40') {
	$size = Get-Meta $meta 'size' $default
	if ($size -notmatch '^\d+x\d+$') { throw "bad size '$size'" }
	return $size
}

# Grid text of a replayed stream.
function Invoke-Replay([string]$raw, [string]$size, [string]$dump, [string]$log, [int]$TimeoutSec = 60, [switch]$Samples) {
	$argv = @($TermExe, '--replay', $raw, '--size', $size)
	if ($dump) { $argv += @('--dump', $dump) }
	return Invoke-Probe -Argv $argv -WorkDir (Split-Path $raw) -Output $log -TimeoutSec $TimeoutSec -Samples:$Samples
}

# A session spec is the hashtable a *.session.ps1 case returns:
#     Feed     strings typed into the session (wbshterm --feed notation: \r Enter, \e Escape, \xHH)
#     Size     'COLSxROWS' (default 100x30)      Delay, Gap, Settle   ms (defaults 600 / 300 / 800)
#     Shell, Args   another program to host       Timeout   seconds (default 60)
#     Expect   regexes the final grid must match  Absent    regexes it must not match
#     Check    { param($r) ... } returns @{ Ok; What; Severity } items; $r has Dump, Work, Probe, Started
#     Severity of Expect / Absent failures (default medium)
function Invoke-Session([hashtable]$spec, [string]$work, [string]$log, [switch]$Samples) {
	$size = $spec.Size ?? '100x30'
	$png = Join-Path $work 'snapshot.png'
	$raw = Join-Path $work 'session.raw'
	# --record appends to an existing file, so a second run would replay both.
	Remove-Item $raw -ErrorAction SilentlyContinue
	$argv = @($TermExe, '--snapshot', $png, '--record', $raw, '--size', $size,
		'--delay', "$($spec.Delay ?? 600)", '--gap', "$($spec.Gap ?? 300)", '--settle', "$($spec.Settle ?? 800)",
		'--config', (Join-Path $work 'wbshterm.conf'))
	$argv += @('--shell', ($spec.Shell ?? $ShellExe))
	if ($spec.Args) { $argv += @('--args', $spec.Args) }
	foreach ($f in $spec.Feed) { $argv += @('--feed', $f) }

	# A home of its own, so the user's .wbshrc (prompt, startup panel) stays out.
	$envs = @{ HOME = $work; STRESS_TMP = (ConvertTo-Slash $work) }
	$started = Get-Date
	$probe = Invoke-Probe -Argv $argv -WorkDir $work -Output $log -TimeoutSec ($spec.Timeout ?? 60) -Env $envs -Samples:$Samples

	$dump = ''
	if (Test-Path $raw) {
		$grid = Join-Path $work 'grid.txt'
		$null = Invoke-Replay $raw $size $grid "$log.replay" 30
		if (Test-Path $grid) { $dump = [IO.File]::ReadAllText($grid) }
	}
	return @{ Probe = $probe; Dump = $dump; Work = $work; Started = $started; Raw = $raw }
}

# Expect / Absent / Check of a session spec, as findings. Returns the failure count.
function Test-SessionResult([string]$suite, [string]$name, [hashtable]$spec, $result, [string]$log) {
	$fails = 0
	$sev = $spec.Severity ?? 'medium'
	$kind = if ($sev -eq 'review') { 'review' } else { 'fail' }
	$weight = if ($sev -eq 'review') { 0 } else { 1 }
	$gridFile = Join-Path $OutDir "logs\$suite-$name.grid.txt"
	Set-Content -Path $gridFile -Value $result.Dump -NoNewline
	foreach ($re in @($spec.Expect)) {
		if (-not $re) { continue }
		if ($result.Dump -notmatch $re) { Add-Finding $suite $name $sev $kind "grid does not show /$re/" $gridFile; $fails += $weight }
	}
	foreach ($re in @($spec.Absent)) {
		if (-not $re) { continue }
		if ($result.Dump -match $re) { Add-Finding $suite $name $sev $kind "grid shows /$re/, which must not appear" $gridFile; $fails += $weight }
	}
	if ($spec.Check) {
		foreach ($item in @(& $spec.Check $result)) {
			if ($item.Ok) { continue }
			$itemKind = if ($item.Severity -eq 'review') { 'review' } else { 'fail' }
			Add-Finding $suite $name ($item.Severity ?? $sev) $itemKind $item.What $gridFile
			if ($itemKind -eq 'fail') { $fails++ }
		}
	}
	return $fails
}

function Get-StreamFile([System.IO.FileInfo]$file, [string]$work, [int]$scale) {
	$bytes = ConvertTo-StreamBytes (& $file.FullName -Scale $scale)
	$raw = Join-Path $work "$(Get-CaseName $file)-x$scale.raw"
	[IO.File]::WriteAllBytes($raw, $bytes)
	return $raw
}

# ---------------------------------------------------------------- perf

function Measure-Replay([string]$raw, [string]$size, [string]$log, $meta) {
	$runs = [int](Get-Meta $meta 'runs' $P.Runs)
	$warmup = [int](Get-Meta $meta 'warmup' $P.Warmup)
	$walls = @(); $peak = 0L
	for ($i = 0; $i -lt $warmup + $runs; $i++) {
		$r = Invoke-Replay $raw $size '' $log ([int](Get-Meta $meta 'timeout' 300))
		if ($r.Crashed -or $r.TimedOut -or $r.ExitCode -ne 0) { return @{ Error = $r } }
		if ($i -lt $warmup) { continue }
		$walls += $r.WallMs; $peak = [Math]::Max($peak, $r.RootPeakPrivate)
	}
	$median = Get-Median $walls
	$mb = (Get-Item $raw).Length / 1MB
	return @{ MedianMs = [Math]::Round($median, 1); InputMB = [Math]::Round($mb, 1)
		MBps = [Math]::Round($mb / [Math]::Max(0.001, $median / 1000), 1); PeakMB = MB $peak }
}

function Measure-Session([hashtable]$spec, [string]$work, [string]$log) {
	$walls = @(); $peak = 0L; $last = $null
	for ($i = 0; $i -lt $P.SessionRuns; $i++) {
		$res = Invoke-Session $spec $work $log
		$r = $res.Probe
		if ($r.Crashed -or $r.TimedOut -or $r.ExitCode -ne 0) { return @{ Error = $r } }
		# The settle wait and the typing delay are fixed costs of the mode, not work.
		$walls += $r.WallMs - ($spec.Settle ?? 800) - ($spec.Delay ?? 600)
		$peak = [Math]::Max($peak, $r.RootPeakPrivate)
		$last = $res
	}
	return @{ MedianMs = [Math]::Round((Get-Median $walls), 1); PeakMB = MB $peak; Result = $last }
}

function Test-PerfBaseline([string]$name, $m, $meta, [string]$log) {
	$base = $Baseline.perf.$name
	if (-not $base) { return $true }
	$m.baselineMs = $base.MedianMs
	$tolerance = [double](Get-Meta $meta 'tolerance' 0.25)
	if ($m.MedianMs -gt $base.MedianMs * (1 + $tolerance) -and ($m.MedianMs - $base.MedianMs) -gt 20) {
		$pct = [int](100 * ($m.MedianMs / $base.MedianMs - 1))
		Add-Finding 'perf' $name 'medium' 'regression' "median $($m.MedianMs) ms is $pct% over the baseline $($base.MedianMs) ms" $log
		return $false
	}
	return $true
}

function Invoke-PerfSuite {
	Write-Host "`n== perf (${Mode}: $($P.Runs) runs, scale $($P.Scale))" -ForegroundColor Cyan
	foreach ($file in Get-CaseFiles 'perf' '' '*.ps1') {
		$name = Get-CaseName $file
		$meta = Get-CaseMeta $file.FullName
		$work = New-WorkDir "perf-$name"
		$log = Join-Path $OutDir "logs\perf-$name.log"
		$isSession = $file.Name -like '*.session.ps1'
		$m = if ($isSession) {
			Measure-Session (& $file.FullName -WorkDir $work -Scale $P.Scale) $work $log
		} else {
			Measure-Replay (Get-StreamFile $file $work $P.Scale) (Get-Size $meta) $log $meta
		}
		if ($m.Error) {
			if (Test-ProbeHealth $m.Error 'perf' $name $log) { Add-Finding 'perf' $name 'medium' 'script-error' "exited $($m.Error.ExitCode)" $log }
			Add-Case 'perf' $name 'fail'; Write-Status 'fail' $name; continue
		}
		$m.Remove('Result')
		$status = 'pass'
		$maxMs = [double](Get-Meta $meta 'maxms' 0)
		if ($maxMs -gt 0 -and $m.MedianMs -gt $maxMs) {
			Add-Finding 'perf' $name 'medium' 'slow' "median $($m.MedianMs) ms exceeds the $maxMs ms budget" $log
			$status = 'fail'
		}
		if (-not (Test-PerfBaseline $name $m $meta $log)) { $status = 'fail' }
		Add-Case 'perf' $name $status $m
		$rate = if ($m.MBps) { "{0,7:N1} MB/s of {1:N1} MB" -f $m.MBps, $m.InputMB } else { 'session' }
		Write-Status $status ("{0,-24} {1,9:N1} ms  {2,6:N1} MB peak   {3}" -f $name, $m.MedianMs, $m.PeakMB, $rate)
		Remove-WorkDir $work
	}
}

# ---------------------------------------------------------------- memory

function Save-Timeline($r, [string]$path) {
	$lines = @('ms,private_bytes,working_set,handles,gdi,user')
	$lines += $r.Samples | ForEach-Object { '{0:F0},{1},{2},{3},{4},{5}' -f $_.Ms, $_.PrivateBytes, $_.WorkingSet, $_.Handles, $_.GdiObjects, $_.UserObjects }
	Set-Content -Path $path -Value $lines
}

# One measurement of a memory case at one scale: the root's peak private
# bytes and handles, and the input size for replay cases.
function Measure-MemoryOnce([System.IO.FileInfo]$file, [string]$work, [string]$log, [int]$scale, $meta) {
	$best = $null
	for ($i = 0; $i -lt $P.MemRuns; $i++) {
		if ($file.Name -like '*.session.ps1') {
			$res = Invoke-Session (& $file.FullName -WorkDir $work -Scale $scale) $work $log -Samples
			$r = $res.Probe; $inBytes = 0
		} else {
			$raw = Get-StreamFile $file $work $scale
			$r = Invoke-Replay $raw (Get-Size $meta) '' $log ([int](Get-Meta $meta 'timeout' 300)) -Samples
			$inBytes = (Get-Item $raw).Length
		}
		if ($r.Crashed -or $r.TimedOut -or $r.ExitCode -ne 0) { return @{ Probe = $r; Input = $inBytes } }
		if (-not $best -or $r.RootPeakPrivate -lt $best.Probe.RootPeakPrivate) { $best = @{ Probe = $r; Input = $inBytes } }
	}
	return $best
}

# Replay keeps its whole input in one growing buffer, so a replay case may
# grow by a few times the extra input; anything beyond that is the grid's.
function Invoke-MemorySuite {
	Write-Host "`n== memory (growth between scale N and 10xN)" -ForegroundColor Cyan
	foreach ($file in Get-CaseFiles 'memory' '' '*.ps1') {
		if ($file.Name -like '*.window.ps1') { continue }
		$name = Get-CaseName $file
		$meta = Get-CaseMeta $file.FullName
		$work = New-WorkDir "memory-$name"
		$log = Join-Path $OutDir "logs\memory-$name.log"
		$small = [int](Get-Meta $meta 'n' 1) * $P.Scale
		$factor = [int](Get-Meta $meta 'factor' 10)

		$a = Measure-MemoryOnce $file $work "$log.small" $small $meta
		$b = if (Test-ProbeHealth $a.Probe 'memory' $name "$log.small" -and $a.Probe.ExitCode -eq 0) {
			Measure-MemoryOnce $file $work $log ($small * $factor) $meta
		}
		if (-not $b -or -not (Test-ProbeHealth $b.Probe 'memory' $name $log) -or $b.Probe.ExitCode -ne 0) {
			if ($b -and -not $b.Probe.Crashed -and -not $b.Probe.TimedOut) { Add-Finding 'memory' $name 'medium' 'script-error' "exited $($b.Probe.ExitCode)" $log }
			Add-Case 'memory' $name 'fail'; Write-Status 'fail' $name; continue
		}
		if ($b.Probe.Samples.Count) { Save-Timeline $b.Probe (Join-Path $OutDir "logs\memory-$name.timeline.csv") }

		$growth = $b.Probe.RootPeakPrivate - $a.Probe.RootPeakPrivate
		$inputGrowth = $b.Input - $a.Input
		$allowed = [Math]::Max([double](Get-Meta $meta 'tolerance' 8) * 1MB, [double](Get-Meta $meta 'ratio' 3) * $inputGrowth)
		$m = @{ scale = $small; smallMB = MB $a.Probe.RootPeakPrivate; largeMB = MB $b.Probe.RootPeakPrivate
			growthMB = MB $growth; inputGrowthMB = MB $inputGrowth; handlesSmall = $a.Probe.PeakHandles; handlesLarge = $b.Probe.PeakHandles
			gdiLarge = $b.Probe.PeakGdiObjects }
		$status = 'pass'
		if ($growth -gt $allowed) {
			Add-Finding 'memory' $name 'high' 'leak' ("memory grows {0} MB from scale {1} to {2} (input grows {3} MB, {4} MB allowed)" -f (MB $growth), $small, ($small * $factor), (MB $inputGrowth), (MB ([long]$allowed))) $log
			$status = 'fail'
		}
		if ($a.Probe.PeakHandles -gt 0 -and $b.Probe.PeakHandles - $a.Probe.PeakHandles -gt [int](Get-Meta $meta 'handles' 64)) {
			Add-Finding 'memory' $name 'high' 'handle-leak' "peak handle count grows from $($a.Probe.PeakHandles) to $($b.Probe.PeakHandles)" $log
			$status = 'fail'
		}
		$base = $Baseline.memory.$name
		if ($base -and $m.largeMB -gt $base.largeMB * 1.2 + 2) {
			Add-Finding 'memory' $name 'medium' 'regression' "peak $($m.largeMB) MB vs baseline $($base.largeMB) MB" $log
			$status = 'fail'
		}
		Add-Case 'memory' $name $status $m
		Write-Status $status ("{0,-24} {1,7:N1} -> {2,7:N1} MB  (input +{3,6:N1} MB)  handles {4} -> {5}" -f $name, $m.smallMB, $m.largeMB, $m.inputGrowthMB, $m.handlesSmall, $m.handlesLarge)
		if ($status -eq 'pass') { Remove-WorkDir $work }
	}
	if ($Gui) { Invoke-WindowSoak }
}

# Opens the real window on a script that prints for a while, and compares
# the first and the last quarter of the samples: memory, handles, GDI and
# USER objects of a long-lived window must stay flat.
function Invoke-WindowSoak {
	foreach ($file in Get-CaseFiles 'memory' '' '*.window.ps1') {
		$name = Get-CaseName $file
		$work = New-WorkDir "window-$name"
		$log = Join-Path $OutDir "logs\memory-$name.log"
		$spec = & $file.FullName -WorkDir $work -Seconds $P.WindowSeconds
		$argv = @($TermExe, '--config', (Join-Path $work 'wbshterm.conf'), '--shell', $ShellExe, '--args', $spec.Args)
		$r = Invoke-Probe -Argv $argv -WorkDir $work -Output $log -TimeoutSec ($P.WindowSeconds + 60) -Env @{ HOME = $work } -Samples
		if (-not (Test-ProbeHealth $r 'memory' $name $log)) { Add-Case 'memory' $name 'fail'; Write-Status 'fail' $name; continue }
		Save-Timeline $r (Join-Path $OutDir "logs\memory-$name.timeline.csv")
		$s = $r.Samples
		$q = [Math]::Max(1, [int]($s.Count / 4))
		$early = $s | Select-Object -Skip $q -First $q
		$late = $s | Select-Object -Last $q
		$avg = { param($set, $prop) ($set | Measure-Object -Property $prop -Average).Average }
		$m = @{
			privateEarlyMB = MB (& $avg $early PrivateBytes); privateLateMB = MB (& $avg $late PrivateBytes)
			gdiEarly = [int](& $avg $early GdiObjects); gdiLate = [int](& $avg $late GdiObjects)
			userEarly = [int](& $avg $early UserObjects); userLate = [int](& $avg $late UserObjects)
			handlesEarly = [int](& $avg $early Handles); handlesLate = [int](& $avg $late Handles)
		}
		$status = 'pass'
		if ($m.privateLateMB - $m.privateEarlyMB -gt 16) { Add-Finding 'memory' $name 'high' 'leak' "window private memory rises from $($m.privateEarlyMB) to $($m.privateLateMB) MB while it runs" $log; $status = 'fail' }
		if ($m.gdiLate - $m.gdiEarly -gt 50) { Add-Finding 'memory' $name 'high' 'gdi-leak' "GDI objects rise from $($m.gdiEarly) to $($m.gdiLate)" $log; $status = 'fail' }
		if ($m.userLate - $m.userEarly -gt 50) { Add-Finding 'memory' $name 'high' 'user-leak' "USER objects rise from $($m.userEarly) to $($m.userLate)" $log; $status = 'fail' }
		if ($m.handlesLate - $m.handlesEarly -gt 64) { Add-Finding 'memory' $name 'high' 'handle-leak' "handles rise from $($m.handlesEarly) to $($m.handlesLate)" $log; $status = 'fail' }
		Add-Case 'memory' $name $status $m
		Write-Status $status ("{0,-24} private {1} -> {2} MB, GDI {3} -> {4}, USER {5} -> {6}" -f $name, $m.privateEarlyMB, $m.privateLateMB, $m.gdiEarly, $m.gdiLate, $m.userEarly, $m.userLate)
		Remove-WorkDir $work
	}
}

# ---------------------------------------------------------------- bugs

function Invoke-SelfTest {
	if ('selftest' -notlike $Filter) { return }
	$work = New-WorkDir 'selftest'
	$report = Join-Path $work 'selftest.txt'
	$log = Join-Path $OutDir 'logs\bugs-selftest.log'
	$r = Invoke-Probe -Argv @($TermExe, '--selftest', $report) -WorkDir $work -Output $log -TimeoutSec 180 -Env @{ HOME = $work }
	$healthy = Test-ProbeHealth $r 'bugs' 'selftest' $log
	$lines = if (Test-Path $report) { Get-Content $report } else { @() }
	Copy-Item $report (Join-Path $OutDir 'logs\bugs-selftest.report.txt') -ErrorAction SilentlyContinue
	$ok = @($lines | Where-Object { $_ -like 'ok   *' }).Count
	$bad = @($lines | Where-Object { $_ -like 'FAIL *' })
	foreach ($l in $bad) { Add-Finding 'bugs' 'selftest' 'high' 'fail' ($l.Substring(5).Trim()) (Join-Path $OutDir 'logs\bugs-selftest.report.txt') }
	if ($healthy -and -not $bad -and $r.ExitCode -ne 0) { Add-Finding 'bugs' 'selftest' 'high' 'fail' "self-test exited $($r.ExitCode)" $log }
	$status = if ($healthy -and -not $bad -and $r.ExitCode -eq 0) { 'pass' } else { 'fail' }
	Add-Case 'bugs' 'selftest' $status @{ ok = $ok; failed = $bad.Count }
	Write-Status $status ("{0,-30} {1} ok, {2} failed (built-in --selftest)" -f 'selftest', $ok, $bad.Count)
	Remove-WorkDir $work
}

# Golden grids: suites/bugs/vt/<name>.vt replayed must give <name>.expect exactly.
function Invoke-VtGoldens {
	foreach ($file in Get-CaseFiles 'bugs' 'vt' '*.vt') {
		$name = "vt/$(Get-CaseName $file)"
		$meta = Get-CaseMeta $file.FullName
		$work = New-WorkDir "vt-$(Get-CaseName $file)"
		$raw = Join-Path $work 'in.raw'
		[IO.File]::WriteAllBytes($raw, (Read-VtCase $file.FullName))
		$dump = Join-Path $work 'grid.txt'
		$log = Join-Path $OutDir "logs\bugs-vt-$(Get-CaseName $file).log"
		$r = Invoke-Replay $raw (Get-Size $meta '20x5') $dump $log 30
		if (-not (Test-ProbeHealth $r 'bugs' $name $log)) { Add-Case 'bugs' $name 'fail'; Write-Status 'fail' $name; continue }
		$got = if (Test-Path $dump) { [IO.File]::ReadAllText($dump) } else { '' }
		$expectPath = [IO.Path]::ChangeExtension($file.FullName, '.expect')
		if (-not (Test-Path $expectPath) -or $Record) {
			if ($Record) {
				[IO.File]::WriteAllText($expectPath, $got)
				Add-Case 'bugs' $name 'skip'; Write-Status 'skip' "$name (recorded; check the .expect file by hand)"
			} else {
				Add-Finding 'bugs' $name 'medium' 'no-golden' 'no .expect file; run with -Record and check it by hand' $dump
				Add-Case 'bugs' $name 'fail'; Write-Status 'fail' "$name (no .expect)"
			}
			continue
		}
		$want = [IO.File]::ReadAllText($expectPath) -replace "`r`n", "`n"
		if ($got -ceq $want) { Add-Case 'bugs' $name 'pass'; Write-Status 'pass' $name; Remove-WorkDir $work; continue }
		$diff = Join-Path $OutDir "logs\bugs-vt-$(Get-CaseName $file).diff"
		Write-TextDiff $want $got $diff 'expected' 'wbshterm'
		$what = (Get-Content $file.FullName -TotalCount 3 | Where-Object { $_ -match '^# [^@]' } | Select-Object -First 1) -replace '^# ', ''
		Add-Finding 'bugs' $name ((Get-Meta $meta 'severity' 'medium')) 'mismatch' "grid differs from the expected one: $what" $diff
		Add-Case 'bugs' $name 'fail'; Write-Status 'fail' $name
	}
}

# Hostile streams with budgets: no crash, no hang, inside maxms / maxmem.
function Invoke-RobustStreams {
	foreach ($file in Get-CaseFiles 'bugs' 'robust' '*.vt.ps1') {
		$name = "robust/$(Get-CaseName $file)"
		$meta = Get-CaseMeta $file.FullName
		$work = New-WorkDir "robust-$(Get-CaseName $file)"
		$log = Join-Path $OutDir "logs\bugs-robust-$(Get-CaseName $file).log"
		$raw = Get-StreamFile $file $work $P.Scale
		$r = Invoke-Replay $raw (Get-Size $meta '80x24') (Join-Path $work 'grid.txt') $log ([int](Get-Meta $meta 'timeout' 60))
		$status = 'pass'
		if (-not (Test-ProbeHealth $r 'bugs' $name $log)) { $status = 'fail' }
		elseif ($r.ExitCode -ne 0) { Add-Finding 'bugs' $name 'medium' 'fail' "replay exited $($r.ExitCode)" $log; $status = 'fail' }
		else {
			$maxMs = [double](Get-Meta $meta 'maxms' 0); $maxMem = [double](Get-Meta $meta 'maxmem' 0)
			if ($maxMs -gt 0 -and $r.WallMs -gt $maxMs) { Add-Finding 'bugs' $name 'medium' 'slow' ("took {0:N0} ms; budget {1} ms" -f $r.WallMs, $maxMs) $log; $status = 'fail' }
			if ($maxMem -gt 0 -and (MB $r.RootPeakPrivate) -gt $maxMem) { Add-Finding 'bugs' $name 'medium' 'memory' "peak $(MB $r.RootPeakPrivate) MB; budget $maxMem MB" $log; $status = 'fail' }
		}
		Add-Case 'bugs' $name $status @{ wallMs = [int]$r.WallMs; peakMB = MB $r.RootPeakPrivate; inputMB = MB (Get-Item $raw).Length }
		Write-Status $status ("{0,-30} {1,7:N0} ms  {2,6:N1} MB peak  ({3:N1} MB input)" -f $name, $r.WallMs, (MB $r.RootPeakPrivate), (MB (Get-Item $raw).Length))
		if ($status -eq 'pass') { Remove-WorkDir $work }
	}
}

function Invoke-SessionCases([string]$suite, [string]$sub = '') {
	foreach ($file in Get-CaseFiles $suite $sub '*.session.ps1') {
		$base = Get-CaseName $file
		$name = if ($sub) { "$sub/$base" } else { $base }
		$work = New-WorkDir "$suite-$base"
		$log = Join-Path $OutDir "logs\$suite-$base.log"
		$spec = & $file.FullName -WorkDir $work -Scale $P.Scale
		$res = Invoke-Session $spec $work $log
		$status = 'pass'
		if (-not (Test-ProbeHealth $res.Probe $suite $name $log)) { $status = 'fail' }
		elseif ((Test-SessionResult $suite $base $spec $res $log) -gt 0) { $status = 'fail' }
		Add-Case $suite $name $status @{ wallMs = [int]$res.Probe.WallMs }
		Write-Status $status ("{0,-30} {1,7:N0} ms session" -f $name, $res.Probe.WallMs)
		if ($status -eq 'pass') { Remove-WorkDir $work }
	}
}

function Invoke-CorpusReplay {
	$files = Get-ChildItem (Join-Path $SuitesDir 'bugs\corpus') -File -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -like $Filter }
	if (-not $files) { return }
	$bad = 0
	foreach ($f in $files) {
		foreach ($size in '80x24', '1x1', '500x3') {
			$log = Join-Path $OutDir "logs\corpus-$($f.Name)-$size.log"
			$r = Invoke-Replay $f.FullName $size '' $log 20
			if (-not (Test-ProbeHealth $r 'bugs' "corpus/$($f.Name) at $size" $log)) { $bad++ } else { Remove-Item $log -ErrorAction SilentlyContinue }
		}
	}
	$status = if ($bad) { 'fail' } else { 'pass' }
	Add-Case 'bugs' 'corpus-replay' $status @{ files = $files.Count; bad = $bad }
	Write-Status $status ("{0,-30} {1} files at 3 grid sizes, {2} crashed or hung" -f 'corpus-replay', $files.Count, $bad)
}

# Random VT streams and mutations of the goldens and the corpus, replayed
# at random grid sizes. Only crashes and hangs count.
function Invoke-VtFuzz {
	$count = $P.Fuzz
	if ($count -le 0) { return }
	$rng = [System.Random]::new($Seed)
	$seeds = @(Get-ChildItem (Join-Path $SuitesDir 'bugs\vt') -Filter *.vt -File | ForEach-Object { , (Read-VtCase $_.FullName) }) +
		@(Get-ChildItem (Join-Path $SuitesDir 'bugs\corpus') -File -ErrorAction SilentlyContinue | ForEach-Object { , [IO.File]::ReadAllBytes($_.FullName) })
	$dir = Join-Path $OutDir 'fuzz-vt'
	New-Item -ItemType Directory -Path $dir -Force | Out-Null
	$sizes = @('80x24', '1x1', '2x2', '132x50', '300x5', '5x300', '1000x1')
	$inputs = for ($i = 0; $i -lt $count; $i++) {
		$bytes = if ($seeds -and $rng.Next(3) -eq 0) { New-MutatedStream $rng $seeds } else { ConvertTo-StreamBytes (New-FuzzStream $rng) }
		$size = Get-FuzzPick $rng $sizes
		$path = Join-Path $dir ('f{0:D5}-{1}.raw' -f $i, $size)
		[IO.File]::WriteAllBytes($path, $bytes)
		[pscustomobject]@{ Path = $path; Size = $size }
	}

	$exe = $TermExe
	$results = $inputs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
		$r = [WbshStress.ProcessProbe]::RunArgs(@($using:exe, '--replay', $_.Path, '--size', $_.Size, '--dump', "$($_.Path).txt"),
			(Split-Path $_.Path), "$($_.Path).log", 15000, $null)
		[pscustomobject]@{ Path = $_.Path; Crashed = $r.Crashed; TimedOut = $r.TimedOut; Crash = $r.CrashName }
	}

	$bad = @($results | Where-Object { $_.Crashed -or $_.TimedOut })
	foreach ($x in $bad) {
		$what = if ($x.Crashed) { "replay crashed: $($x.Crash)" } else { 'replay hung (15 s)' }
		Add-Finding 'bugs' 'fuzz-vt' ($x.Crashed ? 'critical' : 'high') 'crash' "$what; the grid size is in the file name; copy the input to suites/bugs/corpus/ once fixed" $x.Path
	}
	$keep = @($bad | ForEach-Object { $_.Path; "$($_.Path).log" })
	if (-not $KeepWork) { Get-ChildItem $dir -File | Where-Object { $_.FullName -notin $keep } | Remove-Item -Force }
	$status = if ($bad) { 'fail' } else { 'pass' }
	Add-Case 'bugs' 'fuzz-vt' $status @{ inputs = $count; bad = $bad.Count; seed = $Seed }
	Write-Status $status ("{0,-30} {1} streams (seed {2}), {3} crashed or hung" -f 'fuzz-vt', $count, $Seed, $bad.Count)
}

function Invoke-BugsSuite {
	Write-Host "`n== bugs" -ForegroundColor Cyan
	Invoke-SelfTest
	Invoke-VtGoldens
	Invoke-RobustStreams
	Invoke-SessionCases 'bugs' 'sessions'
	Invoke-CorpusReplay
	if ($Filter -eq '*' -or 'fuzz' -like $Filter) { Invoke-VtFuzz }
}

# ---------------------------------------------------------------- security / ux

function Invoke-PsCases([string]$suite) {
	foreach ($file in Get-CaseFiles $suite '' '*.ps1' | Where-Object { $_.Name -notlike '*.session.ps1' }) {
		$name = Get-CaseName $file
		$ctx = @{ Repo = $RepoRoot; Terminal = $TermExe; Shell = $ShellExe; OutDir = $OutDir; Work = (New-WorkDir "$suite-$name") }
		$items = @(& $file.FullName -Context $ctx)
		$fails = 0; $oks = 0
		foreach ($i in $items) {
			if ($i.Ok) { $oks++; continue }
			$kind = if ($i.Severity -eq 'review') { 'review' } else { 'fail' }
			if ($kind -eq 'fail') { $fails++ }
			Add-Finding $suite $name $i.Severity $kind $i.Text $i.Artifact
		}
		$status = if ($fails) { 'fail' } else { 'pass' }
		Add-Case $suite $name $status @{ ok = $oks; items = $items.Count }
		Write-Status $status ("{0,-30} {1} ok, {2} failed, {3} to review" -f $name, $oks, $fails, ($items.Count - $oks - $fails))
		if ($status -eq 'pass') { Remove-WorkDir $ctx.Work }
	}
}

function Invoke-CheckSuite([string]$suite) {
	Write-Host "`n== $suite" -ForegroundColor Cyan
	Invoke-SessionCases $suite
	Invoke-PsCases $suite
}

# ---------------------------------------------------------------- report

function Get-PerfSection {
	$perf = @($Cases | Where-Object { $_.Suite -eq 'perf' -and $_.Metrics.MedianMs })
	if (-not $perf) { return $null }
	$lines = @('## Performance (median)', '', '| case | ms | input MB | MB/s | peak MB | baseline ms |', '|---|---:|---:|---:|---:|---:|')
	foreach ($c in $perf) {
		$m = $c.Metrics
		$lines += "| $($c.Case) | $($m.MedianMs) | $($m.InputMB) | $($m.MBps) | $($m.PeakMB) | $($m.baselineMs) |"
	}
	return ,$lines
}

function Get-MemorySection {
	$mem = @($Cases | Where-Object { $_.Suite -eq 'memory' -and $_.Metrics.Count })
	if (-not $mem) { return $null }
	$lines = @('## Memory (wbshterm peak private bytes)', '', '| case | at N MB | at 10N MB | input growth MB | handles N -> 10N | GDI |', '|---|---:|---:|---:|---|---:|')
	foreach ($c in $mem) {
		$m = $c.Metrics
		if ($m.privateEarlyMB) {
			$lines += "| $($c.Case) (window) | $($m.privateEarlyMB) | $($m.privateLateMB) | | $($m.handlesEarly) -> $($m.handlesLate) | $($m.gdiEarly) -> $($m.gdiLate) |"
		} else {
			$lines += "| $($c.Case) | $($m.smallMB) | $($m.largeMB) | $($m.inputGrowthMB) | $($m.handlesSmall) -> $($m.handlesLarge) | $($m.gdiLarge) |"
		}
	}
	return ,$lines
}

function Get-BaselineSections {
	$perf = @{}; $memory = @{}
	foreach ($c in $Cases | Where-Object { $_.Suite -eq 'perf' -and $_.Metrics.MedianMs }) { $perf[$c.Case] = @{ MedianMs = $c.Metrics.MedianMs } }
	foreach ($c in $Cases | Where-Object { $_.Suite -eq 'memory' -and $_.Metrics.largeMB }) { $memory[$c.Case] = @{ largeMB = $c.Metrics.largeMB } }
	return @{ perf = $perf; memory = $memory }
}

# ---------------------------------------------------------------- main

if ($List) {
	foreach ($s in $Suite) {
		Get-ChildItem (Join-Path $SuitesDir $s) -Recurse -File -Include *.ps1, *.vt -ErrorAction SilentlyContinue |
			ForEach-Object { '{0,-9} {1}' -f $s, [IO.Path]::GetRelativePath((Join-Path $SuitesDir $s), $_.FullName) }
	}
	exit 0
}

New-Item -ItemType Directory -Path (Join-Path $OutDir 'logs') -Force | Out-Null
$version = (Get-Item $TermExe).VersionInfo.ProductVersion
Write-Host "wbshterm stress  mode=$Mode  seed=$Seed  exe=$TermExe"
Write-Host "                 version $version, shell $ShellExe"
Write-Host "                 results: $OutDir"
$AsanDir = Enable-AsanReports $TermExe

$clock = [Diagnostics.Stopwatch]::StartNew()
foreach ($s in $Suite) {
	switch ($s) {
		'perf' { Invoke-Suite $s { Invoke-PerfSuite } }
		'memory' { Invoke-Suite $s { Invoke-MemorySuite } }
		'bugs' { Invoke-Suite $s { Invoke-BugsSuite } }
		default { Invoke-Suite $s { Invoke-CheckSuite $s } }
	}
}
Add-AsanFindings $AsanDir

$header = @("wbshterm: $version ($TermExe)", "shell: $ShellExe", "mode: $Mode, seed $Seed, machine $env:COMPUTERNAME")
$failed = Write-Report "wbshterm stress report $Stamp" $header @((Get-PerfSection), (Get-MemorySection)) @{ seed = $Seed; mode = $Mode }
if ($UpdateBaseline) { Save-Baseline $BaselinePath $Baseline (Get-BaselineSections) $version $Stamp }
Write-Summary $failed $clock
exit ([int]($failed -gt 0))
