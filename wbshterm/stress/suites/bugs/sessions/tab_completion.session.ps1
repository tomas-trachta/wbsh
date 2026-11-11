# Tab completes a command name in the line editor.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('ech\t', ' completed-by-tab\r'); Gap = 500; Expect = @('(?m)^completed-by-tab\s*$') }
