# 复用脚本：激活 ESP-IDF v6.1 环境后执行给定参数。
# 用法: powershell -File tools\idf.ps1 build
#       powershell -File tools\idf.ps1 -p COM5 flash
#       powershell -File tools\idf.ps1 size
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)
$ErrorActionPreference = 'Stop'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' | Out-Null
Set-Location (Split-Path -Parent $PSScriptRoot)
& idf.py @IdfArgs
exit $LASTEXITCODE
