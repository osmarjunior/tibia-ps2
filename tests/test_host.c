/**
 * test_host.c — teste de host das Fases 2/3/4 do TASKS2.md.
 *
 * Requisitos: mock server (tools/mock_tibia_server.py) rodando em 127.0.0.1.
 *
 * Valida:
 *  Fase 2: login RSA+XTEA -> lista de personagens decodificada.
 *  Fase 3: game login -> MapDescription parseado para GameMap
 *          (chão + criaturas + itens), criatura movendo, item update.
 *  Fase 4: walk com predição + confirmação do servidor + rejeição (0xB5)
 *          com reconciliação.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/net/net_stream.h"
#include "../src/net/login.h"
#include "../src/net/game_protocol.h"
#include "../src/poc/dat_loader.h"

/* from posix_net.c */
void posix_net_install(void);
/* from host_assets.c */
int host_load_dat(const char *path);

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            printf("PASS: %s\n", msg);                                       \
        } else {                                                             \
            printf("FAIL: %s\n", msg);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* predição local (espelha o que client_net.c faz na PS2) */
static int pred_x, pred_y, pred_z;
static int pred_pending = 0;
static int rejected_count = 0;
static int reset_count = 0;
static int floor_change_count = 0;

static void cb_own_move_confirmed(int fx, int fy, int fz, int tx, int ty, int tz)
{
    (void)fx; (void)fy; (void)fz;
    pred_x = tx;
    pred_y = ty;
    pred_z = tz;
    pred_pending = 0;
}

static void cb_own_move_rejected(void)
{
    pred_pending = 0;
    rejected_count++;
    /* reconciliação: volta para a posição autoritativa */
    pred_x = gp_map.playerTileX;
    pred_y = gp_map.playerTileY;
    pred_z = gp_map.playerFloor;
}

static void cb_own_position_reset(int x, int y, int z)
{
    pred_x = x;
    pred_y = y;
    pred_z = z;
    pred_pending = 0;
    reset_count++;
}

static void cb_own_floor_change(int up, int nx, int ny, int nz)
{
    (void)up;
    pred_x = nx;
    pred_y = ny;
    pred_z = nz;
    pred_pending = 0;
    floor_change_count++;
}

static void cb_chat(uint8_t type, const char *name, const char *text)
{
    (void)type;
    if (name[0])
        printf("chat> %s: %s\n", name, text);
    else
        printf("chat> %s\n", text);
}

/* varre o mapa procurando um tile com ground id == gid perto do player */
static int count_ground(int gid)
{
    int n = 0;
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++)
                if (gp_map.tiles[z][y][x].groundItemId == gid)
                    n++;
    return n;
}

static int count_ground_on_floor(int gid, int floor)
{
    int n = 0;
    if (floor < 0 || floor >= NET_MAP_FLOORS)
        return 0;
    for (int y = 0; y < NET_MAP_H; y++)
        for (int x = 0; x < NET_MAP_W; x++)
            if (gp_map.tiles[floor][y][x].groundItemId == gid)
                n++;
    return n;
}

static int find_creature(uint32_t id, int *out_z)
{
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                Tile *t = &gp_map.tiles[z][y][x];
                if (gm_tile_find_creature(t, id, NULL)) {
                    if (out_z)
                        *out_z = z;
                    return 1;
                }
            }
    return 0;
}

static int count_item(uint16_t iid)
{
    int n = 0;
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                Tile *t = &gp_map.tiles[z][y][x];
                if (t->groundItemId == iid)
                    n++;
                for (int i = 0; i < t->thingCount; i++)
                    if (!t->things[i].isCreature && t->things[i].itemId == iid)
                        n++;
            }
    return n;
}

/* "anda" como o cliente PS2: passo otimista via game_protocol + espera
 * a confirmação em fatias curtas */
static void walk_step(int dir, int dx, int dy)
{
    int waited = 0;
    while (gp_move_pending() && waited < 700) {
        gp_poll(50);
        waited += 50;
    }
    if (!gp_walk_optimistic(dir))
        return;
    pred_x += dx;
    pred_y += dy;
    waited = 0;
    while (gp_move_pending() && waited < 700) {
        gp_poll(50);
        waited += 50;
    }
}

int main(int argc, char **argv)
{
    printf("Argv: %d\n", argc);
    printf("Argv[0]: %s\n", argv[0]);
    const char *dat_path = argc > 1 ? argv[1] : "Tibia.dat";
    const char *host = argc > 2 ? argv[2] : "192.168.100.7";
    uint16_t login_port = argc > 3 ? (uint16_t)atoi(argv[3]) : 7172;
    uint16_t game_port = argc > 4 ? (uint16_t)atoi(argv[4]) : 7173;

    setvbuf(stdout, NULL, _IONBF, 0);
    posix_net_install();

    if (host_load_dat(dat_path) <= 0) {
        printf("FAIL: nao abriu %s\n", dat_path);
        return 1;
    }
    dat_loader_init();
    CHECK(dat_get_item(5089) != NULL, "dat: limite superior de itens 7.60 (5089)");
    CHECK(dat_get_item(3031) != NULL, "dat: gold coin (3031) presente");
    CHECK(dat_item_is_stackable(3031), "dat: 3031 stackable");
    CHECK(dat_get_item(103) != NULL &&
          (dat_get_item(103)->flags & DATF_GROUND),
          "dat: 103 é ground");
    {
        const DatThingInfo *chest = dat_get_item(2856);
        CHECK(chest && (chest->flags & DATF_CONTAINER), "dat: 2856 container");
    }
    {
        /* moeda: 4x2 patterns -> resolve sprite por count */
        uint16_t spr[4];
        int w, h;
        int n = dat_resolve_sprites(dat_get_item(3031), 12, 1000, 1000, 7,
                                     spr, &w, &h);
        CHECK(n == 1 && w == 1 && h == 1 && spr[0] != 0,
              "dat: sprite da moeda resolvido por count");
    }
    {
        /* Todos os sprites físicos de um item multi-tile devem ser resolvidos. */
        int found = 0;
        for (int id = 100; id <= 5089 && !found; id++) {
            const DatThingInfo *item = dat_get_item(id);
            if (!item || item->w * item->h <= 1 ||
                item->w * item->h > TILE_SPRITE_MAX)
                continue;
            uint16_t spr[TILE_SPRITE_MAX];
            int w, h;
            int n = dat_resolve_sprites(item, 0, 1000, 1000, 7,
                                         spr, &w, &h);
            CHECK(n == item->w * item->h && w == item->w && h == item->h,
                  "dat: todas as peças do primeiro item multi-tile resolvidas");
            found = 1;
        }
        CHECK(found, "dat: existe item multi-tile no conjunto 7.60");
    }
    {
        /* Wall 637 (parede de cave): layers=2; a figura da parede está na
         * layer 1 (dump do .dat real: layer 0 ~vazia, layer 1 = parede).
         * O renderer precisa compor TODAS as layers, senão o wall vira
         * "poeira de pixels". */
        const DatThingInfo *wall = dat_get_item(637);
        uint16_t spr[TILE_SPRITE_MAX];
        int w, h;
        CHECK(wall && wall->layers == 2,
              "dat: wall 637 tem 2 layers (parede/telhado)");
        CHECK(dat_resolve_sprites_anim(wall, 0, 1000, 1000, 7, 0, 1,
                                       spr, &w, &h) == 1 && spr[0] != 0,
              "dat: layer 1 do wall 637 resolvível (figura da parede)");
        CHECK(dat_resolve_sprites_anim(wall, 0, 1000, 1000, 7, 0, 2,
                                       spr, &w, &h) == 0,
              "dat: layer fora do range rejeitada");
    }
    {
        /* Door 1629 (2x2, layers=2): peça (0,0) do sheet é a do tile
         * âncora (bottom-right) — mapeamento do otclient. */
        const DatThingInfo *door = dat_get_item(1629);
        uint16_t spr[TILE_SPRITE_MAX];
        int w, h;
        int n = dat_resolve_sprites(door, 0, 1000, 1000, 7, spr, &w, &h);
        CHECK(door && n == 4 && w == 2 && h == 2 && spr[0] != 0,
              "dat: door 1629 resolve 4 peças 2x2 (slot 0 = âncora)");
    }

    /* ---------- Fase 2: login ---------- */
    CharacterListing listing;
    memset(&listing, 0, sizeof(listing));
    tibia_login_set_server(host, login_port);
    CHECK(tibia_login("111111", "tibia", &listing), "login RSA+XTEA ok");
    CHECK(listing.count == 1, "lista de personagens: 1 char");
    if (listing.count >= 1)
        CHECK(strcmp(listing.chars[0].name, "Ps2 Tester") == 0 &&
              strcmp(listing.chars[0].world, "MockWorld") == 0,
              "char list decodificado (nome/mundo)");
    printf("     premium days=%u ip=0x%08X port=%u\n",
           listing.premiumDays, listing.chars[0].ip, listing.chars[0].port);

    /* senha errada deve falhar */
    {
        CharacterListing bad;
        memset(&bad, 0, sizeof(bad));
        CHECK(!tibia_login("111111", "errada", &bad),
              "login com senha errada rejeitado");
        printf("     erro: %s\n", bad.error);
    }

    /* ---------- Fase 3: entrar no jogo ---------- */
    GpCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_own_move_confirmed = cb_own_move_confirmed;
    cb.on_own_move_rejected = cb_own_move_rejected;
    cb.on_own_position_reset = cb_own_position_reset;
    cb.on_own_floor_change = cb_own_floor_change;
    cb.on_chat = cb_chat;
    gp_set_callbacks(&cb);

    CHECK(gp_enter_game("111111", "tibia", "Ps2 Tester", host, login_port,
                        "127.0.0.1", game_port),
          "enter game (handshake + 0x0A + mapdesc)");
    if (!gp_is_connected()) {
        printf("     erro: %s\n", gp_error());
        return 1;
    }

    gp_poll(200);

    CHECK(gp_map.playerId == 0x10000001, "player id do 0x0A");
    CHECK(gp_map.playerTileX == 1000 && gp_map.playerTileY == 1000 &&
          gp_map.playerFloor == 7, "posição do player do mapdesc");
    CHECK(count_ground(103) > 100, "mapdesc: ~todos tiles com chão 103");
    CHECK(count_item(3031) == 2, "mapdesc: 2 pilhas de moedas");
    CHECK(count_item(2856) == 1, "mapdesc: 1 baú");
    CHECK(find_creature(0x10000001, NULL), "mapdesc: player no tile");
    CHECK(find_creature(0x40000001, NULL), "mapdesc: NPC visível");
    CHECK(gp_map.playerHealth == 100 && gp_map.playerMaxHealth == 100 &&
          gp_map.playerMana == 40, "stats 0xA0 decodificados");
    {
        int z;
        CHECK(find_creature(0x10000001, &z) && z == 7, "player no floor 7");
    }
    {
        /* telhado no floor 6 sobre o spawn (Fase 5) */
        int roofs = 0;
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++)
                if (gp_map.tiles[6][y][x].groundItemId == 637)
                    roofs++;
        CHECK(roofs == 25, "mapdesc: telhado 5x5 no floor 6");
    }

    /* ---------- Fase 4: andar com predição ---------- */
    pred_x = gp_map.playerTileX;
    pred_y = gp_map.playerTileY;
    pred_z = gp_map.playerFloor;

    /* passo rápido demais: o primeiro passo é aceito, o segundo (imediatamente
     * em seguida, sem esperar STEP_MS) deve ser rejeitado com 0xB5 */
    walk_step(1, 1, 0); /* leste: (1001,1000) */
    CHECK(gp_map.playerTileX == 1001,
          "passo 1 confirmado pelo servidor (1001)");
        CHECK(count_ground_on_floor(637, 6) == 25,
            "linha parcial atualiza também o roof do floor 6");
    CHECK(pred_x == 1001 && pred_y == 1000, "predição reconciliada com servidor");

    /* regressão do bug da faixa de tiles pretos: predição rejeitada
     * (0xB5) precisa restaurar a faixa que o gm_scroll otimista
     * descartou. O servidor não reenvia esses tiles (para ele o player
     * nunca saiu), então sem o restore a coluna oeste ficava vazia e o
     * count de grounds do floor 7 caía para 238 (18*14 - 14). */
    CHECK(gp_walk_optimistic(1), "passo otimista imediato enfileirado");
    {
        int was_rejected = rejected_count;
        int waited = 0;
        while (rejected_count == was_rejected && waited < 700) {
            gp_poll(50);
            waited += 50;
        }
        CHECK(rejected_count > was_rejected,
              "passo otimista rápido rejeitado (0xB5 com pred_n=1)");
        CHECK(gp_map.playerTileX == 1001,
              "posição central de volta à origem pós-0xB5");
        CHECK(find_creature(0x10000001, NULL),
              "criatura do player de volta ao tile pós-0xB5");
        CHECK(count_ground_on_floor(103, 7) == NET_MAP_W * NET_MAP_H,
              "0xB5 restaurou a faixa descartada (sem tiles pretos)");
    }

    /* passo imediato: deve ser rejeitado */
    pred_x += 1; pred_y += 0; pred_pending = 1;
    gp_send_walk(1);
    gp_poll(600);
    CHECK(rejected_count >= 1, "passo rápido rejeitado (0xB5 recebido)");
    CHECK(pred_x == gp_map.playerTileX && pred_y == gp_map.playerTileY,
          "reconciliação pós-rejeição");

    /* espera o delay do servidor e anda de novo */
    usleep(400 * 1000);
    walk_step(3, -1, 0); /* oeste de volta */
    CHECK(gp_map.playerTileX == 1000, "passo após delay confirmado (1000)");

    usleep(400 * 1000);
    walk_step(0, 0, -1); /* norte */
    CHECK(gp_map.playerTileY == 999, "passo norte confirmado");

    usleep(400 * 1000);
    walk_step(2, 0, 1); /* sul de volta */
    CHECK(gp_map.playerTileY == 1000, "passo sul confirmado");

    /* ---------- Ações (Fase 7 sem UI): look/use/say/attack ---------- */
    usleep(400 * 1000);
    gp_send_look_at(1000, 1001, 7, 57, 1); /* NPC tile (1001, 999)? usa stackpos 1 */
    gp_poll(600);
    CHECK(gp_chat.count > 0, "resposta de look no chat");

    gp_send_say(1, "hello from ps2 client");
    gp_poll(600);
    {
        int found = 0;
        for (int i = 0; i < gp_chat.count; i++)
            if (strstr(gp_chat_line(i), "hello from ps2 client"))
                found = 1;
        CHECK(found, "eco do say (0xAA) no chat");
    }

    /* usar o baú abre container (0x6E) */
    usleep(400 * 1000);
    gp_send_use(997, 997, 7, 2856, 2, 0);
    gp_poll(600);
    CHECK(gp_containers[0].active, "container (0x6E) aberto");
    CHECK(gp_containers[0].count == 2 &&
          gp_containers[0].items[0] == 3031,
          "container: 2 moedas dentro");

    /* porta abre (0x69 update tile) */
    usleep(400 * 1000);
    gp_send_use(1000, 996, 7, 1629, 2, 0);
    gp_poll(600);
    CHECK(count_item(1633) == 1, "porta aberta via 0x69 (item 1633 no tile)");

    /* attack */
    usleep(400 * 1000);
    gp_send_attack(0x40000001);
    gp_poll(600);
    {
        int found = 0;
        for (int i = 0; i < gp_chat.count; i++)
            if (strstr(gp_chat_line(i), "attack"))
                found = 1;
        CHECK(found, "resposta do attack no chat");
    }

    /* ---------- Fase 4/5: buraco (teleport p/ baixo, mapdesc completo) ---- */
    usleep(400 * 1000);
    walk_step(1, 1, 0); /* leste -> 1001 */
    usleep(400 * 1000);
    walk_step(1, 1, 0); /* leste -> 1002 */
    usleep(400 * 1000);
    walk_step(1, 1, 0); /* leste -> 1003 (buraco) */
    gp_poll(800);
    CHECK(gp_map.playerFloor == 8 && gp_map.playerTileX == 1003,
          "caiu no buraco: floor 8 via mapdesc");
    CHECK(count_ground(410) > 50, "caverna (chão 410) descrita no floor 8");

    /* escada: sobe com 0xBE */
    usleep(400 * 1000);
    walk_step(1, 1, 0); /* leste -> 1004 */
    usleep(400 * 1000);
    walk_step(1, 1, 0); /* leste -> 1005 */
    usleep(400 * 1000);
    walk_step(3, -1, 0); /* oeste -> 1004... a escada está em (1002,1000) */
    usleep(400 * 1000);
    walk_step(3, -1, 0); /* oeste -> 1003 */
    usleep(400 * 1000);
    walk_step(3, -1, 0); /* oeste -> 1002: escada! */
    gp_poll(800);
    CHECK(floor_change_count == 1 && gp_map.playerFloor == 7,
          "subiu pela escada (0xBE) -> floor 7");
    CHECK(gp_map.playerTileX == 1003 && gp_map.playerTileY == 1001,
          "escalada move na diagonal (+1,+1)");

    gp_send_logout();
    gp_disconnect();

    printf("\n%d falhas.\n", failures);
    return failures == 0 ? 0 : 1;
}
