param(
    [Parameter(Mandatory)]
    [string]$PackageRoot,
    [string]$EvidencePath = "$PSScriptRoot\..\p0-evidence-windows.json"
)

$ErrorActionPreference = 'Stop'
$node = (Get-Command node -ErrorAction Stop).Source
& $node "$PSScriptRoot\test-p0-installed.mjs" $PackageRoot $EvidencePath
if ($LASTEXITCODE -ne 0) { throw "PacificDB P0 installed suite failed: $LASTEXITCODE" }
