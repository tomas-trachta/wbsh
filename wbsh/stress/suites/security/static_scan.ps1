# Static review of the wbsh sources and wbsh.exe (rules shared in tools/stress/StaticScan.ps1).
param($Context)

. (Join-Path $Context.Repo 'tools\stress\StaticScan.ps1')

$rules = @(
	@{ Id = 'tls-ignore'; Pattern = 'SECURITY_FLAG_IGNORE|WINHTTP_OPTION_SECURITY_FLAGS'; Why = 'certificate checks turned off: only for -k / --no-check-certificate' }
	@{ Id = 'listen-any'; Pattern = '"0\.0\.0\.0"|INADDR_ANY|in6addr_any'; Why = 'network servers that bind every interface by default (httpd, nc -l): a firewall prompt and LAN exposure' }
	@{ Id = 'env-code'; Pattern = '"(WBSH_FUNCTIONS|WBSH_INIT_COMMAND|WBSH_ALIASES|WBSH_TRAPS|BASH_ENV|ENV)"'; Why = 'environment variables that become code in a new shell' }
	@{ Id = 'privilege'; Pattern = 'runas|ShellExecuteEx|CreateProcessWithLogon|LogonUser|TOKEN_ADJUST_PRIVILEGES'; Why = 'elevation (sudo / su): check no command line or environment is trusted from a lower level' }
	@{ Id = 'git-run'; Pattern = '--no-optional-locks status'; Why = 'git runs in whatever repository the user enters; hostile .git/config can run code (core.fsmonitor)' }
)

Invoke-StaticScan -Sources (Join-Path $Context.Repo 'wbsh\src') -Binaries $Context.Wbsh `
	-OutFile (Join-Path $Context.OutDir 'logs\security-static_scan.txt') -ExtraRules $rules
