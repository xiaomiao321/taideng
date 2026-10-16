param([string]$Name = 'BoardDebugJointLimits')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$previousUserBase = $env:PYTHONUSERBASE
try {
    # Avoid broken redirected Windows user-site paths during DLL collection.
    $env:PYTHONUSERBASE = Join-Path $PSScriptRoot 'build/python_userbase'
    python -s -m PyInstaller --noconfirm --onefile --windowed --name $Name --distpath dist --workpath build app.py
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
} finally {
    $env:PYTHONUSERBASE = $previousUserBase
    Pop-Location
}
