# Generate build_ninja.bat (CRLF + pure ASCII)
lines = [
    "@echo off",
    'call "C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\Common7\\Tools\\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1',
    "set VCPKG_ROOT=E:\\vcpkg-full",
    "set VCPKG_DOWNLOADS=E:\\vcpkg-full\\downloads",
    "rem sandbox blocks reg.exe inside VsDevCmd (SDK lookup) - patch env manually",
    'set "SDKROOT=C:\\Program Files (x86)\\Windows Kits\\10"',
    'set "SDKVER=10.0.26100.0"',
    'set "LIB=%SDKROOT%\\Lib\\%SDKVER%\\um\\x64;%SDKROOT%\\Lib\\%SDKVER%\\ucrt\\x64;%LIB%"',
    'set "INCLUDE=%SDKROOT%\\Include\\%SDKVER%\\um;%SDKROOT%\\Include\\%SDKVER%\\ucrt;%SDKROOT%\\Include\\%SDKVER%\\shared;%SDKROOT%\\Include\\%SDKVER%\\winrt;%INCLUDE%"',
    'set "PATH=%SDKROOT%\\bin\\%SDKVER%\\x64;%PATH%"',
    "cd /d D:\\Modding\\ShadowLimitFix",
    "cmake --preset NINJA",
    "if errorlevel 1 exit /b 1",
    "cmake --build --preset NINJA",
    "if errorlevel 1 exit /b 1",
    "if not exist \"E:\\EJ\\mod\\mods\\ShadowLimitFix\\meta.ini\" (",
    "    echo [General]> \"E:\\EJ\\mod\\mods\\ShadowLimitFix\\meta.ini\"",
    "    echo installedBy=ShadowLimitFix build>> \"E:\\EJ\\mod\\mods\\ShadowLimitFix\\meta.ini\"",
    ")",
    "exit /b 0",
]
with open("D:/Modding/ShadowLimitFix/build_ninja.bat", "wb") as f:
    f.write(("\r\n".join(lines) + "\r\n").encode("ascii"))
print("build_ninja.bat written")
