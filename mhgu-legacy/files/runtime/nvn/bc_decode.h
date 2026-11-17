#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Input is an array of linear 4x4 blocks. Output is RGBA8 or RGBA16F for BC6H.
unsigned bc_decoded_pixel_size(uint32_t format);
bool bc_decode(uint32_t format, const uint8_t *blocks,
               uint32_t width, uint32_t height, void *pixels);

// Copy an aligned rectangle of compressed blocks between linear surfaces.
bool bc_copy_blocks(const uint8_t *src, uint32_t src_width_blocks, uint32_t src_height_blocks,
                    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t bytes_per_block, uint8_t *dst, size_t dst_row_stride);
