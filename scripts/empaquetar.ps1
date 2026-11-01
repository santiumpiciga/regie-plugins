# Compila los dos plugins y arma los paquetes de un Release:
#
#   dist/regie-cs2-plugin-<Tag>.zip    RegieGG.dll, .deps.json, LICENSE,
#                                      GPL-3.0.txt y fuente/ (el código y el .csproj)
#   dist/regie-cs16-plugin-<Tag>.zip   RegieGG.amxx y fuente/ (.sma, .inc, LICENSE, GPL-3.0.txt)
#   dist/SHA256SUMS.txt                la huella de cada zip
#
# Los instaladores de Regie.gg bajan estos zips por versión fija y verifican la
# huella (desktop/plugins-release.json en Regie.gg).
#
#   pwsh ./scripts/empaquetar.ps1 -Tag v0.2.0
#   pwsh ./scripts/empaquetar.ps1 -Tag prueba -ScriptingAmxx 'C:\hlds\cstrike\addons\amxmodx\scripting'
#       (usa un amxxpc.exe y unos include ya instalados en vez de bajarlos)
param(
  [Parameter(Mandatory = $true)][string]$Tag,
  [string]$Salida = '',
  # La misma versión del stack con la que se prueba Regie.gg.
  [string]$AmxxTag = '1.9.0.5303',
  [string]$AmxxZip = 'amxmodx-1.9.0-git5303-base-windows.zip',
  [string]$ReapiTag = '5.29.0.358',
  [string]$ScriptingAmxx = ''
)
$ErrorActionPreference = 'Stop'
$raiz = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Salida) { $Salida = Join-Path $raiz 'dist' }
$Salida = [System.IO.Path]::GetFullPath($Salida)
$trabajo = Join-Path ([System.IO.Path]::GetTempPath()) ("regie_plugins_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $trabajo, $Salida | Out-Null

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Rutas con «/» dentro del zip (Compress-Archive de Windows PowerShell 5.1 las
# escribe con «\», y los instaladores lo descomprimen con Expand-Archive).
function Comprimir($carpeta, $zip) {
  if (Test-Path $zip) { Remove-Item $zip -Force }
  $base = (Resolve-Path $carpeta).Path.TrimEnd('\')
  $flujo = [System.IO.File]::Open($zip, [System.IO.FileMode]::Create)
  $za = New-Object System.IO.Compression.ZipArchive($flujo, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    Get-ChildItem $base -Recurse -File | ForEach-Object {
      $rel = $_.FullName.Substring($base.Length + 1).Replace('\', '/')
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally {
    $za.Dispose()
    $flujo.Dispose()
  }
}

try {
  # --- CS2: CounterStrikeSharp -------------------------------------------
  Write-Host '== CS2: dotnet build'
  $csproj = Join-Path $raiz 'cs2-plugin\Cs2LiveViewerHelper.csproj'
  & dotnet build $csproj -c Release
  if ($LASTEXITCODE -ne 0) { throw 'dotnet build falló' }
  $bin = Join-Path $raiz 'cs2-plugin\bin\Release\net8.0'
  $cs2 = Join-Path $trabajo 'cs2'
  New-Item -ItemType Directory -Force (Join-Path $cs2 'fuente') | Out-Null
  foreach ($f in 'RegieGG.dll', 'RegieGG.deps.json') {
    Copy-Item (Join-Path $bin $f) $cs2
  }
  Copy-Item (Join-Path $raiz 'cs2-plugin\LICENSE') $cs2
  Copy-Item (Join-Path $raiz 'LICENSE') (Join-Path $cs2 'GPL-3.0.txt')
  Get-ChildItem (Join-Path $raiz 'cs2-plugin') -File | ForEach-Object { Copy-Item $_.FullName (Join-Path $cs2 'fuente') }
  Copy-Item (Join-Path $raiz 'LICENSE') (Join-Path $cs2 'fuente\GPL-3.0.txt')
  Comprimir $cs2 (Join-Path $Salida "regie-cs2-plugin-$Tag.zip")

  # --- CS 1.6: AMX Mod X -------------------------------------------------
  Write-Host '== CS 1.6: amxxpc'
  $scripting = Join-Path $trabajo 'scripting'
  if ($ScriptingAmxx) {
    Copy-Item $ScriptingAmxx $scripting -Recurse
  } else {
    $dl = Join-Path $trabajo 'descargas'
    New-Item -ItemType Directory -Force $dl | Out-Null
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest "https://github.com/alliedmodders/amxmodx/releases/download/$AmxxTag/$AmxxZip" -OutFile (Join-Path $dl 'amxx.zip')
    Invoke-WebRequest "https://github.com/rehlds/ReAPI/releases/download/$ReapiTag/reapi-bin-$ReapiTag.zip" -OutFile (Join-Path $dl 'reapi.zip')
    Expand-Archive (Join-Path $dl 'amxx.zip') (Join-Path $dl 'amxx') -Force
    Expand-Archive (Join-Path $dl 'reapi.zip') (Join-Path $dl 'reapi') -Force
    Copy-Item (Join-Path $dl 'amxx\addons\amxmodx\scripting') $scripting -Recurse
    Copy-Item (Join-Path $dl 'reapi\addons\amxmodx\scripting\include\*') (Join-Path $scripting 'include') -Force
  }
  $fuente16 = Join-Path $trabajo 'cs16\fuente'
  New-Item -ItemType Directory -Force $fuente16 | Out-Null
  $compilar = Join-Path $trabajo 'compilar'
  New-Item -ItemType Directory -Force $compilar | Out-Null
  Copy-Item (Join-Path $scripting 'amxxpc.exe'), (Join-Path $scripting 'amxxpc32.dll') $compilar
  Copy-Item (Join-Path $scripting 'include') (Join-Path $compilar 'include') -Recurse
  Get-ChildItem (Join-Path $raiz 'cs16-plugin') -File | Where-Object { $_.Name -notlike '*.amxx' } | ForEach-Object {
    Copy-Item $_.FullName $compilar
    Copy-Item $_.FullName $fuente16
  }
  Copy-Item (Join-Path $raiz 'LICENSE') (Join-Path $fuente16 'GPL-3.0.txt')
  Push-Location $compilar
  try {
    $salidaPc = & .\amxxpc.exe 'RegieGG.sma' '-oRegieGG.amxx' 2>&1 | Out-String
  } finally { Pop-Location }
  Write-Host $salidaPc
  $amxx = Join-Path $compilar 'RegieGG.amxx'
  if (-not (Test-Path $amxx) -or $salidaPc -match '\berror\b') { throw 'amxxpc no compiló RegieGG.sma' }
  Copy-Item $amxx (Join-Path $trabajo 'cs16')
  Comprimir (Join-Path $trabajo 'cs16') (Join-Path $Salida "regie-cs16-plugin-$Tag.zip")

  # --- huellas -----------------------------------------------------------
  $lineas = Get-ChildItem $Salida -Filter "regie-*-plugin-$Tag.zip" | Sort-Object Name | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"
  }
  Set-Content (Join-Path $Salida 'SHA256SUMS.txt') $lineas -Encoding ascii
  Write-Host '== Listo:'
  $lineas | ForEach-Object { Write-Host $_ }
} finally {
  Remove-Item $trabajo -Recurse -Force -ErrorAction SilentlyContinue
}
