# VT stream helpers for the wbshterm stress suites. Dot-sourced by stress.ps1
# and by the generator cases (*.vt.ps1), so everything here is plain functions.

# Escape notation used by the .vt golden cases and the generators:
#     \e ESC   \a BEL   \b BS   \t TAB   \r CR   \n LF   \\ backslash
#     \xHH     one raw byte (may be invalid UTF-8 on purpose)
#     \u{H..}  one code point, written as UTF-8
# Everything else is literal UTF-8 text.
function ConvertFrom-VtNotation([string]$text) {
	$out = [System.IO.MemoryStream]::new()
	$utf8 = [System.Text.UTF8Encoding]::new($false)
	$i = 0
	while ($i -lt $text.Length) {
		$c = $text[$i]
		if ($c -ne '\' -or $i + 1 -ge $text.Length) {
			$len = 1
			if ([char]::IsHighSurrogate($c) -and $i + 1 -lt $text.Length) { $len = 2 }
			$b = $utf8.GetBytes($text.Substring($i, $len)); $out.Write($b, 0, $b.Length)
			$i += $len
			continue
		}
		$n = $text[$i + 1]
		$i += 2
		switch -CaseSensitive ($n) {
			'e' { $out.WriteByte(0x1B) }
			'a' { $out.WriteByte(0x07) }
			'b' { $out.WriteByte(0x08) }
			't' { $out.WriteByte(0x09) }
			'r' { $out.WriteByte(0x0D) }
			'n' { $out.WriteByte(0x0A) }
			'\' { $out.WriteByte(0x5C) }
			'x' { $out.WriteByte([Convert]::ToByte($text.Substring($i, 2), 16)); $i += 2 }
			'u' {
				$end = $text.IndexOf('}', $i)
				$cp = [Convert]::ToInt32($text.Substring($i + 1, $end - $i - 1), 16)
				$b = $utf8.GetBytes([char]::ConvertFromUtf32($cp)); $out.Write($b, 0, $b.Length)
				$i = $end + 1
			}
			default { throw "unknown escape \$n in VT notation" }
		}
	}
	return ,$out.ToArray()
}

# Reads a .vt case: '#' lines are comments (and `# @stress` meta), the
# other lines are joined with no separator; line breaks in the stream are
# written as \r\n explicitly.
function Read-VtCase([string]$path) {
	$body = (Get-Content $path -Encoding utf8 | Where-Object { $_ -notmatch '^#' }) -join ''
	return ConvertFrom-VtNotation $body
}

# A generator case returns a string (UTF-8 text, real ESC characters) or bytes.
function ConvertTo-StreamBytes($value) {
	if ($value -is [byte[]]) { return ,$value }
	return ,[System.Text.UTF8Encoding]::new($false).GetBytes([string]($value -join ''))
}

$script:E = [string][char]27

function Csi([string]$body) { return "$script:E[$body" }
function Osc([string]$body) { return "$script:E]$body$([char]7)" }

# ---------------------------------------------------------------- fuzzing

$script:FuzzNumbers = @(0, 1, 2, 3, 5, 7, 8, 24, 80, 255, 256, 999, 9999, 32767, 32768, 65535, 65536,
	2147483647, 2147483648, 4294967295, 99999999999999999999)
$script:EscFinals = @('7', '8', '=', '>', 'c', 'D', 'E', 'H', 'M', 'N', 'O', 'Z', '#8', '#3', '(0', ')B', '(B', '%G', ' F', '\')
$script:CsiFinals = '@ABCDEFGHIJKLMPSTXZ`abcdefghlmnqrstuxyz{|}~'.ToCharArray()
$script:Texts = @('a', 'Hello', ' ', ('x' * 200), "$([char]0x4F60)$([char]0x597D)", [char]::ConvertFromUtf32(0x1F600),
	"e$([char]0x301)$([char]0x302)$([char]0x303)", "$([char]::ConvertFromUtf32(0x1F468))$([char]0x200D)$([char]::ConvertFromUtf32(0x1F469))",
	"$([char]0xFEFF)", "$([char]0x200B)", "$([char]0x0E01)$([char]0x0E33)", "$([char]0x05D0)$([char]0x05D1)")

function Get-FuzzPick([System.Random]$rng, [object[]]$items) { return $items[$rng.Next($items.Count)] }

function New-FuzzParams([System.Random]$rng) {
	$count = $rng.Next(0, 5)
	$parts = for ($i = 0; $i -lt $count; $i++) {
		if ($rng.Next(6) -eq 0) { '' } else { "$(Get-FuzzPick $rng $script:FuzzNumbers)" }
	}
	$sep = if ($rng.Next(8) -eq 0) { ':' } else { ';' }
	return ($parts -join $sep)
}

function New-FuzzToken([System.Random]$rng) {
	switch ($rng.Next(16)) {
		0 { return Get-FuzzPick $rng $script:Texts }
		1 { return Get-FuzzPick $rng @("`r", "`n", "`t", "`b", [string][char]7, [string][char]5, [string][char]14, [string][char]15, [string][char]0x7F, [string][char]0) }
		2 { return Csi "$(New-FuzzParams $rng)$(Get-FuzzPick $rng $script:CsiFinals)" }
		3 { return Csi "?$(New-FuzzParams $rng)$(Get-FuzzPick $rng @('h', 'l', '$p', 'r', 's'))" }
		4 { return Csi "$(Get-FuzzPick $rng @('>', '=', '<', '!'))$(New-FuzzParams $rng)$(Get-FuzzPick $rng $script:CsiFinals)" }
		5 { return Csi "$(New-FuzzParams $rng)$(Get-FuzzPick $rng @(' q', '$p', '"p', "'}", ' @', '$x', '*x', '#{'))" }
		6 { return Osc "$(Get-FuzzPick $rng @('0', '1', '2', '4', '7', '8', '9', '10', '11', '12', '52', '104', '133', '633', '1337'));$(New-FuzzOscBody $rng)" }
		7 { return $script:E + (Get-FuzzPick $rng $script:EscFinals) }
		8 { return $script:E + 'P' + (Get-FuzzPick $rng @('$q', '+q', '1$r', 'q', '0;1|')) + (New-FuzzOscBody $rng) + $script:E + '\' }
		9 { return $script:E + (Get-FuzzPick $rng @('X', '^', '_')) + (New-FuzzOscBody $rng) + $script:E + '\' }
		10 { return Csi "$(Get-FuzzPick $rng @('1;1', '0;0', '9999;9999', '2;1', '1;2'))r" }
		11 { return "x" + (Csi "$(Get-FuzzPick $rng $script:FuzzNumbers)b") }
		12 { return Get-FuzzPick $rng @("$script:E[", "$script:E]", "$script:E" + 'P', "$script:E[?", "$script:E[1;") }
		13 { return [string][char]$rng.Next(0x80, 0x100) }
		14 { return Csi "$($rng.Next(1, 300));$($rng.Next(1, 300))H" }
		default { return "line $($rng.Next(1000))`r`n" }
	}
}

function New-FuzzOscBody([System.Random]$rng) {
	switch ($rng.Next(6)) {
		0 { return '' }
		1 { return 'x' * $rng.Next(1, 100000) }
		2 { return ';' * $rng.Next(1, 50) }
		3 { return 'c;' + [Convert]::ToBase64String([byte[]](1..$rng.Next(1, 200) | ForEach-Object { [byte]($_ % 256) })) }
		4 { return "https://example.com/$($rng.Next())" }
		default { return 'rgb:ff/00/00' }
	}
}

function New-FuzzStream([System.Random]$rng) {
	$sb = [System.Text.StringBuilder]::new()
	for ($i = $rng.Next(5, 400); $i -gt 0; $i--) { [void]$sb.Append((New-FuzzToken $rng)) }
	return $sb.ToString()
}

# Byte-level mutation of an existing stream: flips, cuts, splices, repeats.
function New-MutatedStream([System.Random]$rng, [byte[][]]$seeds) {
	$list = [System.Collections.Generic.List[byte]]::new([byte[]](Get-FuzzPick $rng $seeds))
	for ($i = $rng.Next(1, 8); $i -gt 0; $i--) {
		$pos = $rng.Next(0, $list.Count + 1)
		switch ($rng.Next(5)) {
			0 { if ($list.Count) { $list[[Math]::Min($pos, $list.Count - 1)] = [byte]$rng.Next(256) } }
			1 { $n = [Math]::Min($rng.Next(1, 30), $list.Count - $pos); if ($n -gt 0) { $list.RemoveRange($pos, $n) } }
			2 { $list.InsertRange($pos, [byte[]]([byte[]](0x1B, 0x5B) + [System.Text.Encoding]::ASCII.GetBytes("$(Get-FuzzPick $rng $script:FuzzNumbers)"))) }
			3 {
				$n = [Math]::Min($rng.Next(1, 64), $list.Count - $pos)
				if ($n -gt 0) { $chunk = $list.GetRange($pos, $n).ToArray(); for ($k = 0; $k -lt $rng.Next(1, 50); $k++) { $list.InsertRange($pos, $chunk) } }
			}
			default { $list.InsertRange($pos, [System.Text.Encoding]::UTF8.GetBytes((New-FuzzToken $rng))) }
		}
	}
	return ,$list.ToArray()
}
