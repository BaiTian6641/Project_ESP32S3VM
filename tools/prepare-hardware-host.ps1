param()
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$environmentPath = Join-Path $projectRoot 'build-hardware-host'
$pythonPath = Join-Path $environmentPath 'Scripts/python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) {
    if (Test-Path -LiteralPath $environmentPath) {
        throw 'Existing hardware environment is incomplete; preserve it instead of overwriting it.'
    }
    python -m venv $environmentPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the local hardware tool environment.' }
}
& $pythonPath -m pip install --disable-pip-version-check 'esptool==5.4.0' 'pyserial==3.5'
if ($LASTEXITCODE -ne 0) { throw 'Hardware tool installation failed.' }
& $pythonPath -m pip freeze | Set-Content -LiteralPath (Join-Path $environmentPath 'installed-packages.txt') -Encoding utf8
Write-Output "Hardware tools ready: $pythonPath"
Write-Output 'No serial port was opened and no firmware was written.'
