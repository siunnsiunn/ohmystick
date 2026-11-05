# Put winvoice in the Startup folder so it runs, without a console window,
# every time this user signs in. Run once:
#   powershell -ExecutionPolicy Bypass -File install-startup.ps1
# Remove with: install-startup.ps1 -Uninstall
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$lnk = Join-Path ([Environment]::GetFolderPath('Startup')) 'winvoice.lnk'

if ($Uninstall) {
    if (Test-Path $lnk) { Remove-Item $lnk; Write-Host "Removed $lnk" } else { Write-Host 'Not installed' }
    exit 0
}

if (-not (Test-Path (Join-Path $dir 'winvoice.ini'))) {
    throw "winvoice.ini not found in $dir (copy winvoice.example.ini and fill in the key first)"
}

# pyw.exe comes with the python.org installer's "py launcher"; pythonw.exe as a fallback.
$pyw = (Get-Command pyw.exe -ErrorAction SilentlyContinue).Source
if (-not $pyw) { $pyw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source }
if (-not $pyw) { throw 'pyw.exe / pythonw.exe not found: install Python from python.org first' }

$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $pyw
$s.Arguments = '"' + (Join-Path $dir 'winvoice.py') + '"'
$s.WorkingDirectory = $dir
$s.Description = 'winvoice: Ctrl+F12 dictation'
$s.Save()
Write-Host "Installed $lnk"
Write-Host 'Starting it now...'
Start-Process -FilePath $pyw -ArgumentList $s.Arguments -WorkingDirectory $dir
