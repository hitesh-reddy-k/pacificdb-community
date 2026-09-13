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
if ($Action -eq 'add') {
    # Community owns the unqualified `pacificdb` command after installation.
    # Prepending is required when the npm Cloud CLI is already present.
    $entries = @($Directory) + $entries
}
[Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
