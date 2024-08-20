/*  PCSX2 - PS2 Emulator for PCs
*  Copyright (C) 2016-2021  PCSX2 Dev Team
*  Copyright (C) 2016 Wisi
*
*  PCSX2 is free software: you can redistribute it and/or modify it under the terms
*  of the GNU Lesser General Public License as published by the Free Software Found-
*  ation, either version 3 of the License, or (at your option) any later version.
*
*  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
*  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
*  PURPOSE.  See the GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License along with PCSX2.
*  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/Pcsx2Types.h"

#include "Memory.h"

/* HW Registers */
union tPGIF_CTRL
{

	struct pgifCtrl_t
	{
		//Please keep in mind, that not all of values are 100% confirmed.
		uint32_t UNK1					: 2;	// 0-1
		uint32_t fifo_GP1_ready_for_data : 1;	// 2
		uint32_t fifo_GP0_ready_for_data : 1;	// 3
		uint32_t data_from_gpu_ready		: 1;	// 4 sets in ps1drv same time as DMA RSEND
		uint32_t UNK2					: 1;	// 5
		uint32_t UNK3					: 2;	// 6-7
		uint32_t GP0_fifo_count			: 5;	// 8-12
		uint32_t UNK4					: 3;	// 13-15
		uint32_t GP1_fifo_count			: 3;	// 16 - 18
		uint32_t UNK5					: 1;	// 19
		uint32_t GP0_fifo_empty			: 1;	// 20
		uint32_t UNK6					: 1;	// 21
		uint32_t UNK7					: 1;	// 22
		uint32_t UNK8					: 8;	// 23-30
		uint32_t BUSY					: 1;	// Busy
	} bits;

	uint32_t _u32;
};

union tPGIF_IMM
{
	struct imm_t
	{
		uint32_t e2;
		uint32_t dummy1[3];
		uint32_t e3;
		uint32_t dummy2[3];
		uint32_t e4;
		uint32_t dummy3[3];
		uint32_t e5;
		uint32_t dummy4[3];

	} reg;
};

struct PGIFregisters
{
	tPGIF_IMM	imm_response;
	u128 		dummy1[2];
	tPGIF_CTRL	ctrl;
};
static PGIFregisters& pgif = (PGIFregisters&)eeHw[0xf310];

union tPGPU_REGS
{
	struct Bits_t
	{
		uint32_t TPXB	: 4;	// 0-3   Texture page X Base   (N*64)
		uint32_t TPYB	: 1;	// 4     Texture page Y Base   (N*256) (ie. 0 or 256)
		uint32_t ST		: 2;	// 5-6   Semi Transparency     (0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4)
		uint32_t TPC		: 2;	// 7-8   Texture page colors   (0=4bit, 1=8bit, 2=15bit, 3=Reserved)
		uint32_t DITH	: 1;	// 9     Dither 24bit to 15bit (0=Off/strip LSBs, 1=Dither Enabled)
		uint32_t DRAW	: 1;	// 10    Drawing to display area
		uint32_t DMSK	: 1;	// 11    Set Mask-bit when drawing pixels (0=No, 1=Yes/Mask)
		uint32_t DPIX	: 1;	// 12    Draw Pixels           (0=Always, 1=Not to Masked areas)
		uint32_t ILAC	: 1;	// 13    Interlace Field       (or, always 1 when GP1(08h).5=0)
		uint32_t RFLG	: 1;	// 14    "Reverseflag"         (0=Normal, 1=Distorted)
		uint32_t TDIS	: 1;	// 15    Texture Disable       (0=Normal, 1=Disable Textures)
		uint32_t HR2		: 1;	// 16    Horizontal Resolution 2     (0=256/320/512/640, 1=368)
		uint32_t HR1		: 2;	// 17-18 Horizontal Resolution 1     (0=256, 1=320, 2=512, 3=640)
		uint32_t VRES	: 1;	// 19    Vertical Resolution         (0=240, 1=480, when Bit22=1)
		uint32_t VMOD	: 1;	// 20    Video Mode                  (0=NTSC/60Hz, 1=PAL/50Hz)
		uint32_t COLD	: 1;	// 21    Display Area Color Depth    (0=15bit, 1=24bit)
		uint32_t VILAC	: 1;	// 22    Vertical Interlace          (0=Off, 1=On)
		uint32_t DE		: 1;	// 23    Display Enable              (0=Enabled, 1=Disabled)
		uint32_t IRQ1	: 1;	// 24    Interrupt Request (IRQ1)    (0=Off, 1=IRQ)       ;GP0(1Fh)/GP1(02h)
		uint32_t DREQ	: 1; 	// 25    DMA / Data Request, meaning depends on GP1(04h) DMA Direction:
							// When GP1(04h)=0 ---> Always zero (0)
							// When GP1(04h)=1 ---> FIFO State  (0=Full, 1=Not Full)
							// When GP1(04h)=2 ---> Same as GPUSTAT.28
							// When GP1(04h)=3 ---> Same as GPUSTAT.27
		uint32_t RCMD	: 1;	// 26    Ready to receive Cmd Word   (0=No, 1=Ready)  ;GP0(...) ;via GP0
		uint32_t RSEND	: 1;	// 27    Ready to send VRAM to CPU   (0=No, 1=Ready)  ;GP0(C0h) ;via GPUREAD
		uint32_t RDMA	: 1;	// 28    Ready to receive DMA Block  (0=No, 1=Ready)  ;GP0(...) ;via GP0
		uint32_t DDIR	: 2;	// 29-30 DMA Direction (0=Off, 1=?, 2=CPUtoGP0, 3=GPUREADtoCPU)
		uint32_t DEO		: 1;	// 31    Drawing even/odd lines in interlace mode (0=Even or Vblank, 1=Odd)
	}bits;

	uint32_t _u32;
};

struct PGPUregisters
{
	tPGPU_REGS	stat;
};
static PGPUregisters& pgpu = (PGPUregisters&)eeHw[0xf300];

/* Internal DMA flags: */
struct dma_t
{
	struct dmaState_t
	{
		bool ll_active;
		bool to_gpu_active;
		bool to_iop_active;
	} state;

	struct ll_dma_t
	{
		uint32_t data_read_address;
		uint32_t total_words; //total number of words
		uint32_t current_word; //current word number
		uint32_t next_address;
	} ll_dma;

	struct normalDma_t
	{
		uint32_t total_words; //total number of words in Normal DMA
		uint32_t current_word; //current word number in Normal DMA
		uint32_t address;
	} normal;
};

union tCHCR_DMA
{
	struct chcrDma_t
	{
		uint32_t DIR		: 1;	//0 Transfer Direction    (0=To Main RAM, 1=From Main RAM)
		uint32_t MAS		: 1;	//1 Memory Address Step   (0=Forward;+4, 1=Backward;-4)
		uint32_t resv0	: 6;
		uint32_t CHE		: 1;	//8 Chopping Enable       (0=Normal, 1=Chopping; run CPU during DMA gaps)
		uint32_t TSM		: 2;	//9-10    SyncMode, Transfer Synchronisation/Mode (0-3):
							//0  Start immediately and transfer all at once (used for CDROM, OTC)
							//1  Sync blocks to DMA requests   (used for MDEC, SPU, and GPU-data)
							//2  Linked-List mode              (used for GPU-command-lists)
							//3  Reserved                      (not used)
		uint32_t resv1	: 5;
		uint32_t CDWS	: 3;	// 16-18   Chopping DMA Window Size (1 SHL N words)
		uint32_t resv2	: 1;
		uint32_t CCWS	: 3;	// 20-22   Chopping CPU Window Size (1 SHL N clks)
		uint32_t resv3	: 1;
		uint32_t BUSY	: 1;	// 24      Start/Busy            (0=Stopped/Completed, 1=Start/Enable/Busy)
		uint32_t resv4	: 3;
		uint32_t TRIG	: 1;	// 28      Start/Trigger         (0=Normal, 1=Manual Start; use for SyncMode=0)
		uint32_t UKN1	: 1;	// 29      Unknown (R/W) Pause?  (0=No, 1=Pause?)     (For SyncMode=0 only?)
		uint32_t UNK2	: 1;	// 30      Unknown (R/W)
		uint32_t resv5	: 1;
	}bits;
	uint32_t _u32;
};

union tBCR_DMA
{
	struct bcrDma_t
	{
		uint32_t block_size : 16;
		uint32_t block_amount : 16;
	} bit;

	uint32_t _u32;
};

union tMADR_DMA
{
	uint32_t address;
};

struct DMAregisters
{
	tMADR_DMA	madr;
	tBCR_DMA	bcr;
	tCHCR_DMA	chcr;
};
static DMAregisters& dmaRegs = (DMAregisters&)iopHw[0x10a0];

/* Generic FIFO-related: */
struct ringBuf_t
{
	uint32_t* buf;
	int size;
	int count;
	int head;
	int tail;
};

void pgifInit(void);

extern void psxGPUw(int, uint32_t);
extern uint32_t psxGPUr(int);

extern void PGIFw(int, uint32_t);
extern uint32_t PGIFr(int);

extern void PGIFwQword(uint32_t addr, void *);
extern void PGIFrQword(uint32_t addr, void *);

extern uint32_t psxDma2GpuR(uint32_t addr);
extern void psxDma2GpuW(uint32_t addr, uint32_t data);
