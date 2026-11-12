# Static security review shared by the wbsh and wbshterm security suites.
# Dot-source it, then call Invoke-StaticScan. It returns finding objects
# (Severity, Text, Artifact): 'review' for places a human must look at,
# 'high' for a missing exploit mitigation in a shipped binary.

$CommonScanRules = @(
	@{ Id = 'process-spawn'; Pattern = '\bCreateProcess[AW]?\s*\(|ShellExecute'; Why = 'every process start: check the application name is resolved by us, not by the CreateProcess search order, and that arguments are quoted' }
	@{ Id = 'cmd-exe'; Pattern = 'cmd\.exe|ComSpec|COMSPEC'; Why = 'cmd.exe re-parses its command line; check quoting of every argument passed through it' }
	@{ Id = 'crt-shell'; Pattern = '\b_?w?system\s*\(|\b_w?popen\s*\('; Why = 'CRT shell calls run cmd.exe' }
	@{ Id = 'unsafe-crt'; Pattern = '\b(strcpy|strcat|sprintf|vsprintf|gets|wcscpy|wcscat|swprintf|_snprintf)\s*\('; Why = 'unbounded C string functions' }
	@{ Id = 'dll-load'; Pattern = 'LoadLibrary(Ex)?[AW]?\s*\('; Why = 'check every DLL is loaded by full path' }
	@{ Id = 'named-pipe'; Pattern = 'CreateNamedPipe[AW]?\s*\('; Why = 'check the DACL and PIPE_REJECT_REMOTE_CLIENTS; other local users must not connect' }
	@{ Id = 'shared-memory'; Pattern = 'CreateFileMapping[AW]?\s*\(|CreateEvent[AW]?\s*\(\s*[^,]+,[^,]+,[^,]+,\s*L?"'; Why = 'named kernel objects: check who else can open them' }
	@{ Id = 'temp-path'; Pattern = 'GetTempPath|GetTempFileName|tmpnam|_tempnam'; Why = 'temporary files: check exclusive creation, cleanup, and what data they hold' }
	@{ Id = 'throwing-parse'; Pattern = '\bstd::sto(i|l|ll|ul|ull|f|d)\s*\('; Why = 'std::stoi and friends throw on bad input; uncaught, that ends the process' }
)

function Invoke-StaticScan {
	param([string[]]$Sources, [string[]]$Binaries, [string]$OutFile, [object[]]$ExtraRules = @())

	$files = Get-ChildItem $Sources -File -Include *.cpp, *.h, *.c -Recurse
	$lines = [System.Collections.Generic.List[string]]::new()

	foreach ($rule in $CommonScanRules + $ExtraRules) {
		$hits = @($files | Select-String -Pattern $rule.Pattern -CaseSensitive)
		if (-not $hits) { continue }
		$lines.Add("== $($rule.Id): $($rule.Why)")
		foreach ($h in $hits) { $lines.Add(('  {0}:{1}: {2}' -f $h.Filename, $h.LineNumber, $h.Line.Trim())) }
		$sev = if ($rule.Severity) { $rule.Severity } else { 'review' }
		[pscustomobject]@{
			Severity = $sev
			Text = "$($rule.Id): $($hits.Count) place(s) in $(@($hits | Select-Object -Unique Filename).Count) file(s) -- $($rule.Why)"
			Artifact = $OutFile
		}
	}

	if (-not ($files | Select-String -Pattern 'SetDefaultDllDirectories' -Quiet)) {
		[pscustomobject]@{ Severity = 'review'; Text = 'SetDefaultDllDirectories is never called: implicit DLL loads use the default search order'; Artifact = '' }
	}

	foreach ($exe in $Binaries) { Test-PeMitigations $exe $lines $OutFile }
	Set-Content -Path $OutFile -Value $lines
}

# DllCharacteristics from the PE optional header.
function Test-PeMitigations([string]$exe, $lines, [string]$OutFile) {
	$name = Split-Path $exe -Leaf
	$bytes = [IO.File]::ReadAllBytes($exe)
	$pe = [BitConverter]::ToInt32($bytes, 0x3C)
	$dllChar = [BitConverter]::ToUInt16($bytes, $pe + 24 + 70)
	$flags = [ordered]@{ HIGH_ENTROPY_VA = 0x20; DYNAMIC_BASE = 0x40; NX_COMPAT = 0x100; GUARD_CF = 0x4000 }
	foreach ($f in $flags.Keys) {
		$set = ($dllChar -band $flags[$f]) -ne 0
		$lines.Add("$name $f = $set")
		if (-not $set) {
			$sev = if ($f -eq 'GUARD_CF') { 'review' } else { 'high' }
			[pscustomobject]@{ Severity = $sev; Text = "$name is built without $f"; Artifact = $OutFile }
		}
	}
}
