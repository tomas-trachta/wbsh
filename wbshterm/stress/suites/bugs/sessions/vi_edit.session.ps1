# A full-screen editor: open, type, save, quit, and the shell screen comes back.
param([string]$WorkDir, [int]$Scale = 1)
@{
	Feed = @('vi notes.txt\r', 'ihello from vi\e', ':wq\r', 'cat notes.txt\r')
	Gap = 500; Expect = @('(?m)^hello from vi\s*$')
	Check = { param($r) @{ Ok = (Test-Path (Join-Path $r.Work 'notes.txt')); What = 'vi did not write notes.txt' } }
}
