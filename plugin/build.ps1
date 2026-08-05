# Build du plugin kernel (.skprx) pour PS TV.
#
# Prerequis installes le 2026-07-31 sur cette machine :
#   - VitaSDK   C:\vitasdk\vitasdk   (autobuild master-win-v2.540, gcc 15.2.0)
#   - taiHEN    installe via "vdpm taihen"
#   - cmake / ninja  via scoop
#   - make      C:\vitasdk\vitasdk\bin\make.exe (requis par vita_create_stubs)
#
# Usage :  .\build.ps1  [-Clean]

param([switch]$Clean)

$ErrorActionPreference = "Stop"

$env:VITASDK = "C:/vitasdk/vitasdk"
$env:PATH = "$env:VITASDK/bin;$env:PATH"

$src = $PSScriptRoot
$build = Join-Path $src "build"

if ($Clean -and (Test-Path $build)) {
    Remove-Item $build -Recurse -Force
}

cmake -S $src -B $build -G Ninja
if ($LASTEXITCODE -ne 0) { throw "Echec de la configuration CMake" }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "Echec de la compilation" }

$skprx = Join-Path $build "ds34vita.skprx"
if (-not (Test-Path $skprx)) { throw "Le .skprx n'a pas ete produit" }

# Un SELF valide commence par le magic "SCE\0".
$magic = [System.IO.File]::ReadAllBytes($skprx)[0..3]
if (($magic[0] -ne 0x53) -or ($magic[1] -ne 0x43) -or ($magic[2] -ne 0x45) -or ($magic[3] -ne 0x00)) {
    throw "Magic SELF invalide dans $skprx"
}

$size = (Get-Item $skprx).Length
Write-Output ""
Write-Output "OK  $skprx  ($size octets, magic SCE valide)"
Write-Output ""
Write-Output "Installation sur la PS TV :"
Write-Output "  1. copier ds34vita.skprx dans ur0:/tai/"
Write-Output "  2. le declarer dans ur0:/tai/config.txt sous la section *KERNEL"
