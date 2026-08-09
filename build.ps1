$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio Build Tools was not found.'
}

$installation = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installation) {
    throw 'MSBuild was not found.'
}

$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$configuration = if ($args -contains '-Debug') { 'Debug' } else { 'Release' }
& $msbuild "$PSScriptRoot\ClipboardTool.vcxproj" /nologo /m /t:Build "/p:Configuration=$configuration;Platform=x64"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$output = Join-Path $PSScriptRoot "x64\$configuration\ClipNest.exe"
Write-Host "Built: $output"
