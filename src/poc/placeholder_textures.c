#include <stdint.h>

// Simple solid-color texture generator for placeholder sprites
void generate_placeholder_texture(uint8_t spriteId, uint8_t *outBuffer, int size) {
    uint8_t r=0,g=0,b=0,a=0xFF;
    switch (spriteId) {
        case 1: r=0x00; g=0xFF; b=0x00; break; // green
        case 2: r=0x80; g=0x80; b=0x80; break; // gray
        case 3: r=0xFF; g=0xFF; b=0x00; break; // yellow
        case 4: r=0xFF; g=0x00; b=0x00; break; // red
        default: r=0x00; g=0x00; b=0xFF; break; // blue default
    }
    for (int i=0;i<size*size;i++) {
        outBuffer[i*4+0]=r;
        outBuffer[i*4+1]=g;
        outBuffer[i*4+2]=b;
        outBuffer[i*4+3]=a;
    }
}
