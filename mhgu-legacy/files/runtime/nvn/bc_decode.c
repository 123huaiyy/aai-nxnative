#include "bc_decode.h"
#include <string.h>
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec.h"

unsigned bc_decoded_pixel_size(uint32_t format) {
    if (format >= 0x41 && format <= 0x4e) return 4;
    if (format == 0x4f || format == 0x50) return 8;
    return 0;
}

bool bc_copy_blocks(const uint8_t *src, uint32_t src_width_blocks, uint32_t src_height_blocks,
                    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t bytes_per_block, uint8_t *dst, size_t dst_row_stride) {
    if (!src || !dst || !width || !height || !bytes_per_block ||
        x > src_width_blocks || width > src_width_blocks - x ||
        y > src_height_blocks || height > src_height_blocks - y ||
        dst_row_stride < (uint64_t)width * bytes_per_block) return false;
    size_t row_bytes = (size_t)width * bytes_per_block;
    for (uint32_t r = 0; r < height; r++)
        memcpy(dst + (size_t)r * dst_row_stride,
               src + ((size_t)(y + r) * src_width_blocks + x) * bytes_per_block, row_bytes);
    return true;
}

bool bc_decode(uint32_t format, const uint8_t *blocks,
               uint32_t width, uint32_t height, void *pixels) {
    unsigned pixel_size = bc_decoded_pixel_size(format);
    if (!blocks || !pixels || !width || !height || !pixel_size) return false;
    uint8_t *rgba = pixels;
    unsigned bytes;
    switch (format) {
    case 0x41: case 0x42: case 0x45: case 0x46: case 0x49: case 0x4a: bytes = 8; break;
    case 0x43: case 0x44: case 0x47: case 0x48: case 0x4b: case 0x4c:
    case 0x4d: case 0x4e: case 0x4f: case 0x50: bytes = 16; break;
    default: return false;
    }
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *src = blocks + ((uint64_t)by * bw + bx) * bytes;
        _Alignas(8) uint8_t tile[128] = {0};
        switch (format) {
        case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(src, tile, 16); break;
        case 0x43: case 0x47: bcdec_bc2(src, tile, 16); break;
        case 0x44: case 0x48: bcdec_bc3(src, tile, 16); break;
        case 0x49: case 0x4a: case 0x4b: case 0x4c: {
            _Alignas(8) uint8_t channels[32] = {0};
            bool signed_format = format == 0x4a || format == 0x4c;
            bool two = format == 0x4b || format == 0x4c;
            if (two) bcdec_bc5(src, channels, 8, signed_format);
            else bcdec_bc4(src, channels, 4, signed_format);
            for (unsigned i = 0; i < 16; i++) {
                tile[i * 4] = channels[i * (two ? 2 : 1)];
                if (two) tile[i * 4 + 1] = channels[i * 2 + 1];
                tile[i * 4 + 3] = signed_format ? 127 : 255;
            }
            break;
        }
        case 0x4f: case 0x50: {
            _Alignas(8) uint16_t rgb[48];
            bcdec_bc6h_half(src, rgb, 12, format == 0x4f);
            uint16_t *half = (uint16_t *)tile;
            for (unsigned i = 0; i < 16; i++) {
                half[i * 4] = rgb[i * 3]; half[i * 4 + 1] = rgb[i * 3 + 1];
                half[i * 4 + 2] = rgb[i * 3 + 2]; half[i * 4 + 3] = 0x3c00;
            }
            break;
        }
        default: bcdec_bc7(src, tile, 16); break;
        }
        for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++) {
            uint32_t count = width - bx * 4;
            if (count > 4) count = 4;
            memcpy(rgba + ((uint64_t)(by * 4 + y) * width + bx * 4) * pixel_size,
                   tile + y * 4 * pixel_size, count * pixel_size);
        }
    }
    return true;
}
