param(
    [ValidateSet('build', 'flash', 'monitor')]
    [string]$Action = 'build',
    [string]$Port = 'COM7',
    [string]$IdfPath = 'D:\esp\v6.1\esp-idf',
    [string]$ToolsPath = 'D:\Espressif'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$venv = Join-Path $ToolsPath 'python_env\idf6.1_py3.13_env'
$python = Join-Path $venv 'Scripts\python.exe'
$exportPython = Join-Path $ToolsPath 'tools\python\v6.1\venv\Scripts\python.exe'
if (!(Test-Path -LiteralPath $python) -or !(Test-Path -LiteralPath $exportPython)) {
    throw 'ESP-IDF 6.1 Python environments were not found. Check -ToolsPath.'
}
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $venv
$env:ESP_IDF_VERSION = '6.1.0'
$env:PYTHONUTF8 = '1'
$exports = & $exportPython (Join-Path $IdfPath 'tools\idf_tools.py') export --format=key-value
if ($LASTEXITCODE -ne 0) { throw 'Failed to export the ESP-IDF toolchain.' }
foreach ($line in $exports) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2].Trim('"')
        Set-Item -Path "Env:$name" -Value $value
    }
}
Push-Location -LiteralPath $project
try {
    & $python (Join-Path $IdfPath 'tools\idf.py') -p $Port $Action
    $result = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $result
