/*
 * QEMU 3dfx Voodoo Banshee/Voodoo3 emulation
 * Register definitions and constants
 *
 * Ported from Bochs implementation by Volker Ruppert
 * 
 * Copyright (c) 2012-2024 The Bochs Project
 * Copyright (c) 2025 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_DISPLAY_VOODOO_REGS_H
#define HW_DISPLAY_VOODOO_REGS_H

/* PCI Configuration */
#define PCI_VENDOR_ID_3DFX          0x121a
#define PCI_DEVICE_ID_VOODOO_BANSHEE 0x0003
#define PCI_DEVICE_ID_VOODOO_3      0x0005

/* Memory sizes */
#define VOODOO_BANSHEE_MEMSIZE      16*1024*1024  /* 16MB */
#define VOODOO_3_MEMSIZE           32*1024*1024   /* 32MB */

/* Register offsets - Configuration space */
#define VOODOO_REG_PCIINIT0         0x04
#define VOODOO_REG_SIPMONITOR       0x08
#define VOODOO_REG_SSTATUS          0x0c
#define VOODOO_REG_PCIINIT1         0x10
#define VOODOO_REG_VGAINIT0         0x28
#define VOODOO_REG_VGAINIT1         0x2c
#define VOODOO_REG_DRAMMODE0        0x30
#define VOODOO_REG_DRAMMODE1        0x34
#define VOODOO_REG_AGPINIT0         0x38
#define VOODOO_REG_MISCINIT0        0x40
#define VOODOO_REG_MISCINIT1        0x44
#define VOODOO_REG_DRAMINIT0        0x48
#define VOODOO_REG_DRAMINIT1        0x4c
#define VOODOO_REG_AGPINIT1         0x50
#define VOODOO_REG_AGPINIT2         0x54
#define VOODOO_REG_TMUGBEINIT       0x58
#define VOODOO_REG_VGAINIT2         0x5c

/* 2D registers - separate address space at 0x100 */
#define VOODOO_2D_CLIP0MIN          0x108
#define VOODOO_2D_CLIP0MAX          0x10c
#define VOODOO_2D_DSTBASE           0x110
#define VOODOO_2D_DSTFORMAT         0x114
#define VOODOO_2D_SRCBASE           0x134
#define VOODOO_2D_COMMANDEXTRA_2D   0x138
#define VOODOO_2D_CLIP1MIN          0x14c
#define VOODOO_2D_CLIP1MAX          0x150
#define VOODOO_2D_SRCFORMAT         0x154
#define VOODOO_2D_SRCSIZE           0x158
#define VOODOO_2D_SRCXY             0x15c
#define VOODOO_2D_COLORBACK         0x160
#define VOODOO_2D_COLORFORE         0x164
#define VOODOO_2D_DSTSIZE           0x168
#define VOODOO_2D_DSTXY             0x16c
#define VOODOO_2D_COMMAND_2D        0x170
#define VOODOO_2D_LAUNCH_2D         0x180
#define VOODOO_2D_PATTERNBASE       0x200

/* VGA registers */
#define VOODOO_VGA_CRTC_INDEX       0x3d4
#define VOODOO_VGA_CRTC_DATA        0x3d5
#define VOODOO_VGA_SEQ_INDEX        0x3c4
#define VOODOO_VGA_SEQ_DATA         0x3c5
#define VOODOO_VGA_MISC_OUTPUT      0x3c2
#define VOODOO_VGA_FEATURE_CTRL     0x3da
#define VOODOO_VGA_INPUT_STATUS_1   0x3da

/* Video overlay registers */
#define VOODOO_VIDPROCCFG           0x5c
#define VOODOO_HWCURPATADDR         0x60
#define VOODOO_HWCURLOC             0x64
#define VOODOO_HWCURC0              0x68
#define VOODOO_HWCURC1              0x6c
#define VOODOO_VIDINFORMAT          0x70
#define VOODOO_VIDINSTATUS          0x74
#define VOODOO_VIDSERIALPARALLELPORT 0x78
#define VOODOO_VIDINADDR0           0x7c
#define VOODOO_VIDINADDR1           0x80
#define VOODOO_VIDINADDR2           0x84
#define VOODOO_VIDINSTRIDE          0x88
#define VOODOO_VIDCURLIN            0x8c
#define VOODOO_VIDSCREENSIZE        0x90
#define VOODOO_VIDOVRSTARTCRD       0x94
#define VOODOO_VIDOVRENDCRD         0x98
#define VOODOO_VIDOVRDUDX           0x9c
#define VOODOO_VIDOVRDUDXOFF        0xa0
#define VOODOO_VIDOVRDVDY           0xa4
#define VOODOO_VIDOVRDVDYOFF        0xa8
#define VOODOO_VIDDESKSTART         0xac
#define VOODOO_VIDDESKSTRIDE        0xb0
#define VOODOO_VIDINIADDR           0xb4
#define VOODOO_VIDININXADDR         0xb8
#define VOODOO_VIDININXDATA         0xbc

/* 3D registers - separate address space at 0x300 */
#define VOODOO_3D_STATUS            0x300
#define VOODOO_3D_INTRCTRL          0x304
#define VOODOO_3D_PCIINIT0          0x308
#define VOODOO_3D_SIPMONITOR        0x30c
#define VOODOO_3D_LFBMEMORYCONFIG   0x310
#define VOODOO_3D_MISCINIT0         0x314
#define VOODOO_3D_MISCINIT1         0x318
#define VOODOO_3D_DRAMINIT0         0x31c
#define VOODOO_3D_DRAMINIT1         0x320
#define VOODOO_3D_AGPINIT0          0x324
#define VOODOO_3D_AGPINIT1          0x328
#define VOODOO_3D_VGAINIT0          0x32c
#define VOODOO_3D_VGAINIT1          0x330
#define VOODOO_3D_DRAMMODE0         0x334
#define VOODOO_3D_DRAMMODE1         0x338

/* Banshee 2D command register bits */
#define VOODOO_2D_ROP_MASK          0x1f
#define VOODOO_2D_X_POSITIVE        (1 << 8)
#define VOODOO_2D_Y_POSITIVE        (1 << 9)
#define VOODOO_2D_PATTERN_MONO      (1 << 13)
#define VOODOO_2D_TRANSP            (1 << 14)
#define VOODOO_2D_STIPPLE_LINE      (1 << 15)

/* ROP operations */
#define VOODOO_ROP_COPY             0x0c
#define VOODOO_ROP_XOR              0x06
#define VOODOO_ROP_AND              0x08
#define VOODOO_ROP_OR               0x0e

/* Pixel formats */
#define VOODOO_PIXFMT_8BPP          0
#define VOODOO_PIXFMT_16BPP_565     1
#define VOODOO_PIXFMT_24BPP         2
#define VOODOO_PIXFMT_32BPP         3

/* Memory regions */
#define VOODOO_MEM_FB_BASE          0x00000000
#define VOODOO_MEM_TEX_BASE         0x00200000
#define VOODOO_MEM_IO_BASE          0x00000000
#define VOODOO_MEM_2D_BASE          0x00000000

/* Memory region sizes */
#define VOODOO_IO_SIZE              0x100
#define VOODOO_2D_SIZE              0x400  /* Power of 2 to accommodate all registers */
#define VOODOO_3D_SIZE              0x200

/* Banshee specific constants */
#define BANSHEE_MAX_PIXCLOCK        270000000
#define BANSHEE_AGP_REGS            8

/* Video overlay constants */
#define VOODOO_OVERLAY_MAX_WIDTH    1024
#define VOODOO_OVERLAY_MAX_HEIGHT   768

/* Hardware cursor constants */
#define VOODOO_CURSOR_SIZE          64
#define VOODOO_CURSOR_PLANE_SIZE    (VOODOO_CURSOR_SIZE * VOODOO_CURSOR_SIZE / 8)

#endif /* HW_DISPLAY_VOODOO_REGS_H */