# Static review of the wbshterm sources and binaries, with terminal-specific rules.
param($Context)

. (Join-Path $Context.Repo 'tools\stress\StaticScan.ps1')
$src = Join-Path $Context.Repo 'wbshterm\src'
$out = Join-Path $Context.OutDir 'logs\security-static_scan.txt'

$rules = @(
	@{ Id = 'clipboard-write'; Pattern = 'clipboardWrite|SetClipboardData'; Why = 'OSC 52 lets any program output (cat of a file, ssh) replace the clipboard; consider a setting or a prompt' }
	@{ Id = 'open-link'; Pattern = 'ShellExecuteW?\('; Why = 'opening links or folders: check the scheme allowlist (linkIsSafeToOpen) covers every call' }
	@{ Id = 'lua-libs'; Pattern = 'luaL_openlibs'; Why = 'config scripts get the full Lua library (os.execute, io.popen); fine for the user''s own file, but never load a script from a folder others can write' }
	@{ Id = 'scrollback-disk'; Pattern = 'GetTempFileName'; Why = 'terminal output written to disk' }
)
Invoke-StaticScan -Sources $src -Binaries @($Context.Terminal, (Join-Path (Split-Path $Context.Terminal) 'OpenConsole.exe'), (Join-Path (Split-Path $Context.Terminal) 'conpty.dll')) `
	-OutFile $out -ExtraRules $rules | ForEach-Object { $_ }

# Bracketed paste must not let the pasted text end the bracket itself: text
# copied from a web page can hold ESC[201~ followed by a command. The
# behaviour is checked by the built-in self-test (bugs suite); this only
# makes sure that check still exists.
$selftest = Get-Content (Join-Path $src 'selftest.cpp') -Raw
[pscustomobject]@{
	Ok = ($selftest -match 'pasted text cannot end bracketed paste'); Severity = 'high'; Artifact = ''
	Text = 'no self-test check that pasted text cannot end bracketed paste (ESC[201~ inside a paste)'
}

$links = Get-Content (Join-Path $src 'screen_vt.cpp') -Raw
[pscustomobject]@{
	Ok = ($links -match 'linkIsSafeToOpen' -and $links -match '"https://"'); Severity = 'high'; Artifact = ''
	Text = 'hyperlinks are opened without a scheme allowlist'
}

# The vendored ConPTY binaries: signed by Microsoft, and the files that
# conpty\fetch.ps1 recorded in VERSION.txt (a swapped binary shows here).
$conpty = Join-Path $Context.Repo 'wbshterm\conpty'
$recorded = Get-Content (Join-Path $conpty 'VERSION.txt') -ErrorAction SilentlyContinue
foreach ($name in 'conpty.dll', 'OpenConsole.exe') {
	$file = Join-Path $conpty $name
	$signature = Get-AuthenticodeSignature $file
	[pscustomobject]@{
		Ok = $signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'CN=Microsoft Corporation'
		Severity = 'high'; Artifact = $file
		Text = "vendored $name has no valid Microsoft signature ($($signature.Status))"
	}

	$hash = (Get-FileHash $file -Algorithm SHA256).Hash
	[pscustomobject]@{
		Ok = [bool]($recorded -match "^SHA256 $hash  $([regex]::Escape($name))$")
		Severity = 'high'; Artifact = $file
		Text = "vendored $name does not match the SHA-256 in conpty\VERSION.txt (run conpty\fetch.ps1)"
	}
}
