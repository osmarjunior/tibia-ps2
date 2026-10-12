# Script: sobe othire + MariaDB via docker compose
# Uso: .\run-othire.ps1 [-LoginHost <seu-ip>]
# Depois: rebuild do client.elf com -NetTestHost apontando pro servidor

param(
    [string]$LoginHost = "192.168.1.23"
)

$ErrorActionPreference = "Continue"

Write-Host "=== OTHire Server (Tibia 7.4) via Docker Compose ===" -ForegroundColor Green
Write-Host "Login server : porta 7171"
Write-Host "Game server  : porta 7172"
Write-Host "Database     : MariaDB (othire/othire)"
Write-Host ""

Set-Location $PSScriptRoot

# Sobe os containers
Write-Host "Subindo containers..." -ForegroundColor Cyan
docker compose up -d --build 2>&1 | Select-String -Pattern "Started|Created|Error" | ForEach-Object { $_.Line }

# Aguarda othire ficar pronto
Write-Host ""
Write-Host "Aguardando OTHire..." -ForegroundColor Cyan
$tries = 0
while ($tries -lt 30) {
    $logs = docker logs othire-server 2>&1 | Select-Object -Last 5
    $logs | ForEach-Object { Write-Host "  $_" }
    if ($logs -match "otserv.*running|Listening") {
        Write-Host ""
        Write-Host "Servidor pronto!" -ForegroundColor Green
        break
    }
    if ($logs -match "Error|FATAL|error") {
        Write-Host "ERRO no servidor. Verifique: docker logs othire-server" -ForegroundColor Red
        break
    }
    $tries++
    Start-Sleep 2
}

Write-Host ""
Write-Host "Para testar o PS2 client contra este servidor:" -ForegroundColor Yellow
Write-Host "  1. Rebuild:  .\build_poc.ps1 -NetTestHost $LoginHost"
Write-Host "  2. PCSX2:    Run ELF -> client.elf"
Write-Host "  3. Login:   conta 111111, senha tibia, char 'Ps2 Tester'"
Write-Host ""
Write-Host "Logs em tempo real: docker logs -f othire-server"
Write-Host "Parar: docker compose down"
