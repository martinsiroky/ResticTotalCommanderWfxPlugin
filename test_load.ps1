Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class DllTest {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr LoadLibraryW(string lpFileName);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FreeLibrary(IntPtr hModule);
}
"@

$searchDir = "D:\Martin\Programming\CPP\Total Commander Restic Plugin"
$wfxFiles = Get-ChildItem $searchDir -Recurse -Filter "*.wfx" -ErrorAction SilentlyContinue

if ($wfxFiles.Count -eq 0) {
    Write-Host "No .wfx files found! Build the project in CLion first."
    exit
}

foreach ($f in $wfxFiles) {
    Write-Host "Testing: $($f.FullName)"
    Write-Host "File size: $($f.Length) bytes"

    $handle = [DllTest]::LoadLibraryW($f.FullName)
    if ($handle -eq [IntPtr]::Zero) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $msg = (New-Object System.ComponentModel.Win32Exception($err)).Message
        Write-Host "FAILED - Error $err : $msg"
    } else {
        Write-Host "SUCCESS - DLL loaded!"
        [DllTest]::FreeLibrary($handle) | Out-Null
    }
    Write-Host ""
}
