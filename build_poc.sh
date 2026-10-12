#!/bin/bash
# Build script for the PS2 ELF targets using PS2BUILD in Docker
# Usage: ./build_poc.sh [net-test-host] [net-test-port] [login-host]

set -e

echo "=== Building PS2 ELF targets with PS2BUILD in Docker ==="

NET_TEST_HOST="${1:-192.168.1.2}"
NET_TEST_PORT="${2:-7777}"
LOGIN_HOST="${3:-$NET_TEST_HOST}"

# Build the Docker image
docker build \
	--build-arg NET_TEST_HOST="$NET_TEST_HOST" \
	--build-arg NET_TEST_PORT="$NET_TEST_PORT" \
	--build-arg LOGIN_HOST="$LOGIN_HOST" \
	-t tibia-ps2-poc .

# Create a temporary container to extract the ELF
docker rm -f tibia-poc-temp 2>/dev/null || true
docker create --name tibia-poc-temp tibia-ps2-poc

# Copy built ELF files out of the PS2BUILD output directory.
docker cp tibia-poc-temp:/src/build/bin/poc.elf ./poc.elf
docker cp tibia-poc-temp:/src/build/bin/net_test.elf ./net_test.elf
docker cp tibia-poc-temp:/src/build/bin/client.elf ./client.elf
docker cp tibia-poc-temp:/src/build/bin/rs2-client.elf ./rs2-client.elf

# Clean up
docker rm tibia-poc-temp

echo ""
echo "=== Build complete ==="
ls -lh poc.elf net_test.elf client.elf rs2-client.elf
echo ""
echo "Generated: poc.elf, net_test.elf (${NET_TEST_HOST}:${NET_TEST_PORT}), client.elf (${LOGIN_HOST}) and rs2-client.elf"
