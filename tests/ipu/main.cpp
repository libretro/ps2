// IPU differential-oracle harness: drives the real IPU objects through
// IPUCMD_WRITE / IPUWorker / the real FIFOs, records a golden trace of
// every output qword and end-of-command register state.  Used to prove
// byte-exactness across VLC-table refactors (audit axis 3).
#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPU_Fifo.h"
extern void IPUProcessInterrupt(void);
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"
#include <cstdio>
#include <cstring>

static FILE* gold;

static void drain_out(const char* tag)
{
	u32 qw[4];
	// OFC is the occupancy truth: at 8 qwords the write pointer has
	// wrapped onto the read pointer and pointer equality reads as empty.
	while (ipuRegs.ctrl.OFC > 0)
	{
		ipu_fifo.out.read(qw, 1);
		fprintf(gold, "%s OUT %08x %08x %08x %08x\n", tag, qw[0], qw[1], qw[2], qw[3]);
	}
}

// Pending input model: the real system refills IPU_in_FIFO by DMA when
// the IPU stalls on input.  The harness queues qwords and tops the FIFO
// up inside the pump loop, mirroring that.
static const u8* pend_data;
static int pend_qw;

static void feed_in(const u8* data, int qwords)
{
	pend_data = data;
	pend_qw = qwords;
}

static void top_up(void)
{
	while (pend_qw > 0 && ipu_fifo.in.write((const u32*)pend_data, 1) == 1)
	{
		pend_data += 16;
		pend_qw--;
	}
}

static void pump(const char* tag)
{
	// IPUWorker returns when done or stalled on FIFO; drain and retry a
	// bounded number of rounds so output-FIFO stalls make progress.
	for (int round = 0; round < 256 && ipuRegs.ctrl.BUSY; round++)
	{
		top_up();
		IPUProcessInterrupt();
		drain_out(tag);
	}
	fprintf(gold, "%s END ctrl=%08x bp=%02x ifc=%d fp=%d cmdDATA=%08x cmdBUSY=%u top=%08x\n",
		tag, ipuRegs.ctrl._u32, g_BP.BP, g_BP.IFC, g_BP.FP,
		ipuRegs.cmd.DATA, ipuRegs.cmd.BUSY ? 1u : 0u, ipuRegs.top);
}

static void cmd(u32 c, const char* tag)
{
	top_up();
	IPUCMD_WRITE(c);
	pump(tag);
}

int main(int argc, char** argv)
{
	gold = fopen(argc > 1 ? argv[1] : "golden.txt", "w");
	ipu_fifo.init();
	ipuReset();

	// --- deterministic input pattern: 24 qwords of LCG bytes
	u8 buf[16 * 64];
	u32 s = 0x12345678;
	for (size_t i = 0; i < sizeof(buf); i++) { s = s * 1103515245 + 12345; buf[i] = (u8)(s >> 16); }

	// T1: CSC one macroblock, RGB32, thresholds off (384 bytes = 24 qw in)
	cmd(0x00000000, "T1-BCLR");
	feed_in(buf, 24);
	cmd(0x70000001, "T1-CSC");           // CSC MBC=1 DTE=0 OFM=0 (RGB32)

	// T2: CSC RGB16+dither, SETTH active
	cmd(0x90000000 | (0x40 << 16) | 0x20, "T2-SETTH"); // TH0=0x20 TH1=0x40
	cmd(0x00000000, "T2-BCLR");
	feed_in(buf, 24);
	cmd(0x78000001 | (1u << 26), "T2-CSC16");  // CSC MBC=1 DTE=1(b26) OFM=1(b27) RGB16

	// T3: SETVQ + PACK to INDX4 (RGB32 in: 24 qw), then PACK to RGB16
	cmd(0x00000000, "T3-BCLR");
	feed_in(buf, 2);                      // 32 bytes CLUT
	cmd(0x60000000, "T3-SETVQ");
	cmd(0x00000000, "T3-BCLR2");
	feed_in(buf, 64);                     // RGB32 macroblock = 64 qwords
	cmd(0x80000001, "T3-PACK4");          // PACK MBC=1 OFM=0 (INDX4)
	cmd(0x00000000, "T3-BCLR3");
	feed_in(buf, 64);
	cmd(0x88000001 | (1u << 26), "T3-PACK16"); // PACK DTE=1(b26) OFM=1(b27) RGB16

	// T4: SETIQ intra + non-intra (8 qw each)
	cmd(0x00000000, "T4-BCLR");
	feed_in(buf, 8);
	cmd(0x50000000, "T4-SETIQ-I");
	feed_in(buf, 8);
	cmd(0x58000000, "T4-SETIQ-N");        // IQM=1 (bit27)

	// T5: VDEC MBAincrement + FDEC on the pattern (exercises VLC path + TOP)
	cmd(0x00000000, "T5-BCLR");
	feed_in(buf, 8);
	cmd(0x30000000, "T5-VDEC");           // TBL=0 FB=0
	cmd(0x40000000, "T5-FDEC");

	// T6: BDEC intra on pseudo-random bits: exercises coefficient decode
	// and the ECD path; end state is the assertion, crash-freedom the gate.
	// T6a: BDEC on all-zero bits: deterministic invalid-VLC -> ECD end.
	static const u8 zeros[16 * 8] = {};
	cmd(0x00000000, "T6-BCLR");
	feed_in(zeros, 8);
	cmd(0x20000000 | (1u << 27), "T6-BDEC-ECD");
	// T6b: BDEC on the pseudo-random pattern: exercises coefficient VLC
	// paths; the deterministic end state (or bounded stall) is the golden.
	cmd(0x00000000, "T6b-BCLR");
	feed_in(buf, 64);
	cmd(0x20000000 | (1u << 27), "T6b-BDEC");

	/* ---- coverage sweep ----------------------------------------------
	 * The cases above are hand-built and readable, but they only touch
	 * VDEC at TBL 0.  The macroblock-layer tables (macroblock_type,
	 * motion_code, dmvector, coded_block_pattern) are reached through
	 * the other TBL selections and through IDEC's slice decoder, and a
	 * truncated CBP_7 shipped once because nothing here exercised them.
	 * This sweep drives every command surface over pseudo-random
	 * bitstreams; the traces are stable, so any change in decode
	 * behaviour shows up as a diff. */
	static u8 buf[16 * 32];
	s = 0xBEEF00D;   /* reuse the LCG state variable declared above */
	/* Exercise VDEC across all four TBL selections and every picture
	 * type, plus BDEC, on many pseudo-random bit patterns.  TBL 1/2/3
	 * are the macroblock-layer tables (macroblock_type, motion_code,
	 * dmvector) and the CBP path inside BDEC - none of which the
	 * committed golden trace touched. */
	for (int iter = 0; iter < 400; iter++)
	{
		for (size_t i = 0; i < sizeof(buf); i++) { s = s * 1103515245 + 12345; buf[i] = (u8)(s >> 16); }
		for (int pct = 1; pct <= 4; pct++)
		{
			for (int tbl = 0; tbl < 4; tbl++)
			{
				char tag[64];
				snprintf(tag, sizeof(tag), "it%03d/pct%d/tbl%d", iter, pct, tbl);
				cmd(0x00000000, "bclr");
				ipuRegs.ctrl.PCT = pct;
				ipuRegs.ctrl.MP1 = (iter & 1);
				ipuRegs.ctrl.IVF = (iter >> 1) & 1;
				feed_in(buf, 32);
				cmd(0x30000000u | ((u32)tbl << 26), tag);
			}
		}
		char btag[64];
		snprintf(btag, sizeof(btag), "it%03d/bdec", iter);
		cmd(0x00000000, "bclr");
		feed_in(buf, 32);
		cmd(0x20000000u | (1u << 27), btag);

		/* IDEC: the intra slice decoder - the FMV path, and the only
		 * caller of the CBP tables and the macroblock-modes lookups. */
		for (int pct = 1; pct <= 3; pct++)
		{
			char itag[64];
			snprintf(itag, sizeof(itag), "it%03d/idec/pct%d", iter, pct);
			cmd(0x00000000, "bclr");
			ipuRegs.ctrl.PCT = pct;
			feed_in(buf, 32);
			cmd(0x10000000u | (1u << 24) | (0x10u << 16), itag); /* IDEC QSC=16, DTD/SGN off */
		}
	}
	fclose(gold);
	return 0;
}
