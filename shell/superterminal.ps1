# SuperTerminal shell integration.
#
# st.exe compiles this file in and loads it into the PowerShell session it hosts.
# It hooks the Enter key: every line is checked with PowerShell's own parser and
# command lookup (nothing is run), and st.exe decides whether to run it as-is or
# translate it. The translation comes back as a real command, shown on a
# fresh prompt line, and runs in this same session.
#
# The PowerShell side talks to st.exe over the named pipe in $env:SUPERTERMINAL_PIPE.

if (-not $env:SUPERTERMINAL_PIPE) { return }

$null = New-Module -Name SuperTerminal -ScriptBlock {
    $script:PipeName = $env:SUPERTERMINAL_PIPE
    $script:E = [char]27
    $script:Utf8 = New-Object System.Text.UTF8Encoding $false
    $script:LastSuggestion = $null
    $script:History = New-Object 'System.Collections.Generic.List[object]'
    $script:LastHistoryId = -1
    $script:LastErrorRecord = $null
    $script:FixHintShown = $false
    $script:OriginalPrompt = $null

    # Ctrl+C / Ctrl+X at the prompt end SuperTerminal. (While a command is
    # running, Ctrl+C still just stops that command; PowerShell handles it.)
    function Exit-SuperTerminal {
        [Console]::Write("$($script:E)[0m`r`n")
        [Environment]::Exit(0)
    }

    # ---------------------------------------------------------------- pipe to st.exe

    function Invoke-STPipe {
        param([System.Collections.IDictionary]$Request, [switch]$Interactive, [int]$TimeoutMs = 5000)
        $client = New-Object System.IO.Pipes.NamedPipeClientStream '.', $script:PipeName, ([System.IO.Pipes.PipeDirection]::InOut)
        try {
            $client.Connect(2000)
            $writer = New-Object System.IO.StreamWriter $client, $script:Utf8
            $writer.AutoFlush = $true
            $reader = New-Object System.IO.StreamReader $client, $script:Utf8
            $writer.WriteLine(($Request | ConvertTo-Json -Compress -Depth 12))
            $task = $reader.ReadLineAsync()
            if ($Interactive) {
                $E = $script:E
                $dots = 0
                while (-not $task.Wait(100)) {
                    while ([Console]::KeyAvailable) {
                        $k = [Console]::ReadKey($true)
                        if ($k.Key -eq [ConsoleKey]::Escape) {
                            return [pscustomobject]@{ kind = 'cancelled' }
                        }
                        if (($k.Key -eq [ConsoleKey]::C -or $k.Key -eq [ConsoleKey]::X) -and
                            ($k.Modifiers -band [ConsoleModifiers]::Control)) {
                            Exit-SuperTerminal
                        }
                    }
                    $dots = ($dots + 1) % 3
                    [Console]::Write("`r$E[2K$E[90m  $('.' * ($dots + 1))$E[0m")
                }
            } elseif (-not $task.Wait($TimeoutMs)) {
                return $null
            }
            $text = $task.Result
            if (-not $text) { return $null }
            return ($text | ConvertFrom-Json)
        } catch {
            return $null
        } finally {
            $client.Dispose()
        }
    }

    # ---------------------------------------------------------------- analysis
    # Parses a line and resolves its commands without running anything. st.exe
    # uses this both to route typed lines and to vet translated commands.

    function Get-STAnalysis {
        param([string]$Text)
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)

        $commands = New-Object 'System.Collections.Generic.List[object]'
        $commandAsts = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true)
        foreach ($c in $commandAsts) {
            $name = $c.GetCommandName()
            $target = $null
            $kind = 'dynamic'
            $resolved = $null
            $resolvedKind = $null
            $verb = $null
            if ($name) {
                $info = Get-Command -Name ([System.Management.Automation.WildcardPattern]::Escape($name)) -ErrorAction Ignore |
                    Select-Object -First 1
                if ($info) {
                    $kind = [string]$info.CommandType
                    $target = $info
                    if ($info.CommandType -eq 'Alias' -and $info.ResolvedCommand) { $target = $info.ResolvedCommand }
                    $resolved = $target.Name
                    $resolvedKind = [string]$target.CommandType
                    if ($target.PSObject.Properties['Verb']) { $verb = $target.Verb }
                } else {
                    $kind = 'NotFound'
                }
            }
            # Parameter metadata, to spot parameters PowerShell would reject ("ls -la", "rm -rf").
            # Only for cmdlets and advanced functions; simple functions take anything in $args.
            $meta = $null
            if ($kind -ne 'NotFound' -and $kind -ne 'dynamic' -and $target -and ($resolvedKind -eq 'Cmdlet' -or $resolvedKind -eq 'Function')) {
                try {
                    $p = $target.Parameters
                    if ($p -and ($resolvedKind -eq 'Cmdlet' -or $p.ContainsKey('Verbose'))) { $meta = $p }
                } catch { }
            }
            $bare = New-Object 'System.Collections.Generic.List[string]'
            $badParams = New-Object 'System.Collections.Generic.List[string]'
            $params = 0
            $other = 0
            $elements = $c.CommandElements
            for ($i = 1; $i -lt $elements.Count; $i++) {
                $el = $elements[$i]
                if ($el -is [System.Management.Automation.Language.CommandParameterAst]) {
                    $params++
                    if ($meta) {
                        $pn = $el.ParameterName
                        $hit = @($meta.Values | Where-Object { $_.Name -eq $pn -or $_.Aliases -contains $pn })
                        if (-not $hit) {
                            $hit = @($meta.Values | Where-Object { $_.Name -like "$pn*" -or @($_.Aliases | Where-Object { $_ -like "$pn*" }).Count })
                        }
                        if ($hit.Count -ne 1) {
                            $badParams.Add("-$pn")
                        } elseif (-not $hit[0].SwitchParameter -and -not $el.Argument) {
                            $next = if ($i + 1 -lt $elements.Count) { $elements[$i + 1] } else { $null }
                            if (-not $next -or $next -is [System.Management.Automation.Language.CommandParameterAst]) {
                                $badParams.Add("-$pn")
                            }
                        }
                    }
                } elseif ($el -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
                          $el.StringConstantType -eq 'BareWord') {
                    $bare.Add($el.Value)
                } else {
                    $other++
                }
            }
            $commands.Add([ordered]@{
                name = $name; kind = $kind; resolved = $resolved; resolvedKind = $resolvedKind; verb = $verb
                barewords = $bare; params = $params; other = $other; badParams = $badParams
            })
        }

        $redirections = 0
        foreach ($r in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FileRedirectionAst] }, $true)) {
            $loc = $r.Location
            $toNull = $loc -is [System.Management.Automation.Language.VariableExpressionAst] -and $loc.VariablePath.UserPath -eq 'null'
            if (-not $toNull) { $redirections++ }
        }
        $members = New-Object 'System.Collections.Generic.List[string]'
        foreach ($m in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true)) {
            if ($m.Member -is [System.Management.Automation.Language.StringConstantExpressionAst]) { $members.Add($m.Member.Value) }
            else { $members.Add('?') }
        }
        $assignments = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)).Count

        $parseErrors = New-Object 'System.Collections.Generic.List[object]'
        foreach ($e in $errors) { $parseErrors.Add([ordered]@{ message = $e.Message; incomplete = [bool]$e.IncompleteInput }) }

        [ordered]@{
            parseErrors = $parseErrors; commands = $commands; redirections = $redirections
            members = $members; assignments = $assignments
        }
    }

    # ---------------------------------------------------------------- output helpers

    function Get-STWrapped {
        param([string]$Text, [int]$Width)
        $lines = New-Object 'System.Collections.Generic.List[string]'
        foreach ($para in ($Text -split "`r?`n")) {
            if ($para.Length -le $Width) { $lines.Add($para); continue }
            $current = ''
            foreach ($word in ($para -split ' ')) {
                if ($current.Length -eq 0) { $current = $word }
                elseif ($current.Length + 1 + $word.Length -le $Width) { $current += ' ' + $word }
                else { $lines.Add($current); $current = $word }
                while ($current.Length -gt $Width) { $lines.Add($current.Substring(0, $Width)); $current = $current.Substring($Width) }
            }
            $lines.Add($current)
        }
        $lines
    }

    # Writes text wrapped to the window and indented two spaces, in a colour (0 = default).
    function Write-STNote {
        param([string]$Text, [int]$Color = 0)
        $E = $script:E
        $width = [Math]::Max(30, [Console]::WindowWidth - 4)
        $lines = @(Get-STWrapped -Text $Text.TrimEnd() -Width $width)
        $out = New-Object System.Text.StringBuilder
        foreach ($l in $lines) {
            if ($Color) { [void]$out.Append("$E[${Color}m  $l$E[0m`r`n") } else { [void]$out.Append("  $l`r`n") }
        }
        [Console]::Write($out.ToString())
    }

    # Draws a fresh prompt on the current line, holding $Text.
    function Restore-STPrompt {
        param([string]$Text)
        [Microsoft.PowerShell.PSConsoleReadLine]::InvokePrompt($null, [Console]::CursorTop)
        $current = $null
        $cursor = 0
        [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref]$current, [ref]$cursor)
        if ($current -cne $Text) {
            [Microsoft.PowerShell.PSConsoleReadLine]::Replace(0, $current.Length, $Text)
        }
    }

    # ---------------------------------------------------------------- the Enter key

    function Invoke-STEnter {
        $line = $null
        $cursor = 0
        [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref]$line, [ref]$cursor)

        if ([string]::IsNullOrWhiteSpace($line)) {
            [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
            return
        }
        # Enter on a suggested command: run it.
        if ($null -ne $script:LastSuggestion -and $line -ceq $script:LastSuggestion) {
            $script:LastSuggestion = $null
            $null = Invoke-STPipe @{ type = 'mark' }
            [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
            return
        }
        $script:LastSuggestion = $null

        $route = Invoke-STPipe @{ type = 'route'; line = $line; analysis = (Get-STAnalysis $line) }
        if (-not $route -or $route.action -ne 'ai') {
            if ($route -and $line.TrimStart().StartsWith('!!')) {
                [Microsoft.PowerShell.PSConsoleReadLine]::Replace(0, $line.Length, [string]$route.line)
            }
            [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
            return
        }
        Invoke-STAsk -Original $line -Request ([string]$route.line) -Reason ([string]$route.reason)
    }

    function Invoke-STAsk {
        param([string]$Original, [string]$Request, [string]$Reason)
        $E = $script:E
        # Leave the typed line on screen and work below it.
        [Microsoft.PowerShell.PSConsoleReadLine]::SetCursorPosition($Original.Length)
        [Console]::Write("$E[0J`r`n")

        $location = Get-Location
        $resp = Invoke-STPipe -Interactive ([ordered]@{
            type = 'ask'; request = $Request; reason = $Reason
            cwd = $location.ProviderPath; isFileSystem = ($location.Provider.Name -eq 'FileSystem')
            psVersion = $PSVersionTable.PSVersion.ToString()
            # .ToArray(), not @(...): @() over this list trips a PowerShell binder bug
            # ("Argument types do not match").
            history = $script:History.ToArray()
        })
        [Console]::Write("`r$E[2K")

        if (-not $resp) {
            Write-STNote 'Lost contact with st.exe. Running nothing.' 91
            Restore-STPrompt $Original
            return
        }
        # Only the command or the answer is shown; nothing about how it was produced.
        switch ([string]$resp.kind) {
            'cancelled' {
                Restore-STPrompt $Original
            }
            'error' {
                Write-STNote ([string]$resp.message) 91
                Restore-STPrompt $Original
            }
            'answer' {
                Write-STNote ([string]$resp.answer)
                [Microsoft.PowerShell.PSConsoleReadLine]::AddToHistory($Original)
                Restore-STPrompt ''
            }
            'passthrough' {
                Restore-STPrompt $Original
                $null = Invoke-STPipe @{ type = 'mark' }
                [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
            }
            'command' {
                $command = [string]$resp.command
                $risk = [string]$resp.risk
                $check = Invoke-STPipe @{ type = 'check'; risk = $risk; analysis = (Get-STAnalysis $command) }
                $auto = [bool]($check -and $check.autorun)
                if (-not $auto) {
                    # The one thing worth saying: it won't run until you press Enter.
                    switch ($risk) {
                        'destructive' { Write-STNote 'This deletes or overwrites. Enter to run, Esc to cancel.' 91 }
                        'modifies' { Write-STNote 'This makes changes. Enter to run, Esc to cancel.' 93 }
                        default { Write-STNote 'Enter to run, Esc to cancel.' 90 }
                    }
                }
                [Microsoft.PowerShell.PSConsoleReadLine]::AddToHistory($Original)
                Restore-STPrompt $command
                if ($auto) {
                    [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
                } else {
                    $script:LastSuggestion = $command
                }
            }
            default {
                Write-STNote "Unexpected reply from st.exe ($($resp.kind))." 91
                Restore-STPrompt $Original
            }
        }
    }

    # ---------------------------------------------------------------- prompt hook
    # Records each command's outcome, so "fix it" and follow-ups have context.

    function Update-STHistory {
        param([bool]$Ok, $ExitCode)
        $last = Get-History -Count 1
        if (-not $last -or $last.Id -eq $script:LastHistoryId) { return }
        $script:LastHistoryId = $last.Id
        $entry = [ordered]@{ line = $last.CommandLine; ok = $Ok }
        if (-not $Ok) {
            if ($ExitCode -is [int] -and $ExitCode -ne 0) { $entry.exit = $ExitCode }
            if ($global:Error.Count -gt 0 -and -not [object]::ReferenceEquals($global:Error[0], $script:LastErrorRecord)) {
                $record = $global:Error[0]
                $entry.error = "$record"
                if ($record -is [System.Management.Automation.ErrorRecord]) { $entry.error += " [$($record.FullyQualifiedErrorId)]" }
            }
        }
        if ($global:Error.Count -gt 0) { $script:LastErrorRecord = $global:Error[0] }
        $script:History.Add($entry)
        while ($script:History.Count -gt 12) { $script:History.RemoveAt(0) }
        if (-not $Ok -and -not $script:FixHintShown) {
            $script:FixHintShown = $true
            [Console]::Write("$($script:E)[90m  Tip: type `"fix it`" to repair that.$($script:E)[0m`r`n")
        }
    }

    $script:OriginalPrompt = (Get-Command -Name prompt -CommandType Function -ErrorAction Ignore).ScriptBlock
    if (-not $script:OriginalPrompt) { $script:OriginalPrompt = { "PS $($executionContext.SessionState.Path.CurrentLocation)$('>' * ($nestedPromptLevel + 1)) " } }
    $wrapper = {
        $ok = $?
        $code = $global:LASTEXITCODE
        try { Update-STHistory $ok $code } catch { }
        $global:LASTEXITCODE = $code
        # Let the original prompt (oh-my-posh and friends) see the same success state.
        if (-not $ok) { Write-Error -Message 'x' -ErrorAction Ignore }
        & $script:OriginalPrompt
    }
    Set-Item -Path function:global:prompt -Value $wrapper

    # ---------------------------------------------------------------- st command

    function Show-STSettings {
        param($Settings)
        if (-not $Settings) { Write-STNote "Can't reach st.exe." 91; return }
        if ($Settings.message) { Write-STNote ([string]$Settings.message) $(if ($Settings.ok) { 96 } else { 91 }) }
        $key = if ($Settings.keyPresent) { 'set' } else { 'NOT SET' }
        $auto = if ($Settings.autorun) { 'on (read-only commands run straight away)' } else { 'off (every command waits for Enter)' }
        [Console]::Write("    model   $($Settings.model)`r`n    effort  $($Settings.effort)`r`n    auto    $auto`r`n    key     ANTHROPIC_API_KEY is $key`r`n")
    }

    function Invoke-SuperTerminal {
        [CmdletBinding()]
        param([Parameter(Position = 0)][string]$Command = 'help', [Parameter(Position = 1)][string]$Value)
        switch ($Command.ToLowerInvariant()) {
            'model' { Show-STSettings (Invoke-STPipe @{ type = 'config'; key = $(if ($Value) { 'model' } else { '' }); value = $Value }) }
            'effort' { Show-STSettings (Invoke-STPipe @{ type = 'config'; key = $(if ($Value) { 'effort' } else { '' }); value = $Value }) }
            { $_ -in 'auto', 'autorun' } { Show-STSettings (Invoke-STPipe @{ type = 'config'; key = $(if ($Value) { 'autorun' } else { '' }); value = $Value }) }
            { $_ -in 'status', 'settings', 'config' } { Show-STSettings (Invoke-STPipe @{ type = 'config' }) }
            default {
                $E = $script:E
                [Console]::Write(@"
$E[96mSuperTerminal$E[0m: type PowerShell as usual, or say what you want in plain English.

  show me all the docx files in this folder
  which files here are bigger than 10 MB?
  dir /w                         $E[90m(cmd.exe and bash habits work too)$E[0m
  fix it                         $E[90m(after a command fails)$E[0m

  ?? <text>          treat the line as plain English, even if it looks like a command
  !! <command>       run exactly as typed
  Esc                cancel a suggested command, or a request in progress
  Ctrl+C / Ctrl+X    exit SuperTerminal (Ctrl+C during a running command just stops it)

  st status          settings
  st auto on|off     run read-only answers straight away, or always wait for Enter

"@.Replace("`r`n", "`n").Replace("`n", "`r`n"))
            }
        }
    }
    Set-Alias -Name st -Value Invoke-SuperTerminal

    # ---------------------------------------------------------------- start up

    if (-not (Get-Module PSReadLine)) { Import-Module PSReadLine -ErrorAction Ignore }
    if (Get-Module PSReadLine) {
        # Esc clears the line (cancels a suggested command) on every OS; PSReadLine's emacs
        # mode on macOS/Linux would otherwise treat it as a prefix key.
        Set-PSReadLineKeyHandler -Key Escape -Function RevertLine
        Set-PSReadLineKeyHandler -Chord 'Ctrl+c', 'Ctrl+x' -BriefDescription SuperTerminalExit `
            -Description 'Exit SuperTerminal' -ScriptBlock { Exit-SuperTerminal }
        Set-PSReadLineKeyHandler -Key Enter -BriefDescription SuperTerminalEnter `
            -Description 'Run the line, or turn plain English into a command' `
            -ScriptBlock {
                try {
                    Invoke-STEnter
                } catch {
                    # Never leave the user stuck: report it and fall back to plain PowerShell behaviour.
                    $where = if ($env:SUPERTERMINAL_DEBUG) { " at $($_.ScriptStackTrace -replace '\r?\n', ' <- ')" } else { '' }
                    if ($env:SUPERTERMINAL_DEBUG) {
                        Add-Content -Path (Join-Path $env:TEMP 'superterminal-debug.log') -Value ("$(Get-Date -Format o) $($_.Exception.GetType().FullName): $($_.Exception.Message)`n$($_.ScriptStackTrace)`n$($_.Exception.StackTrace)`n")
                    }
                    [Console]::Write("`r`n$($script:E)[91m  SuperTerminal hit an error: $($_.Exception.Message)$where$($script:E)[0m`r`n")
                    $line = $null; $cursor = 0
                    [Microsoft.PowerShell.PSConsoleReadLine]::GetBufferState([ref]$line, [ref]$cursor)
                    try { Restore-STPrompt $line } catch { }
                }
            }
    }

    try { [Console]::OutputEncoding = $script:Utf8 } catch { }

    $hello = Invoke-STPipe @{ type = 'hello' }
    if ($hello) {
        $E = $script:E
        [Console]::Write("$E[96mSuperTerminal$E[0m $($hello.version)  $E[90m·  PowerShell $($PSVersionTable.PSVersion)$E[0m`r`n")
        if (-not (Get-Module PSReadLine)) {
            [Console]::Write("$E[93mPSReadLine isn't loaded, so plain-English requests are off.$E[0m`r`n")
        } elseif ($hello.keyPresent) {
            [Console]::Write("$E[90mType commands as usual, or just say what you want. ``st help`` for more.$E[0m`r`n")
        } else {
            [Console]::Write("$E[93mANTHROPIC_API_KEY isn't set, so plain-English requests are off. Set it, then open a new SuperTerminal.$E[0m`r`n")
        }
    }

    Export-ModuleMember -Function Invoke-SuperTerminal -Alias st
} | Import-Module -Global -PassThru

$global:OutputEncoding = New-Object System.Text.UTF8Encoding $false
