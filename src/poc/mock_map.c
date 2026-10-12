#include "mock_map.h"

/*
 * 32x32 mock map with variety:
 * - Border walls (sprite 576)
 * - Stone ground (sprite 98) with scattered variations
 * - Water pools (sprite 106 from .dat analysis: blue)
 * - Gold coins and a Deathslicer
 */
void build_mock_map(MockMap *map) {
    /* Fill all tiles with stone ground */
    for (int y = 0; y < MOCK_MAP_H; ++y) {
        for (int x = 0; x < MOCK_MAP_W; ++x) {
            map->tiles[y][x].groundSpriteId = 98;
            map->tiles[y][x].thingCount = 0;
        }
    }

    /* Border walls around the entire map */
    for (int i = 0; i < MOCK_MAP_W; ++i) {
        map->tiles[0][i].groundSpriteId = 576;
        map->tiles[MOCK_MAP_H-1][i].groundSpriteId = 576;
    }
    for (int i = 0; i < MOCK_MAP_H; ++i) {
        map->tiles[i][0].groundSpriteId = 576;
        map->tiles[i][MOCK_MAP_W-1].groundSpriteId = 576;
    }

    /* Inner wall structures (rooms) */
    for (int x = 8; x < 16; x++) {
        map->tiles[8][x].groundSpriteId = 576;
        map->tiles[20][x].groundSpriteId = 576;
    }
    for (int y = 8; y < 20; y++) {
        map->tiles[y][8].groundSpriteId = 576;
        map->tiles[y][16].groundSpriteId = 576;
    }
    /* Door gap */
    map->tiles[14][8].groundSpriteId = 98;
    map->tiles[8][12].groundSpriteId = 98;

    /* Water pool (top-right area) */
    for (int y = 3; y < 7; y++) {
        for (int x = 24; x < 29; x++) {
            map->tiles[y][x].groundSpriteId = 106; /* water blue */
        }
    }

    /* Second room walls (bottom-right) */
    for (int x = 22; x < 28; x++) map->tiles[22][x].groundSpriteId = 576;
    for (int y = 22; y < 28; y++) {
        map->tiles[y][22].groundSpriteId = 576;
        map->tiles[y][28].groundSpriteId = 576;
    }
    map->tiles[25][22].groundSpriteId = 98; /* door */

    /* Gold coins scattered */
    static const int coin_positions[][2] = {
        {12, 12}, {13, 14}, {5, 25}, {25, 12}, {15, 5}, {24, 24}, {10, 18}
    };
    for (int i = 0; i < 7; i++) {
        int cx = coin_positions[i][0], cy = coin_positions[i][1];
        if (map->tiles[cy][cx].groundSpriteId != 576) {
            map->tiles[cy][cx].thingCount = 1;
            map->tiles[cy][cx].things[0].spriteId = 1053;
            map->tiles[cy][cx].things[0].outfitId = 0;
            map->tiles[cy][cx].things[0].isCreature = 0;
            map->tiles[cy][cx].things[0].isOnTop = 0;
        }
    }

    /* Deathslicer monsters */
    static const int monster_positions[][2] = {
        {12, 14}, {5, 5}, {25, 15}
    };
    for (int i = 0; i < 3; i++) {
        int mx = monster_positions[i][0], my = monster_positions[i][1];
        if (map->tiles[my][mx].groundSpriteId != 576) {
            map->tiles[my][mx].thingCount = 1;
            map->tiles[my][mx].things[0].spriteId = 0;
            map->tiles[my][mx].things[0].outfitId = 3;
            map->tiles[my][mx].things[0].isCreature = 1;
            map->tiles[my][mx].things[0].isOnTop = 0;
        }
    }

    /* Player start position */
    map->playerTileX = MOCK_MAP_W / 2;
    map->playerTileY = MOCK_MAP_H / 2;
}
