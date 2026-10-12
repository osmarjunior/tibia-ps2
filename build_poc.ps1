param(
    [string]$NetTestHost = "192.168.1.2",
    [int]$NetTestPort = 7777,
    [string]$LoginHost = ""
)

# Build script: Tibia client via PS2BUILD in Docker.

$ErrorActionPreference = "Continue"
if ([string]::IsNullOrWhiteSpace($LoginHost)) { $LoginHost = $NetTestHost }

Write-Host "=== tibia-ps2: build (client) ===" -ForegroundColor Green

docker build --build-arg NET_TEST_HOST=$NetTestHost `
    --build-arg NET_TEST_PORT=$NetTestPort `
    --build-arg LOGIN_HOST=$LoginHost `
    -t tibia-ps2-poc .

if ($LASTEXITCODE -ne 0) { throw "build falhou" }

docker rm -f tibia-poc-temp 2>$null | Out-Null
docker create --name tibia-poc-temp tibia-ps2-poc | Out-Null

docker cp tibia-poc-temp:/src/build/bin/client.elf    ./client.elf
docker rm tibia-poc-temp | Out-Null

Write-Host ""
Get-Item ./client.elf |
    Format-Table Name, @{N="KB";E={[math]::Round($_.Length/1KB)}} -AutoSize

Write-Host "Para rodar no PCSX2 (CDVD -> Run ELF):" -ForegroundColor Cyan
Write-Host "  client.elf : cliente Tibia conectado em ${LoginHost}"
