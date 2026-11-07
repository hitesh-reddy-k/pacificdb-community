param(
    [Parameter(Mandatory)]
    [ValidateSet('add', 'remove')]
    [string]$Action,
    [Parameter(Mandatory)]
    [string]$Directory
)

$ErrorActionPreference = 'Stop'

$target = $Directory.TrimEnd('\')
$entries = @([Environment]::GetEnvironmentVariable('Path', 'User') -split ';' |
    Where-Object { $_ -and $_.TrimEnd('\') -ine $target })
if ($Action -eq 'add') {
    # Community owns the unqualified `pacificdb` command after installation.
    # Prepending is required when the npm Cloud CLI is already present.
    $entries = @($Directory) + $entries
}
[Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')

# Notify Explorer as well as installer-launched shells. The Workbench menu invokes
# this helper directly, without NSIS's own Environment broadcast.
Add-Type -Namespace PacificDB -Name EnvironmentNotification -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr window, uint message, System.UIntPtr wParam, string lParam, uint flags, uint timeout, out System.UIntPtr result);
'@
$notificationResult = [UIntPtr]::Zero
[void][PacificDB.EnvironmentNotification]::SendMessageTimeout([IntPtr]0xffff, 0x1a, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$notificationResult)
