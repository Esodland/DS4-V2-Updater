<#
    Extrait les cycles de reconnexion d'une DS4 depuis un journal ds3trace.

    Exemple :
      .\tools\Analyze-Ds3Trace.ps1 -InputPath .\logs\ds3trace-campaign.txt

    Le fichier peut contenir plusieurs demarrages : seul le dernier bloc
    ds3trace est analyse par defaut. Aucune ecriture distante n'est faite.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [string]$Mac = 'B26FBCD3:0000C822',

    [string]$CsvPath,

    [switch]$AllSessions
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $InputPath)) {
    throw "Journal introuvable : $InputPath"
}

$lines = @(Get-Content -LiteralPath $InputPath -Encoding UTF8)
if (-not $AllSessions) {
    $headers = @(
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if ($lines[$index] -match '^\s*===== ds3trace') { $index }
        }
    )
    if ($headers.Count -gt 0) {
        $lines = @($lines[$headers[-1]..($lines.Count - 1)])
    }
}

$records = @()
for ($index = 0; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match '^\s*(?<time>\d+)\s+(?<text>.*)$') {
        $records += [pscustomobject]@{
            Index = $index
            Time = [int64]$Matches.time
            Text = $Matches.text
        }
    }
}

$allStarts = @($records | Where-Object { $_.Text -match 'ReadEvent\s+0x08\s+[0-9A-F]{8}:[0-9A-F]{8}$' })
$starts = @($allStarts | Where-Object { $_.Text -match "ReadEvent\s+0x08\s+$([regex]::Escape($Mac))$" })
if ($starts.Count -eq 0) {
    Write-Warning "Aucun evenement 0x08 pour $Mac dans le bloc selectionne."
    return
}

$cycles = for ($cycleIndex = 0; $cycleIndex -lt $starts.Count; $cycleIndex++) {
    $start = $starts[$cycleIndex]
    # La sonde d'etat ne porte pas elle-meme la MAC. Des que n'importe quelle
    # autre manette emet 0x08, elle devient la cible : le cycle courant doit
    # donc s'arreter a CE 0x08, pas seulement au prochain 0x08 de la V2.
    $nextStart = $allStarts | Where-Object { $_.Index -gt $start.Index } | Select-Object -First 1
    $endIndex = if ($nextStart) { $nextStart.Index } else { [int]::MaxValue }
    $window = @($records | Where-Object { $_.Index -ge $start.Index -and $_.Index -lt $endIndex })

    $accepted = $window | Where-Object { $_.Text -match "ReadEvent\s+0x05\s+$([regex]::Escape($Mac))$" } | Select-Object -First 1
    $feature = $window | Where-Object { $_.Text -match "ReadEvent\s+0x0C\s+$([regex]::Escape($Mac))$" } | Select-Object -First 1
    $disconnected = $window | Where-Object { $_.Text -match "ReadEvent\s+0x06\s+$([regex]::Escape($Mac))$" } | Select-Object -First 1
    $input = $window | Where-Object { $_.Text -match "HidTransfer\s+$([regex]::Escape($Mac))\s+LECTURE" } | Select-Object -First 1
    $state3 = $window | Where-Object { $_.Text -match 'etat 2 -> 3' } | Select-Object -First 1
    $state4 = $window | Where-Object { $_.Text -match 'etat 2 -> 4' } | Select-Object -First 1

    $terminalTime = @(
        if ($state3) { $state3.Time }
        if ($state4) { $state4.Time }
        if ($disconnected) { $disconnected.Time }
    ) | Measure-Object -Minimum | Select-Object -ExpandProperty Minimum
    $driverDisconnect = $window | Where-Object {
        $_.Text -match "\*\*\* StartDisconnect\s+$([regex]::Escape($Mac))" -and
        (-not $terminalTime -or $_.Time -le $terminalTime)
    } | Select-Object -First 1

    $state = if ($state3) { 3 } elseif ($state4) { 4 } else { $null }
    $outcome = if ($state -eq 3 -and $input) {
        'succes-avec-entrees'
    } elseif ($state -eq 3) {
        'succes-sans-entree-observee'
    } elseif ($state -eq 4) {
        'echec-etat-4'
    } else {
        'incomplet-ou-hors-fenetre'
    }

    [pscustomobject]@{
        Cycle = $cycleIndex + 1
        DebutMs = $start.Time
        AcceptationMs = if ($accepted) { $accepted.Time - $start.Time } else { $null }
        EtatFinal = $state
        EtatFinalMs = if ($state3) { $state3.Time - $start.Time } elseif ($state4) { $state4.Time - $start.Time } else { $null }
        Feature06 = [bool]$feature
        EntreeObservee = [bool]$input
        DeconnexionMs = if ($disconnected) { $disconnected.Time - $start.Time } else { $null }
        PiloteRaccroche = [bool]$driverDisconnect
        Verdict = $outcome
    }
}

$cycles | Format-Table -AutoSize

if ($CsvPath) {
    $parent = Split-Path -Parent $CsvPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $cycles | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding UTF8
    Write-Host "CSV ecrit : $CsvPath"
}

$cycles = @($cycles)
$successes = @($cycles | Where-Object { $_.EtatFinal -eq 3 }).Count
$failures = @($cycles | Where-Object { $_.EtatFinal -eq 4 }).Count
Write-Host "Bilan $Mac : $($cycles.Count) cycle(s), $successes succes, $failures echec(s) etat 4."
