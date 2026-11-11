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
# copied from a web page can hold ESC[201~ followed by a command.
$keymap = Get-Content (Join-Path $src 'keymap.cpp') -Raw
if ($keymap -match '(?s)std::string encodePaste\(.*?\n\t\}') {
	$body = $Matches[0]
	$filters = $body -match "\x1b|\033|0x1[bB]|'\e'|27\)|201~.*(erase|replace|find)"
	[pscustomobject]@{
		Ok = $filters; Severity = 'high'; Artifact = ''
		Text = 'encodePaste does not remove ESC (or ESC[201~) from pasted text: a paste can close bracketed paste early and run the rest as typed commands'
	}
}

$links = Get-Content (Join-Path $src 'screen_vt.cpp') -Raw
[pscustomobject]@{
	Ok = ($links -match 'linkIsSafeToOpen' -and $links -match '"https://"'); Severity = 'high'; Artifact = ''
	Text = 'hyperlinks are opened without a scheme allowlist'
}

$conpty = Join-Path $Context.Repo 'wbshterm\conpty'
if (-not (Get-ChildItem $conpty -Filter '*.sha256' -ErrorAction SilentlyContinue) -and -not (Select-String -Path (Join-Path $conpty '*') -Pattern 'sha256|SHA256' -Quiet -ErrorAction SilentlyContinue)) {
	[pscustomobject]@{ Ok = $false; Severity = 'review'; Artifact = ''
		Text = 'the vendored conpty.dll / OpenConsole.exe have no recorded SHA-256; record the hash next to the version so a swapped binary is seen' }
}
