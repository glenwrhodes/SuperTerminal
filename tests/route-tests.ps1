# Routing and auto-run safety tests.
# Runs the shim's own Get-STAnalysis on each line (PowerShell parses and
# resolves, nothing is executed), then feeds the result to st.exe's C++
# router / safety check and compares with what we expect.
#
#   pwsh -NoProfile -File tests\route-tests.ps1 [-Exe build\Release\st.exe]

param([string]$Exe = (Join-Path $PSScriptRoot ".." "build" "windows" "Release" "st.exe"))
$ErrorActionPreference = 'Stop'

# Load Get-STAnalysis straight out of the shim.
$shim = Join-Path $PSScriptRoot ".." "shell" "superterminal.ps1"
$ast = [System.Management.Automation.Language.Parser]::ParseFile($shim, [ref]$null, [ref]$null)
$fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-STAnalysis' }, $true)
. ([scriptblock]::Create($fn.Extent.Text))

Push-Location (Join-Path $PSScriptRoot "..")   # a FileSystem location, like a real session
try {
    # line -> expected: $true = goes to Claude, $false = PowerShell runs it as typed
    $routeCases = [ordered]@{
        'dir'                                        = $false
        'git status'                                 = $false
        'Get-ChildItem -File | Sort-Object Length'   = $false
        'Get-ChildItem -Recurse -Filter *.cpp'       = $false
        'cd ..'                                      = $false
        '$x = 5'                                     = $false
        '2 + 2'                                      = $false
        'echo hello world'                           = $false
        'rm -r somefolder'                           = $false
        'mkdir files'                                = $false
        'git commit -m "fix the bug in my code"'     = $false
        'if ($true) {'                               = $false   # multi-line input continues
        'st model haiku'                             = $false   # (an unknown name here, but fine)
        '!! show me the files'                       = $false
        'show me all the docx files in this folder'  = $true
        'dir /w'                                     = $true
        # On macOS/Linux, ls and rm are the real programs (no PowerShell aliases), so these are valid there.
        'ls -la'                                     = [bool]$IsWindows
        'rm -rf build'                               = [bool]$IsWindows
        'sort these by size'                         = $true
        'where is python'                            = $true
        'start the server'                           = $true
        'find all docx files'                        = $true
        "what's in this folder"                      = $true
        'which files are bigger than 10 MB?'         = $true
        'list the 10 biggest files'                  = $true
        'fix it'                                     = $true
        'gti status'                                 = $true
        '?? git status'                              = $true
        'how much disk space do I have left'         = $true
    }
    # Our `st` function isn't loaded here, so 'st model haiku' shows up as an unknown
    # command; in a session it resolves to Invoke-SuperTerminal. Expect $true here.
    $routeCases['st model haiku'] = $true

    $routeInput = foreach ($k in $routeCases.Keys) { @{ line = $k; analysis = (Get-STAnalysis $k) } | ConvertTo-Json -Compress -Depth 12 }
    $routeOut = $routeInput | & $Exe --route-test | ForEach-Object { $_ | ConvertFrom-Json }

    # command -> risk Claude gave -> expected auto-run
    $safetyCases = @(
        ,@('Get-ChildItem -Filter *.docx', 'read_only', $true)
        ,@('Get-ChildItem -Recurse -File | Sort-Object Length -Descending | Select-Object -First 10 Name, @{n=''MB'';e={[math]::Round($_.Length/1MB,2)}}', 'read_only', $true)
        ,@('git status', 'read_only', $true)
        ,@('git log --oneline -10', 'read_only', $true)
        ,@('Set-Location ~\Downloads', 'read_only', $true)
        ,@('Get-Process | Sort-Object CPU -Descending | Select-Object -First 5', 'read_only', $true)
        ,@('Remove-Item *.tmp', 'read_only', $false)                     # Claude mislabelled it: local check catches it
        ,@('Get-ChildItem *.log | Remove-Item', 'read_only', $false)
        ,@('Get-ChildItem | ForEach-Object { $_.Delete() }', 'read_only', $false)
        ,@('Get-Content a.txt > b.txt', 'read_only', $false)
        ,@('Invoke-Expression "dir"', 'read_only', $false)
        ,@('git push', 'read_only', $false)
        ,@('git branch newbranch', 'read_only', $false)
        ,@('Stop-Process -Name notepad', 'destructive', $false)
        ,@('New-Item -ItemType Directory foo', 'modifies', $false)
        ,@('[IO.File]::Delete("x")', 'read_only', $false)
        ,@('Get-ChildItem > $null', 'read_only', $true)
    )
    $safetyInput = foreach ($c in $safetyCases) { @{ analysis = (Get-STAnalysis $c[0]); risk = $c[1] } | ConvertTo-Json -Compress -Depth 12 }
    $safetyOut = $safetyInput | & $Exe --safety-test | ForEach-Object { $_ | ConvertFrom-Json }
} finally {
    Pop-Location
}

$failed = 0
$i = 0
foreach ($k in $routeCases.Keys) {
    $got = $routeOut[$i++]
    $ok = [bool]$got.toAi -eq $routeCases[$k]
    if (-not $ok) { $failed++ }
    '{0} route  {1,-45} -> {2,-6} {3}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $k, $(if ($got.toAi) { 'claude' } else { 'run' }), $got.reason
}
for ($j = 0; $j -lt $safetyCases.Count; $j++) {
    $c = $safetyCases[$j]
    $got = $safetyOut[$j]
    $ok = [bool]$got.allowed -eq $c[2]
    if (-not $ok) { $failed++ }
    '{0} auto   {1,-45} -> {2,-6} {3}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), ($c[0].Substring(0, [Math]::Min(45, $c[0].Length))), $(if ($got.allowed) { 'auto' } else { 'ask' }), $got.reason
}
"`n$($routeCases.Count + $safetyCases.Count - $failed) passed, $failed failed"
exit $failed
