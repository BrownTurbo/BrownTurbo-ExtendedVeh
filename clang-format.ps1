Get-ChildItem -Path "." -Recurse -Include *.cpp,*.h,*.hpp | Where-Object { $_.FullName -NotMatch '[\\/](~?\.dev|deps|build|vcpkg_installed)[\\/]' } | ForEach-Object {
    Write-Host "Formatting: $($_.FullName)" -ForegroundColor Cyan
    & "M:\Installations\Visual Studio 2026 Build Tools\VC\Tools\Llvm\x64\bin\clang-format" -style=file -i $_.FullName
}
