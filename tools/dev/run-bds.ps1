<#
.SYNOPSIS
    Run the LeviLamina BDS to exercise LeviScript, capture the full server
    console, then stop it gracefully.

.DESCRIPTION
    Starts bedrock_server_mod.exe with stdin/stdout/stderr redirected. stdout and
    stderr are drained asynchronously (so the child never blocks on a full pipe),
    echoed to this terminal and written to -LogFile. After -RunSeconds the script
    writes "stop" as raw ASCII (no BOM) for a clean shutdown, waiting before it
    resorts to killing the process.

.PARAMETER Exe
    Full path to bedrock_server_mod.exe. Defaults to the 26.40.0 test server.

.PARAMETER RunSeconds
    How long to keep the server up before stopping it (long enough to see plugin
    load and the demo plugins' timer callbacks fire).

.PARAMETER LogFile
    Where the captured console is written.
#>
param(
    [string]$Exe = "E:\TelluriumDev\Server\26.40.0\bedrock_server_mod.exe",
    [int]$RunSeconds = 40,
    [string]$LogFile = "E:\TelluriumDev\Server\26.40.0\bds_console.log"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    throw "BDS executable not found: $Exe"
}
$workDir = Split-Path -Parent $Exe

$running = Get-Process -Name (Split-Path $Exe -Leaf) -ErrorAction SilentlyContinue |
           Where-Object { $_.Path -eq (Resolve-Path $Exe).Path }
if ($running) {
    throw "An instance is already running (PID $($running.Id -join ', ')). Stop it first."
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $Exe
$psi.WorkingDirectory       = $workDir
$psi.UseShellExecute        = $false
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true

Write-Host "=== launching BDS: $Exe (cwd: $workDir) ==="
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host "=== PID $($proc.Id); capturing console for $RunSeconds s, then sending 'stop' ==="

# Drain asynchronously so the child's pipe never fills and blocks it.
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

Start-Sleep -Seconds $RunSeconds

if (-not $proc.HasExited) {
    try {
        $stdin = $proc.StandardInput.BaseStream
        # The stdin StreamWriter emits a UTF-8 BOM preamble; a leading CRLF absorbs
        # it so the real "stop" lands on a clean line (BDS would otherwise see
        # "\ufeffstop" as an unknown command).
        $bytes = [System.Text.Encoding]::ASCII.GetBytes("`r`nstop`r`n")
        $stdin.Write($bytes, 0, $bytes.Length)
        $stdin.Flush()
    } catch {
        Write-Host "=== could not write to stdin: $($_.Exception.Message) ==="
    }
    if (-not $proc.WaitForExit(30000)) {
        Write-Host "=== graceful stop timed out; terminating ==="
        $proc.Kill()
        $proc.WaitForExit(5000)
    }
}

$stdoutTask.Wait()
$stderrTask.Wait()
$captured = $stdoutTask.Result + "`r`n" + $stderrTask.Result
Set-Content -Path $LogFile -Value $captured -Encoding UTF8
Write-Host $captured
Write-Host "=== BDS exited (code $($proc.ExitCode)); console saved to $LogFile ==="
