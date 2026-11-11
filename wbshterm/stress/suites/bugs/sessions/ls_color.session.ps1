# Coloured ls output lands with the right names, including non-ASCII ones.
param([string]$WorkDir, [int]$Scale = 1)
New-Item -ItemType Directory (Join-Path $WorkDir 'sub dir') -Force | Out-Null
foreach ($n in 'alpha.txt', 'beta.sh', "$([char]0x17E)lu$([char]0x165).txt") { Set-Content (Join-Path $WorkDir $n) 'x' }
@{ Feed = @('ls --color=always -1\r'); Expect = @('alpha\.txt', 'beta\.sh', "$([char]0x17E)lu$([char]0x165)\.txt", 'sub dir') }
