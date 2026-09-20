param(
    [string]$IdfPath = "",
    [string]$ToolsPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $IdfPath) { $IdfPath = Join-Path $repo "build/toolchains/esp-idf" }
if (-not $ToolsPath) { $ToolsPath = Join-Path $repo "build/toolchains/tools" }
if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    throw "ESP-IDF 5.5 is required. Pass -IdfPath and -ToolsPath for your installation."
}
$python = Get-ChildItem (Join-Path $ToolsPath "python_env/idf5.5_py*_env/Scripts/python.exe") |
    Select-Object -First 1
if (-not $python) { throw "No ESP-IDF 5.5 Python environment found under $ToolsPath." }

$env:IDF_TOOLS_PATH = (Resolve-Path $ToolsPath).Path
$env:IDF_COMPONENT_CACHE_PATH = Join-Path $repo "build/component-cache"
$env:CCACHE_DIR = Join-Path $repo "build/ccache"
$env:PATH = "$($python.DirectoryName);$env:PATH"
Push-Location $repo
try {
    . (Join-Path $IdfPath "export.ps1")
    & $python.FullName (Join-Path $IdfPath "tools/idf.py") -B build/firmware build
    if ($LASTEXITCODE -ne 0) { throw "Firmware build failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}
