#!/usr/bin/env python3
"""Mock server de protocolo OTHire para testar o cliente PS2 sem servidor real.

Implementa (fonte: docs/protocol-74-notes.md, validado contra OTHire):
  - login server (default :7172): RSA-1024 decrypt (chaves default da era),
    XTEA, lista de personagens.
  - game server (default :7173): handshake RSA+XTEA, MapDescription
    (floors com codificação de skip), moves autoritativos com delay de
    passo (rejeita passo rápido com 0xB5), look/use/say/attack, container,
    keep-alive, hole (teleport p/ baixo com mapdesc) e escada (0xBE).

Uso: python tools/mock_tibia_server.py [--login-port 7172] [--game-port 7173]
Conta de teste: 111111/tibia, personagem "Ps2 Tester".
"""
import socket
import struct
import sys
import threading
import time

# ---------------- chaves da era (p,q,d) — par da chave pública do cliente)
P = 14299623962416399520070177382898895550795403345466153217470516082934737582776038882967213386204600674145392845853859217990626450972452084065728686565928113
Q = 7630979195970404721891201847792002125535401292779123937207447574596692788513647179235335529307251350570728407373705564708871762033017096809910315212884101
D = 46730330223584118622160180015036832148732986808519344675210555262940258739805766860224610646919605860206328024326703361630109888417839241959507572247284807035235569619173792292786907845791904955103601652822519121908367187885509270025388641700821735345222087940578381210879116823013776808975766851829020659073
N = P * Q

TEST_ACCOUNT = 111111
TEST_PASSWORD = "tibia"
TEST_CHAR = "Ps2 Tester"

STEP_MS = 340          # delay de passo (speed 220, ground 150)
WORLD_NAME = "MockWorld"

# IDs visuais 7.60 enviados pelo OTB do OTHire para os server IDs do mapa.
I_GRASS = 103
I_STONE = 410
I_WALL = 637           # onBottom, 2 camadas (parede/telhado)
I_COIN = 3031          # stackable
I_CHEST = 2856         # container
I_HOLE = 470           # floorchange down
I_STAIRS = 607         # floorchange up

STACKABLE = {I_COIN}
CONTAINERS = {I_CHEST}

CX, CY = 1000, 1000    # spawn


def log(*a):
    print(f"[{time.strftime('%H:%M:%S')}] mock:", *a, flush=True)


# ---------------- XTEA ----------------

def xtea_crypt_block(v0, v1, k):
    s = 0
    delta = 0x9E3779B9
    for _ in range(32):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1 & 0xFFFFFFFF) ^ ((s + k[s & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
        s = (s + delta) & 0xFFFFFFFF
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0 & 0xFFFFFFFF) ^ ((s + k[(s >> 11) & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
    return v0, v1


def xtea_dec_block(v0, v1, k):
    s = 0xC6EF3720
    delta = 0x9E3779B9
    for _ in range(32):
        v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0 & 0xFFFFFFFF) ^ ((s + k[(s >> 11) & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
        s = (s - delta) & 0xFFFFFFFF
        v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1 & 0xFFFFFFFF) ^ ((s + k[s & 3]) & 0xFFFFFFFF))) & 0xFFFFFFFF
    return v0, v1


def xtea_encrypt(buf, key):
    out = bytearray()
    for i in range(0, len(buf) - 7, 8):
        v0, v1 = struct.unpack_from("<II", buf, i)
        v0, v1 = xtea_crypt_block(v0, v1, key)
        out += struct.pack("<II", v0, v1)
    return bytes(out)


def xtea_decrypt(buf, key):
    out = bytearray()
    for i in range(0, len(buf) - 7, 8):
        v0, v1 = struct.unpack_from("<II", buf, i)
        v0, v1 = xtea_dec_block(v0, v1, key)
        out += struct.pack("<II", v0, v1)
    return bytes(out)


# ---------------- NetMsg writer (corpo: [u16 inner][...]) ----------------

class Msg:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):    self.b += struct.pack("<B", v)
    def u16(self, v):   self.b += struct.pack("<H", v)
    def u32(self, v):   self.b += struct.pack("<I", v)
    def s(self, txt):
        raw = txt.encode()
        self.u16(len(raw))
        self.b += raw
    def pos(self, x, y, z):
        self.u16(x); self.u16(y); self.u8(z)
    def item(self, iid, count=1):
        self.u16(iid)
        if iid in STACKABLE:
            self.u8(count)

    def send(self, sock, key):
        body = bytes(self.b)
        inner = struct.pack("<H", len(body)) + body
        pad = (8 - (len(inner) & 7)) & 7
        inner += b"\x33" * pad
        enc = xtea_encrypt(inner, key)
        sock.sendall(struct.pack("<H", len(enc)) + enc)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        d = sock.recv(n - len(buf))
        if not d:
            raise ConnectionError("eof")
        buf += d
    return buf


def recv_msg(sock, key=None):
    (ln,) = struct.unpack("<H", recv_exact(sock, 2))
    if ln == 0:
        return None
    data = recv_exact(sock, ln)
    if key:
        data = xtea_decrypt(data, key)
        (inner,) = struct.unpack("<H", data[:2])
        return data[2:2 + inner]
    return data


class Reader:
    def __init__(self, b):
        self.b = b
        self.i = 0

    def u8(self):
        v = self.b[self.i]; self.i += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.i)[0]; self.i += 2; return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.i)[0]; self.i += 4; return v

    def s(self):
        n = self.u16()
        v = self.b[self.i:self.i + n].decode(); self.i += n; return v

    def pos(self):
        return (self.u16(), self.u16(), self.u8())


def parse_first_packet(sock, is_game):
    body = recv_msg(sock)
    r = Reader(body)
    r.u8()                       # protocol id (0x01 login / 0x0A game)
    r.u16()                      # os
    r.u16()                      # version
    if not is_game:
        r.i += 12                # 12 bytes ignorados
    m = int.from_bytes(body[r.i:r.i + 128], "big")
    r.i += 128
    plain = pow(m, D, N).to_bytes(128, "big")
    if plain[0] != 0:
        raise ValueError("RSA invalido")
    p = Reader(plain)
    p.i = 1
    key = [p.u32(), p.u32(), p.u32(), p.u32()]
    if is_game:
        gm = p.u8()
        acc = p.u32()
        name = p.s()
        password = p.s()
        return key, (acc, name, password)
    else:
        acc = p.u32()
        password = p.s()
        return key, (acc, password)


# ---------------- mundo ----------------

class World:
    def __init__(self):
        self.chest_open = {}
        self.coin_piles = {}      # (x,y,z) -> count
        for (x, y) in [(CX + 2, CY + 1), (CX - 2, CY + 2)]:
            self.coin_piles[(x, y, 7)] = 12
        self.door_closed = {}      # (x,y,z) -> bool (1210 fechada)
        self.door_closed[(CX, CY - 4, 7)] = True

    def ground(self, x, y, z):
        if z == 7:
            return I_GRASS
        if z == 8:
            # caverna sob a região do buraco
            if CX - 10 <= x <= CX + 12 and CY - 10 <= y <= CY + 12:
                return I_STONE
            return None
        if z == 6:
            # telhado da casa (em cima do spawn)
            if CX - 2 <= x <= CX + 2 and CY - 2 <= y <= CY + 2:
                return I_WALL
            return None
        return None

    def items(self, x, y, z):
        out = []
        if (x, y, z) in self.coin_piles:
            out.append((I_COIN, self.coin_piles[(x, y, z)]))
        if z == 7 and (x, y) == (CX + 3, CY):
            out.append((I_HOLE, 1))
        if z == 8 and (x, y) == (CX + 2, CY):
            out.append((I_STAIRS, 1))
        if z == 7 and (x, y, z) in self.door_closed:
            out.append((1629 if self.door_closed[(x, y, z)] else 1633, 1))
        if z == 7 and (x, y) == (CX - 3, CY - 3):
            out.append((I_CHEST, 1))
        return out

    def walkable(self, x, y, z):
        g = self.ground(x, y, z)
        if g is None:
            return False
        if z == 6:
            return False  # telhado
        if z == 7 and (x, y, z) in self.door_closed and self.door_closed[(x, y, z)]:
            return False  # porta fechada
        if z == 7 and not (CX - 3 <= x <= CX + 5 and CY - 3 <= y <= CY + 4):
            # cerca de paredes ao redor da área de teste p/ conter o player
            return False
        return True


WORLD = World()


def tile_desc(m, world, x, y, z, player_id, creatures):
    """Escreve a descrição de um tile (sem o marcador)."""
    g = world.ground(x, y, z)
    if g is not None:
        m.item(g)
    elif not world.items(x, y, z) and (x, y, z) not in creatures:
        return False
    for (iid, cnt) in world.items(x, y, z):
        m.item(iid, cnt)
    if (x, y, z) in creatures:
        add_creature(m, creatures[(x, y, z)])
    return True


def add_creature(m, c):
    if c["known"]:
        m.u16(0x62)
        m.u32(c["id"])
    else:
        m.u16(0x61)
        m.u32(0)          # remove id
        m.u32(c["id"])
        m.s(c["name"])
    m.u8(c["health"])
    m.u8(c["dir"])
    m.u16(c["outfit"])
    if c["outfit"] != 0:
        m.u8(0); m.u8(0); m.u8(0); m.u8(0)   # cores
    else:
        m.u16(0)
    m.u8(0); m.u8(0)     # luz
    m.u16(c["speed"])
    m.u8(0); m.u8(0)     # skull, party


def get_floor_description(m, world, cx, cy, cam_z, z, creatures, player_id,
                          skip=-1):
    """Um floor 18x14 com encoding de skip.

    skip é COMPARTILHADO entre floors (como OTHire GetFloorDescription):
    o marcador [n][0xFF] só é emitido ANTES de um tile (ou no fim, pelo
    chamador). Retorna o skip atual.
    """
    ox, oy = cx - 8, cy - 6
    off = cam_z - z
    for nx in range(18):
        for ny in range(14):
            x, y = ox + off + nx, oy + off + ny
            desc = Msg()
            has = tile_desc(desc, world, x, y, z, player_id, creatures)
            if has:
                if skip >= 0:
                    m.u8(skip); m.u8(0xFF)
                skip = 0
                m.b += desc.b
            else:
                skip += 1
                if skip == 0xFF:
                    m.u8(0xFF); m.u8(0xFF)
                    skip = -1
    return skip


def send_map_description(sock, key, world, pos, player, creatures):
    m = Msg()
    m.u8(0x64)
    m.pos(*pos)
    x, y, z = pos
    if z <= 7:
        floors = list(range(7, -1, -1))
    else:
        floors = list(range(max(0, z - 2), min(16, z + 2) + 1))
    skip = -1
    for fz in floors:
        skip = get_floor_description(m, world, x, y, z, fz, creatures,
                                     player["id"], skip)
    if skip >= 0:
        m.u8(skip); m.u8(0xFF)
    m.send(sock, key)


def send_own_move(sock, key, frm, to, player, creatures, world):
    """0x6D próprio + linhas novas do novo central."""
    m = Msg()
    m.u8(0x6D)
    m.pos(*frm)
    m.u8(player["stackpos"])
    m.pos(*to)
    # linhas expostas
    fx, fy, fz = frm
    tx, ty, tz = to
    if fz == tz:
        if fy > ty:   # norte
            m.u8(0x65)
            row(m, world, tx - 8, ty - 6, tz, 18, 1, creatures)
        elif fy < ty:  # sul
            m.u8(0x67)
            row(m, world, tx - 8, ty + 7, tz, 18, 1, creatures)
        if fx < tx:   # leste
            m.u8(0x66)
            row(m, world, tx + 9, ty - 6, tz, 1, 14, creatures)
        elif fx > tx:  # oeste
            m.u8(0x68)
            row(m, world, tx - 8, ty - 6, tz, 1, 14, creatures)
    m.send(sock, key)


def row(m, world, ox, oy, cam_z, w, h, creatures):
    """Linha/coluna de todos os pisos visíveis, com skip compartilhado."""
    skip = -1
    if cam_z <= 7:
        floors = range(7, -1, -1)
    else:
        floors = range(max(0, cam_z - 2), min(16, cam_z + 2) + 1)
    for z in floors:
        off = cam_z - z
        for nx in range(w):
            for ny in range(h):
                x, y = ox + off + nx, oy + off + ny
                desc = Msg()
                has = tile_desc(desc, world, x, y, z, 0, creatures)
                if has:
                    if skip >= 0:
                        m.u8(skip); m.u8(0xFF)
                    skip = 0
                    m.b += desc.b
                else:
                    skip += 1
                    if skip == 0xFF:
                        m.u8(0xFF); m.u8(0xFF)
                        skip = -1
    if skip >= 0:
        m.u8(skip); m.u8(0xFF)


# ---------------- servers ----------------

def login_server(port, game_port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    log(f"login server em :{port}")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_login, args=(conn, addr, game_port),
                         daemon=True).start()


def handle_login(conn, addr, game_port):
    try:
        key, (acc, password) = parse_first_packet(conn, is_game=False)
        log(f"login {addr}: acc={acc} pass={password!r}")
        if acc != TEST_ACCOUNT or password != TEST_PASSWORD:
            m = Msg(); m.u8(0x0A); m.s("Account number or password is not correct.")
            m.send(conn, key)
            conn.close()
            return
        m = Msg()
        m.u8(0x14)
        m.s("1\nmock server tibia-ps2 (fase 2/3)")
        m.u8(0x64)
        m.u8(1)
        m.s(TEST_CHAR)
        m.s(WORLD_NAME)
        m.u32(0x7F000001)  # 127.0.0.1 (host order como o OTHire envia)
        m.u16(game_port)
        m.u16(90)          # premium days
        m.send(conn, key)
        conn.close()
    except Exception as e:
        log(f"login erro: {e}")
        try: conn.close()
        except OSError: pass


def game_server(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    log(f"game server em :{port}")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_game, args=(conn, addr),
                         daemon=True).start()


def handle_game(conn, addr):
    
    log(f"game: conexao de {addr}")
    
    player = {"id": 0x10000001, "name": TEST_CHAR, "x": CX, "y": CY, "z": 7,
              "dir": 2, "outfit": 129, "speed": 220, "health": 100,
              "stackpos": 1, "last_step": 0.0, "mana": 40, "known": True}
    npc = {"id": 0x40000001, "name": "Mock NPC", "x": CX + 1, "y": CY - 1,
           "z": 7, "dir": 3, "outfit": 57, "speed": 220, "health": 100,
           "known": True}
    creatures = {}
    try:
        conn.sendall(b"\x00\x00")  # mensagem vazia ao conectar (como OTHire)
        key, (acc, name, password) = parse_first_packet(conn, is_game=True)
        if password != TEST_PASSWORD or name != TEST_CHAR:
            m = Msg(); m.u8(0x14); m.s("Character name or password incorrect.")
            m.send(conn, key)
            conn.close()
            return
        log(f"game {addr}: entrou com {name!r}")

        creatures[(CX + 1, CY - 1, 7)] = npc
        creatures[(CX, CY, 7)] = player

        # 0x0A login OK + pacotes iniciais (como sendAddCreature do OTHire)
        m = Msg()
        m.u8(0x0A)
        m.u32(player["id"])
        m.u16(0x32)
        m.u8(0)
        m.send(conn, key)

        send_map_description(conn, key, WORLD, (CX, CY, 7), player, creatures)

        m = Msg()
        m.u8(0x83)  # efeito de login
        m.pos(CX, CY, 7)
        m.u8(13)
        for slot in range(10):
            m.u8(0x79); m.u8(slot)      # inventário vazio
        m.u8(0xA0)                       # stats
        m.u16(player["health"]); m.u16(100)
        m.u16(400); m.u32(4200); m.u16(8); m.u8(0)
        m.u16(player["mana"]); m.u16(60); m.u8(2); m.u8(0); m.u8(100)
        m.u8(0xA1)                       # skills
        for i in range(7):
            m.u8(10); m.u8(0)
        m.u8(0x82); m.u8(8); m.u8(215)   # luz do mundo
        m.u8(0x8D); m.u32(player["id"]); m.u8(0); m.u8(0)
        m.u8(0xA2); m.u8(0)              # icons
        m.send(conn, key)

        last_ping = time.time()
        while True:
            if time.time() - last_ping > 5.0:
                m = Msg(); m.u8(0x1E); m.send(conn, key)
                last_ping = time.time()

            conn.settimeout(0.5)
            try:
                body = recv_msg(conn, key)
            except socket.timeout:
                continue
            except ConnectionError:
                break
            if body is None:
                continue
            if not handle_client_packet(conn, key, body, player, npc,
                                        creatures):
                break
    except Exception as e:
        log(f"game erro: {e}")
    try: conn.close()
    except OSError: pass


def text_msg(conn, key, txt, mclass=19):
    m = Msg(); m.u8(0xB4); m.u8(mclass); m.s(txt); m.send(conn, key)


def handle_client_packet(conn, key, body, player, npc, creatures):
    op = body[0]
    r = Reader(body); r.i = 1

    if op == 0x14:
        log("logout")
        return False
    if op == 0x1E:
        return True

    if 0x65 <= op <= 0x6D:
        dirs = {0x65: (0, -1, 0), 0x66: (1, 0, 0), 0x67: (0, 1, 0),
                0x68: (-1, 0, 0), 0x6A: (1, -1, 0), 0x6B: (1, 1, 0),
                0x6C: (-1, 1, 0), 0x6D: (-1, -1, 0)}
        if op not in dirs:
            return True
        dx, dy, dz = dirs[op]
        now = time.time() * 1000
        if now - player["last_step"] < STEP_MS * 0.9:
            m = Msg(); m.u8(0xB5); m.u8(player["dir"]); m.send(conn, key)
            log("passo rejeitado (muito rápido) — 0xB5")
            return True
        fx, fy, fz = player["x"], player["y"], player["z"]
        tx, ty, tz = fx + dx, fy + dy, fz
        if not WORLD.walkable(tx, ty, tz):
            m = Msg(); m.u8(0xB5); m.u8(player["dir"]); m.send(conn, key)
            log(f"passo para ({tx},{ty},{tz}) bloqueado — 0xB5")
            return True

        # buraco: teleporta p/ baixo com mapdesc completo
        if any(iid == I_HOLE for (iid, _) in WORLD.items(tx, ty, tz)) and tz == 7:
            player["last_step"] = now
            del creatures[(fx, fy, fz)]
            player["x"], player["y"], player["z"] = tx, ty, 8
            creatures[(tx, ty, 8)] = player
            m = Msg()
            m.u8(0x6C); m.pos(fx, fy, fz); m.u8(player["stackpos"])
            m.send(conn, key)
            send_map_description(conn, key, WORLD, (tx, ty, 8), player, creatures)
            text_msg(conn, key, "You fell through a hole!", 19)
            log(f"player caiu no buraco -> ({tx},{ty},8)")
            return True

        # escada/rampa: move o 0x6D até a rampa e SOBE com 0xBE
        # (convenção do cliente real: 0xBE => central (+1,+1,-1))
        if any(iid == I_STAIRS for (iid, _) in WORLD.items(tx, ty, tz)) and tz == 8:
            player["last_step"] = now
            del creatures[(fx, fy, fz)]
            # passo até a rampa (0x6D), como o OTHire faz antes do 0xBE
            m = Msg()
            m.u8(0x6D)
            m.pos(fx, fy, fz)
            m.u8(player["stackpos"])
            m.pos(tx, ty, tz)
            m.send(conn, key)
            # sobe: 0xBE (floors extras como OTHire MoveUpCreature).
            # Sem faixas 0x68/0x65: na convenção (+1,+1,-1) o effective
            # origin de TODOS os floors existentes não muda (a projeção
            # diagonal compensa), e no modelo otclient cada faixa
            # deslocaria o central 1 tile para LONGE do destino.
            nx_, ny_, nz_ = tx + 1, ty + 1, 7
            player["x"], player["y"], player["z"] = nx_, ny_, nz_
            creatures[(nx_, ny_, nz_)] = player
            m = Msg()
            m.u8(0xBE)
            # indo para a superfície: floors 5..0 com offsets 8-z
            # (ancorados no central ANTIGO, como OTHire — casa com o
            #  modelo de janela do cliente por causa da deriva diagonal)
            skip = -1
            for fz in range(5, -1, -1):
                skip = get_floor_description(m, WORLD, tx, ty, 8, fz,
                                              creatures, player["id"], skip)
            if skip >= 0:
                m.u8(skip); m.u8(0xFF)
            m.send(conn, key)
            text_msg(conn, key, "You climbed up.", 19)
            log(f"player subiu escada -> ({nx_},{ny_},7)")
            return True

        # passo normal
        player["last_step"] = now
        player["dir"] = {0: 2, 1: 1, 2: 2, 3: 3}.get(dy + 1 if dx == 0 else
                                                     (1 if dx > 0 else 3), 2)
        del creatures[(fx, fy, fz)]
        player["x"], player["y"], player["z"] = tx, ty, tz
        player["stackpos"] = 1
        creatures[(tx, ty, tz)] = player
        send_own_move(conn, key, (fx, fy, fz), (tx, ty, tz), player,
                      creatures, WORLD)
        log(f"player andou -> ({tx},{ty},{tz})")
        return True

    if op in (0x6F, 0x70, 0x71, 0x72):  # turn
        player["dir"] = {0x6F: 0, 0x70: 1, 0x71: 2, 0x72: 3}[op]
        return True

    if op == 0x8C:  # look at
        pos = r.pos()
        sprite = r.u16()
        stack = r.u8()
        x, y, z = pos
        items = WORLD.items(x, y, z)
        g = WORLD.ground(x, y, z)
        if (x, y, z) in creatures:
            c = creatures[(x, y, z)]
            text_msg(conn, key,
                     f"You see {c['name']}. Health: {c['health']}%.", 22)
        elif items:
            text_msg(conn, key, f"You see item {items[0][0]}.", 22)
        elif g:
            text_msg(conn, key, "You see some ground.", 22)
        else:
            text_msg(conn, key, "You see nothing.", 22)
        log(f"look em ({x},{y},{z}) stack {stack}")
        return True

    if op == 0x82:  # use item
        pos = r.pos()
        sprite = r.u16()
        stack = r.u8()
        idx = r.u8()
        x, y, z = pos
        if z == 7 and (x, y) == (CX, CY - 4):
            # porta
            was = WORLD.door_closed.get((x, y, z), False)
            WORLD.door_closed[(x, y, z)] = not was
            m = Msg()
            m.u8(0x69)
            m.pos(x, y, z)
            g = WORLD.ground(x, y, z)
            if g: m.item(g)
            for (iid, cnt) in WORLD.items(x, y, z):
                m.item(iid, cnt)
            m.u8(0); m.u8(0xFF)
            m.send(conn, key)
            text_msg(conn, key,
                     "The door is open." if not was else "The door is closed.",
                     19)
            log("porta alternada")
            return True
        if z == 7 and (x, y) == (CX - 3, CY - 3):
            m = Msg()
            m.u8(0x6E)
            m.u8(0)
            m.item(I_CHEST)
            m.s("chest")
            m.u8(8)
            m.u8(0)
            m.u8(2)
            m.item(I_COIN, 3)
            m.item(I_COIN, 42)
            m.send(conn, key)
            log("baú aberto")
            return True
        text_msg(conn, key, "You cannot use this object.", 20)
        return True

    if op == 0xA1:  # attack
        cid = r.u32()
        if cid == npc["id"]:
            text_msg(conn, key, "You attack the mock NPC.", 19)
        else:
            text_msg(conn, key, "Target not found.", 20)
        return True

    if op == 0x96:  # say
        stype = r.u8()
        receiver = None
        if stype in (4, 255):
            receiver = r.s()
        elif stype in (5, 9, 10, 11):
            r.u16()
        txt = r.s()
        m = Msg()
        m.u8(0xAA)
        m.u32(1)
        m.s(player["name"])
        m.u8(1)
        m.pos(player["x"], player["y"], player["z"])
        m.s(txt)
        m.send(conn, key)
        log(f"say({stype}): {txt!r}")
        return True

    log(f"opcode cliente desconhecido 0x{op:02X}")
    return True


def main():
    login_port = 7172
    game_port = 7173
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--login-port": login_port = int(args[i + 1])
        if a == "--game-port": game_port = int(args[i + 1])
    log(f"conta de teste: {TEST_ACCOUNT}/{TEST_PASSWORD} char '{TEST_CHAR}'")
    threading.Thread(target=game_server, args=(game_port,), daemon=True).start()
    login_server(login_port, game_port)


if __name__ == "__main__":
    main()
