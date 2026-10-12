#include <stdint.h>

typedef struct {
    uint16_t spriteId;   /* for static items: direct sprite ID from .spr */
    uint16_t outfitId;   /* for creatures: outfit ID from .dat (animated) */
    uint8_t  isCreature;
    uint8_t  isOnTop;
} Thing;

typedef struct {
    uint16_t groundSpriteId;
    uint8_t  thingCount;
    Thing    things[8];
    uint8_t  elevation;
} Tile;

/* Big map for camera testing */
#define MOCK_MAP_W 32
#define MOCK_MAP_H 32

typedef struct {
    Tile tiles[MOCK_MAP_H][MOCK_MAP_W];
    int  playerTileX;
    int  playerTileY;
} MockMap;

void build_mock_map(MockMap *map);
