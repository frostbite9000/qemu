/*
 * QEMU NVIDIA GeForce Graphics Card Emulation
 * 
 * Ported from Bochs implementation by Vort
 * 
 * Copyright (c) 2025 QEMU Project
 * 
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_DISPLAY_GEFORCE_H
#define HW_DISPLAY_GEFORCE_H

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "ui/console.h"
#include "qemu/timer.h"

#define TYPE_GEFORCE "geforce"
OBJECT_DECLARE_SIMPLE_TYPE(GeForceState, GEFORCE)

/* Constants */
#define GEFORCE_PNPMMIO_SIZE        0x1000000
#define GEFORCE_CHANNEL_COUNT       32
#define GEFORCE_SUBCHANNEL_COUNT    8
#define GEFORCE_CACHE1_SIZE         64
#define GEFORCE_CRTC_MAX           0x9F

/* GeForce Models */
enum {
    GEFORCE_3,
    GEFORCE_FX_5900,
    GEFORCE_6800,
    MAX_GEFORCE_TYPES
};

/* ROP flags */
#define BX_ROP_PATTERN 0x01

/* Channel subchannel state */
typedef struct {
    uint32_t object;
    uint8_t engine;
    uint32_t notifier;
} GeForceSubchannel;

/* DMA state for command processing */
typedef struct {
    uint32_t mthd;
    uint32_t subc;
    uint32_t mcnt;
    bool ni;
} GeForceDataState;

/* Graphics channel state */
typedef struct {
    uint32_t subr_return;
    bool subr_active;
    GeForceDataState dma_state;
    GeForceSubchannel schs[GEFORCE_SUBCHANNEL_COUNT];

    bool notify_pending;
    uint32_t notify_type;

    /* 2D surface state */
    uint32_t s2d_img_src, s2d_img_dst;
    uint32_t s2d_color_fmt, s2d_color_bytes;
    uint32_t s2d_pitch, s2d_ofs_src, s2d_ofs_dst;

    /* Swizzled surface */
    uint32_t swzs_img_obj;
    uint32_t swzs_fmt, swzs_color_bytes, swzs_ofs;

    /* Image from CPU operations */
    bool ifc_color_key_enable;
    uint32_t ifc_operation, ifc_color_fmt, ifc_color_bytes;
    uint32_t ifc_yx, ifc_dhw, ifc_shw;
    uint32_t ifc_words_ptr, ifc_words_left;
    uint32_t *ifc_words;
    bool ifc_upload;
    uint32_t ifc_upload_offset;

    /* Indexed image from CPU */
    uint32_t iifc_palette, iifc_palette_ofs;
    uint32_t iifc_operation, iifc_color_fmt, iifc_color_bytes;
    uint32_t iifc_bpp4, iifc_yx, iifc_dhw, iifc_shw;
    uint32_t iifc_words_ptr, iifc_words_left;
    uint32_t *iifc_words;

    /* Scaled image from CPU */
    uint32_t sifc_operation, sifc_color_fmt, sifc_color_bytes;
    uint32_t sifc_shw, sifc_dxds, sifc_dydt;
    uint32_t sifc_clip_yx, sifc_clip_hw, sifc_syx;
    uint32_t sifc_words_ptr, sifc_words_left;
    uint32_t *sifc_words;

    /* BitBlt operations */
    bool blit_color_key_enable;
    uint32_t blit_operation, blit_syx, blit_dyx, blit_hw;

    /* Textured fill from CPU */
    bool tfc_swizzled;
    uint32_t tfc_color_fmt, tfc_color_bytes;
    uint32_t tfc_yx, tfc_hw, tfc_clip_wx, tfc_clip_hy;
    uint32_t tfc_words_ptr, tfc_words_left;
    uint32_t *tfc_words;

    /* Scaled image from memory */
    uint32_t sifm_src;
    bool sifm_swizzled;
    uint32_t sifm_operation, sifm_color_fmt, sifm_color_bytes;
    uint32_t sifm_syx, sifm_dyx, sifm_shw, sifm_dhw;
    uint32_t sifm_dudx, sifm_dvdy, sifm_sfmt, sifm_sofs;

    /* Memory to memory format */
    uint32_t m2mf_src, m2mf_dst;
    uint32_t m2mf_src_offset, m2mf_dst_offset;
    uint32_t m2mf_src_pitch, m2mf_dst_pitch;
    uint32_t m2mf_line_length, m2mf_line_count;
    uint32_t m2mf_format, m2mf_buffer_notify;

    /* 3D state */
    uint32_t d3d_a_obj, d3d_b_obj;
    uint32_t d3d_color_obj, d3d_zeta_obj;
    uint32_t d3d_vertex_a_obj, d3d_vertex_b_obj;
    uint32_t d3d_report_obj, d3d_semaphore_obj;
    uint32_t d3d_clip_horizontal, d3d_clip_vertical;
    uint32_t d3d_surface_format, d3d_color_bytes, d3d_depth_bytes;
    uint32_t d3d_surface_pitch_a, d3d_surface_pitch_z;
    uint32_t d3d_window_offset;
    uint32_t d3d_surface_color_offset, d3d_surface_zeta_offset;
    uint32_t d3d_blend_enable, d3d_blend_func_sfactor, d3d_blend_func_dfactor;
    uint32_t d3d_cull_face_enable, d3d_depth_test_enable;
    uint32_t d3d_lighting_enable, d3d_shade_mode;
    float d3d_clip_min, d3d_clip_max;
    uint32_t d3d_cull_face, d3d_front_face;
    uint32_t d3d_light_enable_mask;
    float d3d_inverse_model_view_matrix[12];
    float d3d_composite_matrix[16];
    uint32_t d3d_shader_program, d3d_shader_obj, d3d_shader_offset;
    float d3d_scene_ambient_color[4];
    uint32_t d3d_viewport_horizontal, d3d_viewport_vertical;
    float d3d_viewport_offset[4], d3d_viewport_scale[4];
    uint32_t d3d_transform_program[544][4];
    float d3d_transform_constant[512][4];
    float d3d_light_diffuse_color[8][3];
    float d3d_light_infinite_direction[8][3];
    float d3d_normal[3], d3d_diffuse_color[4];
    uint32_t d3d_vertex_data_array_offset[16];
    uint32_t d3d_vertex_data_array_format_type[16];
    uint32_t d3d_vertex_data_array_format_size[16];
    uint32_t d3d_vertex_data_array_format_stride[16];
    bool d3d_vertex_data_array_format_dx[16];
    uint32_t d3d_begin_end;
    bool d3d_primitive_done, d3d_triangle_flip;
    uint32_t d3d_vertex_index, d3d_attrib_index, d3d_comp_index;
    float d3d_vertex_data[4][16][4];
    uint32_t d3d_index_array_offset, d3d_index_array_dma;
    uint32_t d3d_texture_offset[16], d3d_texture_format[16];
    uint32_t d3d_texture_control1[16], d3d_texture_image_rect[16];
    uint32_t d3d_texture_control3[16];
    uint32_t d3d_semaphore_offset;
    uint32_t d3d_zstencil_clear_value, d3d_color_clear_value;
    uint32_t d3d_clear_surface;
    uint32_t d3d_transform_execution_mode;
    uint32_t d3d_transform_program_load, d3d_transform_program_start;
    uint32_t d3d_transform_constant_load;
    uint32_t d3d_attrib_color, d3d_attrib_tex_coord[10];

    /* ROP and pattern state */
    uint8_t rop;
    uint32_t beta;

    /* Clipping */
    uint32_t clip_yx, clip_hw;

    /* Color key */
    uint32_t chroma_color_fmt, chroma_color;

    /* Pattern state */
    uint32_t patt_shape, patt_type;
    uint32_t patt_bg_color, patt_fg_color;
    bool patt_data_mono[64];
    uint32_t patt_data_color[64];

    /* GDI state */
    uint32_t gdi_operation, gdi_color_fmt, gdi_mono_fmt;
    uint32_t gdi_clip_yx0, gdi_clip_yx1;
    uint32_t gdi_rect_color, gdi_rect_xy;
    uint32_t gdi_rect_yx0, gdi_rect_yx1, gdi_rect_wh;
    uint32_t gdi_bg_color, gdi_fg_color;
    uint32_t gdi_image_swh, gdi_image_dwh, gdi_image_xy;
    uint32_t gdi_words_ptr, gdi_words_left;
    uint32_t *gdi_words;
} GeForceChannel;

/* Main GeForce device state */
struct GeForceState {
    PCIDevice parent_obj;

    /* Basic device info */
    uint32_t card_type;
    uint32_t memsize, memsize_mask;
    uint32_t bar2_size, ramin_flip, class_mask;
    uint8_t *vram;

    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion vram_mem;
    MemoryRegion ramin_mem;

    /* Display state */
    QemuConsole *con;
    uint32_t xres, yres, bpp, pitch;
    bool display_enabled;
    uint8_t *disp_ptr;
    uint32_t disp_offset;
    uint32_t bank_base[2];

    /* VGA compatibility */
    struct {
        uint8_t index;
        uint8_t reg[GEFORCE_CRTC_MAX + 1];
    } crtc;

    /* Hardware cursor */
    struct {
        int16_t x, y;
        uint8_t size;
        bool enabled, bpp32;
        uint32_t offset;
    } hw_cursor;

    /* Interrupt state */
    uint32_t mc_intr_en, mc_enable;
    uint32_t bus_intr, bus_intr_en;
    uint32_t fifo_intr, fifo_intr_en;
    uint32_t graph_intr, graph_nsource, graph_intr_en;
    uint32_t graph_ctx_switch1, graph_ctx_switch2, graph_ctx_switch4;
    uint32_t graph_ctxctl_cur, graph_status;
    uint32_t graph_trapped_addr, graph_trapped_data;
    uint32_t graph_notify, graph_fifo, graph_channel_ctx_table;
    uint32_t crtc_intr, crtc_intr_en;
    uint32_t crtc_start, crtc_config;
    uint32_t crtc_cursor_offset, crtc_cursor_config;

    /* FIFO state */
    uint32_t fifo_ramht, fifo_ramfc, fifo_ramro, fifo_mode;
    uint32_t fifo_cache1_push1, fifo_cache1_put;
    uint32_t fifo_cache1_dma_push, fifo_cache1_dma_instance;
    uint32_t fifo_cache1_dma_put, fifo_cache1_dma_get;
    uint32_t fifo_cache1_ref_cnt, fifo_cache1_pull0;
    uint32_t fifo_cache1_semaphore, fifo_cache1_get;
    uint32_t fifo_grctx_instance;
    uint32_t fifo_cache1_method[GEFORCE_CACHE1_SIZE];
    uint32_t fifo_cache1_data[GEFORCE_CACHE1_SIZE];

    /* Timer state */
    uint32_t timer_intr, timer_intr_en;
    uint32_t timer_num, timer_den;
    uint64_t timer_inittime1, timer_inittime2;
    uint32_t timer_alarm;

    /* RAMDAC state */
    uint32_t ramdac_cu_start_pos;
    uint32_t ramdac_vpll, ramdac_vpll_b;
    uint32_t ramdac_pll_select, ramdac_general_control;

    /* Straps */
    uint32_t straps0_primary, straps0_primary_original;

    /* RMA access */
    uint32_t rma_addr;

    /* Channels */
    GeForceChannel channels[GEFORCE_CHANNEL_COUNT];
    bool acquire_active;

    /* ROP handlers (simplified for QEMU) */
    uint8_t rop_flags[0x100];

    /* Timers */
    QEMUTimer *vblank_timer;

    /* Update flags */
    bool needs_update_tile;
    bool needs_update_dispentire;
    bool needs_update_mode;
    bool double_width;
    bool unlock_special;
};

/* Function prototypes */
void geforce_update_irq(GeForceState *s);
uint64_t geforce_get_current_time(GeForceState *s);

/* Memory access functions */
uint8_t geforce_vram_read8(GeForceState *s, uint32_t addr);
uint16_t geforce_vram_read16(GeForceState *s, uint32_t addr);
uint32_t geforce_vram_read32(GeForceState *s, uint32_t addr);
void geforce_vram_write8(GeForceState *s, uint32_t addr, uint8_t val);
void geforce_vram_write16(GeForceState *s, uint32_t addr, uint16_t val);
void geforce_vram_write32(GeForceState *s, uint32_t addr, uint32_t val);
void geforce_vram_write64(GeForceState *s, uint32_t addr, uint64_t val);

uint8_t geforce_ramin_read8(GeForceState *s, uint32_t addr);
uint32_t geforce_ramin_read32(GeForceState *s, uint32_t addr);
void geforce_ramin_write8(GeForceState *s, uint32_t addr, uint8_t val);
void geforce_ramin_write32(GeForceState *s, uint32_t addr, uint32_t val);

uint8_t geforce_dma_read8(GeForceState *s, uint32_t object, uint32_t addr);
uint16_t geforce_dma_read16(GeForceState *s, uint32_t object, uint32_t addr);
uint32_t geforce_dma_read32(GeForceState *s, uint32_t object, uint32_t addr);
void geforce_dma_write8(GeForceState *s, uint32_t object, uint32_t addr, uint8_t val);
void geforce_dma_write16(GeForceState *s, uint32_t object, uint32_t addr, uint16_t val);
void geforce_dma_write32(GeForceState *s, uint32_t object, uint32_t addr, uint32_t val);
void geforce_dma_write64(GeForceState *s, uint32_t object, uint32_t addr, uint64_t val);

/* Command processing */
bool geforce_execute_command(GeForceState *s, uint32_t chid, uint32_t subc, 
                            uint32_t method, uint32_t param);
void geforce_fifo_process(GeForceState *s, uint32_t chid);

#endif /* HW_DISPLAY_GEFORCE_H */