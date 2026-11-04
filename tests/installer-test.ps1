# Installs the SuperTerminal installer silently for the current user, checks what it set up,
# then uninstalls it and checks that everything is gone again. Used by build.ps1 -Installer -Test and CI.
#
#   pwsh -File tests\installer-test.ps1 -Setup build\installer\SuperTerminal-Setup-0.4.0-x64.exe -Version 0.4.0
#   ...add -Keep to leave it installed (skips the uninstall half).
param([Parameter(Mandatory)][string]$Setup, [Parameter(Mandatory)][string]$Version, [switch]$Keep)
$ErrorActionPreference = 'Stop'

$failures = 0
function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

$dir = Join-Path $env:LOCALAPPDATA 'Programs\SuperTerminal'
$exe = Join-Path $dir 'st.exe'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SuperTerminal.lnk'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1C2B7E-3D4A-4B8E-9C61-5A2E7D0F9B13}_is1'
$fragment = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\SuperTerminal\superterminal.json'
$log = Join-Path (Split-Path $Setup) 'install.log'
function UserPathHas { ([Environment]::GetEnvironmentVariable('Path', 'User') -split ';') -contains $dir }

Write-Host "Installing $Setup"
$p = Start-Process $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/LOG=`"$log`"" -Wait -PassThru
Check "setup exit code 0 (got $($p.ExitCode))" ($p.ExitCode -eq 0)
Check "st.exe in $dir" (Test-Path $exe)
if (Test-Path $exe) { Check "st --version says $Version" ((& $exe --version) -eq "SuperTerminal $Version") }
Check 'Start menu shortcut' (Test-Path $shortcut)
if (Test-Path $shortcut) {
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    Check 'shortcut points at st.exe' ($lnk.TargetPath -eq $exe)
}
$entry = Get-ItemProperty $uninstallKey -ErrorAction Ignore
Check 'Installed apps entry' ($null -ne $entry -and $entry.DisplayName -eq 'SuperTerminal' -and $entry.DisplayVersion -eq $Version)
Check 'st on the user PATH' (UserPathHas)
Check 'Windows Terminal profile' (Test-Path $fragment)
if (Test-Path $fragment) { Check 'profile has the icon' ((Get-Content $fragment -Raw) -match 'st\.ico') }

if (-not $Keep) {
    Write-Host 'Uninstalling'
    $uninstaller = Join-Path $dir 'unins000.exe'
    Check 'uninstaller present' (Test-Path $uninstaller)
    if (Test-Path $uninstaller) {
        Start-Process $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
        # The uninstaller hands over to a copy of itself in %TEMP%; wait for that to finish too.
        $deadline = (Get-Date).AddSeconds(60)
        while ((Test-Path $uninstallKey) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    }
    Check 'st.exe removed' (-not (Test-Path $exe))
    Check 'shortcut removed' (-not (Test-Path $shortcut))
    Check 'Installed apps entry removed' (-not (Test-Path $uninstallKey))
    Check 'PATH entry removed' (-not (UserPathHas))
    Check 'Windows Terminal profile removed' (-not (Test-Path $fragment))
}

if ($failures) { Write-Host "$failures installer check(s) failed. Log: $log" -ForegroundColor Red; exit 1 }
Write-Host 'Installer checks passed.'
