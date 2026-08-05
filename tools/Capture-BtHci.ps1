<#
    Capture HCI Bluetooth Windows avec le profil officiel Microsoft busiotools.

    Ouvrir PowerShell en tant qu'administrateur, puis :
      .\tools\Capture-BtHci.ps1 -Action Start
      # allumer UNE manette, attendre sa reconnexion
      .\tools\Capture-BtHci.ps1 -Action Stop -Label v1-reconnect

    Labels admis : v1-pair, v1-reconnect, v2-pair, v2-reconnect.
    Une seule session WPR Bluetooth peut être active.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Start', 'Stop', 'Status')]
    [string]$Action,

    [ValidateSet('v1-pair', 'v1-reconnect', 'v2-pair', 'v2-reconnect')]
    [string]$Label,

    [string]$ParserPath
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Profile = Join-Path $ProjectRoot 'tools\vendor\BluetoothStack.wprp'
$CaptureRoot = Join-Path $ProjectRoot 'captures\hci'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "WPR exige un PowerShell lancé en tant qu'administrateur."
    }
}

function Assert-LastExitCode([string]$ActionName) {
    if ($LASTEXITCODE -ne 0) {
        throw "$ActionName a échoué (code $LASTEXITCODE)."
    }
}

function Get-Timestamp {
    Get-Date -Format 'yyyyMMdd-HHmmss'
}

function Get-DefaultParser {
    $vendored = Join-Path $ProjectRoot 'tools\vendor\BTETLParse.exe'
    if (Test-Path -LiteralPath $vendored) { return $vendored }

    $candidate = Get-Command btetlparse.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if ($candidate) { return $candidate }
    return $null
}

Assert-Administrator
if (-not (Test-Path -LiteralPath $Profile)) {
    throw "Profil Microsoft introuvable : $Profile"
}

switch ($Action) {
    'Status' {
        & wpr.exe -status
        Assert-LastExitCode "Lecture de l'etat WPR"
        break
    }

    'Start' {
        $wprStatus = & wpr.exe -status 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Impossible de lire l'etat WPR." }
        $wprStatus | Out-Host
        if ($wprStatus -match 'recording is in progress|enregistrement.*cours') {
            Write-Warning 'Une trace WPR est déjà active : ne pas en démarrer une seconde. Arrêtez-la avec -Action Stop et relancez Start seulement si vous voulez une fenêtre propre.'
            break
        }

        & wpr.exe -start "$Profile!BluetoothStack" -filemode
        Assert-LastExitCode 'Démarrage de la trace Bluetooth'

        Write-Host ''
        Write-Host 'Trace HCI démarrée.'
        Write-Host 'Allumer ou mettre en appairage UNE seule manette, selon la phase du protocole.'
        Write-Host 'Quand elle est reconnectée, arrêter avec :'
        Write-Host '  .\tools\Capture-BtHci.ps1 -Action Stop -Label v1-reconnect'
        Write-Host 'Les labels admis sont : v1-pair, v1-reconnect, v2-pair, v2-reconnect.'
        break
    }

    'Stop' {
        if (-not $Label) {
            throw '-Label est requis : v1-pair, v1-reconnect, v2-pair ou v2-reconnect.'
        }
        New-Item -ItemType Directory -Force -Path $CaptureRoot | Out-Null
        $stamp = Get-Timestamp
        $etl = Join-Path $CaptureRoot "$Label-$stamp.etl"

        & wpr.exe -stop $etl
        Assert-LastExitCode 'Arrêt de la trace Bluetooth'
        if (-not (Test-Path -LiteralPath $etl)) {
            throw "WPR n'a pas produit le fichier attendu : $etl"
        }

        $metadata = [ordered]@{
            label       = $Label
            captured_at = (Get-Date).ToString('o')
            profile     = $Profile
            profile_sha256 = (Get-FileHash -LiteralPath $Profile -Algorithm SHA256).Hash
            etl         = $etl
        } | ConvertTo-Json
        $metadataPath = [System.IO.Path]::ChangeExtension($etl, '.json')
        Set-Content -LiteralPath $metadataPath -Value $metadata -Encoding UTF8

        Write-Host "Capture sauvegardée : $etl"
        Write-Host "Métadonnées :         $metadataPath"

        $parser = if ($ParserPath) { $ParserPath } else { Get-DefaultParser }
        if ($parser) {
            $base = [System.IO.Path]::ChangeExtension($etl, $null)
            $hci = "$base.hci.txt"
            $pcapng = "$base.pcapng"
            & $parser -hci $hci -pcapng $pcapng $etl
            Assert-LastExitCode 'Conversion BTETLParse'
            Write-Host "HCI texte : $hci"
            Write-Host "PCAPNG :    $pcapng"
        } else {
            Write-Warning "BTETLParse introuvable : ETL conservé, conversion HCI/PCAPNG à faire après installation de l'outil Microsoft Bluetooth Test Platform."
        }
        break
    }
}
