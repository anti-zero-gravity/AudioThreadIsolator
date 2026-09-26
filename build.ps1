$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")

# Kill running ATI.exe if exists
taskkill.exe /F /IM ATI.exe 2>$null

Write-Host "Checking g++ location..."
$gpp = Get-Command g++ -ErrorAction SilentlyContinue
if ($gpp) {
    Write-Host "Found g++: $($gpp.Source)"
} else {
    Write-Host "g++ not found in Path directly, searching..."
    $possible = @(
        "C:\msys64\ucrt64\bin",
        "C:\msys64\mingw64\bin",
        "C:\ProgramData\chocolatey\bin"
    )
    foreach ($p in $possible) {
        if (Test-Path "$p\g++.exe") {
            $env:Path = "$p;" + $env:Path
            Write-Host "Added $p to Path"
            break
        }
    }
}

if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

Write-Host "Compiling resource..."
& windres src/resource.rc -O coff -o build/resource.res
if ($LASTEXITCODE -ne 0) {
    Write-Host "windres failed with exit code $LASTEXITCODE"
    exit 1
}

Write-Host "Compiling C++ sources..."
& g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ src/main.cpp src/process_picker.cpp src/isolator.cpp src/priority_matrix_picker.cpp build/resource.res -lcomctl32 -lshlwapi -ldwmapi -lpsapi -lgdiplus -o ATI.exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "g++ failed with exit code $LASTEXITCODE"
    exit 1
}

Write-Host "Stripping binary..."
& strip ATI.exe

Write-Host "BUILD_SUCCESS"
