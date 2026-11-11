# The real window for some time, with a script that prints colour, clears,
# uses the alternate screen and sets titles. Runs only with -Gui.
param([string]$WorkDir, [int]$Seconds = 20)
$script = @"
end=`$((SECONDS + $Seconds))
i=0
while [ `$SECONDS -lt `$end ]; do
    i=`$((i + 1))
    printf '\e]0;soak %d\a' "`$i"
    printf '\e[3%dmline %d %s\e[0m\n' `$((i % 8)) "`$i" "colour text with some length to it"
    if [ `$((i % 500)) -eq 0 ]; then printf '\e[?1049h\e[2Jalt\e[?1049l'; clear; fi
done
"@
$path = Join-Path $WorkDir 'soak.sh'
[IO.File]::WriteAllText($path, ($script -replace "`r`n", "`n"))
@{ Args = "-r `"$($path -replace '\', '/')`"" }
