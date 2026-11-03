# Builds SuperTerminal on Windows (Release) and optionally runs the tests.
#
#   pwsh -File build.ps1                 build build\windows\Release\st.exe
#   pwsh -File build.ps1 -Test           ...then the routing/safety tests and the offline end-to-end tests
#   pwsh -File build.ps1 -Test -Live     ...plus real plain-English requests (needs ANTHROPIC_API_KEY)
#   pwsh -File build.ps1 -Install        ...then copy st.exe to %LOCALAPPDATA%\Programs\SuperTerminal,
#                                           add it to your PATH and to Windows Terminal's profile list
#
# Needs Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload.
# CMake comes with it; nothing else to install.
param([switch]$Test, [switch]$Live, [switch]$Install)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$cmake = Get-Command cmake -ErrorAction Ignore | ForEach-Object Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
    }
}
if (-not $cmake -or -not (Test-Path $cmake)) {
    Write-Host "Visual Studio 2022 with C++ tools isn't installed. Get the free Build Tools with:" -ForegroundColor Yellow
    Write-Host "  winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive`""
    exit 1
}

$build = 'build\windows'
# No -G: CMake picks the newest installed Visual Studio.
if (-not (Test-Path "$build\CMakeCache.txt")) { & $cmake -S . -B $build -A x64 }
& $cmake --build $build --config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "$build\Release\st.exe"
"Built $exe"

if ($Install) {
    $dir = Join-Path $env:LOCALAPPDATA 'Programs\SuperTerminal'
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $exe $dir -Force
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (($userPath -split ';') -notcontains $dir) {
        [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ";$dir"), 'User')
        "Added $dir to your PATH (open a new terminal to use 'st')."
    }
    & (Join-Path $dir 'st.exe') --install-terminal-profile
    "Installed $dir\st.exe"
}

if ($Test) {
    pwsh -NoProfile -File tests\route-tests.ps1 -Exe $exe
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    python tests\route_tests.py $exe
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    $e2e = Join-Path $PSScriptRoot "$build\Release\st_e2e.exe"
    $mode = @()   # (an if-expression would unwrap a one-item array into a string)
    if (-not $Live) { $mode += '--offline' }
    & $e2e $exe "build\e2e-pwsh.txt" @mode
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    # The WSL test needs an installed distro (CI machines have wsl.exe but none).
    $hasDistro = $false
    if ((Get-Command wsl.exe -ErrorAction Ignore) -and -not $env:GITHUB_ACTIONS) {
        & wsl.exe -e true *> $null
        $hasDistro = $LASTEXITCODE -eq 0
    }
    if ($hasDistro) {
        & $e2e $exe "build\e2e-wsl.txt" --shell wsl @mode
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
}
