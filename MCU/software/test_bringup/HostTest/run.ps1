# Build and run the MCU protocol PC-side self tests.
# No MCU, no serial port, no flashing, no CMake - just gcc on this PC.
#
# Usage (from the test_bringup directory):
#   powershell -ExecutionPolicy Bypass -File HostTest\run.ps1
#
# To use another compiler, set MCU_HOST_GCC first, for example:
#   $env:MCU_HOST_GCC = 'D:\mingw64\bin\gcc.exe'
#
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 files as
# ANSI/GBK when they have no BOM, so non-ASCII characters can break parsing.

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $PSScriptRoot
$gcc = if ($env:MCU_HOST_GCC) { $env:MCU_HOST_GCC } else { 'gcc' }

# Make the console decode child-process output as UTF-8. Chinese Windows uses
# code page 936 (GBK) by default, which turns UTF-8 output into garbage.
# Terminals that refuse the switch are fine; the test output is pure ASCII.
try
{
    $null = chcp 65001
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
}
catch
{
}

$includeArgs = @('-I', (Join-Path $projectDir 'BSP\Inc'), '-I', (Join-Path $projectDir 'Protocol\Inc'), '-I', (Join-Path $projectDir 'App\inc'))
$warnArgs = @('-std=c11', '-Wall', '-Wextra')

$suites = @(
    @{
        Name = 'joint'
        Sources = @(
            (Join-Path $projectDir 'App\src\joint_control.c')
            (Join-Path $projectDir 'HostTest\joint_selftest.c')
        )
    }
    @{
        Name = 'debug_board'
        Sources = @(
            (Join-Path $projectDir 'App\src\debug_board.c')
            (Join-Path $projectDir 'HostTest\debug_board_selftest.c')
        )
    }
    @{
        Name = 'debug_control'
        Sources = @(
            (Join-Path $projectDir 'App\src\debug_control.c')
            (Join-Path $projectDir 'HostTest\debug_control_selftest.c')
        )
    }
    @{
        Name = 'protocol'
        Sources = @(
            (Join-Path $projectDir 'Protocol\Src\mcu_protocol.c')
            (Join-Path $projectDir 'HostTest\protocol_selftest.c')
        )
    }
    @{
        Name = 'command'
        Sources = @(
            (Join-Path $projectDir 'Protocol\Src\mcu_protocol.c')
            (Join-Path $projectDir 'Protocol\Src\mcu_command.c')
            (Join-Path $projectDir 'HostTest\command_selftest.c')
        )
    }
)

$failures = 0

foreach ($suite in $suites)
{
    $name = $suite.Name
    $exePath = Join-Path $projectDir ("HostTest\selftest_{0}.exe" -f $name)
    $gccArgs = $warnArgs + $includeArgs + $suite.Sources + @('-o', $exePath)

    Write-Host ("=== {0} ===" -f $name)
    & $gcc @gccArgs
    if ($LASTEXITCODE -ne 0)
    {
        Write-Host ("BUILD FAILED: {0}" -f $name) -ForegroundColor Red
        ++$failures
        continue
    }

    & $exePath
    if ($LASTEXITCODE -ne 0)
    {
        ++$failures
    }
    Write-Host ''
}

if ($failures -gt 0)
{
    Write-Host ("{0} self test suite(s) failed" -f $failures) -ForegroundColor Red
    exit 1
}

Write-Host 'all self tests passed'
exit 0
