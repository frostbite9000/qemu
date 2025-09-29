/*
 * QEMU 3dfx Voodoo Banshee/Voodoo3 emulation
 * Main device implementation
 *
 * Ported from Bochs implementation by Volker Ruppert
 * 
 * Copyright (c) 2012-2024 The Bochs Project
 * Copyright (c) 2025 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "trace.h"
#include "hw/display/voodoo.h"
#include "hw/display/voodoo_regs.h"

/* Device identification */
#define VOODOO_BANSHEE_DEVICE_NAME  "3dfx Voodoo Banshee"
#define VOODOO_3_DEVICE_NAME        "3dfx Voodoo3"

/* Memory access macros */
#define VOODOO_MEM_VALID(s, addr, size) \
    ((addr) + (size) <= (s)->vram_size)

/* Register access helpers */
static inline uint32_t voodoo_reg_read(VoodooBansheeState *s, uint32_t offset)
{
    if (offset < sizeof(s->regs)) {
        return s->regs[offset / 4];
    }
    return 0;
}

static inline void voodoo_reg_write(VoodooBansheeState *s, uint32_t offset, 
                                   uint32_t value)
{
    if (offset < sizeof(s->regs)) {
        s->regs[offset / 4] = value;
    }
}

/* VRAM access functions */
uint32_t voodoo_mem_readl(VoodooBansheeState *s, uint32_t addr)
{
    if (VOODOO_MEM_VALID(s, addr, 4)) {
        return ldl_le_p(s->vram_ptr + addr);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "voodoo: invalid read at 0x%08x\n", addr);
    return 0;
}

void voodoo_mem_writel(VoodooBansheeState *s, uint32_t addr, uint32_t val)
{
    if (VOODOO_MEM_VALID(s, addr, 4)) {
        stl_le_p(s->vram_ptr + addr, val);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "voodoo: invalid write at 0x%08x\n", addr);
    }
}

/* Display update function */
static void voodoo_update_display(void *opaque)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    
    if (!s->display_enabled || !surface) {
        return;
    }
    
    /* Update display based on current video mode */
    if (s->width > 0 && s->height > 0) {
        int src_linesize = s->pitch;
        int dst_linesize = surface_stride(surface);
        uint8_t *src = s->vram_ptr + s->display_start;
        uint8_t *dst = surface_data(surface);
        
        /* Simple framebuffer copy for now - will be enhanced with format conversion */
        if (s->depth == 32 && surface_bits_per_pixel(surface) == 32) {
            for (int y = 0; y < s->height && y < surface_height(surface); y++) {
                memcpy(dst, src, MIN(src_linesize, dst_linesize));
                src += src_linesize;
                dst += dst_linesize;
            }
        }
        
        dpy_gfx_update(s->con, 0, 0, s->width, s->height);
    }
}

static const GraphicHwOps voodoo_gfx_ops = {
    .gfx_update = voodoo_update_display,
};

/* Hardware cursor support */
void voodoo_update_cursor(VoodooBansheeState *s)
{
    /* Basic cursor implementation stub */
    qemu_log_mask(LOG_UNIMP, "voodoo: hardware cursor not fully implemented\n");
}

/* Video overlay support */
void voodoo_update_overlay(VoodooBansheeState *s)
{  
    /* Basic overlay implementation stub */
    qemu_log_mask(LOG_UNIMP, "voodoo: video overlay not fully implemented\n");
}

/* Memory-mapped I/O read */
static uint64_t voodoo_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(opaque);
    uint32_t offset = addr;
    uint32_t value = 0;
    
    switch (offset) {
    case VOODOO_REG_SSTATUS:
        /* Status register - indicate device ready */
        value = 0x80000000;  /* FBI graphics engine idle */
        if (s->retrace_active) {
            value |= 0x40;  /* Vertical retrace active */
        }
        break;
        
    case VOODOO_REG_PCIINIT0:
        value = s->pci_init_enable;
        break;
        
    case VOODOO_REG_PCIINIT1:
        value = s->pci_init_remap;
        break;
        
    case VOODOO_REG_SIPMONITOR:
    case VOODOO_REG_VGAINIT0:
    case VOODOO_REG_VGAINIT1:
    case VOODOO_REG_DRAMMODE0:
    case VOODOO_REG_DRAMMODE1:
    case VOODOO_REG_AGPINIT0:
    case VOODOO_REG_MISCINIT0:
    case VOODOO_REG_MISCINIT1:
    case VOODOO_REG_DRAMINIT0:
    case VOODOO_REG_DRAMINIT1:
        value = voodoo_reg_read(s, offset);
        break;
        
    /* 2D registers */
    case VOODOO_2D_CLIP0MIN:
        value = s->twoD.clip0_min;
        break;
    case VOODOO_2D_CLIP0MAX:
        value = s->twoD.clip0_max;
        break;
    case VOODOO_2D_DSTBASE:
        value = s->twoD.dst_base;
        break;
    case VOODOO_2D_DSTFORMAT:
        value = s->twoD.dst_format;
        break;
    case VOODOO_2D_SRCBASE:
        value = s->twoD.src_base;
        break;
    case VOODOO_2D_SRCFORMAT:
        value = s->twoD.src_format;
        break;
    case VOODOO_2D_SRCSIZE:
        value = s->twoD.src_size;
        break;
    case VOODOO_2D_SRCXY:
        value = s->twoD.src_xy;
        break;
    case VOODOO_2D_COLORBACK:
        value = s->twoD.color_back;
        break;
    case VOODOO_2D_COLORFORE:
        value = s->twoD.color_fore;
        break;
    case VOODOO_2D_DSTSIZE:
        value = s->twoD.dst_size;
        break;
    case VOODOO_2D_DSTXY:
        value = s->twoD.dst_xy;
        break;
    case VOODOO_2D_COMMAND_2D:
        value = s->twoD.command;
        break;
        
    /* Video overlay registers */
    case VOODOO_VIDPROCCFG:
        value = s->overlay.vidproc_cfg;
        break;
    case VOODOO_HWCURPATADDR:
        value = s->cursor.addr;
        break;
    case VOODOO_HWCURLOC:
        value = (s->cursor.y << 16) | s->cursor.x;
        break;
    case VOODOO_HWCURC0:
        value = s->cursor.color0;
        break;
    case VOODOO_HWCURC1:
        value = s->cursor.color1;
        break;
        
    /* 3D registers */
    case VOODOO_3D_STATUS:
    case VOODOO_3D_INTRCTRL:
    case VOODOO_3D_VGAINIT0:
    case VOODOO_3D_VGAINIT1:
    case VOODOO_3D_DRAMMODE0:
    case VOODOO_3D_DRAMMODE1:
        value = voodoo_3d_reg_read(s, offset);
        break;
        
    default:
        qemu_log_mask(LOG_UNIMP, "voodoo: unimplemented mmio read at 0x%04x\n", 
                      offset);
        break;
    }
    
    trace_voodoo_mmio_read(offset, value, size);
    return value;
}

/* Memory-mapped I/O write */
static void voodoo_mmio_write(void *opaque, hwaddr addr, uint64_t value, 
                             unsigned size)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(opaque);
    uint32_t offset = addr;
    
    trace_voodoo_mmio_write(offset, value, size);
    
    switch (offset) {
    case VOODOO_REG_PCIINIT0:
        s->pci_init_enable = value;
        break;
        
    case VOODOO_REG_PCIINIT1:
        s->pci_init_remap = value;
        break;
        
    case VOODOO_REG_SIPMONITOR:
        /* Silicon monitor register */
        voodoo_reg_write(s, offset, value);
        break;
        
    case VOODOO_REG_VGAINIT0:
        s->display_enabled = (value & 0x01) != 0;
        s->width = ((value >> 8) & 0x1ff) * 8;
        voodoo_reg_write(s, offset, value);
        break;
        
    case VOODOO_REG_VGAINIT1:
        s->height = (value & 0x1fff);
        s->pitch = ((value >> 16) & 0x3fff) * 8;
        voodoo_reg_write(s, offset, value);
        break;
        
    case VOODOO_REG_DRAMMODE0:
    case VOODOO_REG_DRAMMODE1:
    case VOODOO_REG_AGPINIT0:
    case VOODOO_REG_MISCINIT0:
    case VOODOO_REG_MISCINIT1:
    case VOODOO_REG_DRAMINIT0:
    case VOODOO_REG_DRAMINIT1:
        voodoo_reg_write(s, offset, value);
        break;
        
    /* 2D registers */
    case VOODOO_2D_CLIP0MIN:
        s->twoD.clip0_min = value;
        break;
    case VOODOO_2D_CLIP0MAX:
        s->twoD.clip0_max = value;
        break;
    case VOODOO_2D_DSTBASE:
        s->twoD.dst_base = value & 0xffffff;  /* 24-bit address */
        break;
    case VOODOO_2D_DSTFORMAT:
        s->twoD.dst_format = value;
        break;
    case VOODOO_2D_SRCBASE:
        s->twoD.src_base = value & 0xffffff;  /* 24-bit address */
        break;
    case VOODOO_2D_SRCFORMAT:
        s->twoD.src_format = value;
        break;
    case VOODOO_2D_SRCSIZE:
        s->twoD.src_size = value;
        break;
    case VOODOO_2D_SRCXY:
        s->twoD.src_xy = value;
        break;
    case VOODOO_2D_COLORBACK:
        s->twoD.color_back = value;
        break;
    case VOODOO_2D_COLORFORE:
        s->twoD.color_fore = value;
        break;
    case VOODOO_2D_DSTSIZE:
        s->twoD.dst_size = value;
        break;
    case VOODOO_2D_DSTXY:
        s->twoD.dst_xy = value;
        break;
    case VOODOO_2D_COMMAND_2D:
        s->twoD.command = value;
        break;
    case VOODOO_2D_LAUNCH_2D:
        /* Execute 2D operation based on command */
        {
            uint32_t cmd = s->twoD.command & 0x7;
            switch (cmd) {
            case 0: /* NOP */
                break;
            case 1: /* Screen-to-screen BitBlt */
                voodoo_2d_bitblt(s);
                break;
            case 2: /* Pattern fill */
                voodoo_2d_pattern_fill(s);
                break;
            case 3: /* Screen-to-screen stretch BitBlt */
                qemu_log_mask(LOG_UNIMP, "voodoo: stretch BitBlt not implemented\n");
                break;
            case 4: /* Host-to-screen BitBlt */
                qemu_log_mask(LOG_UNIMP, "voodoo: host-to-screen BitBlt not implemented\n");
                break;
            default:
                qemu_log_mask(LOG_GUEST_ERROR, "voodoo: unknown 2D command %d\n", cmd);
                break;
            }
        }
        break;
        
    /* Video overlay registers */
    case VOODOO_VIDPROCCFG:
        s->overlay.vidproc_cfg = value;
        break;
    case VOODOO_HWCURPATADDR:
        s->cursor.addr = value;
        break;
    case VOODOO_HWCURLOC:
        s->cursor.x = value & 0xffff;
        s->cursor.y = (value >> 16) & 0xffff;
        break;
    case VOODOO_HWCURC0:
        s->cursor.color0 = value;
        break;
    case VOODOO_HWCURC1:
        s->cursor.color1 = value;
        break;
        
    /* 3D registers */
    case VOODOO_3D_INTRCTRL:
    case VOODOO_3D_VGAINIT0:
    case VOODOO_3D_VGAINIT1:
    case VOODOO_3D_DRAMMODE0:
    case VOODOO_3D_DRAMMODE1:
    case VOODOO_3D_MISCINIT0:
    case VOODOO_3D_MISCINIT1:
        voodoo_3d_reg_write(s, offset, value);
        break;
        
    default:
        qemu_log_mask(LOG_UNIMP, "voodoo: unimplemented mmio write at 0x%04x\n", 
                      offset);
        voodoo_reg_write(s, offset, value);
        break;
    }
}

static const MemoryRegionOps voodoo_mmio_ops = {
    .read = voodoo_mmio_read,
    .write = voodoo_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* Linear Frame Buffer access */
static uint64_t voodoo_lfb_read(void *opaque, hwaddr addr, unsigned size)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(opaque);
    
    if (addr + size <= s->vram_size) {
        switch (size) {
        case 1:
            return s->vram_ptr[addr];
        case 2:
            return lduw_le_p(s->vram_ptr + addr);
        case 4:
            return ldl_le_p(s->vram_ptr + addr);
        case 8:
            return ldq_le_p(s->vram_ptr + addr);
        }
    }
    
    qemu_log_mask(LOG_GUEST_ERROR, "voodoo: invalid LFB read at 0x%08lx\n", addr);
    return 0;
}

static void voodoo_lfb_write(void *opaque, hwaddr addr, uint64_t value, 
                            unsigned size)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(opaque);
    
    if (addr + size <= s->vram_size) {
        switch (size) {
        case 1:
            s->vram_ptr[addr] = value;
            break;
        case 2:
            stw_le_p(s->vram_ptr + addr, value);
            break;
        case 4:
            stl_le_p(s->vram_ptr + addr, value);
            break;
        case 8:
            stq_le_p(s->vram_ptr + addr, value);
            break;
        }
        
        /* Mark display region as dirty for updates */
        memory_region_set_dirty(&s->vram, addr, size);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "voodoo: invalid LFB write at 0x%08lx\n", addr);
    }
}

static const MemoryRegionOps voodoo_lfb_ops = {
    .read = voodoo_lfb_read,
    .write = voodoo_lfb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* Device reset */
static void voodoo_reset(DeviceState *dev)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(dev);
    
    /* Reset all registers to default values */
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->io_regs, 0, sizeof(s->io_regs));
    
    /* Reset graphics engine state */
    memset(&s->twoD, 0, sizeof(s->twoD));
    memset(&s->threeD, 0, sizeof(s->threeD));
    memset(&s->cursor, 0, sizeof(s->cursor));
    memset(&s->overlay, 0, sizeof(s->overlay));
    
    /* Initialize 3D engine */
    voodoo_3d_init(s);
    
    /* Set default display parameters */
    s->width = 640;
    s->height = 480;
    s->depth = 8;
    s->pitch = 640;
    s->display_start = 0;
    s->display_enabled = false;
    s->retrace_active = false;
    
    /* Initialize chip ID */
    s->chip_id = s->is_voodoo3 ? 0x0005 : 0x0003;
    s->memsize = s->vram_size;
}

/* Device realization */
static void voodoo_realize(PCIDevice *pci_dev, Error **errp)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(pci_dev);
    Object *obj = OBJECT(pci_dev);
    
    /* Validate memory size */
    if (s->vram_size < 4 * MiB) {
        error_setg(errp, "voodoo-banshee: video memory too small (minimum 4MB)");
        return;
    }
    if (s->vram_size > 32 * MiB) {
        error_setg(errp, "voodoo-banshee: video memory too large (maximum 32MB)");
        return;
    }
    
    /* Allocate video memory */
    memory_region_init_ram(&s->vram, obj, "voodoo-banshee.vram", 
                          s->vram_size, &error_fatal);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    
    /* Initialize memory regions */
    memory_region_init_io(&s->mmio, obj, &voodoo_mmio_ops, s,
                          "voodoo-banshee.mmio", VOODOO_2D_SIZE);
    
    memory_region_init_io(&s->lfb, obj, &voodoo_lfb_ops, s,
                          "voodoo-banshee.lfb", s->vram_size);
    
    /* Map memory regions to PCI BARs */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->lfb);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    
    /* Initialize PCI configuration */
    pci_set_word(pci_dev->config + PCI_VENDOR_ID, PCI_VENDOR_ID_3DFX);
    pci_set_word(pci_dev->config + PCI_DEVICE_ID, 
                 s->is_voodoo3 ? PCI_DEVICE_ID_VOODOO_3 : PCI_DEVICE_ID_VOODOO_BANSHEE);
    pci_set_byte(pci_dev->config + PCI_CLASS_PROG, 0x00);
    pci_set_word(pci_dev->config + PCI_CLASS_DEVICE, PCI_CLASS_DISPLAY_VGA);
    pci_set_byte(pci_dev->config + PCI_REVISION_ID, 0x01);
    
    /* Initialize graphics console */
    s->con = graphic_console_init(DEVICE(pci_dev), 0, &voodoo_gfx_ops, s);
    qemu_console_resize(s->con, 640, 480);
}

/* Device instance initialization */
static void voodoo_instance_init(Object *obj)
{
    VoodooBansheeState *s = VOODOO_BANSHEE(obj);
    
    /* Set default configuration */
    s->vram_size = VOODOO_BANSHEE_MEMSIZE;
    s->is_voodoo3 = false;
}

/* Property definitions */
static const Property voodoo_properties[] = {
    DEFINE_PROP_UINT32("vram_size", VoodooBansheeState, vram_size, 
                       VOODOO_BANSHEE_MEMSIZE),
    DEFINE_PROP_BOOL("voodoo3", VoodooBansheeState, is_voodoo3, false),
};

/* VMState for migration */
static const VMStateDescription vmstate_voodoo = {
    .name = "voodoo-banshee",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, VoodooBansheeState),
        VMSTATE_UINT32_ARRAY(regs, VoodooBansheeState, 256),
        VMSTATE_UINT32_ARRAY(io_regs, VoodooBansheeState, 64),
        VMSTATE_UINT32(width, VoodooBansheeState),
        VMSTATE_UINT32(height, VoodooBansheeState),
        VMSTATE_UINT32(depth, VoodooBansheeState),
        VMSTATE_UINT32(pitch, VoodooBansheeState),
        VMSTATE_UINT32(display_start, VoodooBansheeState),
        VMSTATE_BOOL(display_enabled, VoodooBansheeState),
        VMSTATE_BOOL(retrace_active, VoodooBansheeState),
        VMSTATE_END_OF_LIST()
    }
};

/* Class initialization */
static void voodoo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    
    pci->realize = voodoo_realize;
    pci->vendor_id = PCI_VENDOR_ID_3DFX;
    pci->device_id = PCI_DEVICE_ID_VOODOO_BANSHEE;
    pci->class_id = PCI_CLASS_DISPLAY_VGA;
    
    device_class_set_legacy_reset(dc, voodoo_reset);
    dc->vmsd = &vmstate_voodoo;
    device_class_set_props(dc, voodoo_properties);
    dc->desc = "3dfx Voodoo Banshee/Voodoo3";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* Type registration */
static const TypeInfo voodoo_type_info = {
    .name = TYPE_VOODOO_BANSHEE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VoodooBansheeState),
    .instance_init = voodoo_instance_init,
    .class_init = voodoo_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void voodoo_register_types(void)
{
    type_register_static(&voodoo_type_info);
}

type_init(voodoo_register_types)