param(
    [Parameter(Mandatory)]
    [ValidateSet('add', 'remove')]
    [string]$Action,
    [Parameter(Mandatory)]
    [string]$Directory
)

$target = $Directory.TrimEnd('\')
$entries = @([Environment]::GetEnvironmentVariable('Path', 'User') -split ';' |
    Where-Object { $_ -and $_.TrimEnd('\') -ine $target })
if ($Action -eq 'add') { $entries += $Directory }
[Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
