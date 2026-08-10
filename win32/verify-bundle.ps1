#
#     Mosh: the mobile shell
#     Copyright 2012 Keith Winstein
#
#     This program is free software: you can redistribute it and/or modify
#     it under the terms of the GNU General Public License as published by
#     the Free Software Foundation, either version 3 of the License, or
#     (at your option) any later version.
#
#     This program is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU General Public License for more details.
#
#     You should have received a copy of the GNU General Public License
#     along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#     In addition, as a special exception, the copyright holders give
#     permission to link the code of portions of this program with the
#     OpenSSL library under certain conditions as described in each
#     individual source file, and distribute linked combinations including
#     the two.
#
#     You must obey the GNU General Public License in all respects for all
#     of the code used other than OpenSSL. If you modify file(s) with this
#     exception, you may extend this exception to your version of the
#     file(s), but you are not obligated to do so. If you do not wish to do
#     so, delete this exception statement from your version. If you delete
#     this exception statement from all source files in the program, then
#     also delete it here.
#
# ABOUTME: Verifies that a staged Windows ARM64 mosh bundle runs without MSYS2.
# ABOUTME: Retains a missing-DLL negative control from the DLL-bundling era.
#
# The bundle is now a single statically-linked mosh.exe, so the probe-3
# negative control skips by construction (no staged DLL exists to remove) and
# -ExpectDlls has no remaining caller. Both are retained deliberately against a
# future re-introduction of a bundled payload; deleting them would make that
# regression invisible instead of merely unguarded.
#
# Use this after win32/package.sh on Windows CI. It copies rather than mutates the
# deliverable bundle, then launches each probe under a scrubbed child environment.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BundleDir,

    [Parameter(Mandatory = $true)]
    [string]$TestCoreExe,

    [switch]$ExpectDlls
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$probeTimeoutMilliseconds = 30000

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Content
    )

    [System.IO.File]::WriteAllText($Path, $Content)
}

function Start-ScrubbedProbe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string]$StdoutPath,

        [Parameter(Mandatory = $true)]
        [string]$StderrPath
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Executable
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables.Clear()
    $psi.EnvironmentVariables['SystemRoot'] = $env:SystemRoot
    $psi.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    if ($psi.EnvironmentVariables.Count -ne 2) {
        throw 'the probe child environment was not limited to SystemRoot and PATH'
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            Write-TextFile -Path $StdoutPath -Content ''
            Write-TextFile -Path $StderrPath -Content 'Process.Start returned false.'
            return [PSCustomObject]@{ ExitCode = $null; Started = $false; TimedOut = $false }
        }
    }
    catch {
        Write-TextFile -Path $StdoutPath -Content ''
        Write-TextFile -Path $StderrPath -Content $_.Exception.ToString()
        return [PSCustomObject]@{ ExitCode = $null; Started = $false; TimedOut = $false }
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = -not $process.WaitForExit($probeTimeoutMilliseconds)
    if ($timedOut) {
        $process.Kill()
        $process.WaitForExit()
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    Write-TextFile -Path $StdoutPath -Content $stdout
    Write-TextFile -Path $StderrPath -Content $stderr

    return [PSCustomObject]@{
        ExitCode = $process.ExitCode
        Started = $true
        TimedOut = $timedOut
    }
}

function Report-Probe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [bool]$Passed,

        [Parameter(Mandatory = $true)]
        [object]$Result,

        [Parameter(Mandatory = $true)]
        [string]$StdoutPath,

        [Parameter(Mandatory = $true)]
        [string]$StderrPath
    )

    $status = if ($Passed) { 'PASS' } else { 'FAIL' }
    $exitCode = if ($null -eq $Result.ExitCode) { 'not-started' } else { [string]$Result.ExitCode }
    Write-Output "$status $Name exit=$exitCode timed_out=$($Result.TimedOut)"
    if (-not $Passed) {
        Write-Output "--- $Name stdout ($StdoutPath) ---"
        Get-Content -LiteralPath $StdoutPath
        Write-Output "--- $Name stderr ($StderrPath) ---"
        Get-Content -LiteralPath $StderrPath
    }
}

if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) {
    throw 'SystemRoot is required to construct the scrubbed probe environment'
}

$resolvedBundleDir = (Resolve-Path -LiteralPath $BundleDir).Path
$resolvedTestCoreExe = (Resolve-Path -LiteralPath $TestCoreExe).Path
$moshExe = Join-Path $resolvedBundleDir 'mosh.exe'
$manifest = Join-Path $resolvedBundleDir 'MANIFEST.txt'
if (-not (Test-Path -LiteralPath $moshExe -PathType Leaf)) {
    throw "bundle does not contain mosh.exe: $moshExe"
}
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    throw "bundle does not contain MANIFEST.txt: $manifest"
}
if (-not (Test-Path -LiteralPath $resolvedTestCoreExe -PathType Leaf)) {
    throw "test_core.exe does not exist: $resolvedTestCoreExe"
}

$scratchRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("mosh-bundle-verify-" + [Guid]::NewGuid().ToString())
$bundleA = Join-Path $scratchRoot 'bundle-a'
$bundleB = Join-Path $scratchRoot 'bundle-b'
$workingDirectory = Join-Path $scratchRoot 'working-directory'
$overallPassed = $false

try {
    New-Item -ItemType Directory -Path $bundleA, $bundleB, $workingDirectory | Out-Null
    Copy-Item -Path (Join-Path $resolvedBundleDir '*') -Destination $bundleA -Recurse -Force
    Copy-Item -Path (Join-Path $resolvedBundleDir '*') -Destination $bundleB -Recurse -Force
    Copy-Item -LiteralPath $resolvedTestCoreExe -Destination (Join-Path $bundleA 'test_core.exe') -Force

    if ($workingDirectory -eq $bundleA -or $workingDirectory -eq $bundleB) {
        throw 'probe working directory must not be a bundle directory'
    }

    $probe1Stdout = Join-Path $scratchRoot 'probe-1-stdout.txt'
    $probe1Stderr = Join-Path $scratchRoot 'probe-1-stderr.txt'
    $probe1 = Start-ScrubbedProbe -Executable (Join-Path $bundleA 'mosh.exe') -WorkingDirectory $workingDirectory -StdoutPath $probe1Stdout -StderrPath $probe1Stderr
    $probe1Passed = $probe1.Started -and -not $probe1.TimedOut -and $probe1.ExitCode -eq 2
    Report-Probe -Name 'Probe 1 (mosh.exe load-time DLL resolution)' -Passed $probe1Passed -Result $probe1 -StdoutPath $probe1Stdout -StderrPath $probe1Stderr

    $probe2Stdout = Join-Path $scratchRoot 'probe-2-stdout.txt'
    $probe2Stderr = Join-Path $scratchRoot 'probe-2-stderr.txt'
    $probe2 = Start-ScrubbedProbe -Executable (Join-Path $bundleA 'test_core.exe') -WorkingDirectory $workingDirectory -StdoutPath $probe2Stdout -StderrPath $probe2Stderr
    $probe2Passed = $probe2.Started -and -not $probe2.TimedOut -and $probe2.ExitCode -eq 0
    Report-Probe -Name 'Probe 2 (test_core.exe full runtime)' -Passed $probe2Passed -Result $probe2 -StdoutPath $probe2Stdout -StderrPath $probe2Stderr

    $firstDll = $null
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ($line -match '^[0-9a-fA-F]{64}\s+\d+\s+([^\s]+\.dll)\s+source=') {
            $firstDll = $Matches[1]
            break
        }
    }

    if ($null -eq $firstDll) {
        if ($ExpectDlls) {
            Write-Output 'FAIL Probe 3 (missing-DLL negative control) exit=not-run timed_out=False'
            Write-Output 'ERROR: MANIFEST.txt has no staged DLL payload although -ExpectDlls was passed.'
            $probe3Passed = $false
        }
        else {
            Write-Output 'PASS Probe 3 (missing-DLL negative control) exit=skipped timed_out=False'
            Write-Output 'NOTE: MANIFEST.txt has no staged DLL payload; skipping negative control without -ExpectDlls.'
            $probe3Passed = $true
        }
    }
    else {
        $missingDll = Join-Path $bundleB $firstDll
        if (-not (Test-Path -LiteralPath $missingDll -PathType Leaf)) {
            throw "MANIFEST.txt named DLL payload not present in scratch bundle: $firstDll"
        }
        Remove-Item -LiteralPath $missingDll -Force

        $probe3Stdout = Join-Path $scratchRoot 'probe-3-stdout.txt'
        $probe3Stderr = Join-Path $scratchRoot 'probe-3-stderr.txt'
        $probe3 = Start-ScrubbedProbe -Executable (Join-Path $bundleB 'mosh.exe') -WorkingDirectory $workingDirectory -StdoutPath $probe3Stdout -StderrPath $probe3Stderr
        $probe3Passed = $probe3.Started -and -not $probe3.TimedOut -and $probe3.ExitCode -ne 0 -and $probe3.ExitCode -ne 2
        Report-Probe -Name 'Probe 3 (missing-DLL negative control)' -Passed $probe3Passed -Result $probe3 -StdoutPath $probe3Stdout -StderrPath $probe3Stderr
    }

    $overallPassed = $probe1Passed -and $probe2Passed -and $probe3Passed
    if (-not $overallPassed) {
        throw "bundle verification failed; probe diagnostics remain in $scratchRoot"
    }
}
finally {
    if ($overallPassed -and (Test-Path -LiteralPath $scratchRoot)) {
        Remove-Item -LiteralPath $scratchRoot -Recurse -Force
    }
}
