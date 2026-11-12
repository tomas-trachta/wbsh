# The basic loop: the shell starts, takes typed input, and its output reaches the grid.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('echo hello-from-session\r'); Expect = @('(?m)^hello-from-session\s*$'); Severity = 'high' }
