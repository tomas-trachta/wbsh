# Input generators for the fuzz part of the bugs suite. Dot-sourced by stress.ps1.
#
# Two generators, both deterministic for one seed:
#
#   New-FuzzProgram  A random but well-formed script in the subset of bash both
#                    shells must agree on (builtins, expansions, control flow,
#                    small pipelines through bundled filters). It touches only
#                    files f1..f3 in its own work directory. The driver runs it
#                    in wbsh and in the reference bash and compares stdout + exit code.
#
#   New-ParseMutant  A broken mutation of real scripts (wbsh/tests/*.sh plus the
#                    corpus). The driver feeds it to `wbsh --no-ast`, which only
#                    lexes and parses, so nothing in it can run. Only crashes
#                    and hangs count.

$script:FuzzVars = @('v1', 'v2', 'v3', 'n')

$script:FuzzPreamble = @'
exec 2>/dev/null
v1='hello world'
v2='a*b'
v3=''
n=3
arr=(one 'two three' four)
set -- p1 'p 2' p3
: > f1; echo data > f2; printf 'x\ny\n' > f3
'@

function Get-Pick([System.Random]$rng, [object[]]$items) {
	return $items[$rng.Next($items.Count)]
}

function New-FuzzArith([System.Random]$rng, [int]$depth) {
	if ($depth -le 0 -or $rng.Next(3) -eq 0) {
		return Get-Pick $rng @("$($rng.Next(-5, 50))", 'n', '${#v1}', '$#', "$($rng.Next(1, 9))", '${#arr[@]}')
	}

	$a = New-FuzzArith $rng ($depth - 1)
	$op = Get-Pick $rng @('+', '-', '*', '<', '>', '<=', '==', '!=', '&&', '||', '&', '|', '^', '<<', '>>')
	switch ($rng.Next(6)) {
		0 { return "($a) / $($rng.Next(1, 9))" }
		1 { return "($a) % $($rng.Next(1, 9))" }
		2 { return "($a) ? $(New-FuzzArith $rng ($depth - 1)) : $(New-FuzzArith $rng ($depth - 1))" }
		3 { return "-($a)" }
		default {
			$b = New-FuzzArith $rng ($depth - 1)
			if ($op -in '<<', '>>') { $b = "$($rng.Next(0, 8))" }
			return "($a $op $b)"
		}
	}
}

function New-FuzzWord([System.Random]$rng, [int]$depth) {
	$v = Get-Pick $rng $script:FuzzVars
	$choices = @(
		{ Get-Pick $rng @('a', 'foo', '42', '-x', 'a.b', 'k=v', 'é', 'a\ b') },
		{ Get-Pick $rng @("'a b'", "'`$v1'", "'\'", "'*'", "''") },
		{ "`"x `$$v y`"" },
		{ "`"`${$v}`"" },
		{ "`${#$v}" },
		{ "`"`${$v" + (Get-Pick $rng @(':-dflt}"', ':+alt}"', '#?}"', '%?}"', '##*o}"', '%%o*}"', '/o/0}"', '//o/0}"', '^^}"', ',,}"', ':1:3}"', ': -2}"')) },
		{ Get-Pick $rng @('"${arr[@]}"', '${arr[*]}', '"${#arr[@]}"', '"${arr[1]}"', '"${arr[@]:1}"', '"${!arr[@]}"') },
		{ Get-Pick $rng @('"$@"', '$*', '"$#"', '"$1"', '"${2:-none}"', '$?') },
		{ "`$(( $(New-FuzzArith $rng 2) ))" },
		{ if ($depth -gt 0) { "`"`$($(New-FuzzSimple $rng ($depth - 1)))`"" } else { 'z' } },
		{ Get-Pick $rng @('{a,b}', '{1..3}', 'x{,y}', '{c..a}', 'f*', 'f[12]', '"f*"', 'nomatch*') },
		{ "`$$v" }
	)
	return & (Get-Pick $rng $choices)
}

function New-FuzzWords([System.Random]$rng, [int]$depth, [int]$max) {
	$count = $rng.Next(1, $max + 1)
	return (1..$count | ForEach-Object { New-FuzzWord $rng $depth }) -join ' '
}

function New-FuzzSimple([System.Random]$rng, [int]$depth) {
	$v = Get-Pick $rng $script:FuzzVars
	switch ($rng.Next(14)) {
		0 { return "echo $(New-FuzzWords $rng $depth 4)" }
		1 { return "printf '%s|' $(New-FuzzWords $rng $depth 3); echo" }
		2 { return "$v=$(New-FuzzWord $rng $depth)" }
		3 { return "arr+=($(New-FuzzWords $rng $depth 2))" }
		4 { return "(( $(New-FuzzArith $rng 3) ))" }
		5 { return "[[ $(New-FuzzWord $rng $depth) $(Get-Pick $rng @('==', '!=', '<', '>')) $(New-FuzzWord $rng $depth) ]]" }
		6 { return "[[ $(Get-Pick $rng @('-n', '-z')) $(New-FuzzWord $rng $depth) ]]" }
		7 { return "printf '%d\n' `"`$(( $(New-FuzzArith $rng 3) ))`"" }
		8 { return "read -r a b <<< $(New-FuzzWord $rng $depth); echo `"[`$a|`$b]`"" }
		9 { return "echo $(New-FuzzWords $rng $depth 3) | $(Get-Pick $rng @('tr a-z A-Z', 'wc -c', 'cat', 'sort', 'head -n 1', 'tr -d o'))" }
		10 { return "set -- $(New-FuzzWords $rng $depth 3)" }
		11 { return "unset $v" }
		12 { return "echo $(New-FuzzWord $rng $depth) $(Get-Pick $rng @('> f1', '>> f2', '2>/dev/null', '>&2')); cat f1 f2" }
		default { return "[[ $(New-FuzzWord $rng $depth) =~ $(Get-Pick $rng @('^a', 'o+', '[0-9]$', '^(x|h)', 'b\*')) ]] && echo `"m:`${BASH_REMATCH[0]}`"" }
	}
}

function New-FuzzStatement([System.Random]$rng, [int]$depth) {
	if ($depth -le 0) { return New-FuzzSimple $rng 0 }

	$body = { (1..$rng.Next(1, 3) | ForEach-Object { New-FuzzStatement $rng ($depth - 1) }) -join "`n" }
	$i = "i$($rng.Next(1000))"
	switch ($rng.Next(11)) {
		0 { return "if $(New-FuzzSimple $rng 0); then $(& $body); else $(& $body); fi" }
		1 { return "for x in $(New-FuzzWords $rng 0 3); do echo `"<`$x>`"; $(& $body); done" }
		2 { return "$i=0; while (( $i < $($rng.Next(0, 4)) )); do $(& $body); $i=`$(($i + 1)); done" }
		3 { return "case $(New-FuzzWord $rng 0) in $(Get-Pick $rng @('a*', 'h*', '[0-9]*', '*o*', '?')) ) echo c1 ;; *) $(& $body) ;; esac" }
		4 { return "fz() { local v1=`$1; $(& $body); return $($rng.Next(0, 3)); }; fz $(New-FuzzWords $rng 0 2); echo `"rc=`$?`"" }
		5 { return "( $(& $body) ); echo `"sub=`$?`"" }
		6 { return "{ $(& $body); } | cat" }
		7 { return "$(New-FuzzSimple $rng 1) && $(New-FuzzSimple $rng 1) || $(New-FuzzSimple $rng 1)" }
		8 { return "! $(New-FuzzSimple $rng 1); echo `"neg=`$?`"" }
		9 { return "cat <<EOF`n$(New-FuzzWord $rng 0) `$v1`nEOF`n:" }
		default { return New-FuzzSimple $rng ($depth - 1) }
	}
}

function New-FuzzProgram([System.Random]$rng) {
	$lines = [System.Collections.Generic.List[string]]::new()
	$lines.Add($script:FuzzPreamble)
	foreach ($i in 1..$rng.Next(3, 9)) { $lines.Add((New-FuzzStatement $rng 2)) }
	$lines.Add('echo "end:$?:$v1:${arr[*]}:$#:$n"')
	return ($lines -join "`n") + "`n"
}

$script:MutationNoise = @('(', ')', '{', '}', '"', "'", '`', '$', '$(', '$((', '${', '[[', ']]', '\', '|', '&', ';', ';;',
	'<', '>', '<<', '<<-', '<<<', '>&', '#', "`n", 'case', 'esac', 'do', 'done', 'fi', 'then', '!', '*', '?', '[',
	'@(', '+(', '{a,', '..}', '=~', '=(', '\x00', [char]::ConvertFromUtf32(0x1F600), ([string][char]0xFEFF), "`r`n")

function New-ParseMutant([System.Random]$rng, [string[]]$seeds) {
	$text = Get-Pick $rng $seeds
	if ($text.Length -gt 4000) {
		$start = $rng.Next(0, $text.Length - 2000)
		$text = $text.Substring($start, 2000)
	}

	foreach ($i in 1..$rng.Next(1, 6)) {
		$pos = $rng.Next(0, $text.Length + 1)
		switch ($rng.Next(7)) {
			0 { $text = $text.Insert($pos, (Get-Pick $rng $script:MutationNoise)) }
			1 {
				$len = [Math]::Min($rng.Next(1, 40), $text.Length - $pos)
				if ($len -gt 0) { $text = $text.Remove($pos, $len) }
			}
			2 {
				$len = [Math]::Min($rng.Next(1, 200), $text.Length - $pos)
				if ($len -gt 0) { $text = $text.Insert($pos, $text.Substring($pos, $len)) }
			}
			3 { $text = $text.Substring(0, $pos) }
			4 {
				$open = Get-Pick $rng @('$(', '(', '{ ', '$((', '"$(', '`', '[[ ', '$(( (', '<(', '${x:-')
				$count = $rng.Next(50, 3000)
				$text = $text.Insert($pos, ($open * $count))
			}
			5 {
				$other = Get-Pick $rng $seeds
				$text = $text.Insert($pos, $other.Substring(0, [Math]::Min(300, $other.Length)))
			}
			default { $text = $text.Insert($pos, [string][char]$rng.Next(0, 128)) }
		}
	}
	return $text
}
