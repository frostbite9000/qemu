/*
 * QEMU NVIDIA GeForce Graphics Card Emulation
 * 
 * Ported from Bochs implementation by Vort
 * 
 * Copyright (c) 2025 QEMU Project
 * 
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "trace.h"
#include "hw/display/geforce.h"
#include <math.h>

/* Alignment macro */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Color extraction macros */
#define EXTRACT_565_TO_888(val, a, b, c)          \
  (a) = (((val) >> 8) & 0xf8) | (((val) >> 13) & 0x07); \
  (b) = (((val) >> 3) & 0xfc) | (((val) >> 9) & 0x03);  \
  (c) = (((val) << 3) & 0xf8) | (((val) >> 2) & 0x07);

#define EXTRACT_x555_TO_888(val, a, b, c)         \
  (a) = (((val) >> 7) & 0xf8) | (((val) >> 12) & 0x07); \
  (b) = (((val) >> 2) & 0xf8) | (((val) >> 7) & 0x07);  \
  (c) = (((val) << 3) & 0xf8) | (((val) >> 2) & 0x07);

/* Forward declarations */
static void geforce_execute_graphics_op(GeForceState *s, GeForceChannel *ch, 
                                       const char *op_name);

/* Memory access functions */
uint8_t geforce_vram_read8(GeForceState *s, uint32_t addr)
{
    if (addr < s->memsize) {
        return s->vram[addr];
    }
    return 0;
}

uint16_t geforce_vram_read16(GeForceState *s, uint32_t addr)
{
    if (addr + 1 < s->memsize) {
        return lduw_le_p(s->vram + addr);
    }
    return 0;
}

uint32_t geforce_vram_read32(GeForceState *s, uint32_t addr)
{
    if (addr + 3 < s->memsize) {
        return ldl_le_p(s->vram + addr);
    }
    return 0;
}

void geforce_vram_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    if (addr < s->memsize) {
        s->vram[addr] = val;
    }
}

void geforce_vram_write16(GeForceState *s, uint32_t addr, uint16_t val)
{
    if (addr + 1 < s->memsize) {
        stw_le_p(s->vram + addr, val);
    }
}

void geforce_vram_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    if (addr + 3 < s->memsize) {
        stl_le_p(s->vram + addr, val);
    }
}

void geforce_vram_write64(GeForceState *s, uint32_t addr, uint64_t val)
{
    if (addr + 7 < s->memsize) {
        stq_le_p(s->vram + addr, val);
    }
}

uint8_t geforce_ramin_read8(GeForceState *s, uint32_t addr)
{
    return geforce_vram_read8(s, addr ^ s->ramin_flip);
}

uint32_t geforce_ramin_read32(GeForceState *s, uint32_t addr)
{
    return geforce_vram_read32(s, addr ^ s->ramin_flip);
}

void geforce_ramin_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    geforce_vram_write8(s, addr ^ s->ramin_flip, val);
}

void geforce_ramin_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    geforce_vram_write32(s, addr ^ s->ramin_flip, val);
}

/* Physical memory access helpers */
static uint8_t geforce_physical_read8(GeForceState *s, uint32_t addr)
{
    uint8_t data;
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                      &data, 1);
    return data;
}

static uint16_t geforce_physical_read16(GeForceState *s, uint32_t addr)
{
    return lduw_le_phys(&address_space_memory, addr);
}

static uint32_t geforce_physical_read32(GeForceState *s, uint32_t addr)
{
    return ldl_le_phys(&address_space_memory, addr);
}

static void geforce_physical_write8(GeForceState *s, uint32_t addr, uint8_t val)
{
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       &val, 1);
}

static void geforce_physical_write16(GeForceState *s, uint32_t addr, uint16_t val)
{
    stw_le_phys(&address_space_memory, addr, val);
}

static void geforce_physical_write32(GeForceState *s, uint32_t addr, uint32_t val)
{
    stl_le_phys(&address_space_memory, addr, val);
}

static void geforce_physical_write64(GeForceState *s, uint32_t addr, uint64_t val)
{
    stq_le_phys(&address_space_memory, addr, val);
}

/* DMA access functions */
static uint32_t geforce_dma_pt_lookup(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t address_adj = addr + (geforce_ramin_read32(s, object) >> 20);
    uint32_t page_offset = address_adj & 0xFFF;
    uint32_t page_index = address_adj >> 12;
    uint32_t page = geforce_ramin_read32(s, object + 8 + page_index * 4) & 0xFFFFF000;
    return page | page_offset;
}

static uint32_t geforce_dma_lin_lookup(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t adjust = geforce_ramin_read32(s, object) >> 20;
    uint32_t base = geforce_ramin_read32(s, object + 8) & 0xFFFFF000;
    return base + adjust + addr;
}

uint8_t geforce_dma_read8(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        return geforce_physical_read8(s, addr_abs);
    } else {
        return geforce_vram_read8(s, addr_abs);
    }
}

uint16_t geforce_dma_read16(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        return geforce_physical_read16(s, addr_abs);
    } else {
        return geforce_vram_read16(s, addr_abs);
    }
}

uint32_t geforce_dma_read32(GeForceState *s, uint32_t object, uint32_t addr)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        return geforce_physical_read32(s, addr_abs);
    } else {
        return geforce_vram_read32(s, addr_abs);
    }
}

void geforce_dma_write8(GeForceState *s, uint32_t object, uint32_t addr, uint8_t val)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        geforce_physical_write8(s, addr_abs, val);
    } else {
        geforce_vram_write8(s, addr_abs, val);
    }
}

void geforce_dma_write16(GeForceState *s, uint32_t object, uint32_t addr, uint16_t val)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        geforce_physical_write16(s, addr_abs, val);
    } else {
        geforce_vram_write16(s, addr_abs, val);
    }
}

void geforce_dma_write32(GeForceState *s, uint32_t object, uint32_t addr, uint32_t val)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        geforce_physical_write32(s, addr_abs, val);
    } else {
        geforce_vram_write32(s, addr_abs, val);
    }
}

void geforce_dma_write64(GeForceState *s, uint32_t object, uint32_t addr, uint64_t val)
{
    uint32_t flags = geforce_ramin_read32(s, object);
    uint32_t addr_abs;
    
    if (flags & 0x00002000) {
        addr_abs = geforce_dma_lin_lookup(s, object, addr);
    } else {
        addr_abs = geforce_dma_pt_lookup(s, object, addr);
    }
    
    if (flags & 0x00020000) {
        geforce_physical_write64(s, addr_abs, val);
    } else {
        geforce_vram_write64(s, addr_abs, val);
    }
}

/* IRQ management */
void geforce_update_irq(GeForceState *s)
{
    uint32_t level = 0;
    
    if (s->bus_intr & s->bus_intr_en) {
        level |= 0x10000000;
    }
    if (s->fifo_intr & s->fifo_intr_en) {
        level |= 0x00000100;
    }
    if (s->graph_intr & s->graph_intr_en) {
        level |= 0x00001000;
    }
    if (s->crtc_intr & s->crtc_intr_en) {
        level |= 0x01000000;
    }
    
    pci_set_irq(PCI_DEVICE(s), level && (s->mc_intr_en & 1));
}

/* Timer functions */
uint64_t geforce_get_current_time(GeForceState *s)
{
    return (s->timer_inittime1 + 
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->timer_inittime2) & ~0x1FULL;
}

/* RAMFC access */
static uint32_t geforce_ramfc_address(GeForceState *s, uint32_t chid, uint32_t offset)
{
    uint32_t ramfc;
    if (s->card_type < 0x40) {
        ramfc = (s->fifo_ramfc & 0xFFF) << 8;
    } else {
        ramfc = (s->fifo_ramfc & 0xFFF) << 16;
    }
    uint32_t ramfc_ch_size = (s->card_type < 0x40) ? 0x40 : 0x80;
    return ramfc + chid * ramfc_ch_size + offset;
}

static void geforce_ramfc_write32(GeForceState *s, uint32_t chid, 
                                 uint32_t offset, uint32_t value)
{
    geforce_ramin_write32(s, geforce_ramfc_address(s, chid, offset), value);
}

static uint32_t geforce_ramfc_read32(GeForceState *s, uint32_t chid, uint32_t offset)
{
    return geforce_ramin_read32(s, geforce_ramfc_address(s, chid, offset));
}

/* RAMHT lookup */
static void geforce_ramht_lookup(GeForceState *s, uint32_t handle, uint32_t chid, 
                                uint32_t *object, uint8_t *engine)
{
    uint32_t ramht_addr = (s->fifo_ramht & 0xFFF) << 8;
    uint32_t ramht_bits = ((s->fifo_ramht >> 16) & 0xFF) + 9;
    uint32_t ramht_size = 1 << ramht_bits << 3;

    uint32_t hash = 0;
    uint32_t x = handle;
    while (x) {
        hash ^= (x & ((1 << ramht_bits) - 1));
        x >>= ramht_bits;
    }
    hash ^= (chid & 0xF) << (ramht_bits - 4);
    hash = hash << 3;

    uint32_t it = hash;
    do {
        if (geforce_ramin_read32(s, ramht_addr + it) == handle) {
            uint32_t context = geforce_ramin_read32(s, ramht_addr + it + 4);
            uint32_t ctx_chid;
            if (s->card_type < 0x40) {
                ctx_chid = (context >> 24) & 0x1F;
            } else {
                ctx_chid = (context >> 23) & 0x1F;
            }
            if (chid == ctx_chid) {
                if (object) {
                    if (s->card_type < 0x40) {
                        *object = (context & 0xFFFF) << 4;
                    } else {
                        *object = (context & 0xFFFFF) << 4;
                    }
                }
                if (engine) {
                    if (s->card_type < 0x40) {
                        *engine = (context >> 16) & 0xFF;
                    } else {
                        *engine = (context >> 20) & 0x7;
                    }
                }
                return;
            }
        }
        it += 8;
        if (it >= ramht_size) {
            it = 0;
        }
    } while (it != hash);

    qemu_log_mask(LOG_GUEST_ERROR, "GeForce: RAMHT lookup failed for 0x%08x\n", handle);
}

/* Graphics operations implementation */

/* Pixel operations */
static uint32_t geforce_get_pixel(GeForceState *s, uint32_t obj, 
                                 uint32_t ofs, uint32_t x, uint32_t cb)
{
    if (cb == 1) {
        return geforce_dma_read8(s, obj, ofs + x);
    } else if (cb == 2) {
        return geforce_dma_read16(s, obj, ofs + x * 2);
    } else {
        return geforce_dma_read32(s, obj, ofs + x * 4);
    }
}

static void geforce_put_pixel(GeForceState *s, GeForceChannel *ch, 
                             uint32_t ofs, uint32_t x, uint32_t value)
{
    if (ch->s2d_color_bytes == 1) {
        geforce_dma_write8(s, ch->s2d_img_dst, ofs + x, value);
    } else if (ch->s2d_color_bytes == 2) {
        geforce_dma_write16(s, ch->s2d_img_dst, ofs + x * 2, value);
    } else if (ch->s2d_color_fmt == 6) {
        geforce_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value & 0x00FFFFFF);
    } else {
        geforce_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value);
    }
}

/* Rectangle fill operation */
static void geforce_gdi_fillrect(GeForceState *s, GeForceChannel *ch, bool clipped)
{
    int16_t clipx0, clipy0, clipx1, clipy1;
    int16_t dx, dy;
    uint16_t width, height;
    
    if (clipped) {
        clipx0 = ch->gdi_clip_yx0 & 0xFFFF;
        clipy0 = ch->gdi_clip_yx0 >> 16;
        clipx1 = ch->gdi_clip_yx1 & 0xFFFF;
        clipy1 = ch->gdi_clip_yx1 >> 16;
        dx = ch->gdi_rect_yx0 & 0xFFFF;
        dy = ch->gdi_rect_yx0 >> 16;
        clipx0 -= dx;
        clipy0 -= dy;
        clipx1 -= dx;
        clipy1 -= dy;
        width = (ch->gdi_rect_yx1 & 0xFFFF) - dx;
        height = (ch->gdi_rect_yx1 >> 16) - dy;
    } else {
        dx = ch->gdi_rect_xy >> 16;
        dy = ch->gdi_rect_xy & 0xFFFF;
        width = ch->gdi_rect_wh >> 16;
        height = ch->gdi_rect_wh & 0xFFFF;
    }
    
    uint32_t pitch = ch->s2d_pitch >> 16;
    uint32_t srccolor = ch->gdi_rect_color;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch + dx * ch->s2d_color_bytes;
    
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            if (!clipped || (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1)) {
                geforce_put_pixel(s, ch, draw_offset, x, srccolor);
            }
        }
        draw_offset += pitch;
    }
    
    /* Mark display area as updated */
    dpy_gfx_update(s->con, dx, dy, width, height);
}

/* Image from CPU operation */
static void geforce_ifc(GeForceState *s, GeForceChannel *ch)
{
    uint16_t dx = ch->ifc_yx & 0xFFFF;
    uint16_t dy = ch->ifc_yx >> 16;
    uint32_t dwidth = ch->ifc_dhw & 0xFFFF;
    uint32_t height = ch->ifc_dhw >> 16;
    uint32_t swidth = ch->ifc_shw & 0xFFFF;
    uint32_t pitch = ch->s2d_pitch >> 16;
    
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch + dx * ch->s2d_color_bytes;
    uint32_t word_offset = 0;
    
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < dwidth; x++) {
            uint32_t srccolor;
            if (ch->ifc_color_bytes == 4) {
                srccolor = ch->ifc_words[word_offset];
            } else if (ch->ifc_color_bytes == 2) {
                uint16_t *words16 = (uint16_t *)ch->ifc_words;
                srccolor = words16[word_offset];
            } else {
                uint8_t *words8 = (uint8_t *)ch->ifc_words;
                srccolor = words8[word_offset];
            }
            
            geforce_put_pixel(s, ch, draw_offset, x, srccolor);
            word_offset++;
        }
        word_offset += swidth - dwidth;
        draw_offset += pitch;
    }
    
    dpy_gfx_update(s->con, dx, dy, dwidth, height);
}

/* BitBlt operation */
static void geforce_copyarea(GeForceState *s, GeForceChannel *ch)
{
    uint16_t sx = ch->blit_syx & 0xFFFF;
    uint16_t sy = ch->blit_syx >> 16;
    uint16_t dx = ch->blit_dyx & 0xFFFF;
    uint16_t dy = ch->blit_dyx >> 16;
    uint16_t width = ch->blit_hw & 0xFFFF;
    uint16_t height = ch->blit_hw >> 16;
    
    uint32_t spitch = ch->s2d_pitch & 0xFFFF;
    uint32_t dpitch = ch->s2d_pitch >> 16;
    uint32_t src_offset = ch->s2d_ofs_src;
    uint32_t draw_offset = ch->s2d_ofs_dst;
    
    bool xdir = dx > sx;
    bool ydir = dy > sy;
    
    src_offset += (sy + ydir * (height - 1)) * spitch + sx * ch->s2d_color_bytes;
    draw_offset += (dy + ydir * (height - 1)) * dpitch + dx * ch->s2d_color_bytes;
    
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t xa = xdir ? width - x - 1 : x;
            uint32_t srccolor = geforce_get_pixel(s, ch->s2d_img_src, 
                                                 src_offset, xa, ch->s2d_color_bytes);
            geforce_put_pixel(s, ch, draw_offset, xa, srccolor);
        }
        src_offset += spitch * (1 - 2 * ydir);
        draw_offset += dpitch * (1 - 2 * ydir);
    }
    
    dpy_gfx_update(s->con, dx, dy, width, height);
}

/* Memory to memory format operation */
static void geforce_m2mf(GeForceState *s, GeForceChannel *ch)
{
    uint32_t src_offset = ch->m2mf_src_offset;
    uint32_t dst_offset = ch->m2mf_dst_offset;
    
    for (uint16_t y = 0; y < ch->m2mf_line_count; y++) {
        /* Simple memory copy */
        for (uint32_t i = 0; i < ch->m2mf_line_length; i += 4) {
            uint32_t data = geforce_dma_read32(s, ch->m2mf_src, src_offset + i);
            geforce_dma_write32(s, ch->m2mf_dst, dst_offset + i, data);
        }
        src_offset += ch->m2mf_src_pitch;
        dst_offset += ch->m2mf_dst_pitch;
    }
    
    /* Check if destination is display buffer */
    uint32_t dma_target = geforce_ramin_read32(s, ch->m2mf_dst) >> 12 & 0xFF;
    if (dma_target == 0x03 || dma_target == 0x0b) {
        uint32_t width = ch->m2mf_line_length / (s->bpp >> 3);
        dpy_gfx_update(s->con, 0, 0, width, ch->m2mf_line_count);
    }
}

/* Basic 3D operations */
static void geforce_d3d_clear_surface(GeForceState *s, GeForceChannel *ch)
{
    uint32_t dx = ch->d3d_clip_horizontal & 0xFFFF;
    uint32_t dy = ch->d3d_clip_vertical & 0xFFFF;
    uint32_t width = ch->d3d_clip_horizontal >> 16;
    uint32_t height = ch->d3d_clip_vertical >> 16;
    
    if (ch->d3d_clear_surface & 0x000000F0) {
        /* Clear color buffer */
        uint32_t pitch = ch->d3d_surface_pitch_a & 0xFFFF;
        uint32_t draw_offset = ch->d3d_surface_color_offset + 
                              dy * pitch + dx * ch->d3d_color_bytes;
        
        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < width; x++) {
                if (ch->d3d_color_bytes == 2) {
                    geforce_dma_write16(s, ch->d3d_color_obj, 
                                       draw_offset + x * 2, ch->d3d_color_clear_value);
                } else {
                    geforce_dma_write32(s, ch->d3d_color_obj, 
                                       draw_offset + x * 4, ch->d3d_color_clear_value);
                }
            }
            draw_offset += pitch;
        }
        
        dpy_gfx_update(s->con, dx, dy, width, height);
    }
    
    if (ch->d3d_clear_surface & 0x00000001) {
        /* Clear depth buffer */
        uint32_t pitch = ch->d3d_surface_pitch_a >> 16;
        uint32_t draw_offset = ch->d3d_surface_zeta_offset + 
                              dy * pitch + dx * ch->d3d_depth_bytes;
        
        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < width; x++) {
                if (ch->d3d_depth_bytes == 2) {
                    geforce_dma_write16(s, ch->d3d_zeta_obj, 
                                       draw_offset + x * 2, ch->d3d_zstencil_clear_value);
                } else {
                    geforce_dma_write32(s, ch->d3d_zeta_obj, 
                                       draw_offset + x * 4, ch->d3d_zstencil_clear_value);
                }
            }
            draw_offset += pitch;
        }
    }
}

/* Graphics operations dispatcher */
static void geforce_execute_graphics_op(GeForceState *s, GeForceChannel *ch, 
                                       const char *op_name)
{
    if (strcmp(op_name, "fillrect") == 0) {
        geforce_gdi_fillrect(s, ch, false);
    } else if (strcmp(op_name, "fillrect_clipped") == 0) {
        geforce_gdi_fillrect(s, ch, true);
    } else if (strcmp(op_name, "ifc") == 0) {
        geforce_ifc(s, ch);
    } else if (strcmp(op_name, "copyarea") == 0) {
        geforce_copyarea(s, ch);
    } else if (strcmp(op_name, "m2mf") == 0) {
        geforce_m2mf(s, ch);
    } else if (strcmp(op_name, "d3d_clear") == 0) {
        geforce_d3d_clear_surface(s, ch);
    }
}

/* Command processing implementation */

/* Update color bytes helper */
static void geforce_update_color_bytes(uint32_t s2d_color_fmt, uint32_t color_fmt, 
                                      uint32_t *color_bytes)
{
    if (s2d_color_fmt == 1) { /* Y8 */
        *color_bytes = 1;
    } else if (color_fmt == 1 || color_fmt == 2 || color_fmt == 3) {
        /* R5G6B5, A1R5G5B5, X1R5G5B5 */
        *color_bytes = 2;
    } else if (color_fmt == 4 || color_fmt == 5) {
        /* A8R8G8B8, X8R8G8B8 */
        *color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "GeForce: unknown color format: 0x%02x\n", color_fmt);
        *color_bytes = 4; /* Default */
    }
}

/* Command execution handlers */
static void geforce_execute_clip(GeForceState *s, GeForceChannel *ch, 
                                uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x0c0:
        ch->clip_yx = param;
        break;
    case 0x0c1:
        ch->clip_hw = param;
        break;
    }
}

static void geforce_execute_m2mf(GeForceState *s, GeForceChannel *ch, 
                                uint32_t subc, uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x061:
        ch->m2mf_src = param;
        break;
    case 0x062:
        ch->m2mf_dst = param;
        break;
    case 0x0c3:
        ch->m2mf_src_offset = param;
        break;
    case 0x0c4:
        ch->m2mf_dst_offset = param;
        break;
    case 0x0c5:
        ch->m2mf_src_pitch = param;
        break;
    case 0x0c6:
        ch->m2mf_dst_pitch = param;
        break;
    case 0x0c7:
        ch->m2mf_line_length = param;
        break;
    case 0x0c8:
        ch->m2mf_line_count = param;
        break;
    case 0x0c9:
        ch->m2mf_format = param;
        break;
    case 0x0ca:
        ch->m2mf_buffer_notify = param;
        geforce_execute_graphics_op(s, ch, "m2mf");
        
        /* Notify completion */
        if ((geforce_ramin_read32(s, ch->schs[subc].notifier) & 0xFF) != 0x30) {
            geforce_dma_write64(s, ch->schs[subc].notifier, 0x10, 
                               geforce_get_current_time(s));
            geforce_dma_write32(s, ch->schs[subc].notifier, 0x18, 0);
            geforce_dma_write32(s, ch->schs[subc].notifier, 0x1C, 0);
        }
        break;
    }
}

static void geforce_execute_rop(GeForceState *s, GeForceChannel *ch, 
                               uint32_t method, uint32_t param)
{
    if (method == 0x0c0) {
        ch->rop = param;
    }
}

static void geforce_execute_patt(GeForceState *s, GeForceChannel *ch, 
                                uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x0c2:
        ch->patt_shape = param;
        break;
    case 0x0c3:
        ch->patt_type = param;
        break;
    case 0x0c4:
        ch->patt_bg_color = param;
        break;
    case 0x0c5:
        ch->patt_fg_color = param;
        break;
    case 0x0c6:
    case 0x0c7:
        for (uint32_t i = 0; i < 32; i++) {
            ch->patt_data_mono[i + (method & 1) * 32] = 
                (param >> (i ^ 7)) & 1;
        }
        break;
    default:
        if (method >= 0x100 && method < 0x110) {
            uint32_t i = (method - 0x100) * 4;
            ch->patt_data_color[i] = param & 0xFF;
            ch->patt_data_color[i + 1] = (param >> 8) & 0xFF;
            ch->patt_data_color[i + 2] = (param >> 16) & 0xFF;
            ch->patt_data_color[i + 3] = param >> 24;
        }
        break;
    }
}

static void geforce_execute_gdi(GeForceState *s, GeForceChannel *ch, 
                               uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x0bf:
        ch->gdi_operation = param;
        break;
    case 0x0c0:
        ch->gdi_color_fmt = param;
        break;
    case 0x0c1:
        ch->gdi_mono_fmt = param;
        break;
    case 0x0ff:
        ch->gdi_rect_color = param;
        break;
    case 0x17d:
        ch->gdi_clip_yx0 = param;
        break;
    case 0x17e:
        ch->gdi_clip_yx1 = param;
        break;
    case 0x17f:
        ch->gdi_rect_color = param;
        break;
    default:
        if (method >= 0x100 && method < 0x140) {
            if (method & 1) {
                ch->gdi_rect_wh = param;
                geforce_execute_graphics_op(s, ch, "fillrect");
            } else {
                ch->gdi_rect_xy = param;
            }
        } else if (method >= 0x180 && method < 0x1c0) {
            if (method & 1) {
                ch->gdi_rect_yx1 = param;
                geforce_execute_graphics_op(s, ch, "fillrect_clipped");
            } else {
                ch->gdi_rect_yx0 = param;
            }
        }
        break;
    }
}

static void geforce_execute_surf2d(GeForceState *s, GeForceChannel *ch, 
                                  uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x061:
        ch->s2d_img_src = param;
        break;
    case 0x062:
        ch->s2d_img_dst = param;
        break;
    case 0x0c0:
        ch->s2d_color_fmt = param;
        if (ch->s2d_color_fmt == 1) { /* Y8 */
            ch->s2d_color_bytes = 1;
        } else if (ch->s2d_color_fmt == 4) { /* R5G6B5 */
            ch->s2d_color_bytes = 2;
        } else if (ch->s2d_color_fmt == 0x6 || ch->s2d_color_fmt == 0xA || 
                   ch->s2d_color_fmt == 0xB) {
            /* X8R8G8B8_Z8R8G8B8, A8R8G8B8, Y32 */
            ch->s2d_color_bytes = 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, 
                         "GeForce: unknown 2D surface color format: 0x%02x\n", 
                         ch->s2d_color_fmt);
            ch->s2d_color_bytes = 4;
        }
        break;
    case 0x0c1:
        ch->s2d_pitch = param;
        break;
    case 0x0c2:
        ch->s2d_ofs_src = param;
        break;
    case 0x0c3:
        ch->s2d_ofs_dst = param;
        break;
    }
}

static void geforce_execute_ifc(GeForceState *s, GeForceChannel *ch, 
                               uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x061:
        ch->ifc_color_key_enable = (geforce_ramin_read32(s, param) & 0xFF) != 0x30;
        break;
    case 0x0bf:
        ch->ifc_operation = param;
        break;
    case 0x0c0:
        ch->ifc_color_fmt = param;
        geforce_update_color_bytes(ch->s2d_color_fmt, ch->ifc_color_fmt, 
                                  &ch->ifc_color_bytes);
        break;
    case 0x0c1:
        ch->ifc_yx = param;
        break;
    case 0x0c2:
        ch->ifc_dhw = param;
        break;
    case 0x0c3:
        ch->ifc_shw = param;
        ch->ifc_upload = param == 0x10000400 && ch->ifc_dhw == 0x10000400 &&
                        ch->s2d_color_fmt == 0xB && ch->s2d_pitch == 0x10001000;
        if (ch->ifc_upload) {
            uint16_t dx = ch->ifc_yx & 0xFFFF;
            uint16_t dy = ch->ifc_yx >> 16;
            ch->ifc_upload_offset = ch->s2d_ofs_dst + ((dy << 12) | (dx << 2));
        } else {
            uint32_t width = ch->ifc_shw & 0xFFFF;
            uint32_t height = ch->ifc_shw >> 16;
            uint32_t wordCount = ALIGN(width * height * ch->ifc_color_bytes, 4) >> 2;
            g_free(ch->ifc_words);
            ch->ifc_words_ptr = 0;
            ch->ifc_words_left = wordCount;
            ch->ifc_words = g_new0(uint32_t, wordCount);
        }
        break;
    default:
        if (method >= 0x100 && method < 0x800) {
            if (ch->ifc_upload) {
                geforce_dma_write32(s, ch->s2d_img_dst, ch->ifc_upload_offset, param);
                ch->ifc_upload_offset += 4;
            } else {
                ch->ifc_words[ch->ifc_words_ptr++] = param;
                ch->ifc_words_left--;
                if (!ch->ifc_words_left) {
                    geforce_execute_graphics_op(s, ch, "ifc");
                    g_free(ch->ifc_words);
                    ch->ifc_words = NULL;
                }
            }
        }
        break;
    }
}

static void geforce_execute_imageblit(GeForceState *s, GeForceChannel *ch, 
                                     uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x061:
        ch->blit_color_key_enable = (geforce_ramin_read32(s, param) & 0xFF) != 0x30;
        break;
    case 0x0bf:
        ch->blit_operation = param;
        break;
    case 0x0c0:
        ch->blit_syx = param;
        break;
    case 0x0c1:
        ch->blit_dyx = param;
        break;
    case 0x0c2:
        ch->blit_hw = param;
        geforce_execute_graphics_op(s, ch, "copyarea");
        break;
    }
}

static void geforce_execute_d3d(GeForceState *s, GeForceChannel *ch, 
                               uint32_t cls, uint32_t method, uint32_t param)
{
    union {
        uint32_t param_integer;
        float param_float;
    } u;
    u.param_integer = param;

    switch (method) {
    case 0x061:
        ch->d3d_a_obj = param;
        break;
    case 0x062:
        ch->d3d_b_obj = param;
        break;
    case 0x065:
        ch->d3d_color_obj = param;
        break;
    case 0x066:
        ch->d3d_zeta_obj = param;
        break;
    case 0x080:
        ch->d3d_clip_horizontal = param;
        break;
    case 0x081:
        ch->d3d_clip_vertical = param;
        break;
    case 0x082:
        ch->d3d_surface_format = param;
        /* Decode surface format */
        uint32_t format_color = (cls == 0x0097) ? param & 0x0F : param & 0x1F;
        uint32_t format_depth = (cls == 0x0097) ? (param >> 4) & 0x0F : (param >> 5) & 0x07;
        
        if (format_color == 0x9) { /* B8 */
            ch->d3d_color_bytes = 1;
        } else if (format_color == 0x3) { /* R5G6B5 */
            ch->d3d_color_bytes = 2;
        } else if (format_color == 0x4 || format_color == 0x5 || format_color == 0x8) {
            /* X8R8G8B8_Z8R8G8B8, X8R8G8B8_O8R8G8B8, A8R8G8B8 */
            ch->d3d_color_bytes = 4;
        }
        
        if (format_depth == 0x1) { /* Z16 */
            ch->d3d_depth_bytes = 2;
        } else if (format_depth == 0x2) { /* Z24S8 */
            ch->d3d_depth_bytes = 4;
        }
        break;
    case 0x083:
        ch->d3d_surface_pitch_a = param;
        break;
    case 0x084:
        ch->d3d_surface_color_offset = param;
        break;
    case 0x085:
        ch->d3d_surface_zeta_offset = param;
        break;
    case 0x763:
        ch->d3d_zstencil_clear_value = param;
        break;
    case 0x764:
        ch->d3d_color_clear_value = param;
        break;
    case 0x765:
        ch->d3d_clear_surface = param;
        geforce_execute_graphics_op(s, ch, "d3d_clear");
        break;
    /* Vertex data processing */
    case 0x606:
        ch->d3d_vertex_data[ch->d3d_vertex_index][ch->d3d_attrib_index][ch->d3d_comp_index] = u.param_float;
        ch->d3d_comp_index++;
        if (ch->d3d_comp_index == 4) {
            ch->d3d_comp_index = 0;
            ch->d3d_attrib_index++;
            if (ch->d3d_attrib_index == 16) {
                ch->d3d_attrib_index = 0;
                ch->d3d_vertex_index++;
                if (ch->d3d_vertex_index >= 3) {
                    /* Process triangle */
                    ch->d3d_vertex_index = 0;
                }
            }
        }
        break;
    default:
        /* Handle other 3D commands */
        break;
    }
}

/* Main command execution function */
bool geforce_execute_command(GeForceState *s, uint32_t chid, uint32_t subc, 
                            uint32_t method, uint32_t param)
{
    GeForceChannel *ch = &s->channels[chid];
    bool software_method = false;
    
    if (method == 0x000) {
        /* Object binding */
        if (ch->schs[subc].engine == 0x01) {
            /* Update object state before binding */
            uint32_t word1 = geforce_ramin_read32(s, ch->schs[subc].object + 0x4);
            if (s->card_type < 0x40) {
                word1 = (word1 & 0x0000FFFF) | (ch->schs[subc].notifier >> 4 << 16);
            } else {
                word1 = (word1 & 0xFFF00000) | (ch->schs[subc].notifier >> 4);
            }
            geforce_ramin_write32(s, ch->schs[subc].object + 0x4, word1);
        }
        
        geforce_ramht_lookup(s, param, chid, &ch->schs[subc].object, 
                            &ch->schs[subc].engine);
        
        if (ch->schs[subc].engine == 0x01) {
            /* Read object properties */
            uint32_t word1 = geforce_ramin_read32(s, ch->schs[subc].object + 0x4);
            if (s->card_type < 0x40) {
                ch->schs[subc].notifier = word1 >> 16 << 4;
            } else {
                ch->schs[subc].notifier = (word1 & 0xFFFFF) << 4;
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    } else if (method == 0x014) {
        s->fifo_cache1_ref_cnt = param;
    } else if (method >= 0x040) {
        if (ch->schs[subc].engine == 0x01) {
            if (method >= 0x060 && method < 0x080) {
                geforce_ramht_lookup(s, param, chid, &param, NULL);
            }
            
            uint32_t cls = geforce_ramin_read32(s, ch->schs[subc].object) & s->class_mask;
            uint8_t cls8 = cls;
            
            /* Execute based on object class */
            switch (cls8) {
            case 0x19:
                geforce_execute_clip(s, ch, method, param);
                break;
            case 0x39:
                geforce_execute_m2mf(s, ch, subc, method, param);
                break;
            case 0x43:
                geforce_execute_rop(s, ch, method, param);
                break;
            case 0x44:
                geforce_execute_patt(s, ch, method, param);
                break;
            case 0x4a:
                geforce_execute_gdi(s, ch, method, param);
                break;
            case 0x5f:
            case 0x9f:
                geforce_execute_imageblit(s, ch, method, param);
                break;
            case 0x61:
            case 0x65:
            case 0x8a:
                geforce_execute_ifc(s, ch, method, param);
                break;
            case 0x62:
                geforce_execute_surf2d(s, ch, method, param);
                break;
            case 0x97:
                geforce_execute_d3d(s, ch, cls, method, param);
                break;
            default:
                qemu_log_mask(LOG_UNIMP, 
                             "GeForce: unimplemented object class 0x%02x method 0x%03x\n", 
                             cls8, method);
                break;
            }
            
            /* Handle notifications */
            if (ch->notify_pending) {
                ch->notify_pending = false;
                if ((geforce_ramin_read32(s, ch->schs[subc].notifier) & 0xFF) != 0x30) {
                    geforce_dma_write64(s, ch->schs[subc].notifier, 0x0, 
                                       geforce_get_current_time(s));
                    geforce_dma_write32(s, ch->schs[subc].notifier, 0x8, 0);
                    geforce_dma_write32(s, ch->schs[subc].notifier, 0xC, 0);
                }
                
                if (ch->notify_type) {
                    s->graph_intr |= 0x00000001;
                    geforce_update_irq(s);
                    s->graph_nsource |= 0x00000001;
                    s->graph_notify = 0x00110000;
                }
            }
            
            if (method == 0x041) {
                ch->notify_pending = true;
                ch->notify_type = param;
            } else if (method == 0x060) {
                ch->schs[subc].notifier = param;
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    }
    
    if (software_method) {
        s->fifo_intr |= 0x00000001;
        geforce_update_irq(s);
        s->fifo_cache1_pull0 |= 0x00000100;
        s->fifo_cache1_method[s->fifo_cache1_put / 4] = (method << 2) | (subc << 13);
        s->fifo_cache1_data[s->fifo_cache1_put / 4] = param;
        s->fifo_cache1_put += 4;
        if (s->fifo_cache1_put == GEFORCE_CACHE1_SIZE * 4) {
            s->fifo_cache1_put = 0;
        }
    }
    
    return true;
}

/* FIFO processing */
void geforce_fifo_process(GeForceState *s, uint32_t chid)
{
    uint32_t oldchid = s->fifo_cache1_push1 & 0x1F;
    if (oldchid == chid) {
        if (s->fifo_cache1_dma_put == s->fifo_cache1_dma_get) {
            return;
        }
    } else {
        if (geforce_ramfc_read32(s, chid, 0x0) == geforce_ramfc_read32(s, chid, 0x4)) {
            return;
        }
    }
    
    /* Context switch if needed */
    if (oldchid != chid) {
        uint32_t sro = s->card_type < 0x40 ? 0x2C : 0x30;
        
        /* Save old context */
        geforce_ramfc_write32(s, oldchid, 0x0, s->fifo_cache1_dma_put);
        geforce_ramfc_write32(s, oldchid, 0x4, s->fifo_cache1_dma_get);
        geforce_ramfc_write32(s, oldchid, 0x8, s->fifo_cache1_ref_cnt);
        geforce_ramfc_write32(s, oldchid, 0xC, s->fifo_cache1_dma_instance);
        geforce_ramfc_write32(s, oldchid, sro, s->fifo_cache1_semaphore);
        
        /* Load new context */
        s->fifo_cache1_dma_put = geforce_ramfc_read32(s, chid, 0x0);
        s->fifo_cache1_dma_get = geforce_ramfc_read32(s, chid, 0x4);
        s->fifo_cache1_ref_cnt = geforce_ramfc_read32(s, chid, 0x8);
        s->fifo_cache1_dma_instance = geforce_ramfc_read32(s, chid, 0xC);
        s->fifo_cache1_semaphore = geforce_ramfc_read32(s, chid, sro);
        
        s->fifo_cache1_push1 = (s->fifo_cache1_push1 & ~0x1F) | chid;
    }
    
    GeForceChannel *ch = &s->channels[chid];
    
    /* Process FIFO commands */
    while (s->fifo_cache1_dma_get != s->fifo_cache1_dma_put) {
        uint32_t word = geforce_dma_read32(s, s->fifo_cache1_dma_instance << 4, 
                                          s->fifo_cache1_dma_get);
        s->fifo_cache1_dma_get += 4;
        
        if (ch->dma_state.mcnt) {
            /* Execute command */
            if (!geforce_execute_command(s, chid, ch->dma_state.subc, 
                                        ch->dma_state.mthd, word)) {
                s->fifo_cache1_dma_get -= 4;
                break;
            }
            if (!ch->dma_state.ni) {
                ch->dma_state.mthd++;
            }
            ch->dma_state.mcnt--;
        } else {
            /* Parse command header */
            if ((word & 0xe0000003) == 0x20000000) {
                /* Old jump */
                s->fifo_cache1_dma_get = word & 0x1fffffff;
            } else if ((word & 3) == 1) {
                /* Jump */
                s->fifo_cache1_dma_get = word & 0xfffffffc;
            } else if ((word & 3) == 2) {
                /* Call */
                if (ch->subr_active) {
                    qemu_log_mask(LOG_GUEST_ERROR, 
                                 "GeForce: call with subroutine active\n");
                } else {
                    ch->subr_return = s->fifo_cache1_dma_get;
                    ch->subr_active = true;
                    s->fifo_cache1_dma_get = word & 0xfffffffc;
                }
            } else if (word == 0x00020000) {
                /* Return */
                if (!ch->subr_active) {
                    qemu_log_mask(LOG_GUEST_ERROR, 
                                 "GeForce: return with subroutine inactive\n");
                } else {
                    s->fifo_cache1_dma_get = ch->subr_return;
                    ch->subr_active = false;
                }
            } else if ((word & 0xa0030003) == 0) {
                /* Method header */
                ch->dma_state.mthd = (word >> 2) & 0x7ff;
                ch->dma_state.subc = (word >> 13) & 7;
                ch->dma_state.mcnt = (word >> 18) & 0x7ff;
                ch->dma_state.ni = word & 0x40000000;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, 
                             "GeForce: unexpected FIFO word 0x%08x\n", word);
            }
        }
    }
}

/* VBlank timer */
static void geforce_vblank_timer(void *opaque)
{
    GeForceState *s = GEFORCE(opaque);
    
    s->crtc_intr |= 0x00000001;
    geforce_update_irq(s);
    
    if (s->acquire_active) {
        s->acquire_active = false;
        for (int i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
            geforce_fifo_process(s, i);
        }
    }
    
    /* Schedule next VBlank (60 Hz) */
    timer_mod(s->vblank_timer, 
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
}

/* MMIO read handler */
static uint64_t geforce_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    GeForceState *s = GEFORCE(opaque);
    uint64_t val = 0;
    
    switch (addr) {
    case 0x000000: /* PMC_ID */
        if (s->card_type == 0x20) {
            val = 0x020200A5;
        } else {
            val = s->card_type << 20;
        }
        break;
        
    case 0x000100: /* PMC_INTR */
        if (s->bus_intr & s->bus_intr_en) val |= 0x10000000;
        if (s->fifo_intr & s->fifo_intr_en) val |= 0x00000100;
        if (s->graph_intr & s->graph_intr_en) val |= 0x00001000;
        if (s->crtc_intr & s->crtc_intr_en) val |= 0x01000000;
        break;
        
    case 0x000140: /* PMC_INTR_EN */
        val = s->mc_intr_en;
        break;
        
    case 0x000200: /* PMC_ENABLE */
        val = s->mc_enable;
        break;
        
    case 0x001100: /* PBUS_INTR */
        val = s->bus_intr;
        break;
        
    case 0x001140: /* PBUS_INTR_EN */
        val = s->bus_intr_en;
        break;
        
    case 0x002100: /* PFIFO_INTR */
        val = s->fifo_intr;
        break;
        
    case 0x002140: /* PFIFO_INTR_EN */
        val = s->fifo_intr_en;
        break;
        
    case 0x002210: /* PFIFO_RAMHT */
        val = s->fifo_ramht;
        break;
        
    case 0x002214: /* PFIFO_RAMFC (NV20) */
        if (s->card_type < 0x40) {
            val = s->fifo_ramfc;
        }
        break;
        
    case 0x002218: /* PFIFO_RAMRO */
        val = s->fifo_ramro;
        break;
        
    case 0x002220: /* PFIFO_RAMFC (NV40+) */
        if (s->card_type >= 0x40) {
            val = s->fifo_ramfc;
        }
        break;
        
    case 0x002400: /* PFIFO_RUNOUT_STATUS */
        val = 0x00000010;
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            val = 0x00000000;
        }
        break;
        
    case 0x002504: /* PFIFO_MODE */
        val = s->fifo_mode;
        break;
        
    case 0x003204: /* PFIFO_CACHE1_PUSH1 */
        val = s->fifo_cache1_push1;
        break;
        
    case 0x003210: /* PFIFO_CACHE1_PUT */
        val = s->fifo_cache1_put;
        break;
        
    case 0x003214: /* PFIFO_CACHE1_STATUS */
        val = 0x00000010;
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            val = 0x00000000;
        }
        break;
        
    case 0x003220: /* PFIFO_CACHE1_DMA_PUSH */
        val = s->fifo_cache1_dma_push;
        break;
        
    case 0x00322c: /* PFIFO_CACHE1_DMA_INSTANCE */
        val = s->fifo_cache1_dma_instance;
        break;
        
    case 0x003230: /* PFIFO_CACHE1_DMA_CTL */
        val = 0x80000000;
        break;
        
    case 0x003240: /* PFIFO_CACHE1_DMA_PUT */
        val = s->fifo_cache1_dma_put;
        break;
        
    case 0x003244: /* PFIFO_CACHE1_DMA_GET */
        val = s->fifo_cache1_dma_get;
        break;
        
    case 0x003248: /* PFIFO_CACHE1_REF_CNT */
        val = s->fifo_cache1_ref_cnt;
        break;
        
    case 0x003250: /* PFIFO_CACHE1_PULL0 */
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_cache1_pull0 |= 0x00000100;
        }
        val = s->fifo_cache1_pull0;
        break;
        
    case 0x003270: /* PFIFO_CACHE1_GET */
        val = s->fifo_cache1_get;
        break;
        
    case 0x0032e0: /* PFIFO_GRCTX_INSTANCE */
        val = s->fifo_grctx_instance;
        break;
        
    case 0x009100: /* PTIMER_INTR */
        val = s->timer_intr;
        break;
        
    case 0x009140: /* PTIMER_INTR_EN */
        val = s->timer_intr_en;
        break;
        
    case 0x009200: /* PTIMER_NUMERATOR */
        val = s->timer_num;
        break;
        
    case 0x009210: /* PTIMER_DENOMINATOR */
        val = s->timer_den;
        break;
        
    case 0x009400: /* PTIMER_TIME_0 */
        val = (uint32_t)geforce_get_current_time(s);
        break;
        
    case 0x009410: /* PTIMER_TIME_1 */
        val = geforce_get_current_time(s) >> 32;
        break;
        
    case 0x009420: /* PTIMER_ALARM_0 */
        val = s->timer_alarm;
        break;
        
    case 0x100320: /* PFB_ZCOMP_SIZE */
        if (s->card_type == 0x20) {
            val = 0x00007fff;
        } else if (s->card_type == 0x35) {
            val = 0x0005c7ff;
        } else {
            val = 0x0002e3ff;
        }
        break;
        
    case 0x101000: /* PSTRAPS_OPTION */
        val = s->straps0_primary;
        break;
        
    case 0x400100: /* PGRAPH_INTR */
        val = s->graph_intr;
        break;
        
    case 0x400108: /* PGRAPH_NSOURCE */
        val = s->graph_nsource;
        break;
        
    case 0x40013c: /* PGRAPH_INTR_EN (NV40+) */
        if (s->card_type >= 0x40) {
            val = s->graph_intr_en;
        }
        break;
        
    case 0x400140: /* PGRAPH_INTR_EN (NV20) */
        if (s->card_type < 0x40) {
            val = s->graph_intr_en;
        }
        break;
        
    case 0x40014c: /* PGRAPH_CTX_SWITCH1 */
        val = s->graph_ctx_switch1;
        break;
        
    case 0x400150: /* PGRAPH_CTX_SWITCH2 */
        val = s->graph_ctx_switch2;
        break;
        
    case 0x400158: /* PGRAPH_CTX_SWITCH4 */
        val = s->graph_ctx_switch4;
        break;
        
    case 0x400700: /* PGRAPH_STATUS */
        val = s->graph_status;
        break;
        
    case 0x400704: /* PGRAPH_TRAPPED_ADDR */
        val = s->graph_trapped_addr;
        break;
        
    case 0x400708: /* PGRAPH_TRAPPED_DATA */
        val = s->graph_trapped_data;
        break;
        
    case 0x600100: /* PCRTC_INTR_0 */
        val = s->crtc_intr;
        break;
        
    case 0x600140: /* PCRTC_INTR_EN_0 */
        val = s->crtc_intr_en;
        break;
        
    case 0x600800: /* PCRTC_START */
        val = s->crtc_start;
        break;
        
    case 0x600804: /* PCRTC_CONFIG */
        val = s->crtc_config;
        break;
        
    case 0x600808: /* PCRTC_RASTER */
        /* Fake raster position */
        val = 0;
        break;
        
    case 0x680300: /* PRAMDAC_CU_START_POS */
        val = s->ramdac_cu_start_pos;
        break;
        
    case 0x680508: /* PRAMDAC_VPLL_COEFF */
        val = s->ramdac_vpll;
        break;
        
    case 0x68050c: /* PRAMDAC_PLL_COEFF_SELECT */
        val = s->ramdac_pll_select;
        break;
        
    case 0x680578: /* PRAMDAC_VPLL2_COEFF */
        val = s->ramdac_vpll_b;
        break;
        
    case 0x680600: /* PRAMDAC_GENERAL_CONTROL */
        val = s->ramdac_general_control;
        break;
        
    default:
        if (addr >= 0x700000 && addr < 0x800000) {
            /* RAMIN access */
            val = geforce_ramin_read32(s, addr - 0x700000);
        } else if ((addr >= 0x800000 && addr < 0xA00000) ||
                   (addr >= 0xC00000 && addr < 0xE00000)) {
            /* FIFO channel access */
            uint32_t chid, offset;
            if (addr >= 0x800000 && addr < 0xA00000) {
                chid = (addr >> 16) & 0x1F;
                offset = addr & 0x1FFF;
            } else {
                chid = (addr >> 12) & 0x1FF;
                offset = addr & 0x1FF;
            }
            
            if (chid >= GEFORCE_CHANNEL_COUNT) {
                chid = 0;
            }
            
            if (offset == 0x10) {
                val = 0xffff;
            } else if (offset >= 0x40 && offset <= 0x48) {
                uint32_t curchid = s->fifo_cache1_push1 & 0x1F;
                if (curchid == chid) {
                    switch (offset) {
                    case 0x40: val = s->fifo_cache1_dma_put; break;
                    case 0x44: val = s->fifo_cache1_dma_get; break;
                    case 0x48: val = s->fifo_cache1_ref_cnt; break;
                    }
                } else {
                    switch (offset) {
                    case 0x40: val = geforce_ramfc_read32(s, chid, 0x0); break;
                    case 0x44: val = geforce_ramfc_read32(s, chid, 0x4); break;
                    case 0x48: val = geforce_ramfc_read32(s, chid, 0x8); break;
                    }
                }
            }
        } else {
            qemu_log_mask(LOG_UNIMP, "GeForce: unimplemented MMIO read 0x%08lx\n", addr);
        }
        break;
    }
    
    return val;
}

/* MMIO write handler */
static void geforce_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    GeForceState *s = GEFORCE(opaque);
    
    switch (addr) {
    case 0x000140: /* PMC_INTR_EN */
        s->mc_intr_en = val;
        geforce_update_irq(s);
        break;
        
    case 0x000200: /* PMC_ENABLE */
        s->mc_enable = val;
        break;
        
    case 0x001100: /* PBUS_INTR */
        s->bus_intr &= ~val;
        geforce_update_irq(s);
        break;
        
    case 0x001140: /* PBUS_INTR_EN */
        s->bus_intr_en = val;
        geforce_update_irq(s);
        break;
        
    case 0x002100: /* PFIFO_INTR */
        s->fifo_intr &= ~val;
        geforce_update_irq(s);
        break;
        
    case 0x002140: /* PFIFO_INTR_EN */
        s->fifo_intr_en = val;
        geforce_update_irq(s);
        break;
        
    case 0x002210: /* PFIFO_RAMHT */
        s->fifo_ramht = val;
        break;
        
    case 0x002214: /* PFIFO_RAMFC (NV20) */
        if (s->card_type < 0x40) {
            s->fifo_ramfc = val;
        }
        break;
        
    case 0x002218: /* PFIFO_RAMRO */
        s->fifo_ramro = val;
        break;
        
    case 0x002220: /* PFIFO_RAMFC (NV40+) */
        if (s->card_type >= 0x40) {
            s->fifo_ramfc = val;
        }
        break;
        
    case 0x002504: /* PFIFO_MODE */
        s->fifo_mode = val;
        break;
        
    case 0x003204: /* PFIFO_CACHE1_PUSH1 */
        s->fifo_cache1_push1 = val;
        break;
        
    case 0x003210: /* PFIFO_CACHE1_PUT */
        s->fifo_cache1_put = val;
        break;
        
    case 0x003220: /* PFIFO_CACHE1_DMA_PUSH */
        s->fifo_cache1_dma_push = val;
        break;
        
    case 0x00322c: /* PFIFO_CACHE1_DMA_INSTANCE */
        s->fifo_cache1_dma_instance = val;
        break;
        
    case 0x003240: /* PFIFO_CACHE1_DMA_PUT */
        s->fifo_cache1_dma_put = val;
        geforce_fifo_process(s, s->fifo_cache1_push1 & 0x1F);
        break;
        
    case 0x003244: /* PFIFO_CACHE1_DMA_GET */
        s->fifo_cache1_dma_get = val;
        break;
        
    case 0x003248: /* PFIFO_CACHE1_REF_CNT */
        s->fifo_cache1_ref_cnt = val;
        break;
        
    case 0x003250: /* PFIFO_CACHE1_PULL0 */
        s->fifo_cache1_pull0 = val;
        break;
        
    case 0x003270: /* PFIFO_CACHE1_GET */
        s->fifo_cache1_get = val & (GEFORCE_CACHE1_SIZE * 4 - 1);
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_intr |= 0x00000001;
        } else {
            s->fifo_intr &= ~0x00000001;
            s->fifo_cache1_pull0 &= ~0x00000100;
        }
        geforce_update_irq(s);
        break;
        
    case 0x0032e0: /* PFIFO_GRCTX_INSTANCE */
        s->fifo_grctx_instance = val;
        break;
        
    case 0x009100: /* PTIMER_INTR */
        s->timer_intr &= ~val;
        break;
        
    case 0x009140: /* PTIMER_INTR_EN */
        s->timer_intr_en = val;
        break;
        
    case 0x009200: /* PTIMER_NUMERATOR */
        s->timer_num = val;
        break;
        
    case 0x009210: /* PTIMER_DENOMINATOR */
        s->timer_den = val;
        break;
        
    case 0x009400: /* PTIMER_TIME_0 */
        s->timer_inittime2 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->timer_inittime1 = (s->timer_inittime1 & 0xFFFFFFFF00000000ULL) | val;
        break;
        
    case 0x009410: /* PTIMER_TIME_1 */
        s->timer_inittime2 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->timer_inittime1 = (s->timer_inittime1 & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
        break;
        
    case 0x009420: /* PTIMER_ALARM_0 */
        s->timer_alarm = val;
        break;
        
    case 0x101000: /* PSTRAPS_OPTION */
        if (val >> 31) {
            s->straps0_primary = val;
        } else {
            s->straps0_primary = s->straps0_primary_original;
        }
        break;
        
    case 0x400100: /* PGRAPH_INTR */
        s->graph_intr &= ~val;
        geforce_update_irq(s);
        break;
        
    case 0x400108: /* PGRAPH_NSOURCE */
        s->graph_nsource = val;
        break;
        
    case 0x40013c: /* PGRAPH_INTR_EN (NV40+) */
        if (s->card_type >= 0x40) {
            s->graph_intr_en = val;
            geforce_update_irq(s);
        }
        break;
        
    case 0x400140: /* PGRAPH_INTR_EN (NV20) */
        if (s->card_type < 0x40) {
            s->graph_intr_en = val;
            geforce_update_irq(s);
        }
        break;
        
    case 0x600100: /* PCRTC_INTR_0 */
        s->crtc_intr &= ~val;
        geforce_update_irq(s);
        break;
        
    case 0x600140: /* PCRTC_INTR_EN_0 */
        s->crtc_intr_en = val;
        geforce_update_irq(s);
        break;
        
    case 0x600800: /* PCRTC_START */
        s->crtc_start = val;
        s->needs_update_mode = true;
        break;
        
    case 0x600804: /* PCRTC_CONFIG */
        s->crtc_config = val;
        break;
        
    case 0x680300: /* PRAMDAC_CU_START_POS */
        {
            int16_t prevx = s->hw_cursor.x;
            int16_t prevy = s->hw_cursor.y;
            s->ramdac_cu_start_pos = val;
            s->hw_cursor.x = (int32_t)val << 20 >> 20;
            s->hw_cursor.y = (int32_t)val << 4 >> 20;
            if (s->hw_cursor.size != 0) {
                dpy_gfx_update(s->con, prevx, prevy, s->hw_cursor.size, s->hw_cursor.size);
                dpy_gfx_update(s->con, s->hw_cursor.x, s->hw_cursor.y, 
                              s->hw_cursor.size, s->hw_cursor.size);
            }
        }
        break;
        
    case 0x680508: /* PRAMDAC_VPLL_COEFF */
        s->ramdac_vpll = val;
        break;
        
    case 0x68050c: /* PRAMDAC_PLL_COEFF_SELECT */
        s->ramdac_pll_select = val;
        break;
        
    case 0x680578: /* PRAMDAC_VPLL2_COEFF */
        s->ramdac_vpll_b = val;
        break;
        
    case 0x680600: /* PRAMDAC_GENERAL_CONTROL */
        s->ramdac_general_control = val;
        break;
        
    default:
        if (addr >= 0x700000 && addr < 0x800000) {
            /* RAMIN access */
            geforce_ramin_write32(s, addr - 0x700000, val);
        } else if ((addr >= 0x800000 && addr < 0xA00000) ||
                   (addr >= 0xC00000 && addr < 0xE00000)) {
            /* FIFO channel access */
            uint32_t chid, offset;
            if (addr >= 0x800000 && addr < 0xA00000) {
                chid = (addr >> 16) & 0x1F;
                offset = addr & 0x1FFF;
            } else {
                chid = (addr >> 12) & 0x1FF;
                offset = addr & 0x1FF;
            }
            
            if (chid >= GEFORCE_CHANNEL_COUNT) {
                chid = 0;
            }
            
            if (offset == 0x40) {
                uint32_t curchid = s->fifo_cache1_push1 & 0x1F;
                if (curchid == chid) {
                    s->fifo_cache1_dma_put = val;
                } else {
                    geforce_ramfc_write32(s, chid, 0x0, val);
                }
                geforce_fifo_process(s, chid);
            }
        } else {
            qemu_log_mask(LOG_UNIMP, "GeForce: unimplemented MMIO write 0x%08lx = 0x%08lx\n", 
                         addr, val);
        }
        break;
    }
}

static const MemoryRegionOps geforce_mmio_ops = {
    .read = geforce_mmio_read,
    .write = geforce_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* Property definitions */
static const Property geforce_properties[] = {
    DEFINE_PROP_UINT32("model", GeForceState, card_type, GEFORCE_3),
};

/* VMState for save/restore */
static const VMStateDescription vmstate_geforce = {
    .name = "geforce",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, GeForceState),
        VMSTATE_UINT32(card_type, GeForceState),
        VMSTATE_UINT32(memsize, GeForceState),
        VMSTATE_UINT32(xres, GeForceState),
        VMSTATE_UINT32(yres, GeForceState),
        VMSTATE_UINT32(bpp, GeForceState),
        VMSTATE_UINT32(pitch, GeForceState),
        VMSTATE_BOOL(display_enabled, GeForceState),
        
        /* Interrupt state */
        VMSTATE_UINT32(mc_intr_en, GeForceState),
        VMSTATE_UINT32(bus_intr, GeForceState),
        VMSTATE_UINT32(bus_intr_en, GeForceState),
        VMSTATE_UINT32(fifo_intr, GeForceState),
        VMSTATE_UINT32(fifo_intr_en, GeForceState),
        VMSTATE_UINT32(graph_intr, GeForceState),
        VMSTATE_UINT32(graph_intr_en, GeForceState),
        VMSTATE_UINT32(crtc_intr, GeForceState),
        VMSTATE_UINT32(crtc_intr_en, GeForceState),
        
        /* FIFO state */
        VMSTATE_UINT32(fifo_cache1_push1, GeForceState),
        VMSTATE_UINT32(fifo_cache1_put, GeForceState),
        VMSTATE_UINT32(fifo_cache1_get, GeForceState),
        VMSTATE_UINT32(fifo_cache1_dma_put, GeForceState),
        VMSTATE_UINT32(fifo_cache1_dma_get, GeForceState),
        
        /* Hardware cursor */
        VMSTATE_INT16(hw_cursor.x, GeForceState),
        VMSTATE_INT16(hw_cursor.y, GeForceState),
        VMSTATE_UINT8(hw_cursor.size, GeForceState),
        VMSTATE_BOOL(hw_cursor.enabled, GeForceState),
        
        VMSTATE_END_OF_LIST()
    }
};

/* Device reset */
static void geforce_reset(DeviceState *dev)
{
    GeForceState *s = GEFORCE(dev);
    
    /* Reset all state */
    memset(&s->channels, 0, sizeof(s->channels));
    s->mc_intr_en = 0;
    s->bus_intr = s->bus_intr_en = 0;
    s->fifo_intr = s->fifo_intr_en = 0;
    s->graph_intr = s->graph_intr_en = 0;
    s->crtc_intr = s->crtc_intr_en = 0;
    s->fifo_cache1_put = s->fifo_cache1_get = 0;
    s->fifo_cache1_dma_put = s->fifo_cache1_dma_get = 0;
    s->display_enabled = false;
    
    /* Initialize CRTC registers */
    memset(&s->crtc, 0, sizeof(s->crtc));
    
    /* Hardware cursor */
    s->hw_cursor.x = s->hw_cursor.y = 0;
    s->hw_cursor.size = 32;
    s->hw_cursor.enabled = false;
    
    /* Default display mode */
    s->xres = 1024;
    s->yres = 768;
    s->bpp = 32;
    s->pitch = s->xres * (s->bpp >> 3);
    s->disp_ptr = s->vram;
    s->disp_offset = 0;
}

/* Device realization */
static void geforce_realize(PCIDevice *pci_dev, Error **errp)
{
    GeForceState *s = GEFORCE(pci_dev);
    VGACommonState *vga = &s->vga;
    
    /* Set device IDs based on card type */
    uint16_t device_id;
    uint8_t revision_id = 0;
    
    switch (s->card_type) {
    case GEFORCE_3:
        device_id = 0x0202; /* GeForce3 Ti 500 */
        revision_id = 0xA3;
        s->memsize = 64 * MiB;
        s->bar2_size = 0x00080000;
        s->straps0_primary_original = (0x7FF86C6B | 0x00000180);
        break;
    case GEFORCE_FX_5900:
        device_id = 0x0331; /* GeForce FX 5900 */
        s->memsize = 128 * MiB;
        s->bar2_size = 0x01000000;
        s->straps0_primary_original = (0x7FF86C4B | 0x00000180);
        break;
    case GEFORCE_6800:
        device_id = 0x0045; /* GeForce 6800 GT */
        s->memsize = 256 * MiB;
        s->bar2_size = 0x01000000;
        s->straps0_primary_original = (0x7FF86C4B | 0x00000180);
        break;
    default:
        error_setg(errp, "Invalid GeForce model specified");
        return;
    }
    
    /* Initialize PCI configuration */
    pci_config_set_vendor_id(pci_dev->config, 0x10DE); /* NVIDIA */
    pci_config_set_device_id(pci_dev->config, device_id);
    pci_config_set_revision(pci_dev->config, revision_id);
    pci_config_set_class(pci_dev->config, PCI_CLASS_DISPLAY_VGA);
    pci_config_set_prog_interface(pci_dev->config, 0x00);
    
    /* Set subsystem IDs */
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_VENDOR_ID, 0x107D);
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_ID, 
                 s->card_type == GEFORCE_3 ? 0x2863 : 
                 s->card_type == GEFORCE_FX_5900 ? 0x297B : 0x2996);
    
    /* Initialize card-specific values */
    s->memsize_mask = s->memsize - 1;
    s->ramin_flip = s->memsize - 64;
    s->class_mask = s->card_type < 0x40 ? 0x00000FFF : 0x0000FFFF;
    s->straps0_primary = s->straps0_primary_original;
    
    /* Allocate VRAM */
    s->vram = g_malloc0(s->memsize);
    
    /* Initialize memory regions */
    memory_region_init_io(&s->mmio, OBJECT(s), &geforce_mmio_ops, s,
                         "geforce-mmio", GEFORCE_PNPMMIO_SIZE);
    memory_region_init_ram_ptr(&s->vram_mem, OBJECT(s), "geforce-vram",
                              s->memsize, s->vram);
    
    /* Register PCI BARs */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->mmio);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->vram_mem);
    
    /* BAR 2 for newer cards */
    if (s->card_type != GEFORCE_FX_5900) {
        memory_region_init_ram_ptr(&s->ramin_mem, OBJECT(s), "geforce-ramin",
                                  s->bar2_size, s->vram + s->ramin_flip);
        pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ramin_mem);
    }
    
    /* Initialize display console */
    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga_init(vga, OBJECT(s), pci_address_space(pci_dev),
             pci_address_space_io(pci_dev), true);
    vga->con = graphic_console_init(DEVICE(s), 0, s->vga.hw_ops, vga);
    
    /* Initialize timers */
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, geforce_vblank_timer, s);
    timer_mod(s->vblank_timer, 
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
    
    /* Set up PCI interrupt */
    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
}

/* Device unrealization */
static void geforce_unrealize(PCIDevice *pci_dev)
{
    GeForceState *s = GEFORCE(pci_dev);
    
    timer_free(s->vblank_timer);
    graphic_console_close(s->con);
    
    /* Free channel resources */
    for (int i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        g_free(s->channels[i].ifc_words);
        g_free(s->channels[i].iifc_words);
        g_free(s->channels[i].sifc_words);
        g_free(s->channels[i].tfc_words);
        g_free(s->channels[i].gdi_words);
    }
    
    g_free(s->vram);
}

/* Class initialization */
static void geforce_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    
    k->realize = geforce_realize;
    k->exit = geforce_unrealize;
    k->vendor_id = 0x10DE;
    k->device_id = 0x0202; /* Default to GeForce 3 */
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->subsystem_vendor_id = 0x107D;
    k->subsystem_id = 0x2863;
    
    dc->desc = "NVIDIA GeForce Graphics Card";
    device_class_set_legacy_reset(dc, geforce_reset);
    dc->vmsd = &vmstate_geforce;
    device_class_set_props(dc, geforce_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo geforce_info = {
    .name = TYPE_GEFORCE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GeForceState),
    .class_init = geforce_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void geforce_register_types(void)
{
    type_register_static(&geforce_info);
}

type_init(geforce_register_types)