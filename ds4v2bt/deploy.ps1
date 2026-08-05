<#
    deploy.ps1 - construit et envoie ds4v2bt sur la PS TV.

    Utilisation :
        .\deploy.ps1                 # build + envoi du .skprx et du .cfg
        .\deploy.ps1 -Enable         # idem + ajoute la ligne dans config.txt
        .\deploy.ps1 -Disable        # retire la ligne de config.txt
        .\deploy.ps1 -Arm            # arme UN essai (depose ds4v2bt.on)
        .\deploy.ps1 -Disarm         # retire l'armement
        .\deploy.ps1 -Log            # rapatrie ur0:/log/ds4v2bt.txt
        .\deploy.ps1 -Rescue         # boucle FTP : reecrit un config.txt sain

    ARMEMENT. Le plugin ne fait rien tant que ur0:/tai/ds4v2bt.on est absent,
    et il supprime ce fichier avant de toucher a la moindre API. Un essai
    consomme donc son armement : si l'essai plante la console, le demarrage
    suivant la trouve desarmee et elle revient toute seule. Le cycle est donc
    -Arm, redemarrer, -Log ; et il faut re-armer pour chaque nouvel essai.

    Le config.txt est TOUJOURS sauvegarde localement avant modification
    (logs/config.txt.bak-<horodatage>). C'est la lecon du boot loop du
    2026-07-31 : garder une copie saine avant de toucher au fichier permet de
    la reinjecter par polling FTP pendant la fenetre de demarrage.

    Un module kernel n'est jamais decharge : apres un envoi, il faut
    REDEMARRER la console pour que le nouveau binaire soit pris en compte.
    Sans cela on teste l'ancien en croyant tester le nouveau.
#>

param(
    [string]$Ip = "172.20.10.2",
    [int]$Port = 1337,
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Arm,
    [switch]$Disarm,
    [switch]$Log,
    [switch]$Rescue,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$racine   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projet   = Split-Path -Parent $racine
$build    = Join-Path $racine "build"
$skprx    = Join-Path $build "ds4v2bt.skprx"
$cfgLocal = Join-Path $racine "ds4v2bt.cfg"
$logsDir  = Join-Path $projet "logs"
$base     = "ftp://${Ip}:${Port}"
$ligne    = "ur0:tai/ds4v2bt.skprx"

function Envoyer($local, $distant) {
    curl.exe -s -S -T $local "$base/$distant"
    if ($LASTEXITCODE -ne 0) { throw "envoi de $local echoue" }
    Write-Host "  -> $distant"
}

function Rapatrier($distant, $local) {
    curl.exe -s -S -o $local "$base/$distant"
    if ($LASTEXITCODE -ne 0) { throw "recuperation de $distant echouee" }
}

if ($Rescue) {
    # Boucle de secours : reecrit le dernier config.txt sauvegarde jusqu'a ce
    # que la fenetre reseau du demarrage s'ouvre. Le 2026-07-31 elle avait ete
    # attrapee a la 4e tentative (~8 s).
    $dernier = Get-ChildItem $logsDir -Filter "config.txt.bak-*" |
               Sort-Object Name -Descending | Select-Object -First 1
    if (-not $dernier) { throw "aucune sauvegarde config.txt.bak-* dans $logsDir" }
    Write-Host "Reinjection de $($dernier.Name) en boucle..."
    for ($i = 1; $i -le 120; $i++) {
        curl.exe -s -S -m 3 -T $dernier.FullName "$base/ur0:/tai/config.txt" 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { Write-Host "SUCCES a la tentative $i"; return }
        Start-Sleep -Milliseconds 1200
    }
    throw "fenetre jamais attrapee apres 120 tentatives"
}

if ($Arm -or $Disarm) {
    if ($Disarm) {
        curl.exe -s -S -m 8 -Q "-DELE ur0:/tai/ds4v2bt.on" "$base/" 2>&1 | Out-Null
        Write-Host "Armement retire."
    } else {
        $vide = Join-Path $env:TEMP "ds4v2bt.on"
        Set-Content $vide "arme" -Encoding ASCII
        Envoyer $vide "ur0:/tai/ds4v2bt.on"
        Write-Host "UN essai arme. Redemarrer la console."
        Write-Host "Il se consomme au demarrage : re-armer pour l'essai suivant."
    }
    return
}

if ($Log) {
    if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory $logsDir | Out-Null }
    $dest = Join-Path $logsDir ("ds4v2bt-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")
    Rapatrier "ur0:/log/ds4v2bt.txt" $dest
    Write-Host "Journal recupere : $dest"
    Get-Content $dest -Tail 40
    return
}

if (-not $NoBuild) {
    $env:VITASDK = "C:/vitasdk/vitasdk"
    $env:PATH = "C:\vitasdk\vitasdk\bin;C:\Users\DI\scoop\shims;$env:PATH"
    if (-not (Test-Path $build)) {
        cmake -B $build -S $racine -G Ninja
        if ($LASTEXITCODE -ne 0) { throw "configuration cmake echouee" }
    }
    cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "compilation echouee" }
}

if (-not (Test-Path $skprx)) { throw "introuvable : $skprx" }

Write-Host "Envoi vers $Ip :"
Envoyer $skprx    "ur0:/tai/ds4v2bt.skprx"
Envoyer $cfgLocal "ur0:/tai/ds4v2bt.cfg"

if ($Enable -or $Disable) {
    if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory $logsDir | Out-Null }
    $tmp = Join-Path $env:TEMP "config.txt"
    Rapatrier "ur0:/tai/config.txt" $tmp

    $sauvegarde = Join-Path $logsDir ("config.txt.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    Copy-Item $tmp $sauvegarde
    Write-Host "config.txt sauvegarde : $sauvegarde"

    $contenu = Get-Content $tmp
    $present = $contenu | Where-Object { $_.Trim() -eq $ligne }

    if ($Disable) {
        if (-not $present) { Write-Host "La ligne n'y etait pas, rien a faire."; return }
        $contenu = $contenu | Where-Object { $_.Trim() -ne $ligne }
        Write-Host "Ligne retiree."
    } else {
        if ($present) { Write-Host "La ligne y est deja, rien a faire."; return }
        # Ajout en fin de section *KERNEL, sans reordonner l'existant.
        $sortie = @()
        $insere = $false
        foreach ($l in $contenu) {
            if ($insere -eq $false -and $l.Trim().StartsWith("*") -and $sortie.Count -gt 0 `
                -and ($sortie | Where-Object { $_.Trim() -eq "*KERNEL" })) {
                $sortie += $ligne
                $insere = $true
            }
            $sortie += $l
        }
        if (-not $insere) { $sortie += $ligne }
        $contenu = $sortie
        Write-Host "Ligne ajoutee."
    }

    $contenu | Set-Content $tmp -Encoding ASCII
    Envoyer $tmp "ur0:/tai/config.txt"
}

Write-Host ""
Write-Host "REDEMARRER la console : un module kernel n'est jamais decharge."
