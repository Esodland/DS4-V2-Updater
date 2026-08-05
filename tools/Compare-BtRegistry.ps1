<#
    Lit les entrées Bluetooth de system.dreg utiles à l'essai btpurge.

    Exemple :
      .\tools\Compare-BtRegistry.ps1 `
        -Before .\logs\system.dreg.btpurge.20260804-133217.bin `
        -After  .\logs\system.dreg.btpurge.20260804-140000.bin
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Before,

    [ValidateScript({ -not $_ -or (Test-Path -LiteralPath $_ -PathType Leaf) })]
    [string]$After,

    [ValidatePattern('^[0-9A-Fa-f]{2}(:?[0-9A-Fa-f]{2}){5}$')]
    [string]$TargetMac = 'F8:6B:14:C3:62:24'
)

$ErrorActionPreference = 'Stop'

function ConvertTo-MacBytes([string]$Mac) {
    $hex = $Mac -replace ':', ''
    $display = [byte[]]::new(6)
    for ($i = 0; $i -lt 6; $i++) {
        $display[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    [array]::Reverse($display)
    return $display
}

function Find-Pattern([byte[]]$Data, [byte[]]$Pattern) {
    $positions = [System.Collections.Generic.List[int]]::new()
    for ($i = 0; $i -le $Data.Length - $Pattern.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Pattern.Length; $j++) {
            if ($Data[$i + $j] -ne $Pattern[$j]) {
                $match = $false
                break
            }
        }
        if ($match) { $positions.Add($i) }
    }
    return $positions
}

function Read-AsciiZ([byte[]]$Data, [int]$Offset, [int]$MaxLength) {
    $end = $Offset
    $limit = [Math]::Min($Data.Length, $Offset + $MaxLength)
    while ($end -lt $limit -and $Data[$end] -ne 0) { $end++ }
    return [System.Text.Encoding]::ASCII.GetString($Data, $Offset, $end - $Offset)
}

function Get-Entry([byte[]]$Data, [int]$MacOffset) {
    # Les enregistrements BT observés ont leur MAC à +0x60. La MAC y est stockée
    # à l'envers : mac0 little-endian, suivi de mac1 little-endian.
    $base = $MacOffset - 0x60
    if ($base -lt 0 -or ($base + 0x100) -gt $Data.Length) {
        return [pscustomobject]@{ Base = $base; Valid = $false }
    }

    $storedMac = $Data[$MacOffset..($MacOffset + 5)]
    $displayMac = [byte[]]$storedMac.Clone()
    [array]::Reverse($displayMac)
    $linkKey = $Data[($base + 0x70)..($base + 0x7F)]

    return [pscustomobject]@{
        Base       = ('0x{0:X4}' -f $base)
        Valid      = $true
        Mac        = (($displayMac | ForEach-Object { $_.ToString('X2') }) -join ':')
        BtClass    = (($Data[($base + 0x68)..($base + 0x6B)] | ForEach-Object { $_.ToString('X2') }) -join ' ')
        VidPid     = (($Data[($base + 0x88)..($base + 0x8B)] | ForEach-Object { $_.ToString('X2') }) -join ' ')
        LinkKey    = (($linkKey | ForEach-Object { $_.ToString('X2') }) -join ' ')
        LinkKeyZero = -not ($linkKey | Where-Object { $_ -ne 0 })
        Name       = Read-AsciiZ $Data ($base + 0xE0) 64
    }
}

function Show-Entries([string]$Label, [string]$Path, [byte[]]$Pattern) {
    $data = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
    $matches = @(Find-Pattern $data $Pattern)
    Write-Host "$Label : $Path"
    if ($matches.Count -eq 0) {
        Write-Host '  Entrée cible absente.'
        return 0
    }
    foreach ($match in $matches) {
        Get-Entry $data $match | Format-List | Out-Host
    }
    return $matches.Count
}

$storagePattern = ConvertTo-MacBytes $TargetMac
$beforeCount = Show-Entries 'Avant' $Before $storagePattern
$afterCount = if ($After) { Show-Entries 'Après' $After $storagePattern } else { $null }

if ($After) {
    if ($beforeCount -gt 0 -and $afterCount -eq 0) {
        Write-Host 'Verdict : l’entrée cible a disparu après l’essai.'
    } elseif ($beforeCount -eq $afterCount) {
        Write-Host 'Verdict : le nombre d’entrées cibles est inchangé.'
    } else {
        Write-Host "Verdict : $beforeCount entrée(s) avant, $afterCount après."
    }
}
