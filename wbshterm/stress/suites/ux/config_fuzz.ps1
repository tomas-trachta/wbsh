# Broken settings files: the terminal still starts with defaults and does not crash.
# Each file is used for a snapshot session (the real config loader and theme code).
param($Context)

$term = $Context.Terminal
$work = $Context.Work
$log = Join-Path $Context.OutDir 'logs\ux-config_fuzz.log'
$rng = [System.Random]::new(11)
$keys = 'theme', 'font', 'font_size', 'scrollback_lines', 'scrollback_disk_mb', 'cursor', 'opacity', 'padding', 'columns', 'rows', 'bell', 'background', 'foreground', 'palette0', 'status_bar'
$values = '', '-1', '0', '99999999999999999999', 'NaN', 'yes', '#', '#zzzzzz', '#FFF', 'rgb(1,2,3)', ('x' * 5000), '"unterminated', '=', ';;;', [string][char]0, 'Mocha', '1e309'

$configs = @{
	'empty' = ''
	'binary' = -join (1..2000 | ForEach-Object { [char]$rng.Next(1, 256) })
	'huge_values' = ($keys | ForEach-Object { "$_ = 99999999999999999999" }) -join "`n"
	'negative' = ($keys | ForEach-Object { "$_ = -2147483648" }) -join "`n"
	'no_values' = ($keys | ForEach-Object { "$_ =" }) -join "`n"
	'long_line' = 'theme = ' + ('A' * 1000000)
}
for ($i = 0; $i -lt 8; $i++) {
	$configs["random$i"] = (1..40 | ForEach-Object { "$($keys[$rng.Next($keys.Count)]) = $($values[$rng.Next($values.Count)])" }) -join "`n"
}

foreach ($name in $configs.Keys | Sort-Object) {
	$conf = Join-Path $work "$name.conf"
	[IO.File]::WriteAllText($conf, $configs[$name])
	$png = Join-Path $work "$name.png"
	$r = Invoke-Probe -Argv @($term, '--snapshot', $png, '--config', $conf, '--feed', 'echo cfg-ok\r', '--size', '80x20', '--delay', '300', '--settle', '400') `
		-WorkDir $work -Output "$log.$name" -TimeoutSec 20 -Env @{ HOME = $work }
	$ok = -not $r.Crashed -and -not $r.TimedOut -and $r.ExitCode -eq 0 -and (Test-Path $png)
	$what = if ($r.Crashed) { "crashes ($($r.CrashName))" } elseif ($r.TimedOut) { 'hangs' } else { "exit $($r.ExitCode)" }
	[pscustomobject]@{ Ok = $ok; Severity = ($r.Crashed ? 'high' : 'medium'); Artifact = $conf; Text = "settings file '$name' stops the snapshot: $what" }
}
