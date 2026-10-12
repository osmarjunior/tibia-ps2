# Protocolo Tibia 7.4 (era 7.1-7.72) — notas de referência

Fontes consultadas (conforme diretrizes do TASKS2.md):
- Servidor: OTHire (`Ezzz-dev/OTHire`, base OTServ da era; `source/protocolgame.cpp`,
  `source/protocollogin.cpp`, `source/protocol.cpp`, `source/networkmessage.cpp`,
  `source/connection.cpp`, `source/server.cpp`).
- Cliente: otclient (`opentibiabr/otclient`) para formatos de envio e thing/sprite.
- RME (`hampusborgos/rme`) para assinatura/formato do .dat.

Todos os inteiros little-endian exceto o bloco RSA (big-endian).

## 1. Enquadramento (framing)

- TCP. Cada mensagem: `[u16 len][len bytes]`.
- `len == 0`: mensagem vazia enviada pelo game server ao conectar (descartar).
- Após o handshake, mensagem cifrada em XTEA:
  `[u16 total][XTEA(total bytes){ [u16 inner][body][pad até múltiplo de 8] }]`
  - `total` é múltiplo de 8 (contém inner+len+body+pad).
  - Decifrar, ler `inner`, processar `inner` bytes.

## 2. XTEA (Protocol::XTEA_encrypt/decrypt do OTHire)

- 32 rodadas, delta 0x9E3779B9, blocos de 64 bits lidos como u32 LE do buffer.
- Encrypt: sum=0; 32x: `v0 += ((v1<<4 ^ v1>>5)+v1) ^ (sum+k[sum&3]); sum+=delta;
  v1 += ((v0<<4 ^ v0>>5)+v0) ^ (sum+k[(sum>>11)&3]);`
- Decrypt: sum=0xC6EF3720; inverso.
- Preenchimento: bytes 0x33 (servidor) — cliente pode usar 0x00.

## 3. RSA (1024-bit, e=65537)

- Chave pública padrão da era (par que acompanha as chaves privadas default do
  OTServ/OTHire — `otserv.cpp` carrega p, q, d):
  - n = 0x9b646903b45b07ac956568d87353bd7165139dd7940703b03e6dd079399661b4a837aa60561d7ccb9452fa0080594909882ab5bca58a1a1b35f8b1059b72b1212611c6152ad3dbb3cfbee7adc142a75d3d75971509c321c5c24a5bd51fd460f01b4e15beb0de1930528a5d3f15c1e3cbf5c401d6777e10acaab33dbe8d5b7ff5
  - e = 65537 (0x10001)
- Texto claro: 128 bytes big-endian; 1º byte DEVE ser 0x00 (validado pelo servidor).
- `c = m^e mod n`; saída: 128 bytes big-endian (zero-à-esquerda).

## 4. Login server (porta 7172)

Primeiro pacote (sem XTEA):
```
body = [u8 0x01 protocol id][u16 os][u16 version=740][12 bytes (ignorados)][RSA 128]
RSA plaintext = [0x00][u32 key0][u32 key1][u32 key2][u32 key3]
                [u32 account number][string password][pad]
```
- O byte 0x01 é consumido pelo `ServicePort::make_protocol`.
- `os`/version: u16 lidos direto (OTHire ignora `os`).

Resposta (XTEA com a key escolhida pelo cliente):
- `[0x14][string motd]` (motd id + "\n" + texto)
- `[0x64][u8 count]` count×( `[string name][string world][u32 ip][u16 port]` ) `[u16 premium days]`
- Erro: `[0x0A][string message]`, conexão fechada.

## 5. Game server (porta 7172+1)

Ao conectar o servidor NÃO envia nada além de uma mensagem vazia `[00 00]`
(`ProtocolGame::onConnect`). Primeiro pacote do cliente (sem XTEA):
```
body = [u8 0x0A protocol id][u16 os][u16 version=740][RSA 128]
RSA plaintext = [0x00][u32 key0..3][u8 isGM(0)][u32 account]
                [string character name][string password][pad]
```
- O byte 0x0A é consumido por `Connection::parsePacket` (skip "protocol ID").

Login OK (XTEA, tudo em uma ou mais mensagens):
- `[0x0A][u32 player id][u16 0x32][u8 canReportBugs]` (+ `[0x0B][32×0xFF]` se RVs)
- `[0x64]` MapDescription (abaixo)
- `[0x83]` magic effect (teleport), `[0x78/0x79]` inventário ×10 slots,
  `[0xA0]` stats, `[0xA1]` skills, `[0x82]` luz do mundo, `[0x8D]` luz do player,
  `[0xD2]` VIPs, `[0xA2]` ícones.
- Erro de login: `[0x14][string]`.

## 6. MapDescription (0x64) — `AddMapDescription`/`GetMapDescription`

```
[u16 x][u16 y][u8 z]   (posição do player)
floors:
  z <= 7 : nz = 7..0    (descendo), offset = z - nz
  z > 7  : nz = z-2 .. min(15, z+2) (subindo), offset = z - nz
  cada floor: 18×14 tiles em (x-8+offset, y-6+offset)
  tile vazio -> contador skip; emite [u8 skip][0xFF]; skip==0xFF -> [0xFF][0xFF] e zera
  tile: things até [0x00] (consumir o 0x00)
```
- Linhas/colunas parciais ao andar: `0x65` (norte, 18×1), `0x66` (leste, 1×14),
  `0x67` (sul, 18×1), `0x68` (oeste, 1×14) — mesma codificação de floor/tile,
  sem skips além do próprio fim? (usa o mesmo par [skip][0xFF] ao final).
- Troca de andar: `0xBE` (subiu) / `0xBF` (desceu) vêm ANTES dos 0x65/0x66/0x67/0x68,
  com floors extras do novo nível (GetFloorDescription com offsets −3..+2).

### Item (AddItem)
`[u16 clientId]` + `[u8 count]` se stackable + `[u8 fluid]` se splash/fluid container.

### Criatura (AddCreature)
- conhecida: `[u16 0x62][u32 id]`
- desconhecida: `[u16 0x61][u32 removeId][u32 id][string name]`
- depois: `[u8 health%][u8 direction][outfit][u8 lightLevel][u8 lightColor][u16 stepSpeed][u8 skull][u8 party]`
- outfit: `[u16 looktype]` + se !=0: `[u8 head][u8 body][u8 legs][u8 feet]`; senão `[u16 itemId]`
  (invisível: 0x62? looktype=0 e item=0)
- ordem no tile: ground, top items, criaturas (reverse), down items — máx 10 things/tile.

## 7. Pacotes de jogo servidor→cliente (opcodes 7.4)

| Op | Conteúdo |
|----|----------|
|0x0A| login OK (player id, 0x32, bugflag) + mapdesc |
|0x14| game: erro de login/mensagens; (0x14 = disconnect no login server) |
|0x1E| keep-alive (responder 0x1E) |
|0x64| full map description |
|0x65-0x68| linha/coluna nova ao andar N/E/S/W |
|0x69| update tile: pos + tile-description + 0x00 + 0xFF |
|0x6A| add thing: pos + item/creature (sem stackpos) |
|0x6B| transform thing: pos + u8 stackpos + (byte<=3 ? direção : item) — quirk da era |
|0x6C| remove thing: pos + u8 stackpos |
|0x6D| move creature: fromPos + u8 stackpos + toPos |
|0x6E| open container: u8 cid + item + string name + u8 cap + u8 hasParent + u8 n + n×item |
|0x6F| close container: u8 cid |
|0x70/0x71/0x72| add/update/remove item em container: cid (+u8 slot) + item |
|0x78/0x79| add/update (0x78) e remove (0x79) item de slot de inventário: u8 slot + item |
|0x82| luz do mundo: level + color |
|0x83| magic effect: pos + u8 type (servidor já soma +1) |
|0x8C| creature health: u32 id + u8 % |
|0x8D| creature light: u32 id + level + color |
|0x8E| creature outfit: u32 id + outfit |
|0x8F| creature speed: u32 id + u16 |
|0x90| skull: u32 id + u8 |
|0x91| party shield: u32 id + u8 |
|0x96| text window |
|0xA0| stats (abaixo) |
|0xA1| skills: 7×(u8 level, u8 %) |
|0xA2| icons: u8 |
|0xAA| creature speak: u32 guid + string name + u8 type + [pos|u16 chan] + string text |
|0xB4| text message: u8 class + string |
|0xB5| cancel walk: u8 direction |
|0xBE/0xBF| floor change up/down (dados: floors extras) |
|0xC8| outfit window |
|0xD2/0xD3/0xD4| VIP add/login/logout |

Stats (0xA0): `[u16 health][u16 maxHealth][u16 freeCap][u32 exp][u16 level][u8 level%]
[u16 mana][u16 maxMana][u8 magicLevel][u8 ml%][u8 soul]`.

Speak types (7.4): SAY=1, WHISPER=2, YELL=3, private=4(ambos sentidos), channel=5,
rvr=6/7/8, gm broadcast=9, gm channel=10, gm private=11, channel highlight=12,
monster say=16, monster yell=17. Text classes (0xB4): 18=warning(admin),
19=game, 20=login, 21=status, 22=look, 23=failure, 24=blue, 25=red.

## 8. Pacotes cliente→servidor

| Op | Conteúdo |
|----|----------|
|0x14| logout |
|0x1E| keep-alive (resposta ao 0x1E do servidor) |
|0x64| auto-walk: u8 count + count×(4 bits dir + 4 bits flag) |
|0x65-0x68| walk N/E/S/W |
|0x6A-0x6D| walk NE/SE/SW/NW |
|0x6F-0x72| turn N/E/S/W |
|0x78| throw/move thing: fromPos + u16 sprite + u8 stackpos + toPos + u8 count |
|0x82| use item: pos + u16 sprite + u8 stackpos + u8 index |
|0x83| use item with: fromPos + u16 sprite + u8 stackpos + toPos + u16 sprite + u8 stackpos |
|0x84| battle window: fromPos + u16 sprite + u8 stackpos + u32 creatureId |
|0x87| close container: u8 cid |
|0x88| up container: u8 cid |
|0x8C| look at: pos + u16 sprite + u8 stackpos |
|0x96| say: u8 type + [receiver string | u16 channelId] + string text |
|0xA1| attack: u32 creatureId |
|0xA2| follow: u32 creatureId |
|0xBE| cancel attack/follow |
|0xC9| request resend tile: pos |
|0xCA| request resend container: u8 cid |
|0xD2/0xD3| request/change outfit |

Direções (enum Direction): N=0, E=1, S=2, W=3, NE=4, SE=5, SW=6, NW=7.

## 9. Velocidade de passo (Fase 4)

- Creature speed base: 220 (u16 stepSpeed no AddCreature; stats não trazem speed
  na 7.4 — usar AddCreature/0x8F).
- Duração do passo = `groundSpeed * 1000 / (speed * 2)` ms (groundSpeed=150,
  speed=220 → ~341ms/tile, condizente com o cliente original; fórmula do
  otclient: `stepDuration = groundSpeed / (speed/1000) * ...` — ver
  `LocalPlayer::getStepDuration`).
- O servidor é autoritativo: rejeita passos rápidos demais com 0xB5 (cancel walk).

## 10. Formato dos assets (nota importante)

Os assets atuais do workspace são 7.60 e correspondem ao client-ID space do
`items.otb` do OTHire (OTB 1.3.49-7.60). O DAT tem assinatura `0x439D5A33`,
layout de thing com `patternZ` explícito, IDs visuais 100..5089 (gold coin
3031). **Não usar as premissas dos assets 7.4 abaixo sem trocar o DAT/Spr de
volta para 7.4.** Consequências para os assets atuais:
- Header de thing: `[u8 w][u8 h][u8 realsize se w>1|h>1][u8 layers][u8 px][u8 py][u8 pz][u8 phases]` — o byte `pz` EXISTE.
- Flags: byte == valor do enum (Ground=0 c/ u16 speed, Stackable=5, OnBottom=2,
  OnTop=3, Light=0x15 c/ u16+u16, Elevation=0x19 c/ u16, FullGround=0x1E,
  Displacement=0x18 c/ u16+u16, ...), 0xFF termina.
- Itens: IDs visuais 100..5089; outfits/effects/missiles são seções após os itens.
- Índice de sprite: `((((a%pz_total... ver render_game.c))`.
- Stackable 4×2 patterns: count<5 → px=count-1,py=0; <10 → 0,1; <25 → 1,1;
  <50 → 2,1; senão 3,1.
