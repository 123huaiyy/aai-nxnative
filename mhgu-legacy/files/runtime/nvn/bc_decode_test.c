#include "bc_decode.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    // RGB565 red and green endpoints, all pixels select red.
    const uint8_t red_bc1[8] = {0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0};
    uint8_t output[3 * 5 * 4 + 8];
    uint8_t blocks[16];
    memcpy(blocks, red_bc1, 8);
    memcpy(blocks + 8, red_bc1, 8);
    memset(output, 0xa5, sizeof output);
    if (!bc_decode(0x41, blocks, 3, 5, output)) return 1;
    for (unsigned i = 0; i < 15; i++) {
        if (output[i * 4] != 255 || output[i * 4 + 1] != 0 ||
            output[i * 4 + 2] != 0 || output[i * 4 + 3] != 255) return 2;
    }
    for (unsigned i = 60; i < sizeof output; i++) if (output[i] != 0xa5) return 3;
    if (bc_decode(0x51, blocks, 3, 5, output)) return 4;
    if (bc_decode(0x41, blocks, 0, 5, output)) return 5;
    // BC2 explicit 4-bit alpha, and BC3 interpolated alpha.
    const uint8_t bc2[16] = {
        0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
        0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0
    };
    const uint8_t bc3[16] = {
        127, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0
    };
    if (!bc_decode(0x43, bc2, 4, 4, (uint8_t[64]){0})) return 6;
    uint8_t rgba[64];
    if (!bc_decode(0x47, bc2, 4, 4, rgba)) return 7;
    for (unsigned i = 0; i < 16; i++)
        if (rgba[i * 4] != 255 || rgba[i * 4 + 3] != 136) return 8;
    if (!bc_decode(0x48, bc3, 4, 4, rgba)) return 9;
    for (unsigned i = 0; i < 16; i++)
        if (rgba[i * 4] != 255 || rgba[i * 4 + 3] != 127) return 10;
    const uint8_t bc7[16] = {0};
    memset(rgba, 0xa5, sizeof rgba);
    if (!bc_decode(0x4d, bc7, 4, 4, rgba)) return 11;
    for (unsigned i = 0; i < sizeof rgba; i++) if (rgba[i] != 0) return 12;
    const uint8_t bc4[8] = {127, 0, 0, 0, 0, 0, 0, 0};
    if (!bc_decode(0x49, bc4, 4, 4, rgba)) return 13;
    for (unsigned i = 0; i < 16; i++)
        if (rgba[i * 4] != 127 || rgba[i * 4 + 1] || rgba[i * 4 + 2] || rgba[i * 4 + 3] != 255) return 14;
    const uint8_t bc5[16] = {0x81, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0, 0, 0, 0, 0, 0, 0};
    if (!bc_decode(0x4c, bc5, 4, 4, rgba)) return 15;
    for (unsigned i = 0; i < 16; i++)
        if (rgba[i * 4] != 0x81 || rgba[i * 4 + 1] != 0x7f || rgba[i * 4 + 3] != 0x7f) return 16;
    const uint8_t bc6[16] = {0};
    uint16_t hdr[64];
    if (!bc_decode(0x50, bc6, 4, 4, hdr)) return 17;
    for (unsigned i = 0; i < 16; i++)
        if (hdr[i * 4] || hdr[i * 4 + 1] || hdr[i * 4 + 2] || hdr[i * 4 + 3] != 0x3c00) return 18;
    if (bc_decoded_pixel_size(0x4f) != 8 || bc_decoded_pixel_size(0x49) != 4 ||
        bc_decoded_pixel_size(0x51) != 0) return 19;
    uint8_t compressed[4 * 3 * 8], copied[48];
    for (unsigned i = 0; i < sizeof compressed; i++) compressed[i] = (uint8_t)i;
    memset(copied, 0xa5, sizeof copied);
    if (!bc_copy_blocks(compressed, 4, 3, 1, 1, 2, 2, 8, copied, 24)) return 20;
    if (memcmp(copied, compressed + 40, 16) || memcmp(copied + 24, compressed + 72, 16)) return 21;
    for (unsigned i = 16; i < 24; i++) if (copied[i] != 0xa5) return 22;
    if (bc_copy_blocks(compressed, 4, 3, 3, 1, 2, 2, 8, copied, 24)) return 23;
    if (bc_copy_blocks(compressed, 4, 3, 1, 1, 2, 2, 8, copied, 15)) return 24;
    puts("BC decoder smoke test passed");
    return 0;
}

