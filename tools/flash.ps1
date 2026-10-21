# Builds (if needed) and flashes one board from Windows PowerShell.
#
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Board tdisplay -Port COM4
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Board jc3248 -Port COM8 -Monitor
#
# -Idf points to the ESP-IDF checkout (pinned commit, see README), -Tools to its
# IDF_TOOLS_PATH. Build directories go next to the IDF (short paths: Windows'
# 260-character limit breaks deep ESP-IDF builds).
param(
    [ValidateSet('tdisplay', 'jc3248')][string]$Board = 'tdisplay',
    [string]$Port = 'COM4',
    [string]$Idf = 'C:\esp\esp-idf-sdr',
    [string]$Tools = 'C:\esp\tools',
    [switch]$Monitor
)

$env:IDF_TOOLS_PATH = $Tools
. (Join-Path $Idf 'export.ps1') | Out-Null
Set-Location (Join-Path $PSScriptRoot '..')

$target = if ($Board -eq 'jc3248') { 'esp32s3' } else { 'esp32' }
$build = Join-Path (Split-Path $Idf) "build-$Board"
$args_ = @('-B', $build, "-DIDF_TARGET=$target",
           "-DSDKCONFIG=$($build -replace '\\', '/')/sdkconfig",
           "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.$Board",
           '-p', $Port, 'flash')
if ($Monitor) { $args_ += 'monitor' }
idf.py @args_
