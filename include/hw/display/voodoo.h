/*
 * QEMU 3dfx Voodoo Banshee/Voodoo3 emulation
 * Public interface and data structures
 *
 * Ported from Bochs implementation by Volker Ruppert
 * 
 * Copyright (c) 2012-2024 The Bochs Project
 * Copyright (c) 2025 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_DISPLAY_VOODOO_H
#define HW_DISPLAY_VOODOO_H

#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "ui/console.h"
#include "system/memory.h"

#define TYPE_VOODOO_BANSHEE "voodoo-banshee"
OBJECT_DECLARE_SIMPLE_TYPE(VoodooBansheeState, VOODOO_BANSHEE)

/* Forward declarations */
typedef struct VoodooTexture VoodooTexture;
typedef struct Voodoo2D Voodoo2D;
typedef struct Voodoo3D Voodoo3D;

/* 2D graphics state */
struct Voodoo2D {
    uint32_t clip0_min;
    uint32_t clip0_max;
    uint32_t clip1_min;
    uint32_t clip1_max;
    uint32_t dst_base;
    uint32_t dst_format;
    uint32_t dst_size;
    uint32_t dst_xy;
    uint32_t src_base;
    uint32_t src_format;
    uint32_t src_size;
    uint32_t src_xy;
    uint32_t color_back;
    uint32_t color_fore;
    uint32_t command;
    uint32_t pattern[8];
    uint32_t pattern_base;
    bool pattern_mono;
    bool transparent_color;
};

/* 3D graphics state */
struct Voodoo3D {
    uint32_t status;
    uint32_t intrctrl;
    uint32_t vretrace;
    uint32_t hvretrace;
    uint32_t backporch;
    uint32_t dimensions;
    uint32_t fbi_init[8];
    uint32_t tmu_config;
    uint32_t tmu_init[2];
    uint32_t triangle_count;
    float vertices[3][16];  /* 3 vertices, 16 parameters each */
    bool depth_test_enabled;
    bool alpha_test_enabled;
};

/* Texture state */
struct VoodooTexture {
    uint32_t base_addr;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t s_scale;
    uint32_t t_scale;
    bool enabled;
};

/* Hardware cursor state */
typedef struct VoodooCursor {
    uint32_t addr;
    uint32_t x;
    uint32_t y;
    uint32_t color0;
    uint32_t color1;
    bool enabled;
    uint8_t data[512];  /* 64x64 cursor, 1 bpp */
} VoodooCursor;

/* Video overlay state */
typedef struct VoodooOverlay {
    uint32_t vidproc_cfg;
    uint32_t format;
    uint32_t addr[3];
    uint32_t stride;
    uint32_t start_coords;
    uint32_t end_coords;
    uint32_t du_dx;
    uint32_t dv_dy;
    bool enabled;
} VoodooOverlay;

/* VGA compatibility state */
typedef struct VoodooVGA {
    uint32_t crtc[256];
    uint32_t seq[256];
    uint32_t grc[256];
    uint32_t atr[256];
    uint32_t misc_output;
    uint32_t feature_ctrl;
    uint32_t input_status;
    uint8_t crtc_index;
    uint8_t seq_index;
    uint8_t grc_index;
    uint8_t atr_index;
    bool atr_flip_flop;
} VoodooVGA;

/* Main Voodoo Banshee state structure */
struct VoodooBansheeState {
    /*< private >*/
    PCIDevice parent_obj;

    /*< public >*/
    /* Memory */
    MemoryRegion vram;
    MemoryRegion mmio;
    MemoryRegion io;
    MemoryRegion lfb;  /* Linear Frame Buffer */
    uint8_t *vram_ptr;
    uint32_t vram_size;
    
    /* Display */
    QemuConsole *con;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t pitch;
    uint32_t display_start;
    
    /* Device state */
    uint32_t pci_init_enable;
    uint32_t pci_init_remap;
    uint32_t chip_id;
    uint32_t memsize;
    
    /* Graphics engines */
    Voodoo2D twoD;
    Voodoo3D threeD;
    VoodooTexture texture[2];  /* TMU0, TMU1 */
    VoodooCursor cursor;
    VoodooOverlay overlay;
    VoodooVGA vga;
    
    /* Register state */
    uint32_t regs[256];  /* Main configuration registers */
    uint32_t io_regs[64];  /* I/O space registers */
    
    /* Timing and synchronization */
    bool retrace_active;
    bool display_enabled;
    
    /* Device configuration */
    bool is_voodoo3;  /* false = Banshee, true = Voodoo3 */
    uint32_t membase_1;
    uint32_t membase_2;
    uint32_t iobase;
};

/* Function declarations */

/* 2D operations */
void voodoo_2d_bitblt(VoodooBansheeState *s);
void voodoo_2d_pattern_fill(VoodooBansheeState *s);
void voodoo_2d_screen_to_screen(VoodooBansheeState *s);

/* 3D operations */
void voodoo_3d_triangle_setup(VoodooBansheeState *s);
void voodoo_3d_rasterize(VoodooBansheeState *s);
void voodoo_3d_init(VoodooBansheeState *s);
uint32_t voodoo_3d_reg_read(VoodooBansheeState *s, uint32_t offset);
void voodoo_3d_reg_write(VoodooBansheeState *s, uint32_t offset, uint32_t value);
void voodoo_texture_setup(VoodooBansheeState *s, int tmu_index, uint32_t base_addr);
void voodoo_3d_write_pixel(VoodooBansheeState *s, int x, int y, uint32_t color, uint32_t depth);

/* Memory access helpers */
uint32_t voodoo_mem_readl(VoodooBansheeState *s, uint32_t addr);
void voodoo_mem_writel(VoodooBansheeState *s, uint32_t addr, uint32_t val);

/* Cursor and overlay */
void voodoo_update_cursor(VoodooBansheeState *s);
void voodoo_update_overlay(VoodooBansheeState *s);

#endif /* HW_DISPLAY_VOODOO_H */