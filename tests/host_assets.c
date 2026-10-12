/**
 * host_assets.c — fornece _binary_Tibia_dat_start para os testes de host
 * (na PS2 o símbolo vem do .incbin; aqui carregamos do arquivo).
 */
#include <stdio.h>
#include <string.h>

unsigned char _binary_Tibia_dat_start[256 * 1024];

int host_load_dat(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(_binary_Tibia_dat_start, 1, 256 * 1024 - 16, f);
    fclose(f);
    return (int)n;
}
