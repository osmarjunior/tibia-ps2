# Tibia 7.6 Client Port for PlayStation 2

A port of the **Tibia 7.6 client** to PlayStation 2, playable over the network against an **OTHire** server. It works on the **PCSX2** emulator and on **real hardware** (PS2 with a Network Adaptor).

> This project is partially based on the code from [2004sp-ps2port](https://github.com/johnson-cooper/2004sp-ps2port) (a C99 port of the 2004 RuneScape 2 client to PS2 and other consoles). It inherits the PS2 platform bootstrap from that project — gsKit, built-in IRX network modules (dev9/netman/smap), the PS2BUILD build system (`ps2.yaml`), and part of the UI/rendering infrastructure.

<img width="1133" height="850" alt="client  _ _client_20261012150140" src="https://github.com/user-attachments/assets/41dc20a6-aea7-49a0-ac61-36a9ac128a8a" />

## Features

- OTServ-era network protocol (7.4–7.72): **RSA-1024 + XTEA** handshake, login server (MOTD + character list), and game server.
- **7.60 assets** (`Tibia.dat`/`Tibia.spr`) embedded in the ELF, using the same client-ID space as OTHire's `items.otb` (OTB 1.3.49-7.60).
- Server-provided map (MapDescription), with multiple floors, culling, and floor-to-floor fading.
- Predictive walking with reconciliation (authoritative server; handles the 0xB5 cancel-walk response).
- LRU sprite cache in VRAM (384 textures at 32×32) with a real-time statistics overlay (R1 button).
- Cursor-based interactions: **look** (□), **use item** (X), **attack** (△), chat (○), containers, and equipment window (L2).
- On-controller virtual keyboard for login and chat; HUD with HP/MP, chat, battle list, and container.
- Networking via ps2ip/lwIP with **DHCP** (TCP); EE target resolution of 640×448 via gsKit.

## Quick Start

Get Tibia running on your PlayStation 2 or PCSX2 in a few steps.

**Prerequisites**
- [Docker](https://www.docker.com/) installed and running.
- The Tibia 7.60 client assets (`Tibia.dat` and `Tibia.spr`) — **not included in this repository**; you must copy them from your own Tibia 7.60 client installation into the repository root before building (see [Client Assets](#client-assets-required-before-build)).
- PCSX2 with network support enabled, or a PS2 with a Network Adaptor.
- Both the client and server must be able to communicate over the network.

**1. Start the OTHire server**

Configure `docker/othire/.env`, setting `SERVER_IP` to your computer's local network IP address and enabling the test account:

```dotenv
SERVER_IP=192.168.0.10
SEED_TEST_DATA=1
```

Replace `192.168.0.10` with your actual server IP. Set the required MariaDB passwords and other environment variables in the same file.

Start the server:

```powershell
cd docker\othire
.\run-othire.ps1
```

**2. Build the client**

From the repository root, run:

```powershell
.\build_poc.ps1 -NetTestHost 192.168.0.10
```

Replace the example IP with the server's actual IP. The resulting `client.elf` is generated in the parent directory.

**3. Launch the client**

- **PCSX2:** Enable the emulated network adapter, ensure DHCP and network connectivity are configured, then select **CDVD → Run ELF** and load `client.elf`.
- **Real PS2:** Load `client.elf` from USB using uLaunchELF/OPL or through ps2link. Ensure the console is connected to the same network as the server.

**4. Log in**

Use the seeded test credentials:

| Field | Value |
|---|---|
| Account | `111111` |
| Password | `tibia` |
| Character | `Ps2 Tester` |

The default server ports are **7171** (login) and **7172** (game).

**Troubleshooting:** If the client cannot connect, verify the server IP, firewall rules, port accessibility, DHCP configuration, and PCSX2 network settings. The server IP is embedded in the ELF at build time, so changing servers requires rebuilding the client.

## Repository Structure

```text
├── ps2.yaml               # Build target `client` (PS2BUILD)
├── Dockerfile             # Build image (Ubuntu 24.04 + PS2BUILD SDK)
├── build_poc.ps1          # Build client.elf (via Docker)
├── Tibia.dat / Tibia.spr  # Tibia 7.60 client assets (proprietary — NOT committed; provide before build)
├── src/
│   ├── entry/client_net.c # Connectable client entry point (main)
│   ├── net/               # ps2ip/lwIP, XTEA, RSA, login and game protocol
│   ├── poc/               # DAT/SPR loaders, map and game rendering
│   ├── ui/                # Font, controller, virtual keyboard, HUD
│   └── thirdparty/bn.c    # Big-number arithmetic for RSA (libtom)
├── docker/othire/         # OTHire server + MariaDB (Docker Compose)
├── tools/                 # Mock server, echo server, asset parsers
├── tests/                 # Host-side tests (protocol; run on PC)
└── docs/                  # Protocol notes and phase-by-phase reports
```

## Setting Up the Server (OTHire)

The `docker/othire` directory starts a complete **OTHire** server (Tibia 7.72) — game server + MariaDB — using Docker Compose. No installation is required beyond Docker.

### 1. Configure

Edit `docker/othire/.env`:

| Variable | Description |
|---|---|
| `SERVER_IP` | **Your machine's IP address on the local network** (the same address the client will use) |
| `DB_ROOT_PASSWORD` / `DB_PASSWORD` | MariaDB passwords |
| `PASSWORD_TYPE` | `plain` (recommended for testing), `md5`, or `sha1` |
| `SEED_TEST_DATA` | `1` = creates account **111111** (password `tibia`) and character **Ps2 Tester** |
| `OTHIRE_REF` | OTHire commit/tag (pin a hash for reproducible builds) |

### 2. Start the Containers

```powershell
cd docker\othire
.\run-othire.ps1          # or: docker compose up -d --build
```

- Login server: port **7171** · Game server: port **7172**
- On the first `up`, MariaDB automatically creates the database and loads the schema.
  To reapply the schema from scratch: `docker compose down -v` (deletes the data).
- Follow logs in real time: `docker logs -f othire-server`

The OTHire image is compiled from [Ezzz-dev/OTHire](https://github.com/Ezzz-dev/OTHire), with compatibility patches applied during the build (support for protocol 7.4+, Boost 1.74, and MySQL 8).

### Test Account

With `SEED_TEST_DATA=1`, use the following credentials in the client:

- Account: `111111`
- Password: `tibia`
- Character: `Ps2 Tester`

### Quick Alternative: Mock Server (No Docker Required)

To debug the protocol without a real server, using the same `111111` / `tibia` test account:

```sh
python tools/mock_tibia_server.py --login-port 7171 --game-port 7172
```

## Building the Client

The build runs entirely in Docker — there is no need to install a PS2 toolchain.

### Client Assets (Required Before Build)

`Tibia.dat` and `Tibia.spr` are proprietary CipSoft files and are deliberately
**not distributed with this repository** (they are listed in `.gitignore` and
must never be committed). The build embeds both files into `client.elf`, so they
must be present before building:

1. Get the files from your own Tibia 7.60 client installation.
2. Copy them to the **repository root**, next to the `Dockerfile`:

   ```text
   tibia-ps2-limpo/
   ├── Tibia.dat
   ├── Tibia.spr
   └── ...
   ```

The host-side test suite and the Python tools in `tools/` also expect these
files in the repository root. (`tools/Tibia.pic`, used only by asset
experiments, is equally proprietary and also not committed.)

```powershell
.\build_poc.ps1 -NetTestHost <server-IP>
```

- `-NetTestHost` sets the **server IP address embedded in the ELF**. There is no IP address field on the client's login screen (only account, password, and character fields, entered using the virtual keyboard). Switching servers therefore requires rebuilding the client. `-LoginHost` defaults to the value of `-NetTestHost`.
- The ports are fixed and match OTHire's defaults: login **7171**, game **7172**.
- `-NetTestPort` changes the echo test port (default: 7777; optional).
- Output: `client.elf` (~16 MB), extracted to `..\client.elf`.

What the script does:

1. Runs `docker build` to create the `tibia-ps2-poc` image (Ubuntu 24.04 + [PS2BUILD](https://git.techwritescode.dev/ps2/install) SDK).
2. Copies `Tibia.dat`/`Tibia.spr` (provided by you, see [Client Assets](#client-assets-required-before-build)) into the build environment and rewrites `LOGIN_HOST` in `ps2.yaml`.
3. Runs `ps2build build` to compile the `client` target (EE, `-O1`) → `build/bin/client.elf`.

On Linux/macOS, without PowerShell, use the equivalent commands:

```sh
docker build --build-arg NET_TEST_HOST=192.168.0.10 \
             --build-arg NET_TEST_PORT=7777 \
             --build-arg LOGIN_HOST=192.168.0.10 \
             -t tibia-ps2-poc .
docker create --name tibia-poc-temp tibia-ps2-poc
docker cp tibia-poc-temp:/src/build/bin/client.elf ./client.elf
docker rm tibia-poc-temp
```

## Running the Client

### PCSX2

1. Enable the emulated network adapter (dev9/Ethernet) in the settings. The client uses DHCP, so the host interface (TAP/bridge) must provide an IP address (e.g., through Windows Internet Connection Sharing).
2. Select **CDVD → Run ELF** and open `client.elf`.
3. Log in using the test account (`111111` / `tibia` / `Ps2 Tester`).

### Real PS2 Hardware

- Requirements: a PS2 with a **Network Adaptor**, connected to the same LAN as the server, with DHCP available.
- Load the ELF from USB (using uLaunchELF/OPL) or through ps2link.
- Detailed hardware validation instructions: `docs/fase9-hardware.md`.

## Controls

| Button | Action |
|---|---|
| D-pad | Walk |
| L1 + D-pad | Move target cursor |
| X | Use item |
| □ | Look at the object under the cursor |
| △ | Attack |
| ○ | Enter chat (say) |
| START | Log out |
| SELECT | Switch account (return to login) |
| L2 | Open/close equipment window |
| R1 | Toggle sprite cache statistics overlay |
| R2 | Dump the grid to the log (diagnostics) |

## Tests

The host-side test suite validates protocol logic (login, game protocol, map, and walking) against the mock server, without requiring a PS2:

```powershell
docker run --rm --network host -v ${PWD}:/repo gcc:13 sh /repo/tests/run_host_tests.sh
```

The suite loads `Tibia.dat` from the repository root, so the client assets must
be present (see [Client Assets](#client-assets-required-before-build)).

## Additional Documentation

- `docs/protocol-74-notes.md` — Complete protocol reference (framing, XTEA, RSA, 7.4 opcodes, and 7.60 asset format).
- `docs/fase9-hardware.md` — Real hardware validation.
- `docs/vram-relatorio.md` — VRAM budget and sprite cache report.
- `docs/poc-recon.md` — Codebase investigation (RS2/PS2).

## Credits

- [johnson-cooper/2004sp-ps2port](https://github.com/johnson-cooper/2004sp-ps2port) — Porting foundation (RuneScape 2 revision #225 client in C99 for PS2 and other consoles).
- [Ezzz-dev/OTHire](https://github.com/Ezzz-dev/OTHire) — Tibia 7.72-era server and protocol reference.
- [otclient](https://github.com/opentibiabr/otclient) and [RME](https://github.com/hampusborgos/rme) — References for thing/sprite formats and the `.dat` file.

## Disclaimer

This is a research/educational project and is not affiliated with CipSoft. Tibia and its assets are the property of CipSoft. For this reason, the proprietary asset files (`Tibia.dat`, `Tibia.spr`, and `Tibia.pic`) are **not distributed in this repository** — each user must supply their own, obtained from their own client installation, and use them only in their own testing environment.
