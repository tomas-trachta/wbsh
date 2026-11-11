# Static review of the C++ sources and the built binary.
# Returns 'review' items: places a human must look at before a release.
# A missing exploit mitigation in wbsh.exe is a 'high' failure.
param($Context)

$src = Join-Path $Context.Repo 'wbsh\src'
$files = Get-ChildItem $src -File -Include *.cpp, *.h -Recurse
$out = Join-Path $Context.OutDir 'logs\security-static_scan.txt'
$lines = [System.Collections.Generic.List[string]]::new()

$rules = @(
	@{ Id = 'process-spawn'; Pattern = '\bCreateProcess[AW]?\s*\(|ShellExecute'; Why = 'every process start: check the application name is resolved by wbsh, not by the CreateProcess search order, and that arguments are quoted' }
	@{ Id = 'cmd-exe'; Pattern = 'cmd\.exe|ComSpec|COMSPEC'; Why = 'cmd.exe re-parses its command line; check quoting of every argument passed through it' }
	@{ Id = 'crt-shell'; Pattern = '\b_?w?system\s*\(|\b_w?popen\s*\('; Why = 'CRT shell calls run cmd.exe' }
	@{ Id = 'unsafe-crt'; Pattern = '\b(strcpy|strcat|sprintf|vsprintf|gets|wcscpy|wcscat|swprintf|_snprintf)\s*\('; Why = 'unbounded C string functions' }
	@{ Id = 'dll-load'; Pattern = 'LoadLibrary(Ex)?[AW]?\s*\('; Why = 'check every DLL is loaded by full path' }
	@{ Id = 'named-pipe'; Pattern = 'CreateNamedPipe[AW]?\s*\('; Why = 'check the DACL and PIPE_REJECT_REMOTE_CLIENTS; other local users must not connect' }
	@{ Id = 'shared-memory'; Pattern = 'CreateFileMapping[AW]?\s*\(|CreateEvent[AW]?\s*\(\s*[^,]+,[^,]+,[^,]+,\s*L?"'; Why = 'named kernel objects: check who else can open them (signal mailboxes)' }
	@{ Id = 'tls-ignore'; Pattern = 'SECURITY_FLAG_IGNORE|WINHTTP_OPTION_SECURITY_FLAGS'; Why = 'certificate checks turned off: only for -k / --no-check-certificate' }
	@{ Id = 'listen-any'; Pattern = '"0\.0\.0\.0"|INADDR_ANY|in6addr_any'; Why = 'network servers that bind every interface by default (httpd, nc -l): a firewall prompt and LAN exposure' }
	@{ Id = 'env-code'; Pattern = '"(WBSH_FUNCTIONS|WBSH_INIT_COMMAND|WBSH_ALIASES|WBSH_TRAPS|BASH_ENV|ENV)"'; Why = 'environment variables that become code in a new shell' }
	@{ Id = 'privilege'; Pattern = 'runas|ShellExecuteEx|CreateProcessWithLogon|LogonUser|TOKEN_ADJUST_PRIVILEGES'; Why = 'elevation (sudo / su): check no command line or environment is trusted from a lower level' }
	@{ Id = 'temp-path'; Pattern = 'GetTempPath|GetTempFileName|tmpnam|_tempnam'; Why = 'temporary files: check exclusive creation and cleanup' }
	@{ Id = 'git-run'; Pattern = '--no-optional-locks status'; Why = 'git runs in whatever repository the user enters; hostile .git/config can run code (core.fsmonitor)' }
)

$items = foreach ($rule in $rules) {
	$hits = @($files | Select-String -Pattern $rule.Pattern -CaseSensitive)
	if (-not $hits) { continue }
	$lines.Add("== $($rule.Id): $($rule.Why)")
	foreach ($h in $hits) { $lines.Add(('  {0}:{1}: {2}' -f $h.Filename, $h.LineNumber, $h.Line.Trim())) }
	[pscustomobject]@{
		Severity = 'review'
		Text = "$($rule.Id): $($hits.Count) place(s) in $(@($hits | Select-Object -Unique Filename).Count) file(s) -- $($rule.Why)"
		Artifact = $out
	}
}

if (-not ($files | Select-String -Pattern 'SetDefaultDllDirectories' -Quiet)) {
	[pscustomobject]@{ Severity = 'review'; Text = 'SetDefaultDllDirectories is never called: implicit DLL loads of spawned helpers use the default search order'; Artifact = '' }
}

# PE header: DllCharacteristics of wbsh.exe.
$bytes = [IO.File]::ReadAllBytes($Context.Wbsh)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$opt = $pe + 24
$dllChar = [BitConverter]::ToUInt16($bytes, $opt + 70)
$flags = [ordered]@{ HIGH_ENTROPY_VA = 0x20; DYNAMIC_BASE = 0x40; NX_COMPAT = 0x100; GUARD_CF = 0x4000 }
foreach ($f in $flags.Keys) {
	$set = ($dllChar -band $flags[$f]) -ne 0
	$lines.Add("wbsh.exe $f = $set")
	if (-not $set) {
		$sev = if ($f -eq 'GUARD_CF') { 'review' } else { 'high' }
		[pscustomobject]@{ Severity = $sev; Text = "wbsh.exe is built without $f"; Artifact = $out }
	}
}

$items
Set-Content -Path $out -Value $lines
