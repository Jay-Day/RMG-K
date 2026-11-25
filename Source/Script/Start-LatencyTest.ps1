# Launch a built Windows copy with one rollback timing profile.
[CmdletBinding()]
param(
    [ValidateSet('Baseline', 'Hints', 'Display', 'RemoteWait', 'DisplayWait', 'Combined')]
    [string] $Profile = 'Combined',
    [string] $Executable = (Join-Path $PSScriptRoot '../../Bin/Release/RMG-K.exe'),
    [switch] $DrawHiddenFrames,
    [switch] $NoDirectHintProbes,
    [switch] $ButtonHintsOnly,
    [switch] $VerboseStats
)

$exe = Get-Item -LiteralPath $Executable -ErrorAction Stop
$launch = New-Object System.Diagnostics.ProcessStartInfo
$launch.FileName = $exe.FullName
$launch.WorkingDirectory = $exe.DirectoryName
$launch.UseShellExecute = $false

# A profile changes only the child process. All profiles keep the lobby's chosen frame delay.
$settings = $launch.EnvironmentVariables
$settings['RMGK_RAPHNET_HINTS'] = if ($Profile -in 'Hints', 'Combined') { '1' } else { '0' }
$settings['RMGK_RAPHNET_DIRECT_HINTS'] = if ($NoDirectHintProbes) { '0' } else { 'auto' }
$settings['RMGK_SMASH_STICK_HINTS'] = if ($ButtonHintsOnly) { '0' } else { '1' }
$settings['RMGK_PRESENT_LEAD_MS'] = if ($Profile -in 'Display', 'DisplayWait', 'Combined') { '8' } else { '0' }
$settings['RMGK_REMOTE_DECISION_SHIFT_MS'] = if ($Profile -in 'RemoteWait', 'DisplayWait', 'Combined') { '2' } else { '0' }
$settings['RMGK_ROLLBACK_RENDER'] = if ($DrawHiddenFrames) { '1' } else { '0' }
if ($VerboseStats) { $settings['RMGK_GEKKO_LOG'] = '1' }
if ($ButtonHintsOnly) { Write-Host 'Smash stick hints disabled for this comparison.' }

Write-Host ("{0} | hints {1} | direct probe policy {2} | display lead {3} ms | remote wait cap {4} ms | draw hidden {5}" -f
    $Profile, $settings['RMGK_RAPHNET_HINTS'], $settings['RMGK_RAPHNET_DIRECT_HINTS'],
    $settings['RMGK_PRESENT_LEAD_MS'], $settings['RMGK_REMOTE_DECISION_SHIFT_MS'], $settings['RMGK_ROLLBACK_RENDER'])
$process = [System.Diagnostics.Process]::Start($launch)
$process.Dispose()
