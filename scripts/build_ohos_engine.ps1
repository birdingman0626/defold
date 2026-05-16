# Local arm64-ohos engine build for birdingman0626/defold fork.
#
# This is the single entry point future sessions / developers can run
# to attempt a local engine build for arm64-ohos. It sets the env vars
# scripts/build.py expects without going through the interactive
# ./scripts/build.py shell wrapper.
#
# Prereqs:
#   * Python 3.13 on PATH
#   * Java 25 (e.g. graalvm-jdk-25.0.3+9.1) somewhere known
#   * CMake + Ninja on PATH
#   * OHOS Native SDK installed somewhere (DevEco Studio's
#     command-line-tools ships one at sdk/default/openharmony/native)
#   * A scratch DYNAMO_HOME directory (defaults to <repo>/tmp/dynamo_home)
#
# State of play: install_ext + check_sdk succeed for arm64-ohos. The
# `build_engine` step still fails further along on host-side Windows
# SDK detection inside build_tools/waf_dynamo.py (this is a Defold
# Windows-host infra issue, not an OHOS port issue). Linux host is
# the easier path to actually compile from.

[CmdletBinding()]
param(
    [string]$JavaHome  = 'D:\DevTools\graalvm-jdk-25.0.3+9.1',
    [string]$OhosNdk   = 'D:\DevTools\command-line-tools\sdk\default\openharmony\native',
    [string]$DefoldRoot = $PSScriptRoot.Substring(0, $PSScriptRoot.Length - 8), # strip "\scripts"
    [string]$DynamoHome,
    [ValidateSet('install_ext', 'check_sdk', 'build_engine')]
    [string]$Step = 'build_engine'
)

$ErrorActionPreference = 'Stop'

if (-not $DynamoHome) {
    $DynamoHome = Join-Path $DefoldRoot 'tmp\dynamo_home'
}

Write-Host "DEFOLD_HOME    = $DefoldRoot" -ForegroundColor DarkGray
Write-Host "DYNAMO_HOME    = $DynamoHome" -ForegroundColor DarkGray
Write-Host "JAVA_HOME      = $JavaHome" -ForegroundColor DarkGray
Write-Host "OHOS_NDK_PATH  = $OhosNdk" -ForegroundColor DarkGray
Write-Host ""

if (-not (Test-Path $OhosNdk)) {
    throw "OHOS NDK not found at $OhosNdk. Set -OhosNdk or install via DevEco Studio."
}

$env:DEFOLD_HOME       = $DefoldRoot
$env:DYNAMO_HOME       = $DynamoHome
$env:PYTHONPATH        = "$DynamoHome\lib\python;$DefoldRoot\build_tools;$DynamoHome\ext\lib\python"
$env:PYTHONIOENCODING  = 'UTF-8'
$env:JAVA_HOME         = $JavaHome
$env:OHOS_NDK_PATH     = $OhosNdk
$env:OHOS_NDK_BIN_PATH = "$OhosNdk\llvm\bin"
$env:OHOS_NDK_SYSROOT  = "$OhosNdk\sysroot"
$env:PATH = "$DynamoHome\ext\bin\x86_64-win32;$JavaHome\bin;$env:PATH"

Set-Location $DefoldRoot

python scripts/build.py --platform=arm64-ohos --skip-tests --skip-builtins --skip-docs --skip-bob-light $Step
