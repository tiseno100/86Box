/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Trident TGUI9400CXi and TGUI9440 emulation.
 *
 *		TGUI9400CXi has extended write modes, controlled by extended
 *		GDC registers :
 *
 *		GDC[0x10] - Control
 *		      bit 0 - pixel width (1 = 16 bit, 0 = 8 bit)
 *		      bit 1 - mono->colour expansion (1 = enabled,
 *			      0 = disabled)
 *		      bit 2 - mono->colour expansion transparency
 *			      (1 = transparent, 0 = opaque)
 *		      bit 3 - extended latch copy
 *		GDC[0x11] - Background colour (low byte)
 *		GDC[0x12] - Background colour (high byte)
 *		GDC[0x14] - Foreground colour (low byte)
 *		GDC[0x15] - Foreground colour (high byte)
 *		GDC[0x17] - Write mask (low byte)
 *		GDC[0x18] - Write mask (high byte)
 *
 *		Mono->colour expansion will expand written data 8:1 to 8/16
 *		consecutive bytes.
 *		MSB is processed first. On word writes, low byte is processed
 *		first. 1 bits write foreground colour, 0 bits write background
 *		colour unless transparency is enabled.
 *		If the relevant bit is clear in the write mask then the data
 *		is not written.
 *
 *		With 16-bit pixel width, each bit still expands to one byte,
 *		so the TGUI driver doubles up monochrome data.
 *
 *		While there is room in the register map for three byte colours,
 *		I don't believe 24-bit colour is supported. The TGUI9440
 *		blitter has the same limitation.
 *
 *		I don't think double word writes are supported.
 *
 *		Extended latch copy uses an internal 16 byte latch. Reads load
 *		the latch, writing writes out 16 bytes. I don't think the
 *		access size or host data has any affect, but the Windows 3.1
 *		driver always reads bytes and write words of 0xffff.
 *
 *
 *
 * Authors:	Sarah Walker, <http://pcem-emulator.co.uk/>
 *		Miran Grca, <mgrca8@gmail.com>
 *
 *		Copyright 2008-2019 Sarah Walker.
 *		Copyright 2016-2019 Miran Grca.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/io.h>
#include <86box/timer.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/device.h>
#include "cpu.h"
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include <86box/vid_svga_render.h>

/*TGUI9400CXi has extended write modes, controlled by extended GDC registers :
        
  GDC[0x10] - Control
        bit 0 - pixel width (1 = 16 bit, 0 = 8 bit)
        bit 1 - mono->colour expansion (1 = enabled, 0 = disabled)
        bit 2 - mono->colour expansion transparency (1 = tranparent, 0 = opaque)
        bit 3 - extended latch copy
  GDC[0x11] - Background colour (low byte)
  GDC[0x12] - Background colour (high byte)
  GDC[0x14] - Foreground colour (low byte)
  GDC[0x15] - Foreground colour (high byte)
  GDC[0x17] - Write mask (low byte)
  GDC[0x18] - Write mask (high byte)
  
  Mono->colour expansion will expand written data 8:1 to 8/16 consecutive bytes.
  MSB is processed first. On word writes, low byte is processed first. 1 bits write
  foreground colour, 0 bits write background colour unless transparency is enabled.
  If the relevant bit is clear in the write mask then the data is not written.

  With 16-bit pixel width, each bit still expands to one byte, so the TGUI driver
  doubles up monochrome data.
  
  While there is room in the register map for three byte colours, I don't believe
  24-bit colour is supported. The TGUI9440 blitter has the same limitation.
  
  I don't think double word writes are supported.
  
  Extended latch copy uses an internal 16 byte latch. Reads load the latch, writing
  writes out 16 bytes. I don't think the access size or host data has any affect,
  but the Windows 3.1 driver always reads bytes and write words of 0xffff.*/  

#define ROM_TGUI_9400CXI	"roms/video/tgui9440/9400CXI.vbi"
#define ROM_TGUI_9440		"roms/video/tgui9440/9440.VBI"

#define EXT_CTRL_16BIT            0x01
#define EXT_CTRL_MONO_EXPANSION   0x02
#define EXT_CTRL_MONO_TRANSPARENT 0x04
#define EXT_CTRL_LATCH_COPY       0x08

enum
{
        TGUI_9400CXI = 0,
        TGUI_9440
};

typedef struct tgui_t
{
        mem_mapping_t linear_mapping;
        mem_mapping_t accel_mapping;

        rom_t bios_rom;
        
        svga_t svga;
	int pci;
        
        int type;

        struct
        {
        	uint16_t src_x, src_y;
        	uint16_t dst_x, dst_y;
        	uint16_t size_x, size_y;
        	uint16_t fg_col, bg_col;
        	uint8_t rop;
        	uint16_t flags;
        	uint8_t pattern[0x80];
        	int command;
        	int offset;
        	uint8_t ger22;
	
        	int x, y;
        	uint32_t src, dst, src_old, dst_old;
        	int pat_x, pat_y;
        	int use_src;
	
        	int pitch, bpp;
			int val_access;

                uint16_t tgui_pattern[8][8];
        } accel;
        
        uint8_t ext_gdc_regs[16]; /*TGUI9400CXi only*/
        uint8_t copy_latch[16];

        uint8_t tgui_3d8, tgui_3d9, port3CD;
        int oldmode;
        uint8_t oldctrl1;
        uint8_t oldctrl2,newctrl2;

        uint32_t linear_base, linear_size;
        
        int ramdac_state;
        uint8_t ramdac_ctrl;
        
        int clock_m, clock_n, clock_k;
        
        uint32_t vram_size, vram_mask;
        
        volatile int write_blitter;
} tgui_t;

video_timings_t timing_tgui_vlb = {VIDEO_BUS, 4,  8, 16,   4,  8, 16};
video_timings_t timing_tgui_pci = {VIDEO_PCI, 4,  8, 16,   4,  8, 16};

void tgui_recalcmapping(tgui_t *tgui);

uint8_t tgui_accel_read(uint32_t addr, void *priv);
uint16_t tgui_accel_read_w(uint32_t addr, void *priv);
uint32_t tgui_accel_read_l(uint32_t addr, void *priv);

void tgui_accel_write(uint32_t addr, uint8_t val, void *priv);
void tgui_accel_write_w(uint32_t addr, uint16_t val, void *priv);
void tgui_accel_write_l(uint32_t addr, uint32_t val, void *priv);

void tgui_accel_write_fb_b(uint32_t addr, uint8_t val, void *priv);
void tgui_accel_write_fb_w(uint32_t addr, uint16_t val, void *priv);
void tgui_accel_write_fb_l(uint32_t addr, uint32_t val, void *priv);

static uint8_t tgui_ext_linear_read(uint32_t addr, void *p);
static void tgui_ext_linear_write(uint32_t addr, uint8_t val, void *p);
static void tgui_ext_linear_writew(uint32_t addr, uint16_t val, void *p);
static void tgui_ext_linear_writel(uint32_t addr, uint32_t val, void *p);

static uint8_t tgui_ext_read(uint32_t addr, void *p);
static void tgui_ext_write(uint32_t addr, uint8_t val, void *p);
static void tgui_ext_writew(uint32_t addr, uint16_t val, void *p);
static void tgui_ext_writel(uint32_t addr, uint32_t val, void *p);


/*Remap address for chain-4/doubleword style layout*/
static __inline uint32_t
dword_remap(uint32_t in_addr)
{
        return ((in_addr << 2) & 0x3fff0) |
                ((in_addr >> 14) & 0xc) |
                (in_addr & ~0x3fffc);
}
static __inline uint32_t
dword_remap_w(uint32_t in_addr)
{
        return ((in_addr << 2) & 0x1fff8) |
                ((in_addr >> 14) & 0x6) |
                (in_addr & ~0x1fffe);
}

void tgui_out(uint16_t addr, uint8_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
        svga_t *svga = &tgui->svga;

        uint8_t old;

        if (((addr&0xFFF0) == 0x3D0 || (addr&0xFFF0) == 0x3B0) && !(svga->miscout & 1)) addr ^= 0x60;

        switch (addr)
        {
                case 0x3C5:
                switch (svga->seqaddr & 0xf)
                {
                        case 0xB: 
                        tgui->oldmode=1; 
                        break;
                        case 0xC: 
                        if (svga->seqregs[0xe] & 0x80) 
				svga->seqregs[0xc] = val; 
                        break;
                        case 0xd: 
                        if (tgui->oldmode)
                                tgui->oldctrl2 = val; 
                        else 
                                tgui->newctrl2=val; 
                        break;
                        case 0xE:
                        if (tgui->oldmode)
                                tgui->oldctrl1 = val; 
                        else 
                        {
                                svga->seqregs[0xe] = val ^ 2;
                                svga->write_bank = (svga->seqregs[0xe] & 0xf) * 65536;
                                if (!(svga->gdcreg[0xf] & 1)) 
                                        svga->read_bank = svga->write_bank;
                        }
                        return;
                }
                break;

                case 0x3C6:
                if (tgui->type == TGUI_9400CXI)
                {
                        tkd8001_ramdac_out(addr, val, svga->ramdac, svga);
                        return;
                }
                if (tgui->ramdac_state == 4)
                {
                        tgui->ramdac_state = 0;
                        tgui->ramdac_ctrl = val;
                        switch (tgui->ramdac_ctrl & 0xf0)
                        {
                                case 0x10:
                                svga->bpp = 15;
                                break;
                                case 0x30:
                                svga->bpp = 16;
                                break;
                                case 0xd0:
                                svga->bpp = 24;
                                break;
                                default:
                                svga->bpp = 8;
                                break;
                        }
						svga_recalctimings(svga);
                        return;
                }
				break;
				
                case 0x3C7: case 0x3C8: case 0x3C9:
                if (tgui->type == TGUI_9400CXI)
                {
                        tkd8001_ramdac_out(addr, val, svga->ramdac, svga);
                        return;
                }
                tgui->ramdac_state = 0;
				break;

                case 0x3CF:
                if (svga->gdcaddr == 0x23)
                {
                        svga->dpms = !!(val & 0x03);
                        svga_recalctimings(svga);
                }
                if (tgui->type == TGUI_9400CXI && svga->gdcaddr >= 16 && svga->gdcaddr < 32)
                {
                        old = tgui->ext_gdc_regs[svga->gdcaddr & 15];
                        tgui->ext_gdc_regs[svga->gdcaddr & 15] = val;
                        if (svga->gdcaddr == 16)
				tgui_recalcmapping(tgui);
                        return;
                }
                switch (svga->gdcaddr & 15)
                {
			case 0x6:
			if (svga->gdcreg[6] != val)
			{
				svga->gdcreg[6] = val;
				tgui_recalcmapping(tgui);
			}
			return;
			
                        case 0xE:
                        svga->gdcreg[0xe] = val ^ 2;
                        if ((svga->gdcreg[0xf] & 1) == 1)
                           svga->read_bank = (svga->gdcreg[0xe] & 0xf) * 65536;
                        break;
                        case 0xF:
                        if (val & 1) svga->read_bank = (svga->gdcreg[0xe]  & 0xf)  *65536;
                        else         svga->read_bank = (svga->seqregs[0xe] & 0xf)  *65536;
                        svga->write_bank = (svga->seqregs[0xe] & 0xf) * 65536;
                        break;
                }
                break;
                case 0x3D4:
		svga->crtcreg = val & 0x7f;
                return;
                case 0x3D5:
                if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                        return;
                if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                        val = (svga->crtc[7] & ~0x10) | (val & 0x10);
                old = svga->crtc[svga->crtcreg];
                svga->crtc[svga->crtcreg] = val;
                if (old != val)
                {
                        if (svga->crtcreg < 0xE || svga->crtcreg > 0x10)
                        {
                                svga->fullchange = changeframecount;
                                svga_recalctimings(svga);
                        }
                }
                switch (svga->crtcreg)
                {
			case 0x21:
			if (old != val)
			{
                                if (!tgui->pci)
                                {
               	                        tgui->linear_base = ((val & 0xf) | ((val >> 2) & 0x30)) << 20;
                       	                tgui->linear_size = (val & 0x10) ? 0x200000 : 0x100000;
                                        svga->decode_mask = (val & 0x10) ? 0x1fffff : 0xfffff;
                                }
        			tgui_recalcmapping(tgui);
                        }
			break;

			case 0x40: case 0x41: case 0x42: case 0x43:
			case 0x44: case 0x45: case 0x46: case 0x47:
                        if (tgui->type >= TGUI_9440)
                        {
        			svga->hwcursor.x = (svga->crtc[0x40] | (svga->crtc[0x41] << 8)) & 0x7ff;
        			svga->hwcursor.y = (svga->crtc[0x42] | (svga->crtc[0x43] << 8)) & 0x7ff;
        			svga->hwcursor.xoff = svga->crtc[0x46] & 0x3f;
				svga->hwcursor.yoff = svga->crtc[0x47] & 0x3f;
        			svga->hwcursor.addr = (svga->crtc[0x44] << 10) | ((svga->crtc[0x45] & 0x7) << 18) | (svga->hwcursor.yoff * 8);
                        }
			break;
			
			case 0x50:
                        if (tgui->type >= TGUI_9440)
                        {
        			svga->hwcursor.ena = val & 0x80;
        			svga->hwcursor.xsize = (val & 1) ? 64 : 32;
        			svga->hwcursor.ysize = (val & 1) ? 64 : 32;
                        }
			break;
		}
                return;
                case 0x3D8:
                tgui->tgui_3d8 = val;
                if (svga->gdcreg[0xf] & 4)
                {
                        svga->write_bank = (val & 0x1f) * 65536;
                        if (!(svga->gdcreg[0xf] & 1))
                                svga->read_bank = (val & 0x1f) * 65536;
                }
                return;
                case 0x3D9:
                tgui->tgui_3d9 = val;
                if ((svga->gdcreg[0xf] & 5) == 5)
                        svga->read_bank = (val & 0x1F) * 65536;
                return;
                
                case 0x43c8:
                tgui->clock_n = val & 0x7f;
                tgui->clock_m = (tgui->clock_m & ~1) | (val >> 7);
                break;
                case 0x43c9:
                tgui->clock_m = (tgui->clock_m & ~0x1e) | ((val << 1) & 0x1e);
                tgui->clock_k = (val & 0x10) >> 4;
                break;
        }
        svga_out(addr, val, svga);
}

uint8_t tgui_in(uint16_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
        svga_t *svga = &tgui->svga;
        uint8_t temp;

        if (((addr&0xFFF0) == 0x3D0 || (addr&0xFFF0) == 0x3B0) && !(svga->miscout & 1)) addr ^= 0x60;
        
        switch (addr)
        {
                case 0x3C5:
                if ((svga->seqaddr & 0xf) == 0xb)
                {
                        tgui->oldmode = 0;
                        switch (tgui->type)
                        {
                                case TGUI_9400CXI:
                                return 0x93; /*TGUI9400CXi*/
                                case TGUI_9440:
                                return 0xa3; /*TGUI9440AGi*/
                        }
                }
                if ((svga->seqaddr & 0xf) == 0xd)
                {
                        if (tgui->oldmode)
                                return tgui->oldctrl2;
                        return tgui->newctrl2;
                }
                if ((svga->seqaddr & 0xf) == 0xe)
                {
                        if (tgui->oldmode)
                                return tgui->oldctrl1; 
                }
                break;
                case 0x3C6:
                if (tgui->type == TGUI_9400CXI)
					return tkd8001_ramdac_in(addr, svga->ramdac, svga);
                if (tgui->ramdac_state == 4)
					return tgui->ramdac_ctrl;
				tgui->ramdac_state++;
                break;
                case 0x3C7: case 0x3C8: case 0x3C9:
                if (tgui->type == TGUI_9400CXI)
                        return tkd8001_ramdac_in(addr, svga->ramdac, svga);
                tgui->ramdac_state = 0;
                break;
				case 0x3CD:
				return tgui->port3CD;				
				
                case 0x3CF:
                if (tgui->type == TGUI_9400CXI && svga->gdcaddr >= 16 && svga->gdcaddr < 32)
                        return tgui->ext_gdc_regs[svga->gdcaddr & 15];
                break;
                case 0x3D4:
                return svga->crtcreg;
                case 0x3D5:
                temp = svga->crtc[svga->crtcreg];
                return temp;
                case 0x3d8:
		return tgui->tgui_3d8;
		case 0x3d9:
		return tgui->tgui_3d9;
        }
        return svga_in(addr, svga);
}

void tgui_recalctimings(svga_t *svga)
{
        tgui_t *tgui = (tgui_t *)svga->p;

        if (svga->crtc[0x29] & 0x10)
                svga->rowoffset += 0x100;

        if (tgui->type >= TGUI_9440 && svga->bpp == 24)
                svga->hdisp = (svga->crtc[1] + 1) * 8;
        
        if ((svga->crtc[0x1e] & 0xA0) == 0xA0) svga->ma_latch |= 0x10000;
        if ((svga->crtc[0x27] & 0x01) == 0x01) svga->ma_latch |= 0x20000;
        if ((svga->crtc[0x27] & 0x02) == 0x02) svga->ma_latch |= 0x40000;
        
        if (tgui->oldctrl2 & 0x10)
                svga->rowoffset <<= 1;
        if ((tgui->oldctrl2 & 0x10) || (svga->crtc[0x2a] & 0x40))
                svga->ma_latch <<= 1;

        if (tgui->oldctrl2 & 0x10) /*I'm not convinced this is the right register for this function*/
           svga->lowres=0;

	svga->lowres = !(svga->crtc[0x2a] & 0x40); 

        svga->interlace = svga->crtc[0x1e] & 4;
        if (svga->interlace && tgui->type < TGUI_9440)
                svga->rowoffset >>= 1;
        
        if (tgui->type >= TGUI_9440)
        {
                if (svga->miscout & 8)
                        svga->clock = (cpuclock * (double)(1ull << 32)) / (((tgui->clock_n + 8) * 14318180.0) / ((tgui->clock_m + 2) * (1 << tgui->clock_k)));
                        
                if (svga->gdcreg[0xf] & 0x08)
                        svga->clock *= 2;
                else if (svga->gdcreg[0xf] & 0x40)
                        svga->clock *= 3;
        }
        else
        {
                switch (((svga->miscout >> 2) & 3) | ((tgui->newctrl2 << 2) & 4) | ((tgui->newctrl2 >> 3) & 8))
                {
                        case 0x02: svga->clock = (cpuclock * (double)(1ull << 32)) / 44900000.0; break;
                        case 0x03: svga->clock = (cpuclock * (double)(1ull << 32)) / 36000000.0; break;
                        case 0x04: svga->clock = (cpuclock * (double)(1ull << 32)) / 57272000.0; break;
                        case 0x05: svga->clock = (cpuclock * (double)(1ull << 32)) / 65000000.0; break;
                        case 0x06: svga->clock = (cpuclock * (double)(1ull << 32)) / 50350000.0; break;
                        case 0x07: svga->clock = (cpuclock * (double)(1ull << 32)) / 40000000.0; break;
                        case 0x08: svga->clock = (cpuclock * (double)(1ull << 32)) / 88000000.0; break;
                        case 0x09: svga->clock = (cpuclock * (double)(1ull << 32)) / 98000000.0; break;
                        case 0x0a: svga->clock = (cpuclock * (double)(1ull << 32)) /118800000.0; break;
                        case 0x0b: svga->clock = (cpuclock * (double)(1ull << 32)) /108000000.0; break;
                        case 0x0c: svga->clock = (cpuclock * (double)(1ull << 32)) / 72000000.0; break;
                        case 0x0d: svga->clock = (cpuclock * (double)(1ull << 32)) / 77000000.0; break;
                        case 0x0e: svga->clock = (cpuclock * (double)(1ull << 32)) / 80000000.0; break;
                        case 0x0f: svga->clock = (cpuclock * (double)(1ull << 32)) / 75000000.0; break;
                }
                if (svga->gdcreg[0xf] & 0x08)
                {
                        svga->htotal *= 2;
                        svga->hdisp *= 2;
                        svga->hdisp_time *= 2;
                }
        }
                                
        if ((tgui->oldctrl2 & 0x10) || (svga->crtc[0x2a] & 0x40))
        {
                switch (svga->bpp)
                {
                        case 8: 
                        svga->render = svga_render_8bpp_highres; 
                        break;
                        case 15: 
                        svga->render = svga_render_15bpp_highres;
                        if (tgui->type < TGUI_9440)
                                svga->hdisp /= 2;
                        break;
                        case 16: 
                        svga->render = svga_render_16bpp_highres; 
                        if (tgui->type < TGUI_9440)
                                svga->hdisp /= 2;
                        break;
                        case 24: 
                        svga->render = svga_render_24bpp_highres;
                        if (tgui->type < TGUI_9440)
                                svga->hdisp = (svga->hdisp * 2) / 3;
                        break;
                }
        }
}

void tgui_recalcmapping(tgui_t *tgui)
{
        svga_t *svga = &tgui->svga;

        if (tgui->type == TGUI_9400CXI)
        {
                if (tgui->ext_gdc_regs[0] & EXT_CTRL_LATCH_COPY)
                {
                        mem_mapping_set_handler(&tgui->linear_mapping,
                                        tgui_ext_linear_read, NULL, NULL,
                                        tgui_ext_linear_write, tgui_ext_linear_writew, tgui_ext_linear_writel);
                        mem_mapping_set_handler(&svga->mapping,
                                        tgui_ext_read, NULL, NULL,
                                        tgui_ext_write, tgui_ext_writew, tgui_ext_writel);
                }
                else if (tgui->ext_gdc_regs[0] & EXT_CTRL_MONO_EXPANSION)
                {
                        mem_mapping_set_handler(&tgui->linear_mapping,
                                        svga_read_linear, svga_readw_linear, svga_readl_linear,
                                        tgui_ext_linear_write, tgui_ext_linear_writew, tgui_ext_linear_writel);
                        mem_mapping_set_handler(&svga->mapping,
                                        svga_read, svga_readw, svga_readl,
                                        tgui_ext_write, tgui_ext_writew, tgui_ext_writel);
                }
                else
                {
                        mem_mapping_set_handler(&tgui->linear_mapping,
                                        svga_read_linear,  svga_readw_linear,  svga_readl_linear,
                                        svga_write_linear, svga_writew_linear, svga_writel_linear);
                        mem_mapping_set_handler(&svga->mapping,
                                        svga_read, svga_readw, svga_readl,
                                        svga_write, svga_writew, svga_writel);
                }
        }

	if (svga->crtc[0x21] & 0x20)
	{
                mem_mapping_disable(&svga->mapping);
				pclog("Linear size = %08x, linear base = %08x\n", tgui->linear_size, tgui->linear_base);
                mem_mapping_set_addr(&tgui->linear_mapping, tgui->linear_base, tgui->linear_size);
                if (tgui->type >= TGUI_9440)
                {
                        mem_mapping_enable(&tgui->accel_mapping);
                        mem_mapping_disable(&svga->mapping);
                }
                else
                {
                        switch (svga->gdcreg[6] & 0xC)
                        {
                                case 0x0: /*128k at A0000*/
                                mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                                svga->banked_mask = 0xffff;
                                break;
                                case 0x4: /*64k at A0000*/
                                mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
                                svga->banked_mask = 0xffff;
                                break;
                                case 0x8: /*32k at B0000*/
                                mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                                svga->banked_mask = 0x7fff;
                                break;
                                case 0xC: /*32k at B8000*/
                                mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                                svga->banked_mask = 0x7fff;
                                break;
        		}
                }
	}
	else
	{
                mem_mapping_disable(&tgui->linear_mapping);
                mem_mapping_disable(&tgui->accel_mapping);
                switch (svga->gdcreg[6] & 0xC)
                {
                        case 0x0: /*128k at A0000*/
                        mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                        svga->banked_mask = 0xffff;
                        break;
                        case 0x4: /*64k at A0000*/
                        mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
						mem_mapping_enable(&tgui->accel_mapping);
                        svga->banked_mask = 0xffff;
                        break;
                        case 0x8: /*32k at B0000*/
                        mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                        svga->banked_mask = 0x7fff;
                        break;
                        case 0xC: /*32k at B8000*/
                        mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                        svga->banked_mask = 0x7fff;
                        break;
		}
        }
}

void tgui_hwcursor_draw(svga_t *svga, int displine)
{
        uint32_t dat[2];
        int xx, val;
        int offset = svga->hwcursor_latch.x + svga->hwcursor_latch.xoff;
	int pitch = (svga->hwcursor.xsize == 64) ? 16 : 8;
	int byte, bit;
	uint32_t color;
	uint32_t remapped_addr;
        
        if (svga->interlace && svga->hwcursor_oddeven)
			svga->hwcursor_latch.addr += pitch;

		remapped_addr = dword_remap(svga->hwcursor_latch.addr);

        for (xx = 0; xx < svga->hwcursor.xsize; xx++) {
		byte = remapped_addr + (xx >> 3);
		bit = (7 - (xx & 7));
		dat[0] = (svga->vram[byte] >> bit) & 0x01;			/* AND */
		dat[1] = (svga->vram[(pitch >> 1) + byte] >> bit) & 0x01;	/* XOR */
		val = (dat[0] << 1) | dat[1];
		color = ((uint32_t *)buffer32->line[displine])[svga->x_add + offset + xx];
		if (!!(svga->crtc[0x50] & 0x40)) {
			/* X11 style? */
			switch (val) {
				case 0x0:	/* Sceen data (Transparent curor) */
				case 0x1:	/* Sceen data (Transparent curor) */
					break;
				case 0x2:	/* Palette index 0? */
					color = 0x00000000;
					break;
				case 0x3:	/* Palette index 255? */
					color = 0x00ffffff;
					break;
			}
		} else {
			/* Windows style? */
			switch (val) {
				case 0x0:	/* Palette index 0? */
					color = 0x00000000;
					break;
				case 0x1:	/* Palette index 255? */
					color = 0x00ffffff;
					break;
				case 0x2:	/* Sceen data (Transparent curor) */
					break;
				case 0x3:	/* Inverted screen (XOR cursor) */
					color ^= 0x00ffffff;
					break;
			}
		}
		((uint32_t *)buffer32->line[displine])[svga->x_add + offset + xx] = color;
	}

        svga->hwcursor_latch.addr += pitch;
        
        if (svga->interlace && !svga->hwcursor_oddeven)
                svga->hwcursor_latch.addr += pitch;
}

uint8_t tgui_pci_read(int func, int addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;

        switch (addr)
        {
                case 0x00: return 0x23; /*Trident*/
                case 0x01: return 0x10;
                
                case 0x02: return 0x40; /*TGUI9440 (9682)*/
                case 0x03: return 0x94;
                
                case 0x04: return 0x03; /*Respond to IO and memory accesses*/

                case 0x07: return 1 << 1; /*Medium DEVSEL timing*/
                
                case 0x08: return 0; /*Revision ID*/
                case 0x09: return 0; /*Programming interface*/
                
                case 0x0a: return 0x01; /*Supports VGA interface, XGA compatible*/
                case 0x0b: return 0x03;
                
                case 0x10: return 0x00; /*Linear frame buffer address*/
                case 0x11: return 0x00;
                case 0x12: return tgui->linear_base >> 16;
                case 0x13: return tgui->linear_base >> 24;

                case 0x30: return 0x01; /*BIOS ROM address*/
                case 0x31: return 0x00;
                case 0x32: return 0x0C;
                case 0x33: return 0x00;
        }
        return 0;
}

void tgui_pci_write(int func, int addr, uint8_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
        svga_t *svga = &tgui->svga;

        switch (addr)
        {
                case 0x12:
                tgui->linear_base = (tgui->linear_base & 0xff000000) | ((val & 0xe0) << 16);
                tgui->linear_size = 2 << 20;
				svga->decode_mask = 0x1fffff;
                svga->crtc[0x21] = (svga->crtc[0x21] & ~0xf) | (val >> 4);
                tgui_recalcmapping(tgui);
                break;
                case 0x13:
                tgui->linear_base = (tgui->linear_base & 0xe00000) | (val << 24);
                tgui->linear_size = 2 << 20;
				svga->decode_mask = 0x1fffff;
                svga->crtc[0x21] = (svga->crtc[0x21] & ~0xc0) | (val >> 6);
                tgui_recalcmapping(tgui);
                break;
        }
}

static uint8_t tgui_ext_linear_read(uint32_t addr, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;
        int c;

        cycles -= video_timing_read_b;

        addr &= svga->decode_mask;
        if (addr >= svga->vram_max)
                return 0xff;
        
        addr &= ~0xf;
		addr = dword_remap(addr);

        for (c = 0; c < 16; c++) {
                tgui->copy_latch[c] = svga->vram[addr+c];
				addr += ((c & 3) == 3) ? 13 : 1;
		}
		
        return svga->vram[addr & svga->vram_mask]; 
}

static uint8_t tgui_ext_read(uint32_t addr, void *p)
{
        svga_t *svga = (svga_t *)p;
        
        addr = (addr & svga->banked_mask) + svga->read_bank;
        
        return tgui_ext_linear_read(addr, svga);
}

static void tgui_ext_linear_write(uint32_t addr, uint8_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;
        int c;
        uint8_t fg[2] = {tgui->ext_gdc_regs[4], tgui->ext_gdc_regs[5]};
        uint8_t bg[2] = {tgui->ext_gdc_regs[1], tgui->ext_gdc_regs[2]};
        uint8_t mask = tgui->ext_gdc_regs[7];

        cycles -= video_timing_write_b;

        addr &= svga->decode_mask;
        if (addr >= svga->vram_max)
                return;
        addr &= svga->vram_mask;
        addr &= (tgui->ext_gdc_regs[0] & 8) ? ~0xf : ~0x7;

        addr = dword_remap(addr);
        svga->changedvram[addr >> 12] = changeframecount;
        
        switch (tgui->ext_gdc_regs[0] & 0xf)
        {
                /*8-bit mono->colour expansion, unmasked*/
                case 2:
                for (c = 7; c >= 0; c--)
                {
                        if (mask & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = (val & (1 << c)) ? fg[0] : bg[0];
                        addr += (c == 4) ? 13 : 1;
                }
                break;

                /*16-bit mono->colour expansion, unmasked*/
                case 3:
                for (c = 7; c >= 0; c--)
                {
                        if (mask & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = (val & (1 << c)) ? fg[(c & 1) ^ 1] : bg[(c & 1) ^ 1];
                        addr += (c == 4) ? 13 : 1;
                }
                break;

                /*8-bit mono->colour expansion, masked*/
                case 6:
                for (c = 7; c >= 0; c--)
                {
                        if ((val & mask) & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = fg[0];
                        addr += (c == 4) ? 13 : 1;
                }
                break;
                
                /*16-bit mono->colour expansion, masked*/
                case 7:
                for (c = 7; c >= 0; c--)
                {
                        if ((val & mask) & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = fg[(c & 1) ^ 1];
						addr += (c == 4) ? 13 : 1;
                }
                break;

                case 0x8: case 0x9: case 0xa: case 0xb:
                case 0xc: case 0xd: case 0xe: case 0xf:
                for (c = 0; c < 16; c++) {
                        *(uint8_t *)&svga->vram[addr] = tgui->copy_latch[c];
                        addr += ((c & 3) == 3) ? 13 : 1;
				}
                break;
        }
}

static void tgui_ext_linear_writew(uint32_t addr, uint16_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;
        int c;
        uint8_t fg[2] = {tgui->ext_gdc_regs[4], tgui->ext_gdc_regs[5]};
        uint8_t bg[2] = {tgui->ext_gdc_regs[1], tgui->ext_gdc_regs[2]};
        uint16_t mask = (tgui->ext_gdc_regs[7] << 8) | tgui->ext_gdc_regs[8];
        
        cycles -= video_timing_write_w;

        addr &= svga->decode_mask;
        if (addr >= svga->vram_max)
                return;
        addr &= svga->vram_mask;
        addr &= ~0xf;
		
		addr = dword_remap(addr);
        svga->changedvram[addr >> 12] = changeframecount;
        
        val = (val >> 8) | (val << 8);

        switch (tgui->ext_gdc_regs[0] & 0xf)
        {
                /*8-bit mono->colour expansion, unmasked*/
                case 2:
                for (c = 15; c >= 0; c--)
                {
                        if (mask & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = (val & (1 << c)) ? fg[0] : bg[0];
                        addr += (c & 3) ? 1 : 13;
                }
                break;

                /*16-bit mono->colour expansion, unmasked*/
                case 3:
                for (c = 15; c >= 0; c--)
                {
                        if (mask & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = (val & (1 << c)) ? fg[(c & 1) ^ 1] : bg[(c & 1) ^ 1];
                        addr += (c & 3) ? 1 : 13;
                }
                break;

                /*8-bit mono->colour expansion, masked*/
                case 6:
                for (c = 15; c >= 0; c--)
                {
                        if ((val & mask) & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = fg[0];
                        addr += (c & 3) ? 1 : 13;
                }
                break;

                /*16-bit mono->colour expansion, masked*/
                case 7:
                for (c = 15; c >= 0; c--)
                {
                        if ((val & mask) & (1 << c))
                                *(uint8_t *)&svga->vram[addr] = fg[(c & 1) ^ 1];
                        addr += (c & 3) ? 1 : 13;
                }
                break;
                                
                case 0x8: case 0x9: case 0xa: case 0xb:
                case 0xc: case 0xd: case 0xe: case 0xf:
                for (c = 0; c < 16; c++) {
                        *(uint8_t *)&svga->vram[addr+c] = tgui->copy_latch[c];
						addr += ((c & 3) == 3) ? 13 : 1;
				}
                break;
        }
}

static void tgui_ext_linear_writel(uint32_t addr, uint32_t val, void *p)
{
        tgui_ext_linear_writew(addr, val, p);
}

static void tgui_ext_write(uint32_t addr, uint8_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        
        addr = (addr & svga->banked_mask) + svga->read_bank;

        tgui_ext_linear_write(addr, val, svga);
}
static void tgui_ext_writew(uint32_t addr, uint16_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        
        addr = (addr & svga->banked_mask) + svga->read_bank;

        tgui_ext_linear_writew(addr, val, svga);
}
static void tgui_ext_writel(uint32_t addr, uint32_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        
        addr = (addr & svga->banked_mask) + svga->read_bank;

        tgui_ext_linear_writel(addr, val, svga);
}


enum
{
	TGUI_BITBLT = 1
};

enum
{
        TGUI_SRCCPU = 0,
        
	TGUI_SRCDISP = 0x04,	/*Source is from display*/
	TGUI_PATMONO = 0x20,	/*Pattern is monochrome and needs expansion*/
	TGUI_SRCMONO = 0x40,	/*Source is monochrome from CPU and needs expansion*/
	TGUI_TRANSENA  = 0x1000, /*Transparent (no draw when source == bg col)*/
	TGUI_TRANSREV  = 0x2000, /*Reverse fg/bg for transparent*/
	TGUI_SOLIDFILL = 0x4000	/*Pattern all zero?*/
};

#define READ(addr, dat) if (tgui->accel.bpp == 0) dat = svga->vram[dword_remap(addr) & tgui->vram_mask]; \
                        else dat = vram_w[dword_remap_w(addr) & (tgui->vram_mask >> 1)];
                        
#define MIX() do \
	{								\
		out = 0;						\
        	for (c=0;c<16;c++)					\
	        {							\
			d=(dst_dat & (1<<c)) ? 1:0;			\
                       	if (src_dat & (1<<c)) d|=2;			\
               	        if (pat_dat & (1<<c)) d|=4;			\
                        if (tgui->accel.rop & (1<<d)) out|=(1<<c);	\
	        }							\
	} while (0)

#define WRITE(addr, dat)        if (tgui->accel.bpp == 0)                                                \
                                {                                                                       \
                                        svga->vram[dword_remap(addr) & tgui->vram_mask] = dat;                                    \
                                        svga->changedvram[(dword_remap(addr) & tgui->vram_mask) >> 12] = changeframecount;      \
                                }                                                                       \
                                else                                                                    \
                                {                                                                       \
                                        vram_w[dword_remap_w(addr) & (tgui->vram_mask >> 1)] = dat;                                   \
                                        svga->changedvram[(dword_remap_w(addr) & (tgui->vram_mask >> 1)) >> 11] = changeframecount;        \
                                }
                                
void tgui_accel_command(int count, uint32_t cpu_dat, tgui_t *tgui)
{
        svga_t *svga = &tgui->svga;
	int x, y;
	int c, d;
	uint16_t src_dat, dst_dat, pat_dat;
	uint16_t out;
	int xdir = (tgui->accel.flags & 0x200) ? -1 : 1;
	int ydir = (tgui->accel.flags & 0x100) ? -1 : 1;
	uint16_t trans_col = (tgui->accel.flags & TGUI_TRANSREV) ? tgui->accel.fg_col : tgui->accel.bg_col;
        uint16_t *vram_w = (uint16_t *)svga->vram;
        
	if (tgui->accel.bpp == 0)
                trans_col &= 0xff;
	
	if (count != -1 && !tgui->accel.x && (tgui->accel.flags & TGUI_SRCMONO))
	{
		count -= tgui->accel.offset;
		cpu_dat <<= tgui->accel.offset;
	}
	if (count == -1)
	{
		tgui->accel.x = tgui->accel.y = 0;
	}
	if (tgui->accel.flags & TGUI_SOLIDFILL)
	{
		for (y = 0; y < 8; y++)
		{
			for (x = 0; x < 8; x++)
			{
				tgui->accel.tgui_pattern[y][7 - x] = tgui->accel.fg_col;
			}
		}
	}
	else if (tgui->accel.flags & TGUI_PATMONO)
	{
		for (y = 0; y < 8; y++)
		{
			for (x = 0; x < 8; x++)
			{
				tgui->accel.tgui_pattern[y][7 - x] = (tgui->accel.pattern[y] & (1 << x)) ? tgui->accel.fg_col : tgui->accel.bg_col;
			}
		}
	}
	else
	{
                if (tgui->accel.bpp == 0)
                {
        		for (y = 0; y < 8; y++)
        		{
        			for (x = 0; x < 8; x++)
        			{
        				tgui->accel.tgui_pattern[y][7 - x] = tgui->accel.pattern[x + y*8];
        			}
                        }
		}
		else
                {
        		for (y = 0; y < 8; y++)
        		{
        			for (x = 0; x < 8; x++)
        			{
        				tgui->accel.tgui_pattern[y][7 - x] = tgui->accel.pattern[x*2 + y*16] | (tgui->accel.pattern[x*2 + y*16 + 1] << 8);
        			}
                        }
		}
	}
	
		pclog("Accel command = %i\n", tgui->accel.command);
        switch (tgui->accel.command)
	{
		case TGUI_BITBLT:
		if (count == -1)
		{
			tgui->accel.src = tgui->accel.src_old = tgui->accel.src_x + (tgui->accel.src_y * tgui->accel.pitch);
			tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_x + (tgui->accel.dst_y * tgui->accel.pitch);
			tgui->accel.pat_x = tgui->accel.dst_x;
			tgui->accel.pat_y = tgui->accel.dst_y;
		}
		
		switch (tgui->accel.flags & (TGUI_SRCMONO|TGUI_SRCDISP))
		{
			case TGUI_SRCCPU:
			if (count == -1)
			{
				if (svga->crtc[0x21] & 0x20)
				{
                                        tgui->write_blitter = 1;
                                }
				if (tgui->accel.use_src)
                                        return;
			} else {
				count >>= 3;
			}
			while (count)
			{
				if (tgui->accel.bpp == 0)
				{
										src_dat = cpu_dat >> 24;
                                        cpu_dat <<= 8;
                                }
                                else
                                {
										src_dat = (cpu_dat >> 24) | ((cpu_dat >> 8) & 0xff00);
                                        cpu_dat <<= 16;
                                        count--;
                                }
				READ(tgui->accel.dst, dst_dat);
				pat_dat = tgui->accel.tgui_pattern[tgui->accel.pat_y & 7][tgui->accel.pat_x & 7];
	
                                if (!(tgui->accel.flags & TGUI_TRANSENA) || src_dat != trans_col)
                                {
        				MIX();
	
                                        WRITE(tgui->accel.dst, out);
                                }

				tgui->accel.src += xdir;
				tgui->accel.dst += xdir;
				tgui->accel.pat_x += xdir;
	
				tgui->accel.x++;
				if (tgui->accel.x > tgui->accel.size_x)
				{
					tgui->accel.x = 0;
					tgui->accel.y++;
					
					tgui->accel.pat_x = tgui->accel.dst_x;
	
					tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
					tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);
					tgui->accel.pat_y += ydir;
					
					if (tgui->accel.y > tgui->accel.size_y)
					{
						if (svga->crtc[0x21] & 0x20)
						{
                                                        tgui->write_blitter = 0;
						}
						return;
					}
					if (tgui->accel.use_src)
                                                return;
				}
				count--;
			}
			break;
						
			case TGUI_SRCMONO | TGUI_SRCCPU:
			if (count == -1)
			{
				if (svga->crtc[0x21] & 0x20)
                                        tgui->write_blitter = 1;

				if (tgui->accel.use_src)
                                        return;
			}
			while (count--)
			{
				src_dat = ((cpu_dat >> 31) ? tgui->accel.fg_col : tgui->accel.bg_col);
				if (tgui->accel.bpp == 0)
				    src_dat &= 0xff;
				    
				READ(tgui->accel.dst, dst_dat);
				pat_dat = tgui->accel.tgui_pattern[tgui->accel.pat_y & 7][tgui->accel.pat_x & 7];

                                if (!(tgui->accel.flags & TGUI_TRANSENA) || src_dat != trans_col)
                                {
        				MIX();

				        WRITE(tgui->accel.dst, out);
                                }
				cpu_dat <<= 1;
				tgui->accel.src += xdir;
				tgui->accel.dst += xdir;
				tgui->accel.pat_x += xdir;
	
				tgui->accel.x++;
				if (tgui->accel.x > tgui->accel.size_x)
				{
					tgui->accel.x = 0;
					tgui->accel.y++;
					
					tgui->accel.pat_x = tgui->accel.dst_x;
	
					tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
					tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);
					tgui->accel.pat_y += ydir;
					
					if (tgui->accel.y > tgui->accel.size_y)
					{
						if (svga->crtc[0x21] & 0x20)
						{
                                                        tgui->write_blitter = 0;
						}
						return;
					}
					if (tgui->accel.use_src)
                                                return;
				}
			}
			break;

			default:
			while (count--)
			{
				READ(tgui->accel.src, src_dat);
				READ(tgui->accel.dst, dst_dat);                                                                
				pat_dat = tgui->accel.tgui_pattern[tgui->accel.pat_y & 7][tgui->accel.pat_x & 7];
	
                                if (!(tgui->accel.flags & TGUI_TRANSENA) || src_dat != trans_col)
                                {
        				MIX();
	
                                        WRITE(tgui->accel.dst, out);
                                }

				tgui->accel.src += xdir;
				tgui->accel.dst += xdir;
				tgui->accel.pat_x += xdir;
	
				tgui->accel.x++;
				if (tgui->accel.x > tgui->accel.size_x)
				{
					tgui->accel.x = 0;
					tgui->accel.y++;
					
					tgui->accel.pat_x = tgui->accel.dst_x;
	
					tgui->accel.src = tgui->accel.src_old = tgui->accel.src_old + (ydir * tgui->accel.pitch);
					tgui->accel.dst = tgui->accel.dst_old = tgui->accel.dst_old + (ydir * tgui->accel.pitch);
					tgui->accel.pat_y += ydir;
					
					if (tgui->accel.y > tgui->accel.size_y)
						return;
				}
			}
			break;
		}
		break;
	}
}

void tgui_accel_out(uint16_t addr, uint8_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;

	switch (addr)
	{
		case 0x2122:
		tgui->accel.ger22 = val;
		tgui->accel.pitch = 512 << ((val >> 2) & 3);
		tgui->accel.bpp = (val & 3) ? 1 : 0;
		tgui->accel.pitch >>= tgui->accel.bpp;
		break;
                
		case 0x2124: /*Command*/
		tgui->accel.command = val;
		tgui_accel_command(-1, 0, tgui);
		break;
		
		case 0x2127: /*ROP*/
		tgui->accel.rop = val;
		tgui->accel.use_src = (val & 0x33) ^ ((val >> 2) & 0x33);
		break;
		
		case 0x2128: /*Flags*/
		tgui->accel.flags = (tgui->accel.flags & 0xff00) | val;
		break;
		case 0x2129: /*Flags*/
		tgui->accel.flags = (tgui->accel.flags & 0xff) | (val << 8);
		break;

		case 0x212b:
		tgui->accel.offset = val & 7;
		break;
		
		case 0x212c: /*Foreground colour*/
		tgui->accel.fg_col = (tgui->accel.fg_col & 0xff00) | val;
		break;
		case 0x212d: /*Foreground colour*/
		tgui->accel.fg_col = (tgui->accel.fg_col & 0xff) | (val << 8);
		break;

		case 0x2130: /*Background colour*/
		tgui->accel.bg_col = (tgui->accel.bg_col & 0xff00) | val;
		break;
		case 0x2131: /*Background colour*/
		tgui->accel.bg_col = (tgui->accel.bg_col & 0xff) | (val << 8);
		break;

		case 0x2138: /*Dest X*/
		tgui->accel.dst_x = (tgui->accel.dst_x & 0xff00) | val;
		break;
		case 0x2139: /*Dest X*/
		tgui->accel.dst_x = (tgui->accel.dst_x & 0xff) | (val << 8);
		break;
		case 0x213a: /*Dest Y*/
		tgui->accel.dst_y = (tgui->accel.dst_y & 0xff00) | val;
		break;
		case 0x213b: /*Dest Y*/
		tgui->accel.dst_y = (tgui->accel.dst_y & 0xff) | (val << 8);
		break;

		case 0x213c: /*Src X*/
		tgui->accel.src_x = (tgui->accel.src_x & 0xff00) | val;
		break;
		case 0x213d: /*Src X*/
		tgui->accel.src_x = (tgui->accel.src_x & 0xff) | (val << 8);
		break;
		case 0x213e: /*Src Y*/
		tgui->accel.src_y = (tgui->accel.src_y & 0xff00) | val;
		break;
		case 0x213f: /*Src Y*/
		tgui->accel.src_y = (tgui->accel.src_y & 0xff) | (val << 8);
		break;

		case 0x2140: /*Size X*/
		tgui->accel.size_x = (tgui->accel.size_x & 0xff00) | val;
		break;
		case 0x2141: /*Size X*/
		tgui->accel.size_x = (tgui->accel.size_x & 0xff) | (val << 8);
		break;
		case 0x2142: /*Size Y*/
		tgui->accel.size_y = (tgui->accel.size_y & 0xff00) | val;
		break;
		case 0x2143: /*Size Y*/
		tgui->accel.size_y = (tgui->accel.size_y & 0xff) | (val << 8);
		break;
		
		case 0x2180: case 0x2181: case 0x2182: case 0x2183:
		case 0x2184: case 0x2185: case 0x2186: case 0x2187:
		case 0x2188: case 0x2189: case 0x218a: case 0x218b:
		case 0x218c: case 0x218d: case 0x218e: case 0x218f:
		case 0x2190: case 0x2191: case 0x2192: case 0x2193:
		case 0x2194: case 0x2195: case 0x2196: case 0x2197:
		case 0x2198: case 0x2199: case 0x219a: case 0x219b:
		case 0x219c: case 0x219d: case 0x219e: case 0x219f:
		case 0x21a0: case 0x21a1: case 0x21a2: case 0x21a3:
		case 0x21a4: case 0x21a5: case 0x21a6: case 0x21a7:
		case 0x21a8: case 0x21a9: case 0x21aa: case 0x21ab:
		case 0x21ac: case 0x21ad: case 0x21ae: case 0x21af:
		case 0x21b0: case 0x21b1: case 0x21b2: case 0x21b3:
		case 0x21b4: case 0x21b5: case 0x21b6: case 0x21b7:
		case 0x21b8: case 0x21b9: case 0x21ba: case 0x21bb:
		case 0x21bc: case 0x21bd: case 0x21be: case 0x21bf:
		case 0x21c0: case 0x21c1: case 0x21c2: case 0x21c3:
		case 0x21c4: case 0x21c5: case 0x21c6: case 0x21c7:
		case 0x21c8: case 0x21c9: case 0x21ca: case 0x21cb:
		case 0x21cc: case 0x21cd: case 0x21ce: case 0x21cf:
		case 0x21d0: case 0x21d1: case 0x21d2: case 0x21d3:
		case 0x21d4: case 0x21d5: case 0x21d6: case 0x21d7:
		case 0x21d8: case 0x21d9: case 0x21da: case 0x21db:
		case 0x21dc: case 0x21dd: case 0x21de: case 0x21df:
		case 0x21e0: case 0x21e1: case 0x21e2: case 0x21e3:
		case 0x21e4: case 0x21e5: case 0x21e6: case 0x21e7:
		case 0x21e8: case 0x21e9: case 0x21ea: case 0x21eb:
		case 0x21ec: case 0x21ed: case 0x21ee: case 0x21ef:
		case 0x21f0: case 0x21f1: case 0x21f2: case 0x21f3:
		case 0x21f4: case 0x21f5: case 0x21f6: case 0x21f7:
		case 0x21f8: case 0x21f9: case 0x21fa: case 0x21fb:
		case 0x21fc: case 0x21fd: case 0x21fe: case 0x21ff:
		tgui->accel.pattern[addr & 0x7f] = val;
		break;
	}
}

void tgui_accel_out_w(uint16_t addr, uint16_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
		tgui_accel_out(addr, val, tgui);
		tgui_accel_out(addr + 1, val >> 8, tgui);
}

void tgui_accel_out_l(uint16_t addr, uint32_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
		tgui_accel_out(addr, val, tgui);
		tgui_accel_out(addr + 1, val >> 8, tgui);
		tgui_accel_out(addr + 2, val >> 16, tgui);
		tgui_accel_out(addr + 3, val >> 24, tgui);
}

uint8_t tgui_accel_in(uint16_t addr, void *p)
{
		tgui_t *tgui = (tgui_t *)p;

	switch (addr)
	{
		case 0x2120: /*Status*/
		return 0;
		
		case 0x2127: /*ROP*/
		return tgui->accel.rop;
		
		case 0x2128: /*Flags*/
		return tgui->accel.flags & 0xff;
		case 0x2129: /*Flags*/
		return tgui->accel.flags >> 8;

		case 0x212b:
		return tgui->accel.offset;
		
		case 0x212c: /*Background colour*/
		return tgui->accel.bg_col & 0xff;
		case 0x212d: /*Background colour*/
		return tgui->accel.bg_col >> 8;

		case 0x2130: /*Foreground colour*/
		return tgui->accel.fg_col & 0xff;
		case 0x2131: /*Foreground colour*/
		return tgui->accel.fg_col >> 8;

		case 0x2138: /*Dest X*/
		return tgui->accel.dst_x & 0xff;
		case 0x2139: /*Dest X*/
		return tgui->accel.dst_x >> 8;
		case 0x213a: /*Dest Y*/
		return tgui->accel.dst_y & 0xff;
		case 0x213b: /*Dest Y*/
		return tgui->accel.dst_y >> 8;

		case 0x213c: /*Src X*/
		return tgui->accel.src_x & 0xff;
		case 0x213d: /*Src X*/
		return tgui->accel.src_x >> 8;
		case 0x213e: /*Src Y*/
		return tgui->accel.src_y & 0xff;
		case 0x213f: /*Src Y*/
		return tgui->accel.src_y >> 8;

		case 0x2140: /*Size X*/
		return tgui->accel.size_x & 0xff;
		case 0x2141: /*Size X*/
		return tgui->accel.size_x >> 8;
		case 0x2142: /*Size Y*/
		return tgui->accel.size_y & 0xff;
		case 0x2143: /*Size Y*/
		return tgui->accel.size_y >> 8;
		
		case 0x2180: case 0x2181: case 0x2182: case 0x2183:
		case 0x2184: case 0x2185: case 0x2186: case 0x2187:
		case 0x2188: case 0x2189: case 0x218a: case 0x218b:
		case 0x218c: case 0x218d: case 0x218e: case 0x218f:
		case 0x2190: case 0x2191: case 0x2192: case 0x2193:
		case 0x2194: case 0x2195: case 0x2196: case 0x2197:
		case 0x2198: case 0x2199: case 0x219a: case 0x219b:
		case 0x219c: case 0x219d: case 0x219e: case 0x219f:
		case 0x21a0: case 0x21a1: case 0x21a2: case 0x21a3:
		case 0x21a4: case 0x21a5: case 0x21a6: case 0x21a7:
		case 0x21a8: case 0x21a9: case 0x21aa: case 0x21ab:
		case 0x21ac: case 0x21ad: case 0x21ae: case 0x21af:
		case 0x21b0: case 0x21b1: case 0x21b2: case 0x21b3:
		case 0x21b4: case 0x21b5: case 0x21b6: case 0x21b7:
		case 0x21b8: case 0x21b9: case 0x21ba: case 0x21bb:
		case 0x21bc: case 0x21bd: case 0x21be: case 0x21bf:
		case 0x21c0: case 0x21c1: case 0x21c2: case 0x21c3:
		case 0x21c4: case 0x21c5: case 0x21c6: case 0x21c7:
		case 0x21c8: case 0x21c9: case 0x21ca: case 0x21cb:
		case 0x21cc: case 0x21cd: case 0x21ce: case 0x21cf:
		case 0x21d0: case 0x21d1: case 0x21d2: case 0x21d3:
		case 0x21d4: case 0x21d5: case 0x21d6: case 0x21d7:
		case 0x21d8: case 0x21d9: case 0x21da: case 0x21db:
		case 0x21dc: case 0x21dd: case 0x21de: case 0x21df:
		case 0x21e0: case 0x21e1: case 0x21e2: case 0x21e3:
		case 0x21e4: case 0x21e5: case 0x21e6: case 0x21e7:
		case 0x21e8: case 0x21e9: case 0x21ea: case 0x21eb:
		case 0x21ec: case 0x21ed: case 0x21ee: case 0x21ef:
		case 0x21f0: case 0x21f1: case 0x21f2: case 0x21f3:
		case 0x21f4: case 0x21f5: case 0x21f6: case 0x21f7:
		case 0x21f8: case 0x21f9: case 0x21fa: case 0x21fb:
		case 0x21fc: case 0x21fd: case 0x21fe: case 0x21ff:
		return tgui->accel.pattern[addr & 0x7f];
	}
	return 0;
}

uint16_t tgui_accel_in_w(uint16_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	return tgui_accel_in(addr, tgui) | (tgui_accel_in(addr + 1, tgui) << 8);
}

uint32_t tgui_accel_in_l(uint16_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	return tgui_accel_in_w(addr, tgui) | (tgui_accel_in_w(addr + 2, tgui) << 16);
}


void tgui_accel_write(uint32_t addr, uint8_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;

	if ((addr & ~0xff) != 0xbff00)
		return;

	switch (addr & 0xff)
	{
		case 0x22:
		tgui->accel.ger22 = val;
		tgui->accel.pitch = 512 << ((val >> 2) & 3);
		tgui->accel.bpp = (val & 3) ? 1 : 0;
		tgui->accel.pitch >>= tgui->accel.bpp;
		break;
                
		case 0x24: /*Command*/
		tgui->accel.command = val;
		tgui_accel_command(-1, 0, tgui);
		break;
		
		case 0x27: /*ROP*/
		tgui->accel.rop = val;
		tgui->accel.use_src = (val & 0x33) ^ ((val >> 2) & 0x33);
		break;
		
		case 0x28: /*Flags*/
		tgui->accel.flags = (tgui->accel.flags & 0xff00) | val;
		break;
		case 0x29: /*Flags*/
		tgui->accel.flags = (tgui->accel.flags & 0xff) | (val << 8);
		break;

		case 0x2b:
		tgui->accel.offset = val & 7;
		break;
		
		case 0x2c: /*Foreground colour*/
		tgui->accel.fg_col = (tgui->accel.fg_col & 0xff00) | val;
		break;
		case 0x2d: /*Foreground colour*/
		tgui->accel.fg_col = (tgui->accel.fg_col & 0xff) | (val << 8);
		break;

		case 0x30: /*Background colour*/
		tgui->accel.bg_col = (tgui->accel.bg_col & 0xff00) | val;
		break;
		case 0x31: /*Background colour*/
		tgui->accel.bg_col = (tgui->accel.bg_col & 0xff) | (val << 8);
		break;

		case 0x38: /*Dest X*/
		tgui->accel.dst_x = (tgui->accel.dst_x & 0xff00) | val;
		break;
		case 0x39: /*Dest X*/
		tgui->accel.dst_x = (tgui->accel.dst_x & 0xff) | (val << 8);
		break;
		case 0x3a: /*Dest Y*/
		tgui->accel.dst_y = (tgui->accel.dst_y & 0xff00) | val;
		break;
		case 0x3b: /*Dest Y*/
		tgui->accel.dst_y = (tgui->accel.dst_y & 0xff) | (val << 8);
		break;

		case 0x3c: /*Src X*/
		tgui->accel.src_x = (tgui->accel.src_x & 0xff00) | val;
		break;
		case 0x3d: /*Src X*/
		tgui->accel.src_x = (tgui->accel.src_x & 0xff) | (val << 8);
		break;
		case 0x3e: /*Src Y*/
		tgui->accel.src_y = (tgui->accel.src_y & 0xff00) | val;
		break;
		case 0x3f: /*Src Y*/
		tgui->accel.src_y = (tgui->accel.src_y & 0xff) | (val << 8);
		break;

		case 0x40: /*Size X*/
		tgui->accel.size_x = (tgui->accel.size_x & 0xff00) | val;
		break;
		case 0x41: /*Size X*/
		tgui->accel.size_x = (tgui->accel.size_x & 0xff) | (val << 8);
		break;
		case 0x42: /*Size Y*/
		tgui->accel.size_y = (tgui->accel.size_y & 0xff00) | val;
		break;
		case 0x43: /*Size Y*/
		tgui->accel.size_y = (tgui->accel.size_y & 0xff) | (val << 8);
		break;
		
		case 0x80: case 0x81: case 0x82: case 0x83:
		case 0x84: case 0x85: case 0x86: case 0x87:
		case 0x88: case 0x89: case 0x8a: case 0x8b:
		case 0x8c: case 0x8d: case 0x8e: case 0x8f:
		case 0x90: case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96: case 0x97:
		case 0x98: case 0x99: case 0x9a: case 0x9b:
		case 0x9c: case 0x9d: case 0x9e: case 0x9f:
		case 0xa0: case 0xa1: case 0xa2: case 0xa3:
		case 0xa4: case 0xa5: case 0xa6: case 0xa7:
		case 0xa8: case 0xa9: case 0xaa: case 0xab:
		case 0xac: case 0xad: case 0xae: case 0xaf:
		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		case 0xc0: case 0xc1: case 0xc2: case 0xc3:
		case 0xc4: case 0xc5: case 0xc6: case 0xc7:
		case 0xc8: case 0xc9: case 0xca: case 0xcb:
		case 0xcc: case 0xcd: case 0xce: case 0xcf:
		case 0xd0: case 0xd1: case 0xd2: case 0xd3:
		case 0xd4: case 0xd5: case 0xd6: case 0xd7:
		case 0xd8: case 0xd9: case 0xda: case 0xdb:
		case 0xdc: case 0xdd: case 0xde: case 0xdf:
		case 0xe0: case 0xe1: case 0xe2: case 0xe3:
		case 0xe4: case 0xe5: case 0xe6: case 0xe7:
		case 0xe8: case 0xe9: case 0xea: case 0xeb:
		case 0xec: case 0xed: case 0xee: case 0xef:
		case 0xf0: case 0xf1: case 0xf2: case 0xf3:
		case 0xf4: case 0xf5: case 0xf6: case 0xf7:
		case 0xf8: case 0xf9: case 0xfa: case 0xfb:
		case 0xfc: case 0xfd: case 0xfe: case 0xff:
		tgui->accel.pattern[addr & 0x7f] = val;
		break;
	}
}

void tgui_accel_write_w(uint32_t addr, uint16_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	tgui_accel_write(addr, val, tgui);
	tgui_accel_write(addr + 1, val >> 8, tgui);
}

void tgui_accel_write_l(uint32_t addr, uint32_t val, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	tgui_accel_write(addr, val, tgui);
	tgui_accel_write(addr + 1, val >> 8, tgui);
	tgui_accel_write(addr + 2, val >> 16, tgui);
	tgui_accel_write(addr + 3, val >> 24, tgui);
}

uint8_t tgui_accel_read(uint32_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	if ((addr & ~0xff) != 0xbff00)
		return 0xff;
	switch (addr & 0xff)
	{
		case 0x20: /*Status*/
		return 0;
		
		case 0x27: /*ROP*/
		return tgui->accel.rop;
		
		case 0x28: /*Flags*/
		return tgui->accel.flags & 0xff;
		case 0x29: /*Flags*/
		return tgui->accel.flags >> 8;

		case 0x2b:
		return tgui->accel.offset;
		
		case 0x2c: /*Background colour*/
		return tgui->accel.bg_col & 0xff;
		case 0x2d: /*Background colour*/
		return tgui->accel.bg_col >> 8;

		case 0x30: /*Foreground colour*/
		return tgui->accel.fg_col & 0xff;
		case 0x31: /*Foreground colour*/
		return tgui->accel.fg_col >> 8;

		case 0x38: /*Dest X*/
		return tgui->accel.dst_x & 0xff;
		case 0x39: /*Dest X*/
		return tgui->accel.dst_x >> 8;
		case 0x3a: /*Dest Y*/
		return tgui->accel.dst_y & 0xff;
		case 0x3b: /*Dest Y*/
		return tgui->accel.dst_y >> 8;

		case 0x3c: /*Src X*/
		return tgui->accel.src_x & 0xff;
		case 0x3d: /*Src X*/
		return tgui->accel.src_x >> 8;
		case 0x3e: /*Src Y*/
		return tgui->accel.src_y & 0xff;
		case 0x3f: /*Src Y*/
		return tgui->accel.src_y >> 8;

		case 0x40: /*Size X*/
		return tgui->accel.size_x & 0xff;
		case 0x41: /*Size X*/
		return tgui->accel.size_x >> 8;
		case 0x42: /*Size Y*/
		return tgui->accel.size_y & 0xff;
		case 0x43: /*Size Y*/
		return tgui->accel.size_y >> 8;
		
		case 0x80: case 0x81: case 0x82: case 0x83:
		case 0x84: case 0x85: case 0x86: case 0x87:
		case 0x88: case 0x89: case 0x8a: case 0x8b:
		case 0x8c: case 0x8d: case 0x8e: case 0x8f:
		case 0x90: case 0x91: case 0x92: case 0x93:
		case 0x94: case 0x95: case 0x96: case 0x97:
		case 0x98: case 0x99: case 0x9a: case 0x9b:
		case 0x9c: case 0x9d: case 0x9e: case 0x9f:
		case 0xa0: case 0xa1: case 0xa2: case 0xa3:
		case 0xa4: case 0xa5: case 0xa6: case 0xa7:
		case 0xa8: case 0xa9: case 0xaa: case 0xab:
		case 0xac: case 0xad: case 0xae: case 0xaf:
		case 0xb0: case 0xb1: case 0xb2: case 0xb3:
		case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		case 0xb8: case 0xb9: case 0xba: case 0xbb:
		case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		case 0xc0: case 0xc1: case 0xc2: case 0xc3:
		case 0xc4: case 0xc5: case 0xc6: case 0xc7:
		case 0xc8: case 0xc9: case 0xca: case 0xcb:
		case 0xcc: case 0xcd: case 0xce: case 0xcf:
		case 0xd0: case 0xd1: case 0xd2: case 0xd3:
		case 0xd4: case 0xd5: case 0xd6: case 0xd7:
		case 0xd8: case 0xd9: case 0xda: case 0xdb:
		case 0xdc: case 0xdd: case 0xde: case 0xdf:
		case 0xe0: case 0xe1: case 0xe2: case 0xe3:
		case 0xe4: case 0xe5: case 0xe6: case 0xe7:
		case 0xe8: case 0xe9: case 0xea: case 0xeb:
		case 0xec: case 0xed: case 0xee: case 0xef:
		case 0xf0: case 0xf1: case 0xf2: case 0xf3:
		case 0xf4: case 0xf5: case 0xf6: case 0xf7:
		case 0xf8: case 0xf9: case 0xfa: case 0xfb:
		case 0xfc: case 0xfd: case 0xfe: case 0xff:
		return tgui->accel.pattern[addr & 0x7f];
	}
	return 0xff;
}

uint16_t tgui_accel_read_w(uint32_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	return tgui_accel_read(addr, tgui) | (tgui_accel_read(addr + 1, tgui) << 8);
}

uint32_t tgui_accel_read_l(uint32_t addr, void *p)
{
        tgui_t *tgui = (tgui_t *)p;
	return tgui_accel_read_w(addr, tgui) | (tgui_accel_read_w(addr + 2, tgui) << 16);
}

void tgui_accel_write_fb_b(uint32_t addr, uint8_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;

        if (tgui->write_blitter)
        	tgui_accel_command(8, val << 24, tgui);
        else
			svga_write_linear(addr, val, svga);
}

void tgui_accel_write_fb_w(uint32_t addr, uint16_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;

        if (tgui->write_blitter)
        	tgui_accel_command(16, (((val & 0xff00) >> 8) | ((val & 0x00ff) << 8)) << 16, tgui);
        else
			svga_writew_linear(addr, val, svga);
}

void tgui_accel_write_fb_l(uint32_t addr, uint32_t val, void *p)
{
        svga_t *svga = (svga_t *)p;
        tgui_t *tgui = (tgui_t *)svga->p;

        if (tgui->write_blitter)
        	tgui_accel_command(32, ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24), tgui);
        else
            svga_writel_linear(addr, val, svga);
}

static void *tgui_init(const device_t *info)
{
	const char *bios_fn;
	int type = info->local;

        tgui_t *tgui = malloc(sizeof(tgui_t));
        memset(tgui, 0, sizeof(tgui_t));
        
        tgui->vram_size = device_get_config_int("memory") << 20;
        tgui->vram_mask = tgui->vram_size - 1;
        
        tgui->type = type;

	tgui->pci = !!(info->flags & DEVICE_PCI);

	switch(info->local) {
		case TGUI_9400CXI:
			bios_fn = ROM_TGUI_9400CXI;
			break;
		case TGUI_9440:
			bios_fn = ROM_TGUI_9440;
			break;
		default:
			free(tgui);
			return NULL;
	}

        rom_init(&tgui->bios_rom, (char *) bios_fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);

	if (info->flags & DEVICE_PCI)
		video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_tgui_pci);
	else
		video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_tgui_vlb);

        svga_init(info, &tgui->svga, tgui, tgui->vram_size,
                   tgui_recalctimings,
                   tgui_in, tgui_out,
                   tgui_hwcursor_draw,
                   NULL);

        if (tgui->type == TGUI_9400CXI)
		tgui->svga.ramdac = device_add(&tkd8001_ramdac_device);

        mem_mapping_add(&tgui->linear_mapping, 0,       0,      svga_read_linear, svga_readw_linear, svga_readl_linear, tgui_accel_write_fb_b, tgui_accel_write_fb_w, tgui_accel_write_fb_l, NULL, MEM_MAPPING_EXTERNAL, &tgui->svga);
        mem_mapping_add(&tgui->accel_mapping,  0xbc000, 0x4000, tgui_accel_read,  tgui_accel_read_w, tgui_accel_read_l, tgui_accel_write,  tgui_accel_write_w, tgui_accel_write_l, NULL, MEM_MAPPING_EXTERNAL,  tgui);
        mem_mapping_disable(&tgui->accel_mapping);

        io_sethandler(0x03c0, 0x0020, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
        if (tgui->type >= TGUI_9440) {
                io_sethandler(0x43c8, 0x0002, tgui_in, NULL, NULL, tgui_out, NULL, NULL, tgui);
				io_sethandler(0x2120, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2122, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2124, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2127, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2128, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x212b, 0x0001, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x212c, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2130, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2134, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2138, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x213a, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x213c, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x213e, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2140, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2142, 0x0002, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
				io_sethandler(0x2180, 0x0080, tgui_accel_in, tgui_accel_in_w, tgui_accel_in_l, tgui_accel_out, tgui_accel_out_w, tgui_accel_out_l, tgui);
		}

        if ((info->flags & DEVICE_PCI) && (tgui->type >= TGUI_9440))
                pci_add_card(PCI_ADD_VIDEO, tgui_pci_read, tgui_pci_write, tgui);

        return tgui;
}

static int tgui9400cxi_available()
{
        return rom_present(ROM_TGUI_9400CXI);
}

static int tgui9440_available()
{
        return rom_present(ROM_TGUI_9440);
}

void tgui_close(void *p)
{
        tgui_t *tgui = (tgui_t *)p;
        
        svga_close(&tgui->svga);

        free(tgui);
}

void tgui_speed_changed(void *p)
{
        tgui_t *tgui = (tgui_t *)p;
        
        svga_recalctimings(&tgui->svga);
}

void tgui_force_redraw(void *p)
{
        tgui_t *tgui = (tgui_t *)p;

        tgui->svga.fullchange = changeframecount;
}


static const device_config_t tgui9440_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 2
        },
        {
                .type = -1
        }
};

const device_t tgui9400cxi_device =
{
        "Trident TGUI 9400CXi",
        DEVICE_VLB,
        TGUI_9400CXI,
        tgui_init,
        tgui_close,
	NULL,
        { tgui9400cxi_available },
        tgui_speed_changed,
        tgui_force_redraw,
        tgui9440_config
};

const device_t tgui9440_vlb_device =
{
        "Trident TGUI 9440 VLB",
        DEVICE_VLB,
	TGUI_9440,
        tgui_init,
        tgui_close,
	NULL,
        { tgui9440_available },
        tgui_speed_changed,
        tgui_force_redraw,
        tgui9440_config
};

const device_t tgui9440_pci_device =
{
        "Trident TGUI 9440 PCI",
        DEVICE_PCI,
	TGUI_9440,
        tgui_init,
        tgui_close,
	NULL,
        { tgui9440_available },
        tgui_speed_changed,
        tgui_force_redraw,
        tgui9440_config
};
