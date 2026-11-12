# Shared parts of the stress drivers (wbsh/stress/stress.ps1,
# wbshterm/stress/stress.ps1): the process probe, case discovery, findings,
# TAP parsing, report and baseline files. Dot-source it from a driver.
#
# The driver must define, before it calls these functions:
#     $OutDir      results folder of this run
#     $SuitesDir   folder with one sub-folder per suite
#     $Filter      wildcard on case names
#     $KeepWork    [switch] keep the work folders of passed cases
#     $Product     name used in crash messages ('wbsh', 'wbshterm')
#     $Findings, $Cases   from New-StressState

if (-not ('WbshStress.ProcessProbe' -as [type])) {
	Add-Type -Path (Join-Path $PSScriptRoot 'ProcessProbe.cs')
}

$SeverityOrder = @{ critical = 0; high = 1; medium = 2; low = 3; review = 4 }

function New-StressState {
	$script:Findings = [System.Collections.Generic.List[object]]::new()
	$script:Cases = [System.Collections.Generic.List[object]]::new()
}

function ConvertTo-Slash([string]$path) { return $path -replace '\\', '/' }

function Get-MachineBaselinePath([string]$root) {
	return Join-Path $root "baselines\$($env:COMPUTERNAME.ToLower()).json"
}

function Read-Baseline([string]$path) {
	if (Test-Path $path) { return Get-Content $path -Raw | ConvertFrom-Json -AsHashtable }
	return $null
}

function Get-CaseFiles([string]$suite, [string]$sub = '', [string]$pattern = '*.sh') {
	$dir = Join-Path $SuitesDir $suite
	if ($sub) { $dir = Join-Path $dir $sub }
	if (-not (Test-Path $dir)) { return @() }
	return Get-ChildItem $dir -File -Filter $pattern |
		Where-Object { $_.Name -notlike '*.setup.*' -and (Get-CaseName $_) -like $Filter } |
		Sort-Object Name
}

# The case name without the compound extensions a case may carry (x.vt.ps1 -> x).
function Get-CaseName([System.IO.FileInfo]$file) {
	return ($file.Name -split '\.')[0]
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

# Runs argv under the probe. Output and error go to one file.
function Invoke-Probe {
	param([string[]]$Argv, [string]$WorkDir, [string]$Output, [int]$TimeoutSec = 120,
		[hashtable]$Env = @{}, [switch]$Samples, [long]$MemoryLimit = 4GB)

	$opts = [WbshStress.ProbeOptions]::new()
	$opts.CommandLine = [WbshStress.ProcessProbe]::JoinArgs([string[]]$Argv)
	$opts.WorkDir = $WorkDir
	$opts.StdoutPath = $Output
	$opts.StderrPath = $Output
	$opts.TimeoutMs = $TimeoutSec * 1000
	$opts.KeepSamples = [bool]$Samples
	$opts.MemoryLimitBytes = $MemoryLimit
	$opts.Environment = [System.Collections.Hashtable]$Env
	return [WbshStress.ProcessProbe]::Run($opts)
}

function Add-Finding([string]$suite, [string]$case, [string]$severity, [string]$kind, [string]$text, [string]$artifact = '') {
	$rel = ''
	if ($artifact) { $rel = ([IO.Path]::GetRelativePath($OutDir, $artifact)).Replace('\', '/') }
	$Findings.Add([pscustomobject]@{
		Suite = $suite; Case = $case; Severity = $severity; Kind = $kind; Text = $text; Artifact = $rel
	})
}

function Add-Case([string]$suite, [string]$name, [string]$status, [hashtable]$metrics = @{}) {
	$Cases.Add([pscustomobject]@{ Suite = $suite; Case = $name; Status = $status; Metrics = $metrics })
}

# A crash or a hang is a finding whatever the case expected.
function Test-ProbeHealth($r, [string]$suite, [string]$case, [string]$log) {
	if ($r.Crashed) {
		Add-Finding $suite $case 'critical' 'crash' "$Product crashed: $($r.CrashName) (exit 0x$($r.ExitCode.ToString('X8')))" $log
		return $false
	}
	if ($r.TimedOut) {
		Add-Finding $suite $case 'high' 'hang' "timed out after $([int]($r.WallMs / 1000)) s; process tree killed" $log
		return $false
	}
	if ($r.LeftoverProcesses -gt 0) {
		Add-Finding $suite $case 'medium' 'orphan' "$($r.LeftoverProcesses) process(es) still alive after $Product exited" $log
	}
	return $true
}

# Turns the TAP lines of a check case (see wbsh/stress/lib/check.sh) into findings.
function Read-TapLog([string]$suite, [string]$case, [string]$log) {
	$result = @{ Passed = 0; Failed = 0 }
	$text = if (Test-Path $log) { Get-Content $log } else { @() }
	foreach ($line in $text) {
		if ($line -match '^not ok - (?:\[(critical|high|medium|low)\] )?(.*)$') {
			$sev = if ($Matches[1]) { $Matches[1] } else { 'medium' }
			Add-Finding $suite $case $sev 'fail' $Matches[2] $log
			$result.Failed++
		} elseif ($line -match '^ok - ') {
			$result.Passed++
		} elseif ($line -match '^# review: (.*)$') {
			Add-Finding $suite $case 'review' 'review' $Matches[1] $log
		}
	}
	return $result
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
	Write-Host ('  {0,-5} {1}' -f $status, $text) -ForegroundColor $color
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

# An ASan build ends with exit code 1 on a report, which looks like a normal
# failure. Every process of the run (children inherit this) writes its report
# to a file instead, and Add-AsanFindings turns each file into a finding.
function Enable-AsanReports([string]$exe) {
	if (-not (Get-ChildItem (Split-Path $exe) -Filter 'clang_rt.asan*.dll' -ErrorAction SilentlyContinue)) { return $null }
	$dir = Join-Path $OutDir 'asan'
	New-Item -ItemType Directory -Path $dir -Force | Out-Null
	$env:ASAN_OPTIONS = "log_path='$(ConvertTo-Slash $dir)/report':detect_leaks=0"
	Write-Host "             ASan build: reports go to $dir"
	return $dir
}

function Add-AsanFindings([string]$dir) {
	if (-not $dir) { return }
	$reports = @(Get-ChildItem $dir -File)
	foreach ($r in $reports) {
		$text = Get-Content $r.FullName
		$what = ($text | Where-Object { $_ -match 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' } | Select-Object -First 1) ?? 'AddressSanitizer report'
		# A case that asks for gigabytes on purpose runs ASan's own allocator
		# out of memory; that is no memory-safety error.
		$oom = $what -match 'out of memory|allocation-size-too-big'
		Add-Finding 'asan' $r.Name ($oom ? 'review' : 'critical') ($oom ? 'asan-oom' : 'asan') $what.Trim() $r.FullName
	}
	$unsafe = @($Findings | Where-Object { $_.Suite -eq 'asan' -and $_.Kind -eq 'asan' })
	$status = if ($unsafe) { 'fail' } else { 'pass' }
	Add-Case 'asan' 'reports' $status @{ reports = $reports.Count }
	Write-Status $status "AddressSanitizer: $($reports.Count) report(s)"
}

# Writes report.json and report.md. $Sections are extra Markdown blocks
# (lists of lines) the driver builds, such as its perf and memory tables.
function Write-Report([string]$Title, [string[]]$Header, [object[]]$Sections = @(), [hashtable]$Extra = @{}) {
	$json = [ordered]@{ title = $Title; header = $Header; cases = $Cases; findings = $Findings }
	foreach ($k in $Extra.Keys) { $json[$k] = $Extra[$k] }
	$json | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutDir 'report.json')

	$md = [System.Collections.Generic.List[string]]::new()
	$md.Add("# $Title")
	$md.Add('')
	foreach ($h in $Header) { $md.Add("- $h") }
	$md.Add('')
	$md.Add('| suite | cases | pass | fail | skip |')
	$md.Add('|---|---:|---:|---:|---:|')
	foreach ($g in $Cases | Group-Object Suite) {
		$c = $g.Group
		$pass = @($c | Where-Object Status -eq 'pass').Count
		$fail = @($c | Where-Object Status -eq 'fail').Count
		$skip = @($c | Where-Object Status -eq 'skip').Count
		$md.Add("| $($g.Name) | $($c.Count) | $pass | $fail | $skip |")
	}
	foreach ($section in $Sections) {
		if (-not $section) { continue }
		$md.Add('')
		foreach ($line in $section) { $md.Add($line) }
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
	return @($Cases | Where-Object Status -eq 'fail').Count
}

# Merges this run's numbers into the machine's baseline file.
# $Sections maps a baseline section name to { case name -> numbers }.
function Save-Baseline([string]$path, $old, [hashtable]$Sections, [string]$version, [string]$stamp) {
	$data = [ordered]@{ machine = $env:COMPUTERNAME; version = $version; stamp = $stamp }
	foreach ($name in $Sections.Keys) {
		$merged = [ordered]@{}
		if ($old -and $old[$name]) { foreach ($k in $old[$name].Keys) { $merged[$k] = $old[$name][$k] } }
		foreach ($k in $Sections[$name].Keys) { $merged[$k] = $Sections[$name][$k] }
		$data[$name] = $merged
	}
	New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
	$data | ConvertTo-Json -Depth 5 | Set-Content $path
	Write-Host "baseline written: $path"
}

# Runs one suite. A bug in a case or in the driver becomes a finding, and
# the run goes on with the next suite instead of ending without a report.
function Invoke-Suite([string]$suite, [scriptblock]$body) {
	try {
		& $body
	} catch {
		$where = ($_.ScriptStackTrace -split "`n" | Select-Object -First 2) -join ' <- '
		Add-Finding $suite 'driver' 'high' 'driver-error' "the $suite suite stopped: $($_.Exception.Message) at $where"
		Add-Case $suite 'driver' 'fail'
		Write-Status 'fail' "$suite suite stopped: $($_.Exception.Message)"
	}
}

function Write-Summary([int]$failed, [System.Diagnostics.Stopwatch]$clock) {
	$bySeverity = $Findings | Group-Object Severity | ForEach-Object { "$($_.Count) $($_.Name)" }
	$text = if ($bySeverity) { $bySeverity -join ', ' } else { 'none' }
	Write-Host ''
	Write-Host ("{0} cases, {1} failed, findings: {2}  ({3:N0} s)" -f $Cases.Count, $failed, $text, $clock.Elapsed.TotalSeconds)
	Write-Host "report: $(Join-Path $OutDir 'report.md')"
}
