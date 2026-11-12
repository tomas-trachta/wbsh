# A session from start to a finished command: pseudoconsole, wbsh, first prompt.
# The driver takes the fixed delay and settle times off the wall time.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('echo ready\r'); Delay = 100; Settle = 400; Size = '100x30' }
