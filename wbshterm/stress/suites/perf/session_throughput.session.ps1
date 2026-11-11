# Output through the real pseudoconsole: wbsh prints many lines, the terminal keeps up.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @("seq 1 $(200000 * $Scale)\r"); Delay = 600; Settle = 800; Size = '120x40'; Timeout = 300 }
