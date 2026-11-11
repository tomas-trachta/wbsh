#Requires -Version 7
<#
.SYNOPSIS
    wbsh stress-test driver: performance, memory, bugs, security and UX suites.

.DESCRIPTION
    Runs the cases under wbsh/stress/suites/<suite>/, measures every process
    tree through a Windows Job object (lib/ProcessProbe.cs), compares against
    the reference bash and a per-machine baseline, and writes
    results/<timestamp>/report.md + report.json.

    Exit code: 0 when no case failed and nothing regressed, 1 otherwise.
    "review" findings do not change the exit code.

.EXAMPLE
    .\wbsh\stress\stress.ps1                         # quick profile, all suites
    .\wbsh\stress\stress.ps1 -Mode full              # release gate
    .\wbsh\stress\stress.ps1 -Suite perf -Filter 'loop*'
    .\wbsh\stress\stress.ps1 -Suite bugs -FuzzCount 2000 -Seed 7
    .\wbsh\stress\stress.ps1 -Suite perf,memory -UpdateBaseline
    .\wbsh\stress\stress.ps1 -Suite security -Net       # also the checks that need the internet
#>
[CmdletBinding()]
param(
	[ValidateSet('quick', 'full')]
	[string]$Mode = 'quick',
	[ValidateSet('perf', 'memory', 'bugs', 'security', 'ux')]
	[string[]]$Suite = @('perf', 'memory', 'bugs', 'security', 'ux'),
	[string]$Filter = '*',
	[string]$Exe,
	[string]$RefShell,
	[switch]$NoRef,
	[int]$FuzzCount = -1,
	[int]$Seed = -1,
	[int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount / 2),
	[switch]$UpdateBaseline,
	[switch]$KeepWork,
	[switch]$Net,
	[switch]$List
)

$ErrorActionPreference = 'Stop'
$StressRoot = $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $StressRoot)
$SuitesDir = Join-Path $StressRoot 'suites'

. (Join-Path $StressRoot 'lib\Fuzz.ps1')

$Profiles = @{
	quick = @{ Runs = 3; Warmup = 1; Scale = 1; FuzzExec = 150; FuzzParse = 300; MemRuns = 1 }
	full  = @{ Runs = 7; Warmup = 2; Scale = 3; FuzzExec = 1500; FuzzParse = 3000; MemRuns = 2 }
}
$P = $Profiles[$Mode]
if ($FuzzCount -ge 0) { $P.FuzzExec = $FuzzCount; $P.FuzzParse = $FuzzCount * 2 }
if ($Seed -lt 0) { $Seed = Get-Random -Maximum 1000000 }

# ---------------------------------------------------------------- setup

function ConvertTo-Slash([string]$path) { return $path -replace '\\', '/' }

function Resolve-Exe {
	if ($Exe) { return (Resolve-Path $Exe).Path }
	foreach ($c in 'x64\Release\wbsh.exe', 'x64\Debug\wbsh.exe') {
		$p = Join-Path $RepoRoot $c
		if (Test-Path $p) { return $p }
	}
	throw "wbsh.exe not found under $RepoRoot\x64\{Release,Debug}; build first or pass -Exe."
}

function Resolve-RefShell {
	if ($NoRef) { return $null }
	if ($RefShell) { return (Resolve-Path $RefShell).Path }
	foreach ($c in "$env:ProgramFiles\Git\usr\bin\bash.exe", "$env:ProgramFiles\Git\bin\bash.exe") {
		if (Test-Path $c) { return $c }
	}
	return $null
}

# Git's bin\bash.exe is a launcher that adds a process and sets PATH; the real
# usr\bin\bash.exe needs its own folder (coreutils, gzip, awk) first on PATH.
# wbsh is UTF-8 throughout; without a locale Git Bash counts bytes.
function Get-BashEnv {
	if (-not $BashExe) { return @{} }
	return @{ PATH = "$(Split-Path $BashExe);$env:PATH"; STRESS_SHELL = 'bash'; LC_ALL = 'C.UTF-8' }
}

if (-not ('WbshStress.ProcessProbe' -as [type])) {
	Add-Type -Path (Join-Path $StressRoot 'lib\ProcessProbe.cs')
}

$WbshExe = Resolve-Exe
$BashExe = Resolve-RefShell
$BashEnv = Get-BashEnv
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$OutDir = Join-Path $StressRoot "results\$Stamp"
$BaselinePath = Join-Path $StressRoot "baselines\$($env:COMPUTERNAME.ToLower()).json"
$Baseline = if (Test-Path $BaselinePath) { Get-Content $BaselinePath -Raw | ConvertFrom-Json -AsHashtable } else { $null }

$Findings = [System.Collections.Generic.List[object]]::new()
$Cases = [System.Collections.Generic.List[object]]::new()

# ---------------------------------------------------------------- helpers

function Get-CaseFiles([string]$suite, [string]$sub = '', [string]$pattern = '*.sh') {
	$dir = Join-Path $SuitesDir $suite
	if ($sub) { $dir = Join-Path $dir $sub }
	if (-not (Test-Path $dir)) { return @() }
	return Get-ChildItem $dir -File -Filter $pattern |
		Where-Object { $_.Name -notlike '*.setup.sh' -and $_.BaseName -like $Filter } |
		Sort-Object Name
}

# Reads `# @stress key=value ...` lines from the head of a case file.
function Get-CaseMeta([string]$path) {
	$meta = @{}
	foreach ($line in (Get-Content $path -TotalCount 40)) {
		if ($line -match '^#\s*@stress\s+(.*)$') {
			foreach ($m in [regex]::Matches($Matches[1], '(\w+)=(\S+)')) {
				$meta[$m.Groups[1].Value] = $m.Groups[2].Value
			}
		}
	}
	return $meta
}

function Get-Meta($meta, [string]$key, $default) {
	if ($meta.ContainsKey($key)) { return $meta[$key] }
	return $default
}

function New-WorkDir([string]$name) {
	$dir = Join-Path $OutDir "work\$name"
	if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
	New-Item -ItemType Directory -Path $dir -Force | Out-Null
	return $dir
}

function Remove-WorkDir([string]$dir) {
	if (-not $KeepWork -and (Test-Path $dir)) { Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue }
}

function Get-CaseEnv([string]$workDir, [hashtable]$extra = @{}) {
	$envs = @{
		STRESS_WBSH  = ConvertTo-Slash $WbshExe
		STRESS_BASH  = if ($BashExe) { ConvertTo-Slash $BashExe } else { '' }
		STRESS_TMP   = ConvertTo-Slash $workDir
		STRESS_ROOT  = ConvertTo-Slash $StressRoot
		STRESS_REPO  = ConvertTo-Slash $RepoRoot
		STRESS_N     = "$($P.Scale)"
		STRESS_MODE  = $Mode
		STRESS_NET   = if ($Net) { '1' } else { '0' }
	}
	foreach ($k in $extra.Keys) { $envs[$k] = $extra[$k] }
	return $envs
}

# Runs one shell on one script under the probe.
function Invoke-Shell {
	param([string]$Shell, [string]$Script, [string]$WorkDir, [string]$Output, [int]$TimeoutSec = 120,
		[hashtable]$Env = @{}, [switch]$Samples, [string[]]$ShellArgs = @())

	$argv = if ($Shell -eq 'bash') { @($BashExe) + $ShellArgs + @($Script) } else { @($WbshExe) + $ShellArgs + @('-r', $Script) }
	$opts = [WbshStress.ProbeOptions]::new()
	$opts.CommandLine = [WbshStress.ProcessProbe]::JoinArgs([string[]]$argv)
	$opts.WorkDir = $WorkDir
	$opts.StdoutPath = $Output
	$opts.StderrPath = $Output
	$opts.TimeoutMs = $TimeoutSec * 1000
	$opts.KeepSamples = [bool]$Samples
	$opts.MemoryLimitBytes = 4L * 1024 * 1024 * 1024
	$opts.Environment = [System.Collections.Hashtable](Get-CaseEnv $WorkDir $Env)
	$opts.Environment['STRESS_SHELL'] = 'wbsh'
	if ($Shell -eq 'bash') { foreach ($k in $BashEnv.Keys) { $opts.Environment[$k] = $BashEnv[$k] } }
	return [WbshStress.ProcessProbe]::Run($opts)
}

function Add-Finding([string]$suite, [string]$case, [string]$severity, [string]$kind, [string]$text, [string]$artifact = '') {
	$rel = ''
	if ($artifact) { $rel = ([IO.Path]::GetRelativePath($OutDir, $artifact)).Replace('\', '/') }
	$Findings.Add([pscustomobject]@{
		Suite = $suite; Case = $case; Severity = $severity; Kind = $kind; Text = $text; Artifact = $rel
	})
}

# A crash or a hang is a finding whatever the case expected.
function Test-ProbeHealth($r, [string]$suite, [string]$case, [string]$log) {
	if ($r.Crashed) {
		Add-Finding $suite $case 'critical' 'crash' "wbsh crashed: $($r.CrashName) (exit 0x$($r.ExitCode.ToString('X8')))" $log
		return $false
	}
	if ($r.TimedOut) {
		Add-Finding $suite $case 'high' 'hang' "timed out after $([int]($r.WallMs / 1000)) s; process tree killed" $log
		return $false
	}
	if ($r.LeftoverProcesses -gt 0) {
		Add-Finding $suite $case 'medium' 'orphan' "$($r.LeftoverProcesses) process(es) still alive after wbsh exited" $log
	}
	return $true
}

function Get-Median([double[]]$values) {
	if ($values.Count -eq 0) { return 0 }
	$s = $values | Sort-Object
	$mid = [int][Math]::Floor($s.Count / 2)
	if ($s.Count % 2) { return $s[$mid] }
	return ($s[$mid - 1] + $s[$mid]) / 2
}

function MB([long]$bytes) { return [Math]::Round($bytes / 1MB, 1) }

function Write-Status([string]$status, [string]$text) {
	$color = switch ($status) { 'pass' { 'Green' } 'fail' { 'Red' } 'skip' { 'DarkGray' } default { 'Yellow' } }
	Write-Host ('  {0,-5} ' -f $status) -ForegroundColor $color -NoNewline
	Write-Host $text
}

function Add-Case([string]$suite, [string]$name, [string]$status, [hashtable]$metrics = @{}) {
	$Cases.Add([pscustomobject]@{ Suite = $suite; Case = $name; Status = $status; Metrics = $metrics })
}

# ---------------------------------------------------------------- check runner (TAP)

# Runs a property-checking script under wbsh and turns its TAP lines into findings.
function Invoke-CheckCase([string]$suite, [System.IO.FileInfo]$file) {
	$meta = Get-CaseMeta $file.FullName
	$name = if ($file.Directory.Name -ne $suite) { "$($file.Directory.Name)/$($file.BaseName)" } else { $file.BaseName }
	$work = New-WorkDir "$suite-$($file.BaseName)"
	$log = Join-Path $OutDir "logs\$suite-$($file.BaseName).log"
	$r = Invoke-Shell -Shell 'wbsh' -Script (ConvertTo-Slash $file.FullName) -WorkDir $work -Output $log `
		-TimeoutSec ([int](Get-Meta $meta 'timeout' 180))

	$healthy = Test-ProbeHealth $r $suite $name $log
	$passed = 0
	$failed = 0
	$text = if (Test-Path $log) { Get-Content $log } else { @() }
	foreach ($line in $text) {
		if ($line -match '^not ok - (?:\[(critical|high|medium|low)\] )?(.*)$') {
			$sev = if ($Matches[1]) { $Matches[1] } else { 'medium' }
			Add-Finding $suite $name $sev 'fail' $Matches[2] $log
			$failed++
		} elseif ($line -match '^ok - ') {
			$passed++
		} elseif ($line -match '^# review: (.*)$') {
			Add-Finding $suite $name 'review' 'review' $Matches[1] $log
		}
	}

	if ($healthy -and $failed -eq 0 -and $r.ExitCode -ne 0) {
		Add-Finding $suite $name 'medium' 'script-error' "case exited $($r.ExitCode) without a failed check (broken case or wbsh bug)" $log
		$failed++
	}
	$status = if (-not $healthy -or $failed -gt 0) { 'fail' } else { 'pass' }
	Add-Case $suite $name $status @{ passed = $passed; failed = $failed; wallMs = [int]$r.WallMs }
	Write-Status $status ("{0,-34} {1} ok, {2} not ok, {3} ms" -f $name, $passed, $failed, [int]$r.WallMs)
	if ($status -eq 'pass') { Remove-WorkDir $work }
}

# ---------------------------------------------------------------- perf

function Measure-Runs([string]$shell, [string]$script, [string]$work, [string]$log, $meta) {
	$timeout = [int](Get-Meta $meta 'timeout' 300)
	$runs = [int](Get-Meta $meta 'runs' $P.Runs)
	$warmup = [int](Get-Meta $meta 'warmup' $P.Warmup)
	$walls = @(); $cpus = @(); $peak = 0L; $procs = 0
	for ($i = 0; $i -lt $warmup + $runs; $i++) {
		$r = Invoke-Shell -Shell $shell -Script $script -WorkDir $work -Output $log -TimeoutSec $timeout
		if ($r.Crashed -or $r.TimedOut -or $r.ExitCode -ne 0) { return @{ Error = $r } }
		if ($i -lt $warmup) { continue }
		$walls += $r.WallMs; $cpus += $r.CpuMs
		$peak = [Math]::Max($peak, $r.PeakJobMemory); $procs = $r.TotalProcesses
	}
	return @{
		MedianMs = [Math]::Round((Get-Median $walls), 1); MinMs = [Math]::Round(($walls | Measure-Object -Minimum).Minimum, 1)
		MaxMs = [Math]::Round(($walls | Measure-Object -Maximum).Maximum, 1); CpuMs = [Math]::Round((Get-Median $cpus), 1)
		PeakMB = MB $peak; Processes = $procs
	}
}

function Invoke-PerfSuite {
	Write-Host "`n== perf (${Mode}: $($P.Runs) runs, scale $($P.Scale))" -ForegroundColor Cyan
	foreach ($file in Get-CaseFiles 'perf') {
		$meta = Get-CaseMeta $file.FullName
		$name = $file.BaseName
		$script = ConvertTo-Slash $file.FullName
		$work = New-WorkDir "perf-$name"
		$log = Join-Path $OutDir "logs\perf-$name.log"
		$setup = Join-Path $file.DirectoryName "$name.setup.sh"
		if (Test-Path $setup) {
			$r = Invoke-Shell -Shell 'wbsh' -Script (ConvertTo-Slash $setup) -WorkDir $work -Output "$log.setup" -TimeoutSec 600
			if ($r.ExitCode -ne 0) {
				Add-Finding 'perf' $name 'medium' 'script-error' "setup failed (exit $($r.ExitCode))" "$log.setup"
				Add-Case 'perf' $name 'fail'; Write-Status 'fail' "$name (setup)"; continue
			}
		}

		$w = Measure-Runs 'wbsh' $script $work $log $meta
		if ($w.Error) {
			if (Test-ProbeHealth $w.Error 'perf' $name $log) {
				Add-Finding 'perf' $name 'medium' 'script-error' "benchmark exited $($w.Error.ExitCode)" $log
			}
			Add-Case 'perf' $name 'fail'; Write-Status 'fail' $name; continue
		}

		$metrics = @{ wbsh = $w }
		$refText = ''
		if ($BashExe -and (Get-Meta $meta 'ref' 'bash') -eq 'bash') {
			$b = Measure-Runs 'bash' $script $work "$log.bash" $meta
			if (-not $b.Error) {
				$metrics.bash = $b
				$metrics.ratio = [Math]::Round($w.MedianMs / [Math]::Max(1, $b.MedianMs), 2)
				$refText = "bash {0,8:N1} ms  x{1}" -f $b.MedianMs, $metrics.ratio
			} else {
				$refText = "bash failed (exit $($b.Error.ExitCode))"
			}
		}

		$status = 'pass'
		$errors = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -like 'wbsh: *' })
		if ($errors) {
			Add-Finding 'perf' $name 'medium' 'script-error' "benchmark printed $($errors.Count) error line(s), first: $($errors[0]); the numbers are not valid" $log
			$status = 'fail'
		}
		$maxRatio = [double](Get-Meta $meta 'maxratio' 0)
		if ($maxRatio -gt 0 -and $metrics.ratio -gt $maxRatio) {
			Add-Finding 'perf' $name 'medium' 'slow' "wbsh is x$($metrics.ratio) slower than bash (limit x$maxRatio)" $log
			$status = 'fail'
		}
		$maxMem = [double](Get-Meta $meta 'maxmem' 0)
		if ($maxMem -gt 0 -and $w.PeakMB -gt $maxMem) {
			Add-Finding 'perf' $name 'medium' 'memory' "peak $($w.PeakMB) MB exceeds the $maxMem MB budget" $log
			$status = 'fail'
		}
		$base = $Baseline.perf.$name
		if ($base) {
			$metrics.baselineMs = $base.MedianMs
			$tolerance = [double](Get-Meta $meta 'tolerance' 0.25)
			if ($w.MedianMs -gt $base.MedianMs * (1 + $tolerance) -and ($w.MedianMs - $base.MedianMs) -gt 20) {
				$pct = [int](100 * ($w.MedianMs / $base.MedianMs - 1))
				Add-Finding 'perf' $name 'medium' 'regression' "median $($w.MedianMs) ms is $pct% over the baseline $($base.MedianMs) ms" $log
				$status = 'fail'
			}
		}
		Add-Case 'perf' $name $status $metrics
		Write-Status $status ("{0,-26} wbsh {1,8:N1} ms  cpu {2,8:N1}  {3,6:N1} MB   {4}" -f $name, $w.MedianMs, $w.CpuMs, $w.PeakMB, $refText)
		Remove-WorkDir $work
	}
}

# ---------------------------------------------------------------- memory

function Measure-Memory([string]$script, [string]$work, [string]$log, [int]$iters, [int]$timeout) {
	$best = $null
	for ($i = 0; $i -lt $P.MemRuns; $i++) {
		$r = Invoke-Shell -Shell 'wbsh' -Script $script -WorkDir $work -Output $log -TimeoutSec $timeout `
			-Env @{ STRESS_ITERS = "$iters" } -Samples
		if ($r.Crashed -or $r.TimedOut -or $r.ExitCode -ne 0) { return $r }
		if (-not $best -or $r.RootPeakPrivate -lt $best.RootPeakPrivate) { $best = $r }
	}
	return $best
}

function Save-Timeline($r, [string]$path) {
	$lines = @('ms,private_bytes,working_set,handles')
	$lines += $r.Samples | ForEach-Object { '{0:F0},{1},{2},{3}' -f $_.Ms, $_.PrivateBytes, $_.WorkingSet, $_.Handles }
	Set-Content -Path $path -Value $lines
}

function Invoke-MemorySuite {
	Write-Host "`n== memory (leak = growth between N and $(10)xN iterations)" -ForegroundColor Cyan
	foreach ($file in Get-CaseFiles 'memory') {
		$meta = Get-CaseMeta $file.FullName
		$name = $file.BaseName
		$script = ConvertTo-Slash $file.FullName
		$work = New-WorkDir "memory-$name"
		$log = Join-Path $OutDir "logs\memory-$name.log"
		$small = [int](Get-Meta $meta 'n' 500) * $P.Scale
		$factor = [int](Get-Meta $meta 'factor' 10)
		$timeout = [int](Get-Meta $meta 'timeout' 600)

		$a = Measure-Memory $script $work "$log.small" $small $timeout
		$b = if (Test-ProbeHealth $a 'memory' $name "$log.small" -and $a.ExitCode -eq 0) {
			Measure-Memory $script $work $log ($small * $factor) $timeout
		}
		if (-not $b -or -not (Test-ProbeHealth $b 'memory' $name $log) -or $b.ExitCode -ne 0) {
			if ($b -and -not $b.Crashed -and -not $b.TimedOut) {
				Add-Finding 'memory' $name 'medium' 'script-error' "case exited $($b.ExitCode)" $log
			}
			Add-Case 'memory' $name 'fail'; Write-Status 'fail' $name; continue
		}
		Save-Timeline $b (Join-Path $OutDir "logs\memory-$name.timeline.csv")

		$growth = $b.RootPeakPrivate - $a.RootPeakPrivate
		$perIter = [Math]::Round($growth / ($small * ($factor - 1)), 1)
		$limit = [Math]::Max([double](Get-Meta $meta 'tolerance' 4) * 1MB, 0.15 * $a.RootPeakPrivate)
		$handleGrowth = $b.PeakHandles - $a.PeakHandles
		$metrics = @{
			iters = $small; smallMB = MB $a.RootPeakPrivate; largeMB = MB $b.RootPeakPrivate
			growthMB = MB $growth; bytesPerIter = $perIter; jobPeakMB = MB $b.PeakJobMemory
			handlesSmall = $a.PeakHandles; handlesLarge = $b.PeakHandles; wallMs = [int]$b.WallMs
		}
		$status = 'pass'
		if ($growth -gt $limit) {
			Add-Finding 'memory' $name 'high' 'leak' ("private memory grows {0} MB from {1} to {2} iterations (~{3} bytes/iteration)" -f (MB $growth), $small, ($small * $factor), $perIter) $log
			$status = 'fail'
		}
		if ($handleGrowth -gt [int](Get-Meta $meta 'handles' 64)) {
			Add-Finding 'memory' $name 'high' 'handle-leak' "peak handle count grows from $($a.PeakHandles) to $($b.PeakHandles)" $log
			$status = 'fail'
		}
		$maxMem = [double](Get-Meta $meta 'maxmem' 0)
		if ($maxMem -gt 0 -and (MB $b.PeakJobMemory) -gt $maxMem) {
			Add-Finding 'memory' $name 'medium' 'memory' "process tree peak $(MB $b.PeakJobMemory) MB exceeds the $maxMem MB budget" $log
			$status = 'fail'
		}
		$base = $Baseline.memory.$name
		if ($base -and $metrics.largeMB -gt $base.largeMB * 1.2 + 2) {
			Add-Finding 'memory' $name 'medium' 'regression' "peak $($metrics.largeMB) MB vs baseline $($base.largeMB) MB" $log
			$status = 'fail'
		}
		Add-Case 'memory' $name $status $metrics
		Write-Status $status ("{0,-26} {1,7:N1} -> {2,7:N1} MB  ({3,8} B/iter)  handles {4} -> {5}" -f $name, $metrics.smallMB, $metrics.largeMB, $perIter, $a.PeakHandles, $b.PeakHandles)
		if ($status -eq 'pass') { Remove-WorkDir $work }
	}
}

# ---------------------------------------------------------------- bugs

function Invoke-DiffCase([System.IO.FileInfo]$file) {
	$name = "diff/$($file.BaseName)"
	if (-not $BashExe) { Add-Case 'bugs' $name 'skip'; Write-Status 'skip' "$name (no reference bash)"; return }

	$script = ConvertTo-Slash $file.FullName
	$wl = Join-Path $OutDir "logs\bugs-diff-$($file.BaseName).wbsh.txt"
	$bl = Join-Path $OutDir "logs\bugs-diff-$($file.BaseName).bash.txt"
	$wd = New-WorkDir "diff-$($file.BaseName)-wbsh"
	$bd = New-WorkDir "diff-$($file.BaseName)-bash"
	$w = Invoke-Shell -Shell 'wbsh' -Script $script -WorkDir $wd -Output $wl -TimeoutSec 60
	$b = Invoke-Shell -Shell 'bash' -Script $script -WorkDir $bd -Output $bl -TimeoutSec 60
	Remove-WorkDir $wd; Remove-WorkDir $bd
	if (-not (Test-ProbeHealth $w 'bugs' $name $wl)) { Add-Case 'bugs' $name 'fail'; Write-Status 'fail' $name; return }

	$wText = (Get-Content $wl -Raw) ?? ''
	$bText = (Get-Content $bl -Raw) ?? ''
	$accept = Join-Path $file.DirectoryName "$($file.BaseName).accept"
	$same = $wText -ceq $bText -and $w.ExitCode -eq $b.ExitCode
	if (-not $same -and (Test-Path $accept)) { $same = $wText -ceq ((Get-Content $accept -Raw) ?? '') }
	if ($same) { Add-Case 'bugs' $name 'pass'; Write-Status 'pass' $name; return }

	$diffPath = Join-Path $OutDir "logs\bugs-diff-$($file.BaseName).diff"
	Write-TextDiff $bText $wText $diffPath "bash (exit $($b.ExitCode))" "wbsh (exit $($w.ExitCode))"
	Add-Finding 'bugs' $name 'medium' 'mismatch' "output differs from bash (exit wbsh=$($w.ExitCode) bash=$($b.ExitCode)); see diff" $diffPath
	Add-Case 'bugs' $name 'fail'; Write-Status 'fail' $name
}

function Write-TextDiff([string]$a, [string]$b, [string]$path, [string]$labelA, [string]$labelB) {
	$la = $a -split "`n"; $lb = $b -split "`n"
	$out = @("--- $labelA", "+++ $labelB")
	$max = [Math]::Max($la.Count, $lb.Count)
	for ($i = 0; $i -lt $max; $i++) {
		$x = if ($i -lt $la.Count) { $la[$i] } else { '<missing>' }
		$y = if ($i -lt $lb.Count) { $lb[$i] } else { '<missing>' }
		if ($x -cne $y) { $out += "@@ line $($i + 1)"; $out += "-$x"; $out += "+$y" }
	}
	Set-Content -Path $path -Value $out
}

# Replays every corpus file through the parser only: no crash, no hang.
function Invoke-CorpusReplay {
	$files = Get-ChildItem (Join-Path $SuitesDir 'bugs\corpus') -File -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -like $Filter }
	if (-not $files) { return }
	$bad = 0
	foreach ($f in $files) {
		$log = Join-Path $OutDir "logs\corpus-$($f.Name).log"
		$r = [WbshStress.ProcessProbe]::RunArgs(@($WbshExe, '--no-ast', $f.FullName), $OutDir, $log, 15000, $null)
		if (-not (Test-ProbeHealth $r 'bugs' "corpus/$($f.Name)" $log)) { $bad++ } else { Remove-Item $log -ErrorAction SilentlyContinue }
	}
	$status = if ($bad) { 'fail' } else { 'pass' }
	Add-Case 'bugs' 'corpus-replay' $status @{ files = $files.Count; bad = $bad }
	Write-Status $status ("{0,-34} {1} files through the parser, {2} crashed or hung" -f 'corpus-replay', $files.Count, $bad)
}

function Invoke-ParseFuzz {
	$count = $P.FuzzParse
	if ($count -le 0) { return }
	$seedFiles = @(Get-ChildItem (Join-Path $RepoRoot 'wbsh\tests') -Filter *.sh -File) +
		@(Get-ChildItem (Join-Path $SuitesDir 'bugs\corpus') -File -ErrorAction SilentlyContinue)
	$seeds = @($seedFiles | ForEach-Object { Get-Content $_.FullName -Raw } | Where-Object { $_ })
	$rng = [System.Random]::new($Seed)
	$dir = Join-Path $OutDir 'fuzz-parse'
	New-Item -ItemType Directory -Path $dir -Force | Out-Null
	$inputs = for ($i = 0; $i -lt $count; $i++) {
		$path = Join-Path $dir ('m{0:D5}.sh' -f $i)
		[IO.File]::WriteAllText($path, (New-ParseMutant $rng $seeds))
		$path
	}

	$exe = $WbshExe
	$results = $inputs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
		$r = [WbshStress.ProcessProbe]::RunArgs(@($using:exe, '--no-ast', $_), (Split-Path $_), "$_.out", 15000, $null)
		[pscustomobject]@{ Path = $_; Crashed = $r.Crashed; TimedOut = $r.TimedOut; Crash = $r.CrashName; Exit = $r.ExitCode }
	}

	$bad = @($results | Where-Object { $_.Crashed -or $_.TimedOut })
	foreach ($x in $bad) {
		$what = if ($x.Crashed) { "parser crashed: $($x.Crash)" } else { 'parser hung (15 s)' }
		$sev = if ($x.Crashed) { 'critical' } else { 'high' }
		Add-Finding 'bugs' 'fuzz-parse' $sev 'crash' "$what on a mutated script; copy it to suites/bugs/corpus/ once fixed" $x.Path
	}
	$keep = @($bad | ForEach-Object { $_.Path; "$($_.Path).out" })
	if (-not $KeepWork) { Get-ChildItem $dir -File | Where-Object { $_.FullName -notin $keep } | Remove-Item -Force }
	$status = if ($bad) { 'fail' } else { 'pass' }
	Add-Case 'bugs' 'fuzz-parse' $status @{ inputs = $count; bad = $bad.Count; seed = $Seed }
	Write-Status $status ("{0,-34} {1} mutants (seed {2}), {3} crashed or hung" -f 'fuzz-parse', $count, $Seed, $bad.Count)
}

function Invoke-ExecFuzz {
	$count = $P.FuzzExec
	if ($count -le 0) { return }
	if (-not $BashExe) { Add-Case 'bugs' 'fuzz-exec' 'skip'; Write-Status 'skip' 'fuzz-exec (no reference bash)'; return }

	$rng = [System.Random]::new($Seed + 1)
	$dir = Join-Path $OutDir 'fuzz-exec'
	New-Item -ItemType Directory -Path $dir -Force | Out-Null
	$inputs = for ($i = 0; $i -lt $count; $i++) {
		$path = Join-Path $dir ('p{0:D5}.sh' -f $i)
		[IO.File]::WriteAllText($path, (New-FuzzProgram $rng))
		$path
	}

	$exe = $WbshExe; $bash = $BashExe; $benv = [System.Collections.Hashtable]$BashEnv
	$results = $inputs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
		$base = [IO.Path]::ChangeExtension($_, $null).TrimEnd('.')
		$wd = "$base.wd"; $bd = "$base.bd"
		New-Item -ItemType Directory $wd, $bd -Force | Out-Null
		$w = [WbshStress.ProcessProbe]::RunArgs(@($using:exe, '-r', $_), $wd, "$base.wbsh.txt", 10000, $null)
		$b = [WbshStress.ProcessProbe]::RunArgs(@($using:bash, $_), $bd, "$base.bash.txt", 10000, $using:benv)
		Remove-Item $wd, $bd -Recurse -Force -ErrorAction SilentlyContinue
		[pscustomobject]@{ Path = $_; Base = $base; W = $w; B = $b }
	}

	$bad = 0; $mismatch = 0
	foreach ($x in $results) {
		$wOut = [IO.File]::ReadAllText("$($x.Base).wbsh.txt")
		$bOut = [IO.File]::ReadAllText("$($x.Base).bash.txt")
		$keep = $false
		if ($x.B.TimedOut -and -not $x.W.Crashed) {
			$keep = $false
		} elseif ($x.W.Crashed -or $x.W.TimedOut) {
			$what = if ($x.W.Crashed) { "crashed: $($x.W.CrashName)" } else { 'hung (10 s)' }
			Add-Finding 'bugs' 'fuzz-exec' ($x.W.Crashed ? 'critical' : 'high') 'crash' "wbsh $what on a generated script" $x.Path
			$bad++; $keep = $true
		} elseif ($x.B.TimedOut) {
			$keep = $false
		} elseif ($wOut -cne $bOut -or $x.W.ExitCode -ne $x.B.ExitCode) {
			Write-TextDiff $bOut $wOut "$($x.Base).diff" "bash (exit $($x.B.ExitCode))" "wbsh (exit $($x.W.ExitCode))"
			$mismatch++; $keep = $true
		}
		if (-not $keep -and -not $KeepWork) { Remove-Item $x.Path, "$($x.Base).wbsh.txt", "$($x.Base).bash.txt" -ErrorAction SilentlyContinue }
	}
	if ($mismatch) {
		Add-Finding 'bugs' 'fuzz-exec' 'review' 'mismatch' "$mismatch of $count generated scripts print other output than bash; triage fuzz-exec/*.diff (minimise, then add a diff/ case or fix)" $dir
	}
	$status = if ($bad) { 'fail' } else { 'pass' }
	Add-Case 'bugs' 'fuzz-exec' $status @{ inputs = $count; bad = $bad; mismatches = $mismatch; seed = $Seed + 1 }
	Write-Status $status ("{0,-34} {1} programs (seed {2}), {3} crashed or hung, {4} differ from bash" -f 'fuzz-exec', $count, ($Seed + 1), $bad, $mismatch)
}

function Invoke-BugsSuite {
	Write-Host "`n== bugs" -ForegroundColor Cyan
	foreach ($f in Get-CaseFiles 'bugs' 'robust') { Invoke-CheckCase 'bugs' $f }
	foreach ($f in Get-CaseFiles 'bugs' 'diff') { Invoke-DiffCase $f }
	Invoke-CorpusReplay
	if ($Filter -eq '*' -or 'fuzz' -like $Filter) {
		Invoke-ParseFuzz
		Invoke-ExecFuzz
	}
}

# ---------------------------------------------------------------- security / ux

# A suite may hold PowerShell cases too: each gets a context and returns
# objects with Severity, Text and optionally Artifact.
function Invoke-PsCases([string]$suite) {
	foreach ($file in Get-CaseFiles $suite '' '*.ps1') {
		$ctx = @{ Repo = $RepoRoot; Wbsh = $WbshExe; OutDir = $OutDir }
		$items = @(& $file.FullName -Context $ctx)
		$fails = 0
		foreach ($i in $items) {
			$kind = if ($i.Severity -eq 'review') { 'review' } else { 'fail' }
			if ($kind -eq 'fail') { $fails++ }
			Add-Finding $suite $file.BaseName $i.Severity $kind $i.Text $i.Artifact
		}
		$status = if ($fails) { 'fail' } else { 'pass' }
		Add-Case $suite $file.BaseName $status @{ items = $items.Count }
		Write-Status $status ("{0,-34} {1} item(s) to look at" -f $file.BaseName, $items.Count)
	}
}

function Invoke-CheckSuite([string]$suite) {
	Write-Host "`n== $suite" -ForegroundColor Cyan
	foreach ($f in Get-CaseFiles $suite) { Invoke-CheckCase $suite $f }
	Invoke-PsCases $suite
}

# ---------------------------------------------------------------- report

$SeverityOrder = @{ critical = 0; high = 1; medium = 2; low = 3; review = 4 }

function Write-Report([string]$version) {
	$failed = @($Cases | Where-Object Status -eq 'fail').Count
	$json = [ordered]@{
		stamp = $Stamp; mode = $Mode; seed = $Seed; machine = $env:COMPUTERNAME; wbsh = $WbshExe; version = $version
		reference = $BashExe; cases = $Cases; findings = $Findings
	}
	$json | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutDir 'report.json')

	$md = [System.Collections.Generic.List[string]]::new()
	$md.Add("# wbsh stress report $Stamp")
	$md.Add('')
	$md.Add("- wbsh: ``$version`` ($WbshExe)")
	$md.Add("- mode: $Mode, seed $Seed, machine $env:COMPUTERNAME")
	$md.Add("- reference shell: $(if ($BashExe) { $BashExe } else { 'none' })")
	$md.Add('')
	$md.Add('| suite | cases | pass | fail | skip |')
	$md.Add('|---|---:|---:|---:|---:|')
	foreach ($g in $Cases | Group-Object Suite) {
		$c = $g.Group
		$md.Add("| $($g.Name) | $($c.Count) | $(@($c | ? Status -eq 'pass').Count) | $(@($c | ? Status -eq 'fail').Count) | $(@($c | ? Status -eq 'skip').Count) |")
	}

	$perf = @($Cases | Where-Object { $_.Suite -eq 'perf' -and $_.Metrics.wbsh })
	if ($perf) {
		$md.Add(''); $md.Add('## Performance (median wall time)'); $md.Add('')
		$md.Add('| case | wbsh ms | cpu ms | peak MB | processes | bash ms | wbsh/bash | baseline ms |')
		$md.Add('|---|---:|---:|---:|---:|---:|---:|---:|')
		foreach ($c in $perf) {
			$m = $c.Metrics
			$md.Add("| $($c.Case) | $($m.wbsh.MedianMs) | $($m.wbsh.CpuMs) | $($m.wbsh.PeakMB) | $($m.wbsh.Processes) | $($m.bash.MedianMs) | $($m.ratio) | $($m.baselineMs) |")
		}
	}

	$mem = @($Cases | Where-Object { $_.Suite -eq 'memory' -and $_.Metrics.largeMB })
	if ($mem) {
		$md.Add(''); $md.Add('## Memory (root process peak private bytes)'); $md.Add('')
		$md.Add('| case | N | at N MB | at 10N MB | bytes/iter | tree peak MB | handles N -> 10N |')
		$md.Add('|---|---:|---:|---:|---:|---:|---|')
		foreach ($c in $mem) {
			$m = $c.Metrics
			$md.Add("| $($c.Case) | $($m.iters) | $($m.smallMB) | $($m.largeMB) | $($m.bytesPerIter) | $($m.jobPeakMB) | $($m.handlesSmall) -> $($m.handlesLarge) |")
		}
	}

	$md.Add(''); $md.Add('## Findings'); $md.Add('')
	$sorted = $Findings | Sort-Object { $SeverityOrder[$_.Severity] }, Suite, Case
	if (-not $sorted) { $md.Add('None.') }
	else {
		$md.Add('| severity | suite | case | kind | finding | artifact |')
		$md.Add('|---|---|---|---|---|---|')
		foreach ($f in $sorted) {
			$t = $f.Text -replace '\|', '\|'
			$md.Add("| $($f.Severity) | $($f.Suite) | $($f.Case) | $($f.Kind) | $t | $($f.Artifact) |")
		}
	}
	Set-Content -Path (Join-Path $OutDir 'report.md') -Value $md
	return $failed
}

function Save-Baseline([string]$version) {
	$data = [ordered]@{ machine = $env:COMPUTERNAME; version = $version; stamp = $Stamp; mode = $Mode; perf = [ordered]@{}; memory = [ordered]@{} }
	if ($Baseline) {
		foreach ($k in $Baseline.perf.Keys) { $data.perf[$k] = $Baseline.perf[$k] }
		foreach ($k in $Baseline.memory.Keys) { $data.memory[$k] = $Baseline.memory[$k] }
	}
	foreach ($c in $Cases | Where-Object { $_.Suite -eq 'perf' -and $_.Metrics.wbsh }) {
		$data.perf[$c.Case] = @{ MedianMs = $c.Metrics.wbsh.MedianMs; PeakMB = $c.Metrics.wbsh.PeakMB }
	}
	foreach ($c in $Cases | Where-Object { $_.Suite -eq 'memory' -and $_.Metrics.largeMB }) {
		$data.memory[$c.Case] = @{ largeMB = $c.Metrics.largeMB }
	}
	New-Item -ItemType Directory -Path (Split-Path $BaselinePath) -Force | Out-Null
	$data | ConvertTo-Json -Depth 5 | Set-Content $BaselinePath
	Write-Host "baseline written: $BaselinePath"
}

# ---------------------------------------------------------------- main

if ($List) {
	foreach ($s in $Suite) {
		Get-ChildItem (Join-Path $SuitesDir $s) -Recurse -File -Include *.sh, *.ps1 |
			Where-Object { $_.Name -notlike '*.setup.sh' } |
			ForEach-Object { '{0,-9} {1}' -f $s, [IO.Path]::GetRelativePath((Join-Path $SuitesDir $s), $_.FullName) }
	}
	exit 0
}

New-Item -ItemType Directory -Path (Join-Path $OutDir 'logs') -Force | Out-Null
$version = (& $WbshExe --version 2>&1 | Select-Object -First 1) -as [string]
Write-Host "wbsh stress  mode=$Mode  seed=$Seed  exe=$WbshExe"
Write-Host "             $version"
Write-Host "             reference: $(if ($BashExe) { $BashExe } else { 'none' })"
Write-Host "             results:   $OutDir"

# An ASan build ends with exit code 1 on a report, which looks like a normal
# failure. Every process of the run (children inherit this) writes its report
# to a file instead, and each file becomes a finding at the end.
$AsanDir = $null
if (Get-ChildItem (Split-Path $WbshExe) -Filter 'clang_rt.asan*.dll' -ErrorAction SilentlyContinue) {
	$AsanDir = Join-Path $OutDir 'asan'
	New-Item -ItemType Directory -Path $AsanDir -Force | Out-Null
	$env:ASAN_OPTIONS = "log_path='$(ConvertTo-Slash $AsanDir)/report':detect_leaks=0"
	Write-Host "             ASan build: reports go to $AsanDir"
}

$clock = [Diagnostics.Stopwatch]::StartNew()
foreach ($s in $Suite) {
	switch ($s) {
		'perf' { Invoke-PerfSuite }
		'memory' { Invoke-MemorySuite }
		'bugs' { Invoke-BugsSuite }
		default { Invoke-CheckSuite $s }
	}
}

if ($AsanDir) {
	$reports = @(Get-ChildItem $AsanDir -File)
	foreach ($r in $reports) {
		$text = Get-Content $r.FullName
		$what = ($text | Where-Object { $_ -match 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' } | Select-Object -First 1) ?? 'AddressSanitizer report'
		Add-Finding 'asan' $r.Name 'critical' 'asan' $what.Trim() $r.FullName
	}
	Add-Case 'asan' 'reports' ($reports ? 'fail' : 'pass') @{ reports = $reports.Count }
	Write-Status ($reports ? 'fail' : 'pass') "AddressSanitizer: $($reports.Count) report(s)"
}

$failed = Write-Report $version
if ($UpdateBaseline) { Save-Baseline $version }

$bySeverity = $Findings | Group-Object Severity | ForEach-Object { "$($_.Count) $($_.Name)" }
Write-Host ''
Write-Host ("{0} cases, {1} failed, findings: {2}  ({3:N0} s)" -f $Cases.Count, $failed, (($bySeverity -join ', ') -replace '^$', 'none'), $clock.Elapsed.TotalSeconds)
Write-Host "report: $(Join-Path $OutDir 'report.md')"
exit ([int]($failed -gt 0))
