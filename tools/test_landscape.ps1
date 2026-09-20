param(
    [string]$Compiler = "gcc",
    [string]$LvglDir = "",
    [switch]$Rebuild,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo "build/host"
if (-not $LvglDir) { $LvglDir = Join-Path $output "lvgl" }
if (-not (Test-Path (Join-Path $LvglDir "lvgl.h"))) {
    throw "LVGL 9.5.0 is required. Run: git clone --branch v9.5.0 --depth 1 https://github.com/lvgl/lvgl.git build/host/lvgl"
}
$LvglDir = (Resolve-Path $LvglDir).Path
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
New-Item -ItemType Directory -Force -Path $output | Out-Null

function Write-ResponseFile([string]$Path, [string[]]$Arguments) {
    # GCC response files use their own quoting, independent of PowerShell.
    $quoted = $Arguments | ForEach-Object { '"' + $_.Replace('\', '/').Replace('"', '\"') + '"' }
    [System.IO.File]::WriteAllLines($Path, $quoted, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-Compiler([string]$ResponsePath) {
    & $compilerPath "@$ResponsePath"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed (exit $LASTEXITCODE)." }
}

$common = @(
    "-std=c11", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_KCONFIG_IGNORE",
    "-I$(Join-Path $repo 'tests')", "-I$(Join-Path $repo 'tests/include')",
    "-I$(Join-Path $repo 'main')", "-I$LvglDir"
)
$library = Join-Path $output "lvgl.o"
$stampPath = Join-Path $output "lvgl-build.txt"
$configHash = (Get-FileHash (Join-Path $repo "tests/lv_conf.h")).Hash
$version = (& $compilerPath --version | Select-Object -First 1)
$libraryHead = if (Test-Path (Join-Path $LvglDir ".git")) {
    & git -C $LvglDir rev-parse HEAD
} else {
    (Get-FileHash (Join-Path $LvglDir "lv_version.h")).Hash
}
$sourceExclusion = '[\\/]drivers[\\/]|[\\/]libs[\\/]gltf[\\/]'
$stamp = "$version`n$compilerPath`n$libraryHead`n$configHash`n$sourceExclusion`n$($common -join ' ')"
$previousStamp = if (Test-Path $stampPath) { [System.IO.File]::ReadAllText($stampPath) } else { "" }
if ($Rebuild -or -not (Test-Path $library) -or $previousStamp -ne $stamp) {
    Write-Host "Building LVGL with a 16 KB allocator..."
    # No display driver or glTF renderer is used by the memory-only preview.
    $sources = Get-ChildItem (Join-Path $LvglDir "src") -Filter *.c -Recurse |
        Where-Object { $_.FullName -notmatch $sourceExclusion } |
        Sort-Object FullName | Select-Object -ExpandProperty FullName
    $response = Join-Path $output "lvgl.rsp"
    Write-ResponseFile $response ($common + @("-r", "-nostdlib", "-o", $library) + $sources)
    Invoke-Compiler $response
    [System.IO.File]::WriteAllText($stampPath, $stamp)
}

$executable = Join-Path $output "preview_landscape.exe"
$modelTest = Join-Path $output "test_landscape.exe"
$response = Join-Path $output "model-test.rsp"
Write-ResponseFile $response ($common + @(
    "-Wall", "-Wextra", "-Werror", "-o", $modelTest,
    (Join-Path $repo "tests/test_landscape.c"),
    (Join-Path $repo "main/landscape.c"), "-lm"
))
Invoke-Compiler $response
$appSources = @("tests/preview_landscape.c", "main/landscape.c", "main/landscape_art.c", "main/ui_landscape.c") |
    ForEach-Object { Join-Path $repo $_ }
$response = Join-Path $output "landscape.rsp"
Write-ResponseFile $response ($common + @("-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-o", $executable) + $appSources + @($library, "-lm"))
Invoke-Compiler $response
Write-Host "Built $executable"
if (-not $BuildOnly) {
    Push-Location $output
    try {
        & $modelTest
        if ($LASTEXITCODE -ne 0) { throw "Landscape model tests failed (exit $LASTEXITCODE)." }
        $scenarios = @(
            "day", "dusk", "night", "cloudy", "rain", "snow", "storm", "fog",
            "forecast", "stale", "waiting", "watch", "warning", "stale-warning", "recovery",
            "long-watch", "long-warning", "missing-temperature", "aging"
        )
        foreach ($scenario in $scenarios) {
            & $executable $scenario
            if ($LASTEXITCODE -ne 0) { throw "Landscape preview '$scenario' failed (exit $LASTEXITCODE)." }
        }
    } finally {
        Pop-Location
    }
}
