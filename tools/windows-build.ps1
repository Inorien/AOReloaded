# Builds a CMake project of ours on Windows with its windows-release preset (Visual Studio's clang-cl, through
# cmake\windows-clangcl.cmake). Dot-source it, then:
#   Invoke-WindowsBuild <source dir> [-Target <target>] [-BuildDir <the preset's binaryDir, relative to the source dir>]
# and check $LASTEXITCODE. CMake is the one on PATH, else Visual Studio's (found with vswhere).

function Find-CMake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
        if ($vs) {
            $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $cmake) { return $cmake }
        }
    }
    throw "no CMake: install Visual Studio Build Tools with 'C++ CMake tools' and 'C++ Clang Compiler for Windows'"
}

function Invoke-WindowsBuild([string]$Dir, [string]$Target, [string]$BuildDir = "build\windows-release") {
    $cmake = Find-CMake
    Push-Location $Dir
    try {
        # configure once; later builds re-run CMake by themselves when a CMakeLists changes
        if (-not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
            & $cmake --preset windows-release | Out-Host
            if ($LASTEXITCODE -ne 0) { return }
        }
        $buildArgs = @("--build", "--preset", "windows-release")
        if ($Target) { $buildArgs += @("--target", $Target) }
        & $cmake @buildArgs | Out-Host
    } finally {
        Pop-Location
    }
}
