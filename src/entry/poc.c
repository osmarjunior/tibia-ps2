/**
 * PS2 POC — Tibia 7.4 client with offscreen render target.
 *
 * Phase 1: camera in integer pixels (compute once per frame).
 * Phase 2: map rendered to offscreen VRAM RT, presented as single texture.
 *
 * USE_MAP_RT=1: draw map to RT, present as texture (Método B)
 * USE_MAP_RT=0: draw map directly to back buffer (fallback/comparison)
 */
#include <kernel.h>
#include <loadfile.h>
#include <malloc.h>
#include <sifrpc.h>
#include <string.h>
#include <stdio.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <tamtypes.h>
#include <libpad.h>

#include "../poc/mock_map.h"
#include "../poc/spr_loader.h"
#include "../poc/dat_loader.h"
#include "../poc/map_rt.h"

#define USE_MAP_RT 1

#define SCREEN_W 640
#define SCREEN_H 448
#define TILE_SIZE 32

/* Viewport rectangle (pixels) comes from map_rt.h (VIEW_X/Y/W/H).
 * The viewport is NOT centered anymore: the HUD column (battle list
 * and equipment windows, hud.c) took the right side of the screen
 * (x 528..640). Validate only that the viewport fits the screen. */
#if VIEW_X < 0 || VIEW_Y < 0 || VIEW_X + VIEW_W > SCREEN_W || VIEW_Y + VIEW_H > SCREEN_H
#error "VIEW_* constants out of sync with SCREEN_*"
#endif
/* Center of the viewport must be the center of a tile. */
#if (VIEW_W / TILE_SIZE) % 2 == 0 || (VIEW_H / TILE_SIZE) % 2 == 0
#error "VIEW_W/VIEW_H must be an odd number of tiles"
#endif

#define PLAYER_OUTFIT 29
#define NPC_ANIM_DELAY 20

#define WALK_DURATION 10
#define WALK_ANIM_DIV (WALK_DURATION / 3)

#define DIR_NORTH 0
#define DIR_EAST  1
#define DIR_SOUTH 2
#define DIR_WEST  3

GSGLOBAL *gsGlobal;

/* Player state */
static int player_x, player_y;
static int player_from_x, player_from_y;
static int player_dir = DIR_SOUTH;
static int walk_progress = 0;
static int walking = 0;
static int walk_anim_phase = 0;
static int move_cooldown = 0;

/* Camera interpolation for the current step, in whole pixels.
 * player_x/y already point at the target tile from the start of the
 * step, so the offset starts NEGATIVE: -dir * (TILE_SIZE - off). */
static int walk_off_x = 0;
static int walk_off_y = 0;

/* NPC animation */
static int npc_tick = 0;

/* Pad */
static u8 padBuf[256] __attribute__((aligned(64)));

static void pad_init(void) {
    SifLoadFileInit();
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padBuf);
    int tries = 0;
    while (padGetState(0, 0) != PAD_STATE_STABLE && tries < 200) {
        tries++;
        for (volatile int i = 0; i < 10000; i++);
    }
}

static void start_walk(int dx, int dy, int dir) {
    player_dir = dir;
    player_from_x = player_x;
    player_from_y = player_y;
    int nx = player_x + dx;
    int ny = player_y + dy;

    if (nx < 1 || nx >= MOCK_MAP_W - 1 || ny < 1 || ny >= MOCK_MAP_H - 1) {
        walking = 0;
        return;
    }

    player_x = nx;
    player_y = ny;
    walking = 1;
    walk_progress = 0;
}

static void handle_input(void) {
    struct padButtonStatus pad;
    u32 ret = padRead(0, 0, &pad);
    if (ret == 0) return;

    u16 pressed = pad.btns ^ 0xFFFF;

    if (walking || move_cooldown > 0) {
        if (move_cooldown > 0) move_cooldown--;
        return;
    }

    int dx = 0, dy = 0, dir = -1;

    if (pressed & PAD_UP)         { dy = -1; dir = DIR_NORTH; }
    else if (pressed & PAD_DOWN)   { dy =  1; dir = DIR_SOUTH; }
    else if (pressed & PAD_LEFT)   { dx = -1; dir = DIR_WEST; }
    else if (pressed & PAD_RIGHT)  { dx =  1; dir = DIR_EAST; }

    if (dir >= 0) {
        start_walk(dx, dy, dir);
    }
}

static void update_walk(void) {
    walk_off_x = 0;
    walk_off_y = 0;

    if (!walking) return;

    walk_progress++;
    if (walk_progress >= WALK_DURATION) {
        walking = 0;
        walk_progress = 0;
        move_cooldown = 2;
    } else {
        /* off goes 0 -> TILE_SIZE over the step, integer only:
         *   off = (TILE_SIZE * walk_frame) / WALK_FRAMES */
        int off = (TILE_SIZE * walk_progress) / WALK_DURATION;
        walk_off_x = -(player_x - player_from_x) * (TILE_SIZE - off);
        walk_off_y = -(player_y - player_from_y) * (TILE_SIZE - off);
    }

    walk_anim_phase = (walk_progress / WALK_ANIM_DIV) % 3;
}

/*
 * Phase 1: camera = player's center in world pixels, interpolated over
 * the step, integer arithmetic only (no float, no rounding).
 */
static void get_camera_px(int *cx, int *cy) {
    *cx = player_x * TILE_SIZE + TILE_SIZE / 2 + walk_off_x;
    *cy = player_y * TILE_SIZE + TILE_SIZE / 2 + walk_off_y;
}

/* C division truncates toward zero; the tile range needs floor. */
static inline int floor_div(int a, int b) {
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/*
 * Render the map in viewport-local coordinates: (0,0) is the top-left
 * corner of the viewport. left/top = world pixel shown at that corner.
 *
 * Visible tile range is FIXED every frame (VIEW_W/TILE + 1 columns,
 * VIEW_H/TILE + 1 rows), computed with floor_div so negative world
 * positions work. Tiles fully outside the map are skipped; tiles
 * partially outside the viewport are still drawn — the RT scissor
 * (phase 2) clips them. No culling of partially visible tiles.
 * Diagonal sweep keeps the painter's order for overlapping things.
 */
static void render_map(MockMap *map, int left, int top) {
    const int tx0 = floor_div(left, TILE_SIZE);
    const int ty0 = floor_div(top, TILE_SIZE);
    const int ox  = tx0 * TILE_SIZE - left;   /* in (-TILE_SIZE, 0] */
    const int oy  = ty0 * TILE_SIZE - top;
    const int nx  = (VIEW_W + TILE_SIZE - 1) / TILE_SIZE + 1;
    const int ny  = (VIEW_H + TILE_SIZE - 1) / TILE_SIZE + 1;

    const int npc_phase = (npc_tick / NPC_ANIM_DELAY) % 3;
    const int num_diagonals = nx + ny - 1;

    for (int diag = 0; diag < num_diagonals; diag++) {
        for (int ty = 0; ty < ny; ty++) {
            const int tx = diag - ty;
            if (tx < 0 || tx >= nx) continue;

            const int mx = tx0 + tx;
            const int my = ty0 + ty;
            if (mx < 0 || mx >= MOCK_MAP_W || my < 0 || my >= MOCK_MAP_H) continue;

            Tile *tile = &map->tiles[my][mx];
            const int sx = ox + tx * TILE_SIZE;
            const int sy = oy + ty * TILE_SIZE;

            /* Layer 1: ground */
            spr_draw(gsGlobal, tile->groundSpriteId, sx, sy, TILE_SIZE);

            /* Layer 2: items */
            for (int i = 0; i < tile->thingCount; i++) {
                if (!tile->things[i].isCreature) {
                    spr_draw(gsGlobal, tile->things[i].spriteId,
                             sx, sy, TILE_SIZE);
                }
            }

            /* Layer 3: creatures */
            for (int i = 0; i < tile->thingCount; i++) {
                if (tile->things[i].isCreature) {
                    uint16_t sprite = dat_outfit_sprite(
                        tile->things[i].outfitId, DIR_SOUTH, npc_phase);
                    if (sprite > 0)
                        spr_draw(gsGlobal, sprite, sx, sy, TILE_SIZE);
                }
            }
        }
    }
}

/*
 * Draw the player. The camera IS the player's interpolated position,
 * so the player is always at the center of the viewport — no sub-tile
 * compensation needed.
 * use_viewport_local: 1 → viewport-local coords (inside the RT),
 *                     0 → absolute screen coords (back buffer).
 */
static void render_player(int use_viewport_local) {
    int px = VIEW_W / 2 - TILE_SIZE / 2;
    int py = VIEW_H / 2 - TILE_SIZE / 2;
    if (!use_viewport_local) {
        px += VIEW_X;
        py += VIEW_Y;
    }

    int phase = walking ? walk_anim_phase : 0;
    uint16_t sprite = dat_outfit_sprite(PLAYER_OUTFIT, player_dir, phase);
    if (sprite > 0) {
        spr_draw(gsGlobal, sprite, px, py, TILE_SIZE);
    }
}

int main(void) {
    SifInitRpc(0);

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsGlobal = gsKit_init_global();
    gsGlobal->Width = SCREEN_W;
    gsGlobal->Height = SCREEN_H;
    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gsGlobal, 0, 0);
    gsGlobal->Test->ATE = 1;
    gsGlobal->Test->ATST = 6;
    gsGlobal->Test->AREF = 0;
    gsGlobal->Test->ZTE = 0;
    gsGlobal->Test->ZTST = 1;
    gsKit_set_test(gsGlobal, GS_ATEST_ON);

    gsKit_init_screen(gsGlobal);

#if USE_MAP_RT
    /* Allocate the RT BEFORE any sprites (so sprites don't overwrite it) */
    if (map_framebuffer_init(gsGlobal) != 0) {
        printf("FATAL: map_framebuffer_init failed, falling back to direct\n");
    }
#endif

    spr_loader_init();
    dat_loader_init();
    pad_init();

    MockMap map = {0};
    build_mock_map(&map);

    player_x = map.playerTileX;
    player_y = map.playerTileY;

    while (1) {
        gsKit_sync_flip(gsGlobal);

        handle_input();
        update_walk();
        npc_tick++;

        /* Phase 1: camera in integer pixels, once per frame */
        int cam_x, cam_y;
        get_camera_px(&cam_x, &cam_y);
        const int left = cam_x - VIEW_W / 2;   /* world px at viewport origin */
        const int top  = cam_y - VIEW_H / 2;

        /* Clear the back buffer */
        gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0xFF, 0x00));

#if USE_MAP_RT
        /* Phase 3: map AND player inside the RT, clipped by the same
         * viewport scissor (a partially visible entity must not leak
         * into the black margin). Coordinates are viewport-local;
         * VIEW_X/VIEW_Y only appear in the present (map_rt.c). */
        map_framebuffer_begin(gsGlobal);
        render_map(&map, left, top);
        map_framebuffer_end(gsGlobal);
        map_framebuffer_present(gsGlobal);
#else
        /* Direct rendering to back buffer: shift the map to the
         * viewport's screen position; player in absolute coords. */
        render_map(&map, left - VIEW_X, top - VIEW_Y);
#endif

        gsKit_queue_exec(gsGlobal);
    }

    return 0;
}
