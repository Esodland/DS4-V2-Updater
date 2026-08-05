<#
    Déploie de façon contrôlée les sondes kernel de ce dépôt sur une PS TV.

    Exemples :
      .\tools\Deploy-KernelProbe.ps1 -Tool ds3trace -Build
      .\tools\Deploy-KernelProbe.ps1 -Tool ds3trace -Enable -Arm
      .\tools\Deploy-KernelProbe.ps1 -Tool ds3trace -Log
      .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Snapshot
      .\tools\Deploy-KernelProbe.ps1 -Tool btpurge -Enable -Arm

    Aucune écriture distante n'est effectuée sans -Upload, -Enable, -Disable,
    -Arm, -Disarm ou -Rescue. -WhatIf permet de vérifier ces opérations.
#>

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('ds3trace', 'btpurge', 'sdiotrace', 'wlanbttrace', 'btfailtrace', 'btpathtrace', 'btcoretrace', 'btlookuptrace', 'btparsertrace', 'ds4v2reco')]
    [string]$Tool,

    [string]$Ip = '172.20.10.2',
    [int]$Port = 1337,
    [int]$CommandPort = 1338,
    [switch]$Build,
    [switch]$NoBuild,
    [switch]$Upload,
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Arm,
    [switch]$Disarm,
    [switch]$Log,
    [switch]$Snapshot,
    [switch]$Rescue,
    [switch]$Reboot,
    [switch]$AllowConcurrent
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ToolRoot = Join-Path $ProjectRoot $Tool
$BuildRoot = Join-Path $ToolRoot 'build'
$Binary = Join-Path $BuildRoot "$Tool.skprx"
$Config = Join-Path $ToolRoot "$Tool.cfg"
$LogsRoot = Join-Path $ProjectRoot 'logs'
$RemoteBase = "ftp://${Ip}:${Port}"
$RemoteBinary = "ur0:/tai/$Tool.skprx"
$RemoteConfig = "ur0:/tai/$Tool.cfg"
$RemoteArm = "ur0:/tai/$Tool.on"
$ConfigLine = "ur0:tai/$Tool.skprx"

function Assert-LastExitCode([string]$Action) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Action a échoué (code $LASTEXITCODE)."
    }
}

function Ensure-LogsRoot {
    if (-not (Test-Path -LiteralPath $LogsRoot)) {
        New-Item -ItemType Directory -Path $LogsRoot | Out-Null
    }
}

function New-Timestamp {
    Get-Date -Format 'yyyyMMdd-HHmmss'
}

function Get-RemoteFile([string]$RemotePath, [string]$LocalPath) {
    & curl.exe -f -s -S -o $LocalPath "$RemoteBase/$RemotePath"
    Assert-LastExitCode "Lecture FTP de $RemotePath"
}

function Send-RemoteFile([string]$LocalPath, [string]$RemotePath) {
    if ($PSCmdlet.ShouldProcess($RemotePath, "envoyer $LocalPath")) {
        & curl.exe -f -s -S -T $LocalPath "$RemoteBase/$RemotePath"
        Assert-LastExitCode "Envoi FTP vers $RemotePath"
        Write-Host "  -> $RemotePath"
    }
}

function Remove-RemoteFile([string]$RemotePath) {
    if ($PSCmdlet.ShouldProcess($RemotePath, 'supprimer')) {
        & curl.exe -f -s -S -Q "-DELE $RemotePath" "$RemoteBase/"
        Assert-LastExitCode "Suppression FTP de $RemotePath"
        Write-Host "  -> supprimé : $RemotePath"
    }
}

function Build-Target {
    if (-not (Test-Path -LiteralPath $ToolRoot)) {
        throw "Outil introuvable : $ToolRoot"
    }

    $vitaSdk = if ($env:VITASDK) { $env:VITASDK } else { 'C:/vitasdk/vitasdk' }
    if (-not (Test-Path -LiteralPath $vitaSdk)) {
        throw "VitaSDK introuvable : $vitaSdk"
    }
    $env:VITASDK = $vitaSdk
    $env:PATH = (Join-Path $vitaSdk 'bin') + ';' + $env:PATH

    if (-not (Test-Path -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt'))) {
        & cmake -S $ToolRoot -B $BuildRoot -G Ninja
        Assert-LastExitCode "Configuration CMake de $Tool"
    }

    & cmake --build $BuildRoot
    Assert-LastExitCode "Compilation de $Tool"

    if (-not (Test-Path -LiteralPath $Binary)) {
        throw "Binaire absent après compilation : $Binary"
    }
}

function Save-Snapshot {
    if ($WhatIfPreference) {
        Write-Host '[WhatIf] Lecture de ur0:/tai/config.txt et vd0:/registry/system.dreg.'
        return
    }
    Ensure-LogsRoot
    $stamp = New-Timestamp
    $configSnapshot = Join-Path $LogsRoot "config.txt.$Tool.$stamp"
    $registrySnapshot = Join-Path $LogsRoot "system.dreg.$Tool.$stamp.bin"

    Get-RemoteFile 'ur0:/tai/config.txt' $configSnapshot
    Get-RemoteFile 'vd0:/registry/system.dreg' $registrySnapshot
    Write-Host "Instantané enregistré :"
    Write-Host "  $configSnapshot"
    Write-Host "  $registrySnapshot"
}

function Update-RemoteConfig([bool]$Present) {
    if ($WhatIfPreference) {
        $action = if ($Present) { 'ajouter' } else { 'retirer' }
        Write-Host "[WhatIf] $action $ConfigLine dans ur0:/tai/config.txt."
        return
    }
    Ensure-LogsRoot
    $tempConfig = Join-Path $env:TEMP "ps-tv-$Tool-config.txt"
    Get-RemoteFile 'ur0:/tai/config.txt' $tempConfig

    $backup = Join-Path $LogsRoot ("config.txt.bak-" + (New-Timestamp))
    Copy-Item -LiteralPath $tempConfig -Destination $backup
    Write-Host "Sauvegarde locale : $backup"

    $lines = @(Get-Content -LiteralPath $tempConfig)
    $alreadyPresent = $lines | Where-Object { $_.Trim() -eq $ConfigLine }

    if ($Present) {
        if ($alreadyPresent) {
            Write-Host 'La ligne kernel est déjà présente.'
            return
        }

        if (-not $AllowConcurrent) {
            $knownProbeLines = @(
                'ur0:tai/ds4v2fix.skprx',
                'ur0:tai/ds4v2bt.skprx',
                'ur0:tai/ds4v2reco.skprx',
                'ur0:tai/ds3trace.skprx',
                'ur0:tai/btpurge.skprx',
                'ur0:tai/sdiotrace.skprx',
                'ur0:tai/wlanbttrace.skprx',
                'ur0:tai/btfailtrace.skprx',
                'ur0:tai/btpathtrace.skprx',
                'ur0:tai/btcoretrace.skprx',
                'ur0:tai/btlookuptrace.skprx',
                'ur0:tai/btparsertrace.skprx'
            ) | Where-Object { $_ -ne $ConfigLine }
            $activeProbes = $lines | Where-Object { $knownProbeLines -contains $_.Trim() }
            if ($activeProbes) {
                throw "Sonde(s) déjà active(s) : $($activeProbes -join ', '). Désactivez-les avant d’activer $Tool, ou passez explicitement -AllowConcurrent."
            }
        }

        $result = @()
        $inKernel = $false
        $inserted = $false
        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if ($inKernel -and -not $inserted -and $trimmed.StartsWith('*')) {
                $result += $ConfigLine
                $inserted = $true
                $inKernel = $false
            }
            $result += $line
            if ($trimmed -eq '*KERNEL') {
                $inKernel = $true
            }
        }
        if ($inKernel -and -not $inserted) {
            $result += $ConfigLine
            $inserted = $true
        }
        if (-not $inserted) {
            throw 'Section *KERNEL introuvable dans la configuration distante.'
        }
        $lines = $result
        Write-Host "Ligne ajoutée : $ConfigLine"
    } else {
        if (-not $alreadyPresent) {
            Write-Host 'La ligne kernel était absente.'
            return
        }
        $lines = @($lines | Where-Object { $_.Trim() -ne $ConfigLine })
        Write-Host "Ligne retirée : $ConfigLine"
    }

    Set-Content -LiteralPath $tempConfig -Value $lines -Encoding ASCII
    Send-RemoteFile $tempConfig 'ur0:/tai/config.txt'
}

function Set-Arm([bool]$Present) {
    if ($Present) {
        if ($WhatIfPreference) {
            Write-Host "[WhatIf] Création et envoi de $RemoteArm."
            return
        }
        $armFile = Join-Path $env:TEMP "$Tool.on"
        try {
            Set-Content -LiteralPath $armFile -Value 'armed' -Encoding ASCII
            Send-RemoteFile $armFile $RemoteArm
            Write-Host 'Un seul essai est armé. Redémarrer la console pour le consommer.'
        } finally {
            Remove-Item -LiteralPath $armFile -Force -ErrorAction SilentlyContinue
        }
    } else {
        Remove-RemoteFile $RemoteArm
    }
}

function Get-Log {
    if ($WhatIfPreference) {
        Write-Host "[WhatIf] Récupération de ur0:/log/$Tool.txt."
        return
    }
    Ensure-LogsRoot
    $destination = Join-Path $LogsRoot ("$Tool-" + (New-Timestamp) + '.txt')
    Get-RemoteFile "ur0:/log/$Tool.txt" $destination
    Write-Host "Journal récupéré : $destination"
    Get-Content -LiteralPath $destination -Tail 60
}

function Rescue-Config {
    Ensure-LogsRoot
    $backup = Get-ChildItem -Path $LogsRoot -Filter 'config.txt.bak-*' |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $backup) {
        throw 'Aucune sauvegarde config.txt.bak-* disponible.'
    }

    Write-Host "Réinjection de secours de $($backup.Name)..."
    for ($attempt = 1; $attempt -le 120; $attempt++) {
        if (-not $PSCmdlet.ShouldProcess('ur0:/tai/config.txt', "réinjecter la sauvegarde, tentative $attempt")) {
            return
        }
        & curl.exe -s -S -m 3 -T $backup.FullName "$RemoteBase/ur0:/tai/config.txt" 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Secours réussi à la tentative $attempt."
            return
        }
        Start-Sleep -Milliseconds 1200
    }
    throw 'La fenêtre FTP n’a pas été atteinte après 120 tentatives.'
}

function Restart-PsTv {
    if ($WhatIfPreference) {
        Write-Host "[WhatIf] Envoi de la commande reboot à $Ip`:$CommandPort."
        return
    }
    if (-not $PSCmdlet.ShouldProcess("$Ip`:$CommandPort", 'redémarrer la PS TV via VitaCompanion')) {
        return
    }

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect($Ip, $CommandPort)
        $stream = $client.GetStream()
        $command = [System.Text.Encoding]::ASCII.GetBytes("reboot`n")
        $stream.Write($command, 0, $command.Length)
        $stream.Flush()
        Write-Host 'Commande de redémarrage envoyée à VitaCompanion.'
    } finally {
        $client.Dispose()
    }
}

$hasAction = $Build -or $Upload -or $Enable -or $Disable -or $Arm -or $Disarm -or $Log -or $Snapshot -or $Rescue -or $Reboot
if (-not $hasAction) {
    $Build = $true
}
if ($NoBuild -and ($Build -or $Upload -or $Enable)) {
    if ($Build) { throw '-Build et -NoBuild sont incompatibles.' }
} elseif (-not $NoBuild -and ($Build -or $Upload -or $Enable)) {
    Build-Target
}

if ($Upload -or $Enable) {
    Send-RemoteFile $Binary $RemoteBinary
    Send-RemoteFile $Config $RemoteConfig
}
if ($Snapshot) { Save-Snapshot }
if ($Enable) { Update-RemoteConfig $true }
if ($Disable) { Update-RemoteConfig $false }
if ($Arm) { Set-Arm $true }
if ($Disarm) { Set-Arm $false }
if ($Log) { Get-Log }
if ($Rescue) { Rescue-Config }
if ($Reboot) { Restart-PsTv }

Write-Host 'Terminé. Les modules kernel ne sont rechargés qu’après redémarrage de la console.'
