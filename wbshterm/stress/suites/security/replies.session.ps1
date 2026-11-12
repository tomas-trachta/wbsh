# Answers to terminal queries must never carry bytes the program chose.
# A program (or a file someone cats) prints a title, a DECRQSS request or a
# clipboard read; if the terminal echoed that data back as input, the shell
# would get attacker text as if the user typed it (title-report injection).
# Nothing reads the answers here, so they arrive at the prompt, where any
# printable payload would show in the grid.
param([string]$WorkDir, [int]$Scale = 1)
$probe = @'
printf '\033]2;PWNEDTITLE\007\033[21t\033[20t\033P$qPWNEDRQ\033\\\033]52;c;?\007\033[c\033[>c\033[6n\033[5n\033]10;?\007\033[?2004$p'
'@
[IO.File]::WriteAllText((Join-Path $WorkDir 'replies.sh'), ($probe -replace "`r`n", "`n"))
@{
	Feed = @('source replies.sh\r'); Settle = 1500; Size = '200x30'; Severity = 'critical'
	Absent = @('PWNED', 'c;[A-Za-z0-9+/=]{4,}')
}
