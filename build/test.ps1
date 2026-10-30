param([string]$Compiler = 'D:\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
# Copy to a fresh directory because MinGW cannot reliably handle Chinese source paths.
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('dw-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$testRoot\src", "$testRoot\tests" | Out-Null
Copy-Item "$repoRoot\src\*.cpp", "$repoRoot\src\*.h" "$testRoot\src"
Copy-Item "$repoRoot\tests\startup_tests.cpp" "$testRoot\tests"
$flags = @(
    '-std=c++17', '-municode', '-O2', '-DUNICODE', '-D_UNICODE',
    '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000',
    '-Wall', '-Wno-unknown-pragmas', '-include', 'windowsx.h',
    '-o', 'startup_tests.exe', 'tests/startup_tests.cpp',
    'src/gfx.cpp', 'src/ui.cpp', 'src/core.cpp', 'src/thumbs.cpp',
    '-lole32', '-luuid', '-luser32', '-lgdi32', '-lshell32', '-lshlwapi',
    '-ladvapi32', '-ldwmapi', '-ld2d1', '-ldwrite', '-ld3d11', '-ldxgi',
    '-ldcomp', '-lwindowscodecs', '-lwinmm', '-limm32',
    '-static', '-static-libgcc', '-static-libstdc++'
)
Push-Location $testRoot
try {
    & $Compiler @flags
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed.' }
    & "$testRoot\startup_tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Startup regression tests failed.' }
} finally {
    Pop-Location
    Write-Host "Test artifacts: $testRoot"
}
