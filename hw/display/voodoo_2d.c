/*
 * QEMU 3dfx Voodoo Banshee/Voodoo3 emulation
 * 2D graphics operations
 *
 * Ported from Bochs implementation by Volker Ruppert
 * 
 * Copyright (c) 2012-2024 The Bochs Project
 * Copyright (c) 2025 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/display/voodoo.h"
#include "hw/display/voodoo_regs.h"
#include "trace.h"

/* 2D BitBlt operation */
void voodoo_2d_bitblt(VoodooBansheeState *s)
{
    Voodoo2D *twoD = &s->twoD;
    uint32_t src_x = twoD->src_xy & 0xffff;
    uint32_t src_y = (twoD->src_xy >> 16) & 0xffff;
    uint32_t dst_x = twoD->dst_xy & 0xffff;
    uint32_t dst_y = (twoD->dst_xy >> 16) & 0xffff;
    uint32_t width = twoD->dst_size & 0xffff;
    uint32_t height = (twoD->dst_size >> 16) & 0xffff;
    uint32_t rop = twoD->command & VOODOO_2D_ROP_MASK;
    
    trace_voodoo_2d_bitblt(src_x, src_y, dst_x, dst_y, width, height);
    
    /* Simple screen-to-screen copy for now */
    if (rop == VOODOO_ROP_COPY) {
        uint32_t src_base = twoD->src_base;
        uint32_t dst_base = twoD->dst_base;
        uint32_t src_format = twoD->src_format & 0x7;
        uint32_t dst_format = twoD->dst_format & 0x7;
        uint32_t bytes_per_pixel = 1;
        
        /* Determine bytes per pixel */
        switch (src_format) {
        case VOODOO_PIXFMT_8BPP:
            bytes_per_pixel = 1;
            break;
        case VOODOO_PIXFMT_16BPP_565:
            bytes_per_pixel = 2;
            break;
        case VOODOO_PIXFMT_24BPP:
            bytes_per_pixel = 3;
            break;
        case VOODOO_PIXFMT_32BPP:
            bytes_per_pixel = 4;
            break;
        }
        
        /* Perform copy if formats match */
        if (src_format == dst_format) {
            for (uint32_t y = 0; y < height; y++) {
                uint32_t src_offset = src_base + ((src_y + y) * s->pitch) + (src_x * bytes_per_pixel);
                uint32_t dst_offset = dst_base + ((dst_y + y) * s->pitch) + (dst_x * bytes_per_pixel);
                uint32_t line_bytes = width * bytes_per_pixel;
                
                if (src_offset + line_bytes <= s->vram_size && 
                    dst_offset + line_bytes <= s->vram_size) {
                    memmove(s->vram_ptr + dst_offset, s->vram_ptr + src_offset, line_bytes);
                }
            }
            
            /* Mark display region as dirty */
            memory_region_set_dirty(&s->vram, dst_base + (dst_y * s->pitch) + (dst_x * bytes_per_pixel),
                                   height * s->pitch);
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "voodoo: unimplemented 2D ROP operation 0x%02x\n", rop);
    }
}

/* 2D pattern fill operation */
void voodoo_2d_pattern_fill(VoodooBansheeState *s)
{
    Voodoo2D *twoD = &s->twoD;
    uint32_t dst_x = twoD->dst_xy & 0xffff;
    uint32_t dst_y = (twoD->dst_xy >> 16) & 0xffff;
    uint32_t width = twoD->dst_size & 0xffff;
    uint32_t height = (twoD->dst_size >> 16) & 0xffff;
    uint32_t dst_base = twoD->dst_base;
    uint32_t dst_format = twoD->dst_format & 0x7;
    uint32_t color = twoD->color_fore;
    uint32_t bytes_per_pixel = 1;
    
    /* Determine bytes per pixel */
    switch (dst_format) {
    case VOODOO_PIXFMT_8BPP:
        bytes_per_pixel = 1;
        break;
    case VOODOO_PIXFMT_16BPP_565:
        bytes_per_pixel = 2;
        break;
    case VOODOO_PIXFMT_24BPP:
        bytes_per_pixel = 3;
        break;
    case VOODOO_PIXFMT_32BPP:
        bytes_per_pixel = 4;
        break;
    }
    
    /* Fill rectangle with solid color */
    for (uint32_t y = 0; y < height; y++) {
        uint32_t line_offset = dst_base + ((dst_y + y) * s->pitch) + (dst_x * bytes_per_pixel);
        
        if (line_offset + (width * bytes_per_pixel) <= s->vram_size) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t pixel_offset = line_offset + (x * bytes_per_pixel);
                
                switch (bytes_per_pixel) {
                case 1:
                    s->vram_ptr[pixel_offset] = color & 0xff;
                    break;
                case 2:
                    *(uint16_t *)(s->vram_ptr + pixel_offset) = color & 0xffff;
                    break;
                case 3:
                    s->vram_ptr[pixel_offset] = color & 0xff;
                    s->vram_ptr[pixel_offset + 1] = (color >> 8) & 0xff;
                    s->vram_ptr[pixel_offset + 2] = (color >> 16) & 0xff;
                    break;
                case 4:
                    *(uint32_t *)(s->vram_ptr + pixel_offset) = color;
                    break;
                }
            }
        }
    }
    
    /* Mark display region as dirty */
    memory_region_set_dirty(&s->vram, dst_base + (dst_y * s->pitch) + (dst_x * bytes_per_pixel),
                           height * s->pitch);
}

/* 2D screen-to-screen copy */
void voodoo_2d_screen_to_screen(VoodooBansheeState *s)
{
    /* This is essentially the same as bitblt for screen copies */
    voodoo_2d_bitblt(s);
}