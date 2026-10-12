/**
 * game_protocol.c �?" cliente do protocolo de jogo 7.4 (era OTHire/TFS).
 *
 * Handlers escrevem DIRETAMENTE na GameMap (struct Tile/Thing que o
 * renderer consome). Modelo de janela:
 *   window[z][ly][lx] = tile mundial (central + (centralZ - z) + (lx-8, ly-6))
 * Os dados chegam do servidor JÁ pré-deslocados (empilhamento diagonal
 * "de graça"); trocas de andar movem o central em diagonal (+1,+1,-1) o
 * que mantém as janelas de todos os andares alinhadas sem rebase.
 *
 * Layout dos pacotes: docs/protocol-74-notes.md (fonte: OTHire
 * protocolgame.cpp, validado contra mock server em testes de host).
 */
#include <stdio.h>
#include <string.h>

#include "net_stream.h"
#include "netmsg.h"
#include "xtea.h"
#include "tibia_rsa.h"
#include "login.h"
#include "../poc/dat_loader.h"
#include "game_protocol.h"

#define GP_AWARE_UNDERGROUND 2
#define PRED_TIMEOUT_FRAMES 60 /* ~2s a 30fps */
#define PRED_MAX 2
/* duração de passo de criatura (ms) �?" 7.60: 1000*ground/(2*speed);
 * mock: speed 220, ground 150 �?' 340ms */
#define CREATURE_STEP_MS 340

GameMap gp_map;
GpChatLog gp_chat;
GpContainer gp_containers[GP_CONTAINERS];
uint16_t gp_inventory[GP_INV_SLOTS];

static GpCallbacks gp_cb;
static char gp_err[128];

static int gp_fd = -1;
static XteaKey gp_key;
static int gp_logged_in = 0; /* recebeu 0x0A + mapdesc */
static uint32_t gp_known[130];
static int gp_known_count = 0;
/* Floor change (modelo otclient protocolgameparse.cpp): dentro de uma
 * msg com 0xBE/0xBF, cada 0x65..0x68 também desloca o central 1 tile.
 * O 0x6D próprio com troca de andar N�fO mexe no central (o 0xBE/0BF
 * faz o ±1/±1/±1) �?" mas a criatura é colocada normalmente no tile de
 * destino: a projeção diagonal garante que o tile mapeia para a mesma
 * célula local antes e depois do 0xBE. */
static int fc_sync = 0;
static int fc_up = 0, fc_cb = 0;
/* x VELHO do passo do player (0x6D próprio): OTHire ancora as faixas
 * N/S no x antigo (não no novo) em walks diagonais. Zerado por msg. */
static int own_from_x = 0;
static int own_from_valid = 0;
/* ---------- criaturas conhecidas (espelha o servidor) ---------- */

static int known_find(uint32_t id)
{
    for (int i = 0; i < gp_known_count; i++)
        if (gp_known[i] == id)
            return i;
    return -1;
}

static void known_add(uint32_t id)
{
    if (known_find(id) >= 0)
        return;
    if (gp_known_count >= (int)(sizeof(gp_known) / sizeof(gp_known[0]))) {
        memmove(gp_known, gp_known + 1,
                (gp_known_count - 1) * sizeof(gp_known[0]));
        gp_known_count--;
    }
    gp_known[gp_known_count++] = id;
}

static void known_remove(uint32_t id)
{
    int i = known_find(id);
    if (i < 0)
        return;
    memmove(gp_known + i, gp_known + i + 1,
            (gp_known_count - i - 1) * sizeof(gp_known[0]));
    gp_known_count--;
}

/* ---------- janela ---------- */

static int eff_origin_x(int z)
{
    return gp_map.playerTileX + (gp_map.playerFloor - z) - 8;
}

static int eff_origin_y(int z)
{
    return gp_map.playerTileY + (gp_map.playerFloor - z) - 6;
}

static Tile *tile_at_world(int wx, int wy, int wz)
{
    if (wz < 0 || wz >= NET_MAP_FLOORS)
        return NULL;
    int lx = wx - eff_origin_x(wz);
    int ly = wy - eff_origin_y(wz);
    if (lx < 0 || lx >= NET_MAP_W || ly < 0 || ly >= NET_MAP_H)
        return NULL;
    return &gp_map.tiles[wz][ly][lx];
}

static void set_central(int x, int y, int z)
{
    gp_map.playerTileX = x;
    gp_map.playerTileY = y;
    gp_map.playerFloor = z;
}

/* ---------- chat ---------- */

void gp_chat_push(const char *text)
{
    strncpy(gp_chat.lines[gp_chat.head], text, GP_CHAT_LEN - 1);
    gp_chat.lines[gp_chat.head][GP_CHAT_LEN - 1] = 0;
    gp_chat.head = (gp_chat.head + 1) % GP_CHAT_LINES;
    if (gp_chat.count < GP_CHAT_LINES)
        gp_chat.count++;
}

const char *gp_chat_line(int i)
{
    if (i < 0 || i >= gp_chat.count)
        return "";
    int idx = (gp_chat.head - gp_chat.count + i + GP_CHAT_LINES * 2) % GP_CHAT_LINES;
    return gp_chat.lines[idx];
}

/* ---------- predição otimista (Fase 4) ---------- */

/* O gm_scroll do passo otimista descarta a faixa de tiles do lado
 * oposto ao movimento e cria uma vazia na frente. O gm_scroll(-dx,-dy)
 * da reversão NÃO traz a faixa descartada de volta (a faixa nova dessa
 * direção fica vazia) e o servidor não a reenvia — para ele o player
 * nunca saiu do lugar. Sem salvar/restaurar, cada cancelamento (0xB5,
 * 0x6D mismatch, timeout) deixa uma faixa de tiles pretos (vazios)
 * que desliza para a área visível ao continuar andando. Os tiles são
 * indexados por coordenada de mundo (a projeção diagonal difere por
 * floor, por isso wx/wy dependem de z). */
#define PRED_SAVE_MAX (NET_MAP_FLOORS * (NET_MAP_W + NET_MAP_H))

typedef struct {
    int wx, wy, wz;
    Tile tile;
} SavedTile;

typedef struct {
    int from[3], to[3];
    SavedTile *saved; /* -> pred_save_buf[índice do slot] */
    int nsaved;
} PredStep;

static PredStep predq[PRED_MAX];
static SavedTile pred_save_buf[PRED_MAX][PRED_SAVE_MAX];
static int pred_n = 0;
static int pred_age = 0;
static int pred_head = 0; /* ring: índice do passo mais antigo */

/* passo i da fila (0 = mais antigo) — slots em anel: o pop avança o
 * head em vez de copiar struct + tiles salvos de slot em slot */
static PredStep *pred_step(int i)
{
    return &predq[(pred_head + i) % PRED_MAX];
}

static void pred_clear(void)
{
    pred_n = 0;
    pred_head = 0;
    pred_age = 0;
}

/* acha o tile que contém o próprio player */
static Tile *find_own_tile(void)
{
    /* caminho quente (gp_own_dir roda todo frame, gp_walk_optimistic a
     * cada passo): o player fica no tile central — tenta o acesso direto
     * antes de varrer os 4032 tiles da janela */
    Tile *c = tile_at_world(gp_map.playerTileX, gp_map.playerTileY,
                            gp_map.playerFloor);
    if (c && gm_tile_find_creature(c, gp_map.playerId, NULL))
        return c;
    /* fallback: varredura completa (só em caso excepcional) */
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                Tile *t = &gp_map.tiles[z][y][x];
                if (gm_tile_find_creature(t, gp_map.playerId, NULL))
                    return t;
            }
    return NULL;
}

/* forward: helpers de inserção por prioridade (definidos na seção
 * "things", usados pela predição e pelos handlers de pacote) */
static int tile_insert_thing(Tile *t, const Thing *th);
static void tile_renumber(Tile *t);

/* move o thing do próprio player entre tiles da janela */
static void move_own_thing(int fx, int fy, int fz, int tx, int ty, int tz)
{
    Tile *from = tile_at_world(fx, fy, fz);
    Tile *to = tile_at_world(tx, ty, tz);
    Thing *th = from ? gm_tile_find_creature(from, gp_map.playerId, NULL) : NULL;
    if (!th)
        return;
    Thing moved = *th;
    gm_tile_remove_creature(from, gp_map.playerId);
    if (to)
        tile_insert_thing(to, &moved); /* prioridade: antes de loot */
}

static void pred_confirmed(int *cx, int *cy)
{
    *cx = gp_map.playerTileX;
    *cy = gp_map.playerTileY;
    for (int i = 0; i < pred_n; i++) {
        PredStep *s = pred_step(i);
        *cx -= s->to[0] - s->from[0];
        *cy -= s->to[1] - s->from[1];
    }
}

/* Copia um tile da faixa que vai cair fora da janela. Tiles vazios não
 * são salvos: o scroll de volta os deixa limpos (gm_tile_clear) e
 * restaurar um tile vazio equivale a não restaurar nada — em áreas
 * esparsas isto corta quase toda a cópia de Tile (struct grande). */
static void pred_save_one(PredStep *s, int z, int x, int y)
{
    Tile *t = &gp_map.tiles[z][y][x];
    if (!t->groundItemId && !t->thingCount)
        return; /* vazio: restaurar equivale a limpar */
    if (s->nsaved >= PRED_SAVE_MAX)
        return;
    SavedTile *st = &s->saved[s->nsaved++];
    st->wx = eff_origin_x(z) + x;
    st->wy = eff_origin_y(z) + y;
    st->wz = z;
    st->tile = *t;
}

/* Captura, ANTES do gm_scroll do passo, os tiles que o scroll vai
 * descartar: os que caem fora da janela no destino (lx,ly) ->
 * (lx-dx, ly-dy). Só a linha e/ou coluna da borda oposta ao movimento
 * caem fora — percorrê-las diretamente evita testar os 4032 tiles.
 * Deve rodar com o central AINDA na origem do passo (as coordenadas de
 * mundo vêm do eff_origin do estado atual). */
static void pred_save_dropped(PredStep *s, int dx, int dy)
{
    s->nsaved = 0;
    int drop_y = (dy > 0) ? 0 : NET_MAP_H - 1;
    int drop_x = (dx > 0) ? 0 : NET_MAP_W - 1;
    for (int z = 0; z < NET_MAP_FLOORS; z++) {
        if (dy)
            for (int x = 0; x < NET_MAP_W; x++)
                pred_save_one(s, z, x, drop_y);
        if (dx)
            for (int y = 0; y < NET_MAP_H; y++)
                if (!(dy && y == drop_y)) /* canto já salvo na linha */
                    pred_save_one(s, z, drop_x, y);
    }
}

/* Devolve ao mapa os tiles capturados por pred_save_dropped. Chamar
 * APÓS o gm_scroll(-dx,-dy) + set_central da reversão: o central volta
 * a ser a origem do passo, então as coordenadas de mundo mapeiam para
 * as mesmas células locais de quando foram salvas. */
static void pred_restore_dropped(const PredStep *s)
{
    for (int i = 0; i < s->nsaved; i++) {
        Tile *t = tile_at_world(s->saved[i].wx, s->saved[i].wy,
                                s->saved[i].wz);
        if (t)
            *t = s->saved[i].tile;
    }
}

static void pred_revert_all(void)
{
    while (pred_n > 0) {
        pred_n--;
        PredStep *s = pred_step(pred_n); /* mais recente primeiro */
        int dx = s->to[0] - s->from[0];
        int dy = s->to[1] - s->from[1];
        if (gp_map.playerFloor == s->to[2] && (dx || dy)) {
            gm_scroll(&gp_map, -dx, -dy);
            set_central(s->from[0], s->from[1], s->from[2]);
            /* o scroll de volta deixa a faixa descartada pelo passo
             * VAZIA (o servidor não a reenvia); recoloca os tiles
             * salvos antes de devolver a criatura ao tile de origem */
            pred_restore_dropped(s);
            move_own_thing(s->to[0], s->to[1], s->to[2],
                           s->from[0], s->from[1], s->from[2]);
        }
    }
    pred_clear();
}

/* Confirmação 0x6D do passo mais antigo: sai da fila avançando o
 * índice do ring — nada é copiado (o buffer de tiles salvos fica com o
 * próprio slot e é reescrito quando o slot volta a entrar). */
static void pred_pop(void)
{
    pred_head = (pred_head + 1) % PRED_MAX;
    pred_n--;
    pred_age = 0;
}

static const int walk_delta[8][2] = {
    {0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1},
};

/* ---------- things ---------- */

static void recalc_tile_flags(Tile *t)
{
    t->blocksView = (t->groundItemId != 0);
    t->blocksProjectile = 0;
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].isCreature)
            continue;
        const DatThingInfo *d = dat_get_item(t->things[i].itemId);
        if (!d)
            continue;
        if (d->flags & DATF_BLOCKPROJECTILE)
            t->blocksProjectile = 1;
        if ((d->flags & DATF_ONBOTTOM) && (d->flags & DATF_BLOCKPROJECTILE))
            t->blocksView = 1;
    }
}

static void apply_elevation(Tile *t, uint16_t itemId)
{
    const DatThingInfo *d = dat_get_item(itemId);
    if (d && (d->flags & DATF_ELEVATION) && d->elevation > t->elevation)
        t->elevation = (uint8_t)d->elevation;
}

static uint16_t item_anchor_sprite(uint16_t itemId, uint8_t count,
                                    int wx, int wy, int wz)
{
    const DatThingInfo *d = dat_get_item(itemId);
    if (!d)
        return 0;
    uint16_t spr[TILE_SPRITE_MAX];
    int w, h;
    int n = dat_resolve_sprites(d, count, wx, wy, wz, spr, &w, &h);
    if (n <= 0)
        return 0;
    return spr[n - 1]; /* peça âncora (bottom-right p/ multi-tile) */
}

static void read_thing(NetMsg *m, Tile *t, int wx, int wy, int wz);

/*
 * Prioridade de stack do protocolo (OTHire Tile::getThingIndex):
 * 0 = ground, 1 = onBottom/onTop (walls, portas), 2 = criaturas,
 * 3 = itens comuns (loot). O servidor envia nesta ordem; adições
 * posteriores (0x6A, 0x6D) precisam INSERTAR na posição certa,
 * não anexar no fim — senão stackpos diverge e o 0x6D seguinte
 * não acha a criatura.
 */
static int thing_stack_priority(const Thing *th)
{
    if (th->isCreature)
        return 2;
    const DatThingInfo *d = dat_get_item(th->itemId);
    if (!d)
        return 3;
    if (d->flags & (DATF_ONTOP | DATF_ONBOTTOM))
        return 1;
    return 3;
}

/* Insere o thing na posição correta do tile (após tudo de prioridade
 * <= à sua). Renumbera os stackpos. 0 ok, -1 tile cheio. */
static int tile_insert_thing(Tile *t, const Thing *th)
{
    if (t->thingCount >= TILE_THINGS_MAX)
        return -1;
    int prio = thing_stack_priority(th);
    int pos = t->thingCount;
    for (int i = 0; i < t->thingCount; i++) {
        if (thing_stack_priority(&t->things[i]) > prio) {
            pos = i;
            break;
        }
    }
    for (int i = t->thingCount; i > pos; i--)
        t->things[i] = t->things[i - 1];
    t->things[pos] = *th;
    t->thingCount++;
    int off = (t->groundItemId != 0) ? 1 : 0;
    for (int i = 0; i < t->thingCount; i++)
        t->things[i].stackpos = (uint8_t)(i + off);
    return 0;
}

/* Renumera os stackpos após qualquer mutação que muda o array. */
static void tile_renumber(Tile *t)
{
    int off = (t->groundItemId != 0) ? 1 : 0;
    for (int i = 0; i < t->thingCount; i++)
        t->things[i].stackpos = (uint8_t)(i + off);
}

/*
 * Lê a descrição de um tile: things até aparecer um u16 >= 0xFF00
 * (marcador da era: o byte ALTO 0xFF termina o tile e o byte BAIXO é o
 * número de tiles vazios que se seguem �?" fonte: otclient
 * setTileDescription/OTHire GetFloorDescription).
 * Retorna o skip count (byte baixo do marcador).
 */
static int read_tile_description(NetMsg *m, Tile *t, int wx, int wy, int wz)
{
    gm_tile_clear(t);
    while (nm_remaining(m) >= 2) {
        uint16_t peek = (uint16_t)(m->buf[2 + m->pos] |
                                   (m->buf[2 + m->pos + 1] << 8));
        if (peek >= 0xFF00) {
            nm_get_u16(m);
            recalc_tile_flags(t);
            return peek & 0xFF;
        }
        read_thing(m, t, wx, wy, wz);
    }
    recalc_tile_flags(t);
    return 0;
}

static void read_thing(NetMsg *m, Tile *t, int wx, int wy, int wz)
{
    uint16_t id = nm_get_u16(m);

    if (id == 0x61 || id == 0x62) { /* criatura */
        char name[32] = "";
        uint32_t cid;
        if (id == 0x61) { /* desconhecida */
            uint32_t removeId = nm_get_u32(m);
            known_remove(removeId);
            cid = nm_get_u32(m);
            nm_get_string(m, name, sizeof(name));
        } else {
            cid = nm_get_u32(m);
        }
        known_add(cid);

        uint8_t health = nm_get_u8(m);
        uint8_t dir = nm_get_u8(m);
        uint16_t looktype = nm_get_u16(m);
        if (looktype != 0)
            nm_skip(m, 4); /* head, body, legs, feet */
        else
            nm_get_u16(m); /* looktypeEx */
        nm_skip(m, 2);     /* luz: level, cor */
        uint16_t speed = nm_get_u16(m);
        nm_skip(m, 2);     /* skull, party */

        /* inserção por prioridade (criaturas antes de itens comuns):
         * no mapdesc o servidor já envia nesta ordem (equivalente a
         * append), mas no 0x6A/0x6D a criatura precisa entrar ANTES
         * de loot no chão — senão stackpos diverge do servidor */
        Thing th;
        memset(&th, 0, sizeof(th));
        th.isCreature = 1;
        th.creatureId = cid;
        th.outfitId = looktype;
        th.dir = dir;
        th.healthPct = health;
        if (name[0])
            strncpy(th.name, name, sizeof(th.name) - 1);
        tile_insert_thing(t, &th);

        if (cid == gp_map.playerId && t == tile_at_world(wx, wy, wz)) {
            gp_map.hasPlayer = 1;
            gp_map.playerSpeed = speed;
        }
        return;
    }

    /* item */
    const DatThingInfo *d = dat_get_item(id);
    if (!d) {
        /* ID fora do DAT: o byte extra de stackable/fluido abaixo NÃO é
         * lido e o parse desalinha a partir daqui (bytes dos tiles
         * seguintes viram ids). Áreas novas trazem itens/monstros que a
         * área inicial não tinha. Log 1x por id. */
        static uint8_t seen[8192];
        if (!(seen[id >> 3] & (1u << (id & 7)))) {
            seen[id >> 3] |= (uint8_t)(1u << (id & 7));
            printf("gp: item id %u NAO esta no DAT (parse pode desalinhar)\n",
                   (unsigned)id);
        }
    }
    uint8_t count = 0;
    if (d && (d->flags & DATF_STACKABLE))
        count = nm_get_u8(m);
    else if (d && ((d->flags & DATF_FLUIDCONTAINER) || (d->flags & DATF_SPLASH)))
        nm_get_u8(m); /* tipo de fluido */

    if (t->groundItemId == 0 && d && (d->flags & DATF_GROUND) &&
        t->thingCount == 0) {
        t->groundItemId = id;
        t->groundSpriteId = item_anchor_sprite(id, count, wx, wy, wz);
    } else {
        Thing th;
        memset(&th, 0, sizeof(th));
        th.itemId = id;
        th.spriteId = item_anchor_sprite(id, count, wx, wy, wz);
        th.count = count;
        tile_insert_thing(t, &th);
    }
    apply_elevation(t, id);
}

/*
 * Lê um floor descrito em (ox, oy, z) com width x height tiles
 * (mesma máquina de estados do otclient setFloorDescription).
 * Ordem do servidor: coluna-major (nx externo, ny interno).
 */
static int read_floor(NetMsg *m, int ox, int oy, int z, int width, int height,
                       int skip_in)
{
    int skip = skip_in;
    for (int nx = 0; nx < width; nx++) {
        for (int ny = 0; ny < height; ny++) {
            int wx = ox + nx;
            int wy = oy + ny;
            Tile *t = tile_at_world(wx, wy, z);
            if (skip == 0) {
                if (t)
                    skip = read_tile_description(m, t, wx, wy, z);
                else {
                    Tile dummy;
                    memset(&dummy, 0, sizeof(dummy));
                    skip = read_tile_description(m, &dummy, wx, wy, z);
                }
            } else {
                if (t)
                    gm_tile_clear(t);
                skip--;
            }
        }
    }
    return skip;
}

/*
 * MapDescription (0x64): posição do player + todos os floors da janela.
 * O contador de skip atravessa floors (o servidor só emite no fim).
 */
static void read_map_description(NetMsg *m)
{
    uint16_t px = nm_get_u16(m);
    uint16_t py = nm_get_u16(m);
    uint8_t pz = nm_get_u8(m);

    set_central(px, py, pz);
    gp_map.hasPlayer = 1;
    pred_clear(); /* mapdesc = posição autoritativa */
    fc_sync = 0; /* floor-change state não atravessa mapdesc */

    /* limpa só os tiles (preserva playerId/stats/velocidade) */
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++)
                gm_tile_clear(&gp_map.tiles[z][y][x]);

    int skip = 0;
    if (pz <= 7) {
        for (int z = 7; z >= 0; z--)
            skip = read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                              NET_MAP_W, NET_MAP_H, skip);
    } else {
        int z0 = pz - 2;
        int z1 = pz + 2;
        if (z1 > NET_MAP_FLOORS - 1)
            z1 = NET_MAP_FLOORS - 1;
        for (int z = z0; z <= z1; z++)
            skip = read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                              NET_MAP_W, NET_MAP_H, skip);
    }

    if (gp_cb.on_own_position_reset)
        gp_cb.on_own_position_reset(px, py, pz);

    /* Diagnóstico limitado ao mapa completo: IDs de item recebidos do
     * servidor e sprites resolvidos pelo DAT, numa janela 5x5 centrada
     * no jogador. �stil para distinguir ID incompatível de renderização. */
    printf("gp: map sample center=%u,%u,%u player=%08X\n", px, py, pz,
           (unsigned)gp_map.playerId);
    int ground_count = 0;
    int creature_count = 0;
    int own_found = 0;
    for (int y = 0; y < NET_MAP_H; y++) {
        for (int x = 0; x < NET_MAP_W; x++) {
            Tile *t = &gp_map.tiles[pz][y][x];
            ground_count += (t->groundItemId != 0);
            for (int i = 0; i < t->thingCount; i++) {
                if (t->things[i].isCreature) {
                    creature_count++;
                    if (t->things[i].creatureId == gp_map.playerId) {
                        printf("gp: own creature local=%d,%d world=%d,%d,%d\n",
                               x, y, gp_map.playerTileX + x - 8,
                               gp_map.playerTileY + y - 6, pz);
                        own_found = 1;
                    }
                }
            }
        }
    }
    printf("gp: floor %u populated ground=%d creatures=%d own=%d\n",
           pz, ground_count, creature_count, own_found);
    for (int z = 0; z < NET_MAP_FLOORS; z++) {
        int dz = (int)pz - z;
        Tile *direct = tile_at_world(px, py, z);
        Tile *covered = tile_at_world((int)px + dz, (int)py + dz, z);
        printf("gp: floorcheck z=%d direct=%u/s%u covered=%u/s%u\n", z,
               direct ? (unsigned)direct->groundItemId : 0,
               direct ? (unsigned)direct->groundSpriteId : 0,
               covered ? (unsigned)covered->groundItemId : 0,
               covered ? (unsigned)covered->groundSpriteId : 0);
    }
    printf("gp: map grid floor %u (x west->east, y north->south)\n", pz);
    for (int y = 0; y < NET_MAP_H; y++) {
        printf("  ");
        for (int x = 0; x < NET_MAP_W; x++) {
            Tile *t = &gp_map.tiles[pz][y][x];
            char cell = t->groundItemId ? '#' : '.';
            for (int i = 0; i < t->thingCount; i++)
                if (t->things[i].isCreature)
                    cell = 'C';
            putchar(cell);
        }
        putchar('\n');
    }
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            Tile *t = tile_at_world((int)px + dx, (int)py + dy, pz);
            if (!t) {
                printf("  (%+d,%+d) out\n", dx, dy);
                continue;
            }
            printf("  (%+d,%+d) g=%u/s%u", dx, dy,
                   (unsigned)t->groundItemId, (unsigned)t->groundSpriteId);
            if (t->groundItemId && !dat_get_item(t->groundItemId))
                printf("!");
            for (int i = 0; i < t->thingCount; i++) {
                Thing *th = &t->things[i];
                if (th->isCreature) {
                    printf(" c=%s#%08X/look%u", th->name,
                           (unsigned)th->creatureId, (unsigned)th->outfitId);
                } else {
                    printf(" i=%u/s%u%s", (unsigned)th->itemId,
                           (unsigned)th->spriteId,
                           dat_get_item(th->itemId) ? "" : "!");
                }
            }
            putchar('\n');
        }
    }
    printf("gp: after mapdesc pos=%d remaining=%d next=%02X\n",
           m->pos, nm_remaining(m), nm_peek_u8(m));
}

/* linha/coluna ao andar (0x65 N, 0x66 E, 0x67 S, 0x68 W) */
static void read_partial_row(NetMsg *m, uint8_t op)
{
    /* Dentro de uma msg com 0xBE/0xBF, cada faixa também desloca o
     * central 1 tile na direção da faixa (modelo otclient). Em walk
     * normal a predição já aplicou o passo �?" só preenche a faixa. */
    if (fc_sync) {
        int dx = (op == 0x66) - (op == 0x68);
        int dy = (op == 0x67) - (op == 0x65);
        gm_scroll(&gp_map, dx, dy);
        set_central(gp_map.playerTileX + dx, gp_map.playerTileY + dy,
                    gp_map.playerFloor);
    }

    int cx, cy;
    pred_confirmed(&cx, &cy);
    /* OTHire: faixas N/S são ancoradas no x VELHO do passo (em walks
     * diagonais NE/NW/SE/SW a linha norte/sul cobre o range do x
     * antigo, não do novo). Faixas E/W usam o x novo. Em fc_sync o
     * central vai deslocando faixa a faixa — já está certo. */
    int sx = (!fc_sync && own_from_valid) ? own_from_x : cx;
    int pz = gp_map.playerFloor;
    int ox, oy, w, h;

    switch (op) {
    case 0x65: ox = sx - 8; oy = cy - 6; w = NET_MAP_W; h = 1; break; /* norte */
    case 0x66: ox = cx + 9; oy = cy - 6; w = 1; h = NET_MAP_H; break; /* leste */
    case 0x67: ox = sx - 8; oy = cy + 7; w = NET_MAP_W; h = 1; break; /* sul   */
    default:   ox = cx - 8; oy = cy - 6; w = 1; h = NET_MAP_H; break; /* oeste */
    }
    int z0, z1, step;
    if (pz <= 7) {
        z0 = 7;
        z1 = 0;
        step = -1;
    } else {
        z0 = pz - GP_AWARE_UNDERGROUND;
        z1 = pz + GP_AWARE_UNDERGROUND;
        if (z1 >= NET_MAP_FLOORS)
            z1 = NET_MAP_FLOORS - 1;
        step = 1;
    }

    /* Cada opcode 0x65..0x68 descreve a faixa em todos os pisos visíveis.
     * O skip é compartilhado entre floors e cada floor usa o offset
     * diagonal relativo ao piso central, igual ao MapDescription 0x64. */
    int skip = 0;
    for (int z = z0; z != z1 + step; z += step) {
        int offset = pz - z;
        skip = read_floor(m, ox + offset, oy + offset, z, w, h, skip);
    }
    /* o skip zera ao consumir a última célula da faixa; sobra != 0 =
     * bytes faltando/sobrando (sintoma de parse desalinhado acima) */
    if (skip != 0)
        printf("gp: faixa 0x%02X terminou com skip=%d (desalinhado?)\n",
               (unsigned)op, skip);
}

/*
 * 0xBE/0xBF �?" troca de andar do próprio player. Floors extras vêm
 * "inline" (sem opcode). O 0xBE/0BF desloca o frame ±1/±1/±1 e liga
 * o modo fc_sync: as faixas 0x65..0x68 seguintes também deslocam o
 * central (modelo otclient). O callback on_own_floor_change dispara
 * no FIM da mensagem (finish_floor_change), com a posição real.
 */
static void read_floor_change_up(NetMsg *m)
{
    /* novo central: x+1, y+1, z-1 (convenção do protocolo �?" rampas) */
    set_central(gp_map.playerTileX + 1, gp_map.playerTileY + 1,
                gp_map.playerFloor - 1);
    int nz = gp_map.playerFloor;

    if (nz == 7) {
        int skip = 0;
        for (int z = 5; z >= 0; z--)
            skip = read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                              NET_MAP_W, NET_MAP_H, skip);
        (void)skip;
    } else if (nz > 7) {
        int z = nz - 2;
        read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                   NET_MAP_W, NET_MAP_H, 0);
    }

    fc_sync = 1;
    fc_up = 1;
    fc_cb = 1;
}

static void read_floor_change_down(NetMsg *m)
{
    set_central(gp_map.playerTileX - 1, gp_map.playerTileY - 1,
                gp_map.playerFloor + 1);
    int nz = gp_map.playerFloor;

    if (nz == 8) {
        int skip = 0;
        for (int z = 8; z <= 10; z++)
            skip = read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                              NET_MAP_W, NET_MAP_H, skip);
        (void)skip;
    } else if (nz > 8 && nz < 14) {
        int z = nz + 2;
        read_floor(m, eff_origin_x(z), eff_origin_y(z), z,
                   NET_MAP_W, NET_MAP_H, 0);
    }

    fc_sync = 1;
    fc_up = 0;
    fc_cb = 1;
}

/* 0x69 �?" update do tile inteiro: things + marcador 0xFF00 */
static void read_update_tile(NetMsg *m)
{
    uint16_t x = nm_get_u16(m);
    uint16_t y = nm_get_u16(m);
    uint8_t z = nm_get_u8(m);
    Tile *t = tile_at_world(x, y, z);
    if (t)
        read_tile_description(m, t, x, y, z);
    else {
        Tile dummy;
        memset(&dummy, 0, sizeof(dummy));
        read_tile_description(m, &dummy, x, y, z);
    }
    /* o marcador (0x00 0xFF = u16 0xFF00) já foi consumido */
}

/* 0x6A �?" add thing */
static void read_add_thing(NetMsg *m)
{
    uint16_t x = nm_get_u16(m);
    uint16_t y = nm_get_u16(m);
    uint8_t z = nm_get_u8(m);
    Tile *t = tile_at_world(x, y, z);
    if (!t) {
        /* fora da janela: descarta o thing lendo em dummy */
        Tile dummy;
        memset(&dummy, 0, sizeof(dummy));
        read_thing(m, &dummy, x, y, z);
        return;
    }
    read_thing(m, t, x, y, z);
}

/* 0x6B �?" transform thing OU virar criatura (quirk da era: byte<=3 = dir) */
static void read_transform_thing(NetMsg *m)
{
    uint16_t x = nm_get_u16(m);
    uint16_t y = nm_get_u16(m);
    uint8_t z = nm_get_u8(m);
    uint8_t stackpos = nm_get_u8(m);
    Tile *t = tile_at_world(x, y, z);

    /* peek u16 sem consumir: 0x0063 = creature turn (OTHire
     * sendCreatureTurn: u16 0x63, u32 cid, u8 dir) */
    uint16_t peek16 = 0;
    if (nm_remaining(m) >= 2)
        peek16 = (uint16_t)(m->buf[2 + m->pos] |
                            ((uint16_t)m->buf[2 + m->pos + 1] << 8));
    if (peek16 == 0x0063) {
        nm_get_u16(m);
        uint32_t cid = nm_get_u32(m);
        uint8_t dir = nm_get_u8(m);
        if (t) {
            Thing *th = gm_tile_find_creature(t, cid, NULL);
            if (th)
                th->dir = dir;
        }
        return;
    }

    if (!t) {
        Tile dummy;
        memset(&dummy, 0, sizeof(dummy));
        read_thing(m, &dummy, x, y, z);
        return;
    }

    /* ground transform (stackpos 0): troca direta, sem mexer no array */
    if (stackpos == 0 && t->groundItemId != 0) {
        uint16_t new_id = nm_get_u16(m);
        const DatThingInfo *d = dat_get_item(new_id);
        uint8_t count = 0;
        if (d && (d->flags & DATF_STACKABLE)) {
            count = nm_get_u8(m);
        } else if (d && ((d->flags & DATF_FLUIDCONTAINER) ||
                         (d->flags & DATF_SPLASH))) {
            nm_get_u8(m);
        }
        t->groundItemId = new_id;
        t->groundSpriteId = item_anchor_sprite(new_id, count, x, y, z);
        recalc_tile_flags(t);
        return;
    }

    /* thing comum: acha o índice pelo stackpos e substitui NO LUGAR */
    int idx = -1;
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].stackpos == stackpos) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        Tile dummy;
        memset(&dummy, 0, sizeof(dummy));
        read_thing(m, &dummy, x, y, z);
        return;
    }

    /* lê o novo thing num tile temporário */
    Tile tmp;
    memset(&tmp, 0, sizeof(tmp));
    read_thing(m, &tmp, x, y, z);

    if (tmp.groundItemId != 0) {
        /* transform resultou num ground: só faz sentido se o tile
         * perdeu o ground (caso raro); vira thing para segurança */
        Thing th;
        memset(&th, 0, sizeof(th));
        th.itemId = tmp.groundItemId;
        th.spriteId = tmp.groundSpriteId;
        t->things[idx] = th;
    } else if (tmp.thingCount > 0) {
        t->things[idx] = tmp.things[0];
    } else {
        /* transform resultou em nada: remove */
        for (int j = idx; j < t->thingCount - 1; j++)
            t->things[j] = t->things[j + 1];
        t->thingCount--;
    }
    tile_renumber(t);
    recalc_tile_flags(t);
}

/* 0x6C �?" remove thing */
static void read_remove_thing(NetMsg *m)
{
    uint16_t x = nm_get_u16(m);
    uint16_t y = nm_get_u16(m);
    uint8_t z = nm_get_u8(m);
    uint8_t stackpos = nm_get_u8(m);
    /* Descida 7�?'8 (buraco): o servidor manda 0x6C na posição antiga em
     * vez de 0x6D. Se houve passo otimista até esse tile, reverte a
     * predição antes �?" senão o remove apagaria o thing errado (o
     * player já não está lá no mapa do servidor). */
    int sx, sy;
    pred_confirmed(&sx, &sy);
    if (pred_n > 0 && x == (uint16_t)sx && y == (uint16_t)sy &&
        z == (uint8_t)gp_map.playerFloor)
        pred_revert_all();
    Tile *t = tile_at_world(x, y, z);
    if (!t)
        return;
    if (stackpos == 0 && t->groundItemId != 0) {
        t->groundItemId = 0;
        t->groundSpriteId = 0;
        tile_renumber(t); /* todos os stackpos decrescem 1 */
        recalc_tile_flags(t);
        return;
    }
    int matched = 0;
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].stackpos == stackpos) {
            for (int j = i; j < t->thingCount - 1; j++)
                t->things[j] = t->things[j + 1];
            t->thingCount--;
            tile_renumber(t);
            matched = 1;
            break;
        }
    }
    if (!matched) {
        /* stackpos local divergiu do servidor: o thing alvo fica no tile
         * (fantasma) e desenha sprites a mais a cada frame */
        printf("gp: 0x6C sem match stackpos=%d em %d,%d,%d (thingCount=%d)\n",
               (int)stackpos, (int)x, (int)y, (int)z, (int)t->thingCount);
    }
    recalc_tile_flags(t);
}

/* 0x6D �?" mover criatura */
static void read_move_creature(NetMsg *m)
{
    uint16_t fx = nm_get_u16(m);
    uint16_t fy = nm_get_u16(m);
    uint8_t fz = nm_get_u8(m);
    uint8_t fstack = nm_get_u8(m);
    uint16_t tx = nm_get_u16(m);
    uint16_t ty = nm_get_u16(m);
    uint8_t tz = nm_get_u8(m);

    /* Fase 4: se é o passo previsto, já foi aplicado otimistamente �?"
     * apenas confirma (o tile de origem já não contém a criatura). */
    if (pred_n > 0) {
        PredStep *p0 = pred_step(0);
        if (p0->from[0] == (int)fx && p0->from[1] == (int)fy &&
            p0->from[2] == (int)fz &&
            p0->to[0] == (int)tx && p0->to[1] == (int)ty &&
            p0->to[2] == (int)tz) {
            own_from_x = (int)fx;
            own_from_valid = 1; /* faixas N/S ancoram no x velho */
            pred_pop();
            if (gp_cb.on_own_move_confirmed)
                gp_cb.on_own_move_confirmed(fx, fy, fz, tx, ty, tz);
            return;
        }
    }

    Tile *from = tile_at_world(fx, fy, fz);
    /* achar a criatura por stackpos (o protocolo referencia por stackpos) */
    Thing *found = NULL;
    if (from) {
        for (int i = 0; i < from->thingCount; i++) {
            if (from->things[i].isCreature &&
                from->things[i].stackpos == fstack) {
                found = &from->things[i];
                break;
            }
        }
    }
    /* Um movimento próprio divergente parte da posição confirmada do
     * servidor, enquanto a criatura pode estar vários tiles à frente na
     * janela otimista. Reverte antes de procurar/remover a criatura. */
    int server_x, server_y;
    pred_confirmed(&server_x, &server_y);
    int own_diverged = pred_n > 0 && fx == server_x && fy == server_y &&
                       fz == gp_map.playerFloor &&
                       (!found || found->creatureId == gp_map.playerId);
    if (own_diverged) {
#ifdef GP_DEBUG_MSG
        /* printf de caminho quente: pode disparar a cada 0x6D durante
         * um desync; em serial/stdio custa caro e derruba o FPS */
        PredStep *p0 = pred_step(0);
        printf("gp: 0x6D MISMATCH from=%d,%d to=%d,%d pred=%d,%d->%d,%d n=%d\n",
               fx, fy, tx, ty,
               pred_n ? p0->from[0] : -1,
               pred_n ? p0->from[1] : -1,
               pred_n ? p0->to[0] : -1,
               pred_n ? p0->to[1] : -1, pred_n);
#endif
        pred_revert_all();
        from = tile_at_world(fx, fy, fz);
        found = from ? gm_tile_find_creature(from, gp_map.playerId, NULL) : NULL;
    }
    if (!from) {
#ifdef GP_DEBUG_MSG
        printf("gp: move de (%d,%d,%d) fora da janela\n", fx, fy, fz);
#endif
        return;
    }
    if (!found) {
        /* criatura esperada neste stackpos não está no tile de origem:
         * stackpos local divergiu do servidor (ordem de prioridade,
         * tile cheio). Sem o remove, a criatura fica duplicada/fantasma. */
        printf("gp: 0x6D criatura nao achada sp=%d em %d,%d,%d\n",
               (int)fstack, (int)fx, (int)fy, (int)fz);
        return;
    }

    Thing moved = *found;
#ifdef GP_DEBUG_MSG
    printf("gp: move 0x6D (%d,%d,%d)sp%d -> (%d,%d,%d) cid=0x%08X own=%d\n",
           fx, fy, fz, fstack, tx, ty, tz, moved.creatureId,
           moved.creatureId == gp_map.playerId);
#endif
    gm_tile_remove_creature(from, moved.creatureId);

    /* Walk animation p/ criaturas (modelo Creature::walk do otclient):
     * a criatura fica no tile de DESTINO e o renderer aplica o offset
     * visual que desliza da origem. O player tem sistema próprio
     * (predição + cam_off no client_net) e não recebe walk state.
     * Teleport/mudança de andar: sem animação. */
    moved.walking = 0;
    if (moved.creatureId != gp_map.playerId && fz == tz) {
        int wdx = (int)tx - (int)fx;
        int wdy = (int)ty - (int)fy;
        if (wdx >= -1 && wdx <= 1 && wdy >= -1 && wdy <= 1 &&
            (wdx != 0 || wdy != 0)) {
            moved.walking = 1;
            moved.walkDx = (int8_t)wdx;
            moved.walkDy = (int8_t)wdy;
            moved.walkStartMs = gm_now_ms();
            moved.walkDurationMs = CREATURE_STEP_MS;
            /* direção do passo (Creature::setDirection do otclient:
             * diagonais usam a componente horizontal p/ o sprite) */
            if (wdx > 0)
                moved.dir = 1;      /* East (NE/SE �?' East) */
            else if (wdx < 0)
                moved.dir = 3;      /* West (NW/SW �?' West) */
            else if (wdy > 0)
                moved.dir = 2;      /* South */
            else
                moved.dir = 0;      /* North */
        }
    }

    if (moved.creatureId == gp_map.playerId) {
        own_from_x = (int)fx;
        own_from_valid = 1; /* faixas N/S ancoram no x velho */
        if (tz != fz) {
            /* Troca de andar (parseCreatureMove do otclient): a criatura
             * é colocada no destino normalmente (código comum abaixo) —
             * a projeção diagonal mapeia o tile para a mesma célula
             * local antes/depois do 0xBE. O central NÃO muda aqui; quem
             * move o frame é o 0xBE/0xBF ±1/±1/±1 + as faixas. */
            pred_clear();
            gp_map.hasPlayer = 1;
        } else {
            int dx = (int)tx - (int)fx;
            int dy = (int)ty - (int)fy;
            if (dx != 0 || dy != 0)
                gm_scroll(&gp_map, dx, dy);
            set_central(tx, ty, tz);
            pred_clear();
            gp_map.hasPlayer = 1;
            if (gp_cb.on_own_move_confirmed)
                gp_cb.on_own_move_confirmed(fx, fy, fz, tx, ty, tz);
        }
    }

    Tile *to = tile_at_world(tx, ty, tz);
    if (to)
        tile_insert_thing(to, &moved); /* prioridade: antes de loot */
}

/* 0x0A �?" login OK + (mapdesc vem em seguida na mesma mensagem) */
static void read_own_login(NetMsg *m)
{
    gp_map.playerId = nm_get_u32(m);
    nm_get_u16(m); /* 0x32 drawing speed */
    nm_get_u8(m);  /* can report bugs */
    if (nm_peek_u8(m) == 0x0B) {
        nm_get_u8(m);
        nm_skip(m, 32);
    }
    gp_logged_in = 1;
}

/* 0xA0 �?" stats */
static void read_stats(NetMsg *m)
{
    gp_map.playerHealth = nm_get_u16(m);
    gp_map.playerMaxHealth = nm_get_u16(m);
    nm_get_u16(m); /* free capacity */
    gp_map.playerExperience = nm_get_u32(m);
    gp_map.playerLevel = nm_get_u16(m);
    nm_get_u8(m); /* level % */
    gp_map.playerMana = nm_get_u16(m);
    gp_map.playerMaxMana = nm_get_u16(m);
    nm_get_u8(m);  /* magic level */
    nm_get_u8(m);  /* ml % */
    nm_get_u8(m);  /* soul */
}

/* 0xB4 �?" mensagem de texto */
static void read_text_message(NetMsg *m)
{
    uint8_t mclass = nm_get_u8(m);
    char text[GP_CHAT_LEN];
    nm_get_string(m, text, sizeof(text));
    (void)mclass;
    gp_chat_push(text);
    if (gp_cb.on_chat)
        gp_cb.on_chat(mclass, "", text);
}

/* 0xAA �?" criatura falando */
static void read_creature_speak(NetMsg *m)
{
    nm_get_u32(m); /* statement guid */
    char name[32];
    nm_get_string(m, name, sizeof(name));
    uint8_t type = nm_get_u8(m);
    switch (type) {
    case 1: case 2: case 3: case 16: case 17: /* say/whisper/yell/monster */
        nm_skip(m, 5); /* posição x,y,z */
        break;
    case 5: case 9: case 10: case 11: /* canal */
        nm_skip(m, 2);
        break;
    default:
        break;
    }
    char text[GP_CHAT_LEN];
    nm_get_string(m, text, sizeof(text));

    char line[GP_CHAT_LEN + 32];
    snprintf(line, sizeof(line), "%s: %.60s", name, text);
    gp_chat_push(line);
    if (gp_cb.on_chat)
        gp_cb.on_chat(type, name, text);
}

/* 0x6E �?" abrir container */
static void read_open_container(NetMsg *m)
{
    uint8_t cid = nm_get_u8(m);
    if (cid >= GP_CONTAINERS)
        return;
    GpContainer *c = &gp_containers[cid];
    c->active = 1;
    c->cid = cid;
    c->itemId = nm_get_u16(m);
    nm_get_string(m, c->name, sizeof(c->name));
    c->capacity = nm_get_u8(m);
    c->hasParent = nm_get_u8(m);
    uint8_t n = nm_get_u8(m);
    if (n > GP_CONT_ITEMS)
        n = GP_CONT_ITEMS;
    c->count = n;
    for (int i = 0; i < n; i++) {
        uint16_t id = nm_get_u16(m);
        const DatThingInfo *d = dat_get_item(id);
        uint8_t count = 0;
        if (d && (d->flags & DATF_STACKABLE))
            count = nm_get_u8(m);
        else if (d && ((d->flags & DATF_FLUIDCONTAINER) || (d->flags & DATF_SPLASH)))
            nm_get_u8(m);
        c->items[i] = id;
        c->itemCount[i] = count;
    }
}

static void read_container_item(NetMsg *m, int kind) /* 0x70 add, 0x71 upd */
{
    uint8_t cid = nm_get_u8(m);
    if (cid >= GP_CONTAINERS || !gp_containers[cid].active)
        return;
    GpContainer *c = &gp_containers[cid];
    uint8_t slot = 0;
    if (kind == 1) {
        slot = nm_get_u8(m);
        if (slot >= GP_CONT_ITEMS)
            return;
    } else {
        slot = c->count;
        if (slot >= GP_CONT_ITEMS)
            return;
        c->count++;
    }
    uint16_t id = nm_get_u16(m);
    const DatThingInfo *d = dat_get_item(id);
    uint8_t count = 0;
    if (d && (d->flags & DATF_STACKABLE))
        count = nm_get_u8(m);
    else if (d && ((d->flags & DATF_FLUIDCONTAINER) || (d->flags & DATF_SPLASH)))
        nm_get_u8(m);
    c->items[slot] = id;
    c->itemCount[slot] = count;
}

static void read_remove_container_item(NetMsg *m) /* 0x72 */
{
    uint8_t cid = nm_get_u8(m);
    if (cid >= GP_CONTAINERS || !gp_containers[cid].active)
        return;
    GpContainer *c = &gp_containers[cid];
    uint8_t slot = nm_get_u8(m);
    if (slot >= GP_CONT_ITEMS || slot >= c->count)
        return;
    for (int i = slot; i < c->count - 1 && i < GP_CONT_ITEMS - 1; i++) {
        c->items[i] = c->items[i + 1];
        c->itemCount[i] = c->itemCount[i + 1];
    }
    if (c->count > 0)
        c->count--;
}

static void read_close_container(NetMsg *m) /* 0x6F */
{
    uint8_t cid = nm_get_u8(m);
    if (cid < GP_CONTAINERS)
        gp_containers[cid].active = 0;
}

/* 0x78 add/update inventário, 0x79 remove */
static void read_inventory(NetMsg *m, int remove)
{
    uint8_t slot = nm_get_u8(m);
    if (remove) {
        if (slot < GP_INV_SLOTS)
            gp_inventory[slot] = 0;
        return;
    }
    uint16_t id = nm_get_u16(m);
    const DatThingInfo *d = dat_get_item(id);
    if (d && (d->flags & DATF_STACKABLE))
        nm_get_u8(m);
    else if (d && ((d->flags & DATF_FLUIDCONTAINER) || (d->flags & DATF_SPLASH)))
        nm_get_u8(m);
    if (slot < GP_INV_SLOTS)
        gp_inventory[slot] = id;
}

/* 0x8C creature health, 0x8E outfit, 0x8F speed */
static void read_creature_health(NetMsg *m)
{
    uint32_t cid = nm_get_u32(m);
    uint8_t pct = nm_get_u8(m);
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                Tile *t = &gp_map.tiles[z][y][x];
                Thing *th = gm_tile_find_creature(t, cid, NULL);
                if (th)
                    th->healthPct = pct;
            }
}

static void read_creature_outfit(NetMsg *m)
{
    uint32_t cid = nm_get_u32(m);
    uint16_t looktype = nm_get_u16(m);
    if (looktype != 0)
        nm_skip(m, 4);
    else
        nm_get_u16(m);
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                Tile *t = &gp_map.tiles[z][y][x];
                Thing *th = gm_tile_find_creature(t, cid, NULL);
                if (th)
                    th->outfitId = looktype;
            }
}

static void read_creature_speed(NetMsg *m)
{
    uint32_t cid = nm_get_u32(m);
    uint16_t speed = nm_get_u16(m);
    if (cid == gp_map.playerId)
        gp_map.playerSpeed = speed;
}

/* ---------- dispatch ---------- */

/* Fim de mensagem com floor change: dispara o callback com a posição
 * real do central (após 0xBE/0BF + faixas 0x65..0x68). */
static void finish_floor_change(void)
{
    if (fc_cb) {
        fc_cb = 0;
        if (gp_cb.on_own_floor_change)
            gp_cb.on_own_floor_change(fc_up, gp_map.playerTileX,
                                      gp_map.playerTileY,
                                      gp_map.playerFloor);
    }
    fc_sync = 0;
}

static void handle_message(NetMsg *m)
{
    fc_sync = 0; /* estado por mensagem: faixas só deslocam com 0xBE/0xBF */
    own_from_valid = 0; /* x velho do 0x6D não atravessa mensagens */
    while (nm_remaining(m) > 0) {
        uint8_t op = nm_get_u8(m);
        switch (op) {
        case 0x0A: read_own_login(m); break;
        case 0x0B: nm_skip(m, 32); break;
        case 0x14: { /* erro de login de jogo (ou mensagem antes do 0x0A) */
            char text[GP_CHAT_LEN];
            nm_get_string(m, text, sizeof(text));
            if (!gp_logged_in) {
                snprintf(gp_err, sizeof(gp_err), "%s", text);
                gp_chat_push(text);
            } else {
                gp_chat_push(text);
            }
            break;
        }
        case 0x1E: /* keep-alive do servidor */
            gp_send_ping();
            break;
        case 0x64: read_map_description(m); break;
        case 0x65: case 0x66: case 0x67: case 0x68:
            read_partial_row(m, op);
            break;
        case 0x69: read_update_tile(m); break;
        case 0x6A: read_add_thing(m); break;
        case 0x6B: read_transform_thing(m); break;
        case 0x6C: read_remove_thing(m); break;
        case 0x6D: read_move_creature(m); break;
        case 0x6E: read_open_container(m); break;
        case 0x6F: read_close_container(m); break;
        case 0x70: read_container_item(m, 0); break;
        case 0x71: read_container_item(m, 1); break;
        case 0x72: read_remove_container_item(m); break;
        case 0x78: read_inventory(m, 0); break;
        case 0x79: read_inventory(m, 1); break;
        case 0x82: /* luz do mundo */ nm_skip(m, 2); break;
        case 0x83: /* efeito mágico: pos + tipo */ nm_skip(m, 6); break;
        case 0x84: /* texto animado */ {
            nm_skip(m, 5);
            nm_get_u8(m);  /* cor */
            uint16_t n = nm_get_u16(m);
            nm_skip(m, (int)n);
            break;
        }
        case 0x85: /* distance shoot: from + to + tipo */ nm_skip(m, 11); break;
        case 0x8C: read_creature_health(m); break;
        case 0x8D: /* luz de criatura */ nm_skip(m, 6); break;
        case 0x8E: read_creature_outfit(m); break;
        case 0x8F: read_creature_speed(m); break;
        case 0x90: /* skull */ nm_skip(m, 5); break;
        case 0x91: /* party shield */ nm_skip(m, 5); break;
        case 0x96: /* text window: u32 id, item, [maxlen u16], str texto,
                     str escritor */ {
            nm_skip(m, 4); /* window id */
            uint16_t id = nm_get_u16(m);
            const DatThingInfo *d = dat_get_item(id);
            if (d && (d->flags & DATF_STACKABLE))
                nm_skip(m, 1);
            nm_skip(m, 2); /* maxlen */
            uint16_t n = nm_get_u16(m);
            nm_skip(m, (int)n);
            uint16_t nw = nm_get_u16(m);
            nm_skip(m, (int)nw);
            break;
        }
        case 0xA0: read_stats(m); break;
        case 0xA1: nm_skip(m, 14); break; /* skills */
        case 0xA2: nm_skip(m, 1); break; /* icons */
        case 0xAA: read_creature_speak(m); break;
        case 0xB4: read_text_message(m); break;
        case 0xB5: /* cancel walk �?" reconciliar p/ posição autoritativa */
            nm_get_u8(m); /* direção */
#ifdef GP_DEBUG_MSG
            /* printf de caminho quente (andar contra parede = 1 0xB5
             * por passo segurado no D-pad) */
            printf("gp: 0xB5 cancel walk (pred_n=%d)\n", pred_n);
#endif
            pred_revert_all();
            if (gp_cb.on_own_move_rejected)
                gp_cb.on_own_move_rejected();
            break;
        case 0xBE: read_floor_change_up(m); break;
        case 0xBF: read_floor_change_down(m); break;
        case 0xC8: /* outfit window */ {
            nm_skip(m, 6);
            nm_skip(m, 4);
            break;
        }
        case 0xD2: { /* VIP*/
            nm_skip(m, 4);
            uint16_t n = nm_get_u16(m);
            nm_skip(m, (int)n);
            nm_get_u8(m);
            break;
        }
        case 0xD3: case 0xD4: nm_skip(m, 4); break;
        default:
            printf("gp: opcode desconhecido 0x%02X (restam %d bytes)\n",
                   op, nm_remaining(m));
            nm_skip(m, nm_remaining(m));
            break;
        }
    }
    finish_floor_change();
}

/* ---------- API ---------- */

void gp_set_callbacks(const GpCallbacks *cb)
{
    if (cb)
        gp_cb = *cb;
    else
        memset(&gp_cb, 0, sizeof(gp_cb));
}

const char *gp_error(void)
{
    return gp_err;
}

int gp_is_connected(void)
{
    return gp_fd >= 0;
}

static int send_game_first_packet(const char *account, const char *password,
                                    const char *charName)
{
    tibia_random_seed();
    tibia_random_bytes((uint8_t *)gp_key.k, 16);
    return tibia_send_first_packet(gp_fd, 772, 2, &gp_key, account,
                                   password, charName);
}

int gp_enter_game(const char *account, const char *password,
                  const char *charName, const char *loginHost,
                  uint16_t loginPort, const char *gameHost, uint16_t gamePort)
{
    CharacterListing listing;

    gp_err[0] = '\0';
    gp_logged_in = 0;
    gp_known_count = 0;
    fc_sync = 0;
    fc_cb = 0;

    (void)loginHost;
    (void)loginPort;

    /* 1) login server (Fase 2) �?" a lista de chars define o game server */
    tibia_login_set_server(loginHost, loginPort);
    if (tibia_login(account, password, &listing)) {
        printf("gp: conta ok, %d personagem(ns):\n", listing.count);
        for (int i = 0; i < listing.count; i++) {
            uint32_t ip = listing.chars[i].ip; /* network order */
            printf("gp:   [%d] %s (%s) %u.%u.%u.%u:%u\n", i,
                   listing.chars[i].name, listing.chars[i].world,
                   (unsigned)((ip >> 24) & 0xFF),
                   (unsigned)((ip >> 16) & 0xFF),
                   (unsigned)((ip >> 8) & 0xFF),
                   (unsigned)(ip & 0xFF),
                   (unsigned)listing.chars[i].port);
        }
    } else {
        snprintf(gp_err, sizeof(gp_err), "login: %.100s", listing.error);
        return 0;
    }

    /* seleciona o char pedido */
    CharacterInfo *chosen = NULL;
    for (int i = 0; i < listing.count; i++) {
        if (strcmp(listing.chars[i].name, charName) == 0) {
            chosen = &listing.chars[i];
            break;
        }
    }
    if (!chosen) {
        snprintf(gp_err, sizeof(gp_err), "personagem '%s' nao está na lista",
                 charName);
        return 0;
    }

    /* gameHost/gamePort explícitos têm prioridade (facilita testes);
     * senão usa o ip/porta da lista (conversão host order -> dotted) */
    char ipstr[16];
    const char *host = gameHost;
    uint16_t port = gamePort ? gamePort : chosen->port;
    if (!host || !host[0]) {
        uint32_t ip = chosen->ip; /* network order */
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                 (unsigned)((ip >> 24) & 0xFF),
                 (unsigned)((ip >> 16) & 0xFF),
                 (unsigned)((ip >> 8) & 0xFF),
                 (unsigned)(ip & 0xFF));
        host = ipstr;
    }

    /* 2) game server */
    gp_fd = net_connect(host, port, 8000);
    if (gp_fd < 0) {
        snprintf(gp_err, sizeof(gp_err), "connect ao game server %s:%u falhou",
                 host, port);
        return 0;
    }

    if (send_game_first_packet(account, password, chosen->name) != 0) {
        snprintf(gp_err, sizeof(gp_err), "envio do handshake do jogo falhou");
        net_close(gp_fd);
        gp_fd = -1;
        return 0;
    }

    /* espera o 0x0A + mapdesc (com tolerância a mensagens vazias) */
    for (int tries = 0; tries < 100 && !gp_logged_in; tries++) {
        NetMsg m;
        int r = nm_recv(gp_fd, &m, &gp_key, 5000);
        if (r == -2) {
            snprintf(gp_err, sizeof(gp_err), "timeout esperando o jogo entrar");
            net_close(gp_fd);
            gp_fd = -1;
            return 0;
        }
        if (r == -1) {
            snprintf(gp_err, sizeof(gp_err), "conexão caiu no login do jogo");
            net_close(gp_fd);
            gp_fd = -1;
            return 0;
        }
        if (r == -3)
            continue; /* mensagem vazia (o servidor manda ao conectar) */
        handle_message(&m);
        if (gp_err[0] && !gp_logged_in) {
            net_close(gp_fd);
            gp_fd = -1;
            return 0;
        }
    }

    if (!gp_logged_in) {
        snprintf(gp_err, sizeof(gp_err), "login do jogo não completou");
        net_close(gp_fd);
        gp_fd = -1;
        return 0;
    }

    return 1;
}

void gp_disconnect(void)
{
    if (gp_fd >= 0) {
        net_close(gp_fd);
        gp_fd = -1;
    }
    gp_logged_in = 0;
    fc_sync = 0;
    fc_cb = 0;
}

int gp_poll(int timeout_ms)
{
    NetMsg m;
    int processed = 0;

    if (gp_fd < 0) {
        net_yield();
        return -1;
    }

    for (;;) {
        /* janela deslizante: cada mensagem processada renova o timeout
         * (mensagens subsequentes encadeadas não se perdem); timeout 0 =
         * drena só o que já chegou (loop principal da PS2). */
        int r = nm_recv(gp_fd, &m, &gp_key, timeout_ms);
        if (r == -3) {
            if (timeout_ms <= 0)
                break;
            continue; /* mensagem vazia: espera de novo */
        }
        if (r == -2)
            break; /* timeout: normal no polling */
        if (r == -1) {
            if (gp_cb.on_disconnect)
                gp_cb.on_disconnect();
            gp_disconnect();
            return processed > 0 ? processed : -1;
        }
#ifdef GP_DEBUG_MSG
        printf("gp: msg: op=0x%02X inner=%d\n", m.buf[4], m.len - 2);
#endif
        handle_message(&m);
        processed++;
    }

    net_yield();
    return processed;
}

/* ---------- envio + predição (API pública) ---------- */

int gp_move_pending(void)
{
    return pred_n >= PRED_MAX;
}

/* Timeout da fila de predições baseado em FRAMES (chamado 1x por frame do
 * loop principal). Ao expirar, reverte a predição para que uma confirmação
 * 0x6D atrasada seja aplicada como movimento não-predito. */
void gp_move_tick(void)
{
    if (pred_n && ++pred_age >= PRED_TIMEOUT_FRAMES) {
#ifdef GP_DEBUG_MSG
        printf("gp: pred timeout, revertendo\n");
#endif
        pred_revert_all();
    }
}

int gp_own_dir(void)
{
    Tile *t = find_own_tile();
    if (!t)
        return 2;
    Thing *th = gm_tile_find_creature(t, gp_map.playerId, NULL);
    return th ? (th->dir & 3) : 2;
}

/* Destino localmente bloqueado: sem dados de janela, sem chão (tile
 * preto/vazio), item não-andável (porta fechada, parede) ou criatura
 * em cima. Passo assim quase certamente vira 0xB5 — não vale prever. */
static int tile_blocked_local(int wx, int wy, int wz)
{
    Tile *t = tile_at_world(wx, wy, wz);
    if (!t)
        return 1; /* fora da janela / nunca descrito */
    if (t->groundItemId == 0)
        return 1;
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].isCreature)
            return 1;
        const DatThingInfo *d = dat_get_item(t->things[i].itemId);
        if (d && (d->flags & DATF_NOTWALKABLE))
            return 1;
    }
    return 0;
}

int gp_walk_optimistic(int dir)
{
    if (dir < 0 || dir > 7)
        return 0;
    if (pred_n >= PRED_MAX)
        return 0; /* fila cheia; aguardando confirmação do servidor */

    int dx = walk_delta[dir][0];
    int dy = walk_delta[dir][1];
    int fx = gp_map.playerTileX, fy = gp_map.playerTileY, fz = gp_map.playerFloor;
    int tx = fx + dx, ty = fy + dy, tz = fz;

    /* Tile obviamente bloqueado: envia SEM prever (o servidor é
     * autoritativo — dados locais podem estar desatualizados). Se ele
     * recusar, o 0xB5 não tem predição para reverter; se aceitar, o
     * 0x6D aplica o passo pelo caminho normal. Retorna 1 para o caller
     * manter a animação do passo: ela é o rate-limit do envio e o
     * "bounce" visual quando o passo é negado (retornar 0 faria o
     * caller reenviar o walk a cada frame). */
    if (tile_blocked_local(tx, ty, tz)) {
        gp_send_walk(dir);
        return 1;
    }

    if (pred_n == 0)
        pred_age = 0;
    PredStep *s = pred_step(pred_n);
    s->from[0] = fx; s->from[1] = fy; s->from[2] = fz;
    s->to[0] = tx; s->to[1] = ty; s->to[2] = tz;
    s->saved = pred_save_buf[(pred_head + pred_n) % PRED_MAX];

    /* aplica localmente (janela + central + thing) */
    pred_save_dropped(s, dx, dy); /* ANTES do gm_scroll: captura a faixa que ele descarta */
    gm_scroll(&gp_map, dx, dy);
    set_central(tx, ty, tz);
    move_own_thing(fx, fy, fz, tx, ty, tz);
    {
        Tile *t = find_own_tile();
        if (t) {
            Thing *th = gm_tile_find_creature(t, gp_map.playerId, NULL);
            if (th)
                th->dir = (uint8_t)(dir & 3);
        }
    }
    pred_n++;

    gp_send_walk(dir);
    return 1;
}

int gp_step_delay_ms(int dir)
{
    if (dir < 0 || dir > 7)
        return -1;
    int dx = walk_delta[dir][0];
    int dy = walk_delta[dir][1];
    Tile *t = tile_at_world(gp_map.playerTileX + dx,
                             gp_map.playerTileY + dy, gp_map.playerFloor);
    uint16_t speed = gp_map.playerSpeed ? gp_map.playerSpeed : 220;
    if (t && t->groundItemId) {
        const DatThingInfo *d = dat_get_item(t->groundItemId);
        if (d && (d->flags & DATF_GROUND) && d->groundSpeed > 0) {
            uint32_t ms = (uint32_t)d->groundSpeed * 1000u /
                          (uint32_t)speed;
            return (int)((ms * 105u + 99u) / 100u);
        }
    }
    {
        uint32_t ms = 150u * 1000u / (uint32_t)speed;
        return (int)((ms * 105u + 99u) / 100u);
    }
}

static void send_simple(uint8_t op)
{
    if (gp_fd < 0)
        return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, op);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_ping(void)  { send_simple(0x1E); }
void gp_send_logout(void){ send_simple(0x14); }
void gp_send_attack(uint32_t creatureId)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0xA1);
    nm_add_u32(&m, creatureId);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_walk(int dir)
{
    /* 0x65 N, 0x66 E, 0x67 S, 0x68 W, 0x6A NE, 0x6B SE, 0x6C SW, 0x6D NW */
    static const uint8_t ops[8] = {0x65, 0x66, 0x67, 0x68, 0x6A, 0x6B, 0x6C, 0x6D};
    if (dir < 0 || dir > 7)
        return;
    send_simple(ops[dir]);
}

void gp_send_turn(int dir)
{
    static const uint8_t ops[4] = {0x6F, 0x70, 0x71, 0x72};
    if (dir < 0 || dir > 3)
        return;
    send_simple(ops[dir]);
}

void gp_send_look_at(uint16_t x, uint16_t y, uint8_t z,
                      uint16_t spriteId, uint8_t stackpos)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x8C);
    nm_add_position(&m, x, y, z);
    nm_add_u16(&m, spriteId);
    nm_add_u8(&m, stackpos);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_use(uint16_t x, uint16_t y, uint8_t z,
                  uint16_t spriteId, uint8_t stackpos, uint8_t index)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x82);
    nm_add_position(&m, x, y, z);
    nm_add_u16(&m, spriteId);
    nm_add_u8(&m, stackpos);
    nm_add_u8(&m, index);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_use_with(uint16_t fx, uint16_t fy, uint8_t fz, uint16_t fSprite,
                       uint8_t fStack, uint16_t tx, uint16_t ty, uint8_t tz,
                       uint16_t tSprite, uint8_t tStack)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x83);
    nm_add_position(&m, fx, fy, fz);
    nm_add_u16(&m, fSprite);
    nm_add_u8(&m, fStack);
    nm_add_position(&m, tx, ty, tz);
    nm_add_u16(&m, tSprite);
    nm_add_u8(&m, tStack);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_say(uint8_t type, const char *text)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x96);
    nm_add_u8(&m, type); /* 1 = say */
    nm_add_string(&m, text);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_container_close(uint8_t cid)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x87);
    nm_add_u8(&m, cid);
    nm_send(gp_fd, &m, &gp_key);
}

void gp_send_container_up(uint8_t cid)
{
    if (gp_fd < 0) return;
    NetMsg m;
    nm_init(&m);
    nm_add_u8(&m, 0x88);
    nm_add_u8(&m, cid);
    nm_send(gp_fd, &m, &gp_key);
}

/* ---------- diagnóstico (tecla de debug: chamar quando a tela preta
 * aparecer) ---------- */

/* Decisivo p/ separar "dado não chegou" de "renderer não desenhou":
 * grid cheio de # com tela preta => suspeitar do cache de sprites;
 * grid com . onde está preto => dado realmente vazio (parse/rede). */
void gp_debug_dump_window(void)
{
    int pz = gp_map.playerFloor, empty = 0, maxth = 0;
    printf("gp: dump floor %d central=%d,%d\n", pz,
           gp_map.playerTileX, gp_map.playerTileY);
    for (int y = 0; y < NET_MAP_H; y++) {
        printf("  ");
        for (int x = 0; x < NET_MAP_W; x++) {
            Tile *t = &gp_map.tiles[pz][y][x];
            if (!t->groundItemId)
                empty++;
            if (t->thingCount > maxth)
                maxth = t->thingCount;
            putchar(t->groundItemId ? '#' : '.');
        }
        putchar('\n');
    }
    printf("gp: empty=%d maxThingCount=%d\n", empty, maxth);
}
