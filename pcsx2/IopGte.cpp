/*  PCSX2 - PS2 Emulator for PCs
*  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include "IopGte.h"
#include "IopHw.h"
#include "R3000A.h"
#include "IopDma.h"
#include "IopMem.h"
#include "IopPgpuGif.h"
#include "Common.h"
#include "Sif.h"
#include "Sio.h"
#include "FW.h"
#include "CDVD/Ps1CD.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "IopCounters.h"

#include "Mdec.h"

#include "common/MathUtils.h"

#include "HwInternal.h"

#define SUM_FLAG if(gteFLAG & 0x7F87E000) gteFLAG |= 0x80000000;

#ifdef _MSC_VER_
#pragma warning(disable:4244)
#pragma warning(disable:4761)
#endif

#define gteVX0     ((int16_t*)psxRegs.CP2D.r)[0]
#define gteVY0     ((int16_t*)psxRegs.CP2D.r)[1]
#define gteVZ0     ((int16_t*)psxRegs.CP2D.r)[2]
#define gteVX1     ((int16_t*)psxRegs.CP2D.r)[4]
#define gteVY1     ((int16_t*)psxRegs.CP2D.r)[5]
#define gteVZ1     ((int16_t*)psxRegs.CP2D.r)[6]
#define gteVX2     ((int16_t*)psxRegs.CP2D.r)[8]
#define gteVY2     ((int16_t*)psxRegs.CP2D.r)[9]
#define gteVZ2     ((int16_t*)psxRegs.CP2D.r)[10]
#define gteRGB     psxRegs.CP2D.r[6]
#define gteOTZ     ((int16_t*)psxRegs.CP2D.r)[7*2]
#define gteIR0     ((int32_t*)psxRegs.CP2D.r)[8]
#define gteIR1     ((int32_t*)psxRegs.CP2D.r)[9]
#define gteIR2     ((int32_t*)psxRegs.CP2D.r)[10]
#define gteIR3     ((int32_t*)psxRegs.CP2D.r)[11]
#define gteSXY0    ((int32_t*)psxRegs.CP2D.r)[12]
#define gteSXY1    ((int32_t*)psxRegs.CP2D.r)[13]
#define gteSXY2    ((int32_t*)psxRegs.CP2D.r)[14]
#define gteSXYP    ((int32_t*)psxRegs.CP2D.r)[15]
#define gteSX0     ((int16_t*)psxRegs.CP2D.r)[12*2]
#define gteSY0     ((int16_t*)psxRegs.CP2D.r)[12*2+1]
#define gteSX1     ((int16_t*)psxRegs.CP2D.r)[13*2]
#define gteSY1     ((int16_t*)psxRegs.CP2D.r)[13*2+1]
#define gteSX2     ((int16_t*)psxRegs.CP2D.r)[14*2]
#define gteSY2     ((int16_t*)psxRegs.CP2D.r)[14*2+1]
#define gteSXP     ((int16_t*)psxRegs.CP2D.r)[15*2]
#define gteSYP     ((int16_t*)psxRegs.CP2D.r)[15*2+1]
#define gteSZx     ((uint16_t*)psxRegs.CP2D.r)[16*2]
#define gteSZ0     ((uint16_t*)psxRegs.CP2D.r)[17*2]
#define gteSZ1     ((uint16_t*)psxRegs.CP2D.r)[18*2]
#define gteSZ2     ((uint16_t*)psxRegs.CP2D.r)[19*2]
#define gteRGB0    psxRegs.CP2D.r[20]
#define gteRGB1    psxRegs.CP2D.r[21]
#define gteRGB2    psxRegs.CP2D.r[22]
#define gteMAC0    psxRegs.CP2D.r[24]
#define gteMAC1    ((int32_t*)psxRegs.CP2D.r)[25]
#define gteMAC2    ((int32_t*)psxRegs.CP2D.r)[26]
#define gteMAC3    ((int32_t*)psxRegs.CP2D.r)[27]
#define gteIRGB    psxRegs.CP2D.r[28]
#define gteORGB    psxRegs.CP2D.r[29]
#define gteLZCS    psxRegs.CP2D.r[30]
#define gteLZCR    psxRegs.CP2D.r[31]

#define gteR       ((u8 *)psxRegs.CP2D.r)[6*4]
#define gteG       ((u8 *)psxRegs.CP2D.r)[6*4+1]
#define gteB       ((u8 *)psxRegs.CP2D.r)[6*4+2]
#define gteCODE    ((u8 *)psxRegs.CP2D.r)[6*4+3]
#define gteC       gteCODE

#define gteR0      ((u8 *)psxRegs.CP2D.r)[20*4]
#define gteG0      ((u8 *)psxRegs.CP2D.r)[20*4+1]
#define gteB0      ((u8 *)psxRegs.CP2D.r)[20*4+2]
#define gteCODE0   ((u8 *)psxRegs.CP2D.r)[20*4+3]
#define gteC0      gteCODE0

#define gteR1      ((u8 *)psxRegs.CP2D.r)[21*4]
#define gteG1      ((u8 *)psxRegs.CP2D.r)[21*4+1]
#define gteB1      ((u8 *)psxRegs.CP2D.r)[21*4+2]
#define gteCODE1   ((u8 *)psxRegs.CP2D.r)[21*4+3]
#define gteC1      gteCODE1

#define gteR2      ((u8 *)psxRegs.CP2D.r)[22*4]
#define gteG2      ((u8 *)psxRegs.CP2D.r)[22*4+1]
#define gteB2      ((u8 *)psxRegs.CP2D.r)[22*4+2]
#define gteCODE2   ((u8 *)psxRegs.CP2D.r)[22*4+3]
#define gteC2      gteCODE2



#define gteR11  ((int16_t*)psxRegs.CP2C.r)[0]
#define gteR12  ((int16_t*)psxRegs.CP2C.r)[1]
#define gteR13  ((int16_t*)psxRegs.CP2C.r)[2]
#define gteR21  ((int16_t*)psxRegs.CP2C.r)[3]
#define gteR22  ((int16_t*)psxRegs.CP2C.r)[4]
#define gteR23  ((int16_t*)psxRegs.CP2C.r)[5]
#define gteR31  ((int16_t*)psxRegs.CP2C.r)[6]
#define gteR32  ((int16_t*)psxRegs.CP2C.r)[7]
#define gteR33  ((int16_t*)psxRegs.CP2C.r)[8]
#define gteTRX  ((int32_t*)psxRegs.CP2C.r)[5]
#define gteTRY  ((int32_t*)psxRegs.CP2C.r)[6]
#define gteTRZ  ((int32_t*)psxRegs.CP2C.r)[7]
#define gteL11  ((int16_t*)psxRegs.CP2C.r)[16]
#define gteL12  ((int16_t*)psxRegs.CP2C.r)[17]
#define gteL13  ((int16_t*)psxRegs.CP2C.r)[18]
#define gteL21  ((int16_t*)psxRegs.CP2C.r)[19]
#define gteL22  ((int16_t*)psxRegs.CP2C.r)[20]
#define gteL23  ((int16_t*)psxRegs.CP2C.r)[21]
#define gteL31  ((int16_t*)psxRegs.CP2C.r)[22]
#define gteL32  ((int16_t*)psxRegs.CP2C.r)[23]
#define gteL33  ((int16_t*)psxRegs.CP2C.r)[24]
#define gteRBK  ((int32_t*)psxRegs.CP2C.r)[13]
#define gteGBK  ((int32_t*)psxRegs.CP2C.r)[14]
#define gteBBK  ((int32_t*)psxRegs.CP2C.r)[15]
#define gteLR1  ((int16_t*)psxRegs.CP2C.r)[32]
#define gteLR2  ((int16_t*)psxRegs.CP2C.r)[33]
#define gteLR3  ((int16_t*)psxRegs.CP2C.r)[34]
#define gteLG1  ((int16_t*)psxRegs.CP2C.r)[35]
#define gteLG2  ((int16_t*)psxRegs.CP2C.r)[36]
#define gteLG3  ((int16_t*)psxRegs.CP2C.r)[37]
#define gteLB1  ((int16_t*)psxRegs.CP2C.r)[38]
#define gteLB2  ((int16_t*)psxRegs.CP2C.r)[39]
#define gteLB3  ((int16_t*)psxRegs.CP2C.r)[40]
#define gteRFC  ((int32_t*)psxRegs.CP2C.r)[21]
#define gteGFC  ((int32_t*)psxRegs.CP2C.r)[22]
#define gteBFC  ((int32_t*)psxRegs.CP2C.r)[23]
#define gteOFX  ((int32_t*)psxRegs.CP2C.r)[24]
#define gteOFY  ((int32_t*)psxRegs.CP2C.r)[25]
#define gteH    ((uint16_t*)psxRegs.CP2C.r)[52]
#define gteDQA  ((int16_t*)psxRegs.CP2C.r)[54]
#define gteDQB  ((int32_t*)psxRegs.CP2C.r)[28]
#define gteZSF3 ((int16_t*)psxRegs.CP2C.r)[58]
#define gteZSF4 ((int16_t*)psxRegs.CP2C.r)[60]
#define gteFLAG psxRegs.CP2C.r[31]

__inline uint32_t MFC2(int reg)
{
	if (reg == 29)
	{
		gteORGB = (((gteIR1 >> 7) & 0x1f))
			| (((gteIR2 >> 7) & 0x1f) << 5)
			| (((gteIR3 >> 7) & 0x1f) << 10);
		return gteORGB;
	}
	return psxRegs.CP2D.r[reg];
}

__inline void MTC2(uint32_t value, int reg)
{
	switch (reg) {
	case 8: case 9: case 10: case 11:
		psxRegs.CP2D.r[reg] = (int16_t)value;
		break;

	case 15:
		gteSXY0 = gteSXY1;
		gteSXY1 = gteSXY2;
		gteSXY2 = value;
		gteSXYP = value;
		break;

	case 16: case 17: case 18: case 19:
		psxRegs.CP2D.r[reg] = (value & 0xffff);
		break;

	case 28:
		psxRegs.CP2D.r[28] = value;
		gteIR1 = ((value)& 0x1f) << 7;
		gteIR2 = ((value >> 5) & 0x1f) << 7;
		gteIR3 = ((value >> 10) & 0x1f) << 7;
		break;

	case 30:
		psxRegs.CP2D.r[30] = value;
		psxRegs.CP2D.r[31] = count_leading_sign_bits(value);
		break;

	default:
		psxRegs.CP2D.r[reg] = value;
	}
}

void gteMFC2(void)
{
	if (_Rt_)
		psxRegs.GPR.r[_Rt_] = MFC2(_Rd_);
}

void gteCFC2(void)
{
	if (_Rt_)
		psxRegs.GPR.r[_Rt_] = psxRegs.CP2C.r[_Rd_];
}

void gteMTC2(void) { MTC2(psxRegs.GPR.r[_Rt_], _Rd_); }
void gteCTC2(void) { psxRegs.CP2C.r[_Rd_] = psxRegs.GPR.r[_Rt_]; }

#define _oB_ (psxRegs.GPR.r[_Rs_] + _Imm_)

void gteLWC2(void) { MTC2(iopMemRead32(_oB_), _Rt_); }
void gteSWC2(void) { iopMemWrite32(_oB_, MFC2(_Rt_)); }

/////LIMITATIONS AND OTHER STUFF************************************

__inline double NC_OVERFLOW1(double x)
{
	if (x<-2147483648.0) gteFLAG |= 1 << 29;
	else if (x> 2147483647.0) gteFLAG |= 1 << 26;
	return x;
}

__inline double NC_OVERFLOW2(double x)
{
	if (x<-2147483648.0) gteFLAG |= 1 << 28;
	else if (x> 2147483647.0) gteFLAG |= 1 << 25;
	return x;
}

__inline double NC_OVERFLOW3(double x)
{
	if (x<-2147483648.0) gteFLAG |= 1 << 27;
	else if (x> 2147483647.0) gteFLAG |= 1 << 24;
	return x;
}

__inline double NC_OVERFLOW4(double x)
{
	if (x<-2147483648.0)
		gteFLAG |= 1 << 16;
	else if (x> 2147483647.0)
		gteFLAG |= 1 << 15;

	return x;
}

__inline int32_t FNC_OVERFLOW1(int64_t x)
{
	if (x< (int64_t)0xffffffff80000000)
		gteFLAG |= 1 << 29;
	else if (x> 2147483647)
		gteFLAG |= 1 << 26;

	return (int32_t)x;
}

__inline int32_t FNC_OVERFLOW2(int64_t x)
{
	if (x< (int64_t)0xffffffff80000000)
		gteFLAG |= 1 << 28;
	else if (x> 2147483647)
		gteFLAG |= 1 << 25;

	return (int32_t)x;
}

__inline int32_t FNC_OVERFLOW3(int64_t x)
{
	if (x< (int64_t)0xffffffff80000000)
		gteFLAG |= 1 << 27;
	else if (x> 2147483647)
		gteFLAG |= 1 << 24;

	return (int32_t)x;
}

__inline int32_t FNC_OVERFLOW4(int64_t x)
{
	if (x< (int64_t)0xffffffff80000000)
		gteFLAG |= 1 << 16;
	else if (x> 2147483647)
		gteFLAG |= 1 << 15;

	return (int32_t)x;
}

#define _LIMX(negv, posv, flagb) { \
	if (x < (negv)) { x = (negv); gteFLAG |= (1<<(flagb)); } \
	else if (x > (posv)) { x = (posv); gteFLAG |= (1<<(flagb)); } \
	return (x); \
}

__inline double limA1S(double x) { _LIMX(-32768.0, 32767.0, 24); }
__inline double limA2S(double x) { _LIMX(-32768.0, 32767.0, 23); }
__inline double limA3S(double x) { _LIMX(-32768.0, 32767.0, 22); }
__inline double limA1U(double x) { _LIMX(0.0, 32767.0, 24); }
__inline double limA2U(double x) { _LIMX(0.0, 32767.0, 23); }
__inline double limA3U(double x) { _LIMX(0.0, 32767.0, 22); }
__inline double limB1(double x) { _LIMX(0.0, 255.0, 21); }
__inline double limB2(double x) { _LIMX(0.0, 255.0, 20); }
__inline double limB3(double x) { _LIMX(0.0, 255.0, 19); }
__inline double limC(double x) { _LIMX(0.0, 65535.0, 18); }
__inline double limD1(double x) { _LIMX(-1024.0, 1023.0, 14); }
__inline double limD2(double x) { _LIMX(-1024.0, 1023.0, 13); }
__inline double limE(double x) { _LIMX(0.0, 4095.0, 12); }

__inline double limG1(double x) {
	if (x > 2147483647.0)
		gteFLAG |= (1 << 16);
	else
		if (x <-2147483648.0)
			gteFLAG |= (1 << 15);

	if (x >       1023.0)
	{
		x = 1023.0;
		gteFLAG |= (1 << 14);
	}
	else
		if (x <      -1024.0)
		{
			x = -1024.0;
			gteFLAG |= (1 << 14);
		}

	return (x);
}

__inline double limG2(double x) {
	if (x > 2147483647.0) { gteFLAG |= (1 << 16); }
	else
		if (x <-2147483648.0) { gteFLAG |= (1 << 15); }

	if (x >       1023.0) { x = 1023.0; gteFLAG |= (1 << 13); }
	else
		if (x < -1024.0) { x = -1024.0; gteFLAG |= (1 << 13); }

	return (x);
}

__inline int32_t F12limA1S(int64_t x) { _LIMX(-(32768 << 12), 32767 << 12, 24); }
__inline int32_t F12limA2S(int64_t x) { _LIMX(-(32768 << 12), 32767 << 12, 23); }
__inline int32_t F12limA3S(int64_t x) { _LIMX(-(32768 << 12), 32767 << 12, 22); }
__inline int32_t F12limA1U(int64_t x) { _LIMX(0, 32767 << 12, 24); }
__inline int32_t F12limA2U(int64_t x) { _LIMX(0, 32767 << 12, 23); }
__inline int32_t F12limA3U(int64_t x) { _LIMX(0, 32767 << 12, 22); }

__inline int16_t FlimA1S(int32_t x) { _LIMX(-32768, 32767, 24); }
__inline int16_t FlimA2S(int32_t x) { _LIMX(-32768, 32767, 23); }
__inline int16_t FlimA3S(int32_t x) { _LIMX(-32768, 32767, 22); }
__inline int16_t FlimA1U(int32_t x) { _LIMX(0, 32767, 24); }
__inline int16_t FlimA2U(int32_t x) { _LIMX(0, 32767, 23); }
__inline int16_t FlimA3U(int32_t x) { _LIMX(0, 32767, 22); }
__inline u8  FlimB1(int32_t x) { _LIMX(0, 255, 21); }
__inline u8  FlimB2(int32_t x) { _LIMX(0, 255, 20); }
__inline u8  FlimB3(int32_t x) { _LIMX(0, 255, 19); }
__inline uint16_t FlimC(int32_t x) { _LIMX(0, 65535, 18); }
__inline int32_t FlimD1(int32_t x) { _LIMX(-1024, 1023, 14); }
__inline int32_t FlimD2(int32_t x) { _LIMX(-1024, 1023, 13); }
__inline int32_t FlimE(int32_t x) { _LIMX(0, 65535, 12); }

__inline int32_t FlimG1(int64_t x)
{
	if (x > 2147483647)
		gteFLAG |= (1 << 16);
	else if (x < (int64_t)0xffffffff80000000)
		gteFLAG |= (1 << 15);

	if (x >       1023)
	{
		x = 1023;
		gteFLAG |= (1 << 14);
	}
	else if (x <      -1024)
	{
		x = -1024;
		gteFLAG |= (1 << 14);
	}

	return (x);
}

__inline int32_t FlimG2(int64_t x)
{
	if (x > 2147483647)
		gteFLAG |= (1 << 16);
	else
		if (x < (int64_t)0xffffffff80000000)
			gteFLAG |= (1 << 15);

	if (x > 1023)
	{
		x = 1023;
		gteFLAG |= (1 << 13);
	}
	else
		if (x < -1024)
		{
			x = -1024;
			gteFLAG |= (1 << 13);
		}

	return (x);
}

#define MAC2IR() { \
	if (gteMAC1 < (int32_t)(-32768)) { gteIR1=(int32_t)(-32768); gteFLAG|=1<<24;} \
else \
	if (gteMAC1 > (int32_t)( 32767)) { gteIR1=(int32_t)( 32767); gteFLAG|=1<<24;} \
else gteIR1=(int32_t)gteMAC1; \
	if (gteMAC2 < (int32_t)(-32768)) { gteIR2=(int32_t)(-32768); gteFLAG|=1<<23;} \
else \
	if (gteMAC2 > (int32_t)( 32767)) { gteIR2=(int32_t)( 32767); gteFLAG|=1<<23;} \
else gteIR2=(int32_t)gteMAC2; \
	if (gteMAC3 < (int32_t)(-32768)) { gteIR3=(int32_t)(-32768); gteFLAG|=1<<22;} \
else \
	if (gteMAC3 > (int32_t)( 32767)) { gteIR3=(int32_t)( 32767); gteFLAG|=1<<22;} \
else gteIR3=(int32_t)gteMAC3; \
}


#define MAC2IR1() {           \
	if (gteMAC1 < (int32_t)0) { gteIR1=(int32_t)0; gteFLAG|=1<<24;}  \
else if (gteMAC1 > (int32_t)(32767)) { gteIR1=(int32_t)(32767); gteFLAG|=1<<24;} \
else gteIR1=(int32_t)gteMAC1;                                                         \
	if (gteMAC2 < (int32_t)0) { gteIR2=(int32_t)0; gteFLAG|=1<<23;}      \
else if (gteMAC2 > (int32_t)(32767)) { gteIR2=(int32_t)(32767); gteFLAG|=1<<23;}    \
else gteIR2=(int32_t)gteMAC2;                                                            \
	if (gteMAC3 < (int32_t)0) { gteIR3=(int32_t)0; gteFLAG|=1<<22;}         \
else if (gteMAC3 > (int32_t)(32767)) { gteIR3=(int32_t)(32767); gteFLAG|=1<<22;}       \
else gteIR3=(int32_t)gteMAC3; \
}

//********END OF LIMITATIONS**********************************/

#define GTE_RTPS1(vn) \
	gteMAC1 = FNC_OVERFLOW1(((int32_t)(gteR11*gteVX##vn + gteR12*gteVY##vn + gteR13*gteVZ##vn)>>12) + gteTRX); \
	gteMAC2 = FNC_OVERFLOW2(((int32_t)(gteR21*gteVX##vn + gteR22*gteVY##vn + gteR23*gteVZ##vn)>>12) + gteTRY); \
	gteMAC3 = FNC_OVERFLOW3(((int32_t)(gteR31*gteVX##vn + gteR32*gteVY##vn + gteR33*gteVZ##vn)>>12) + gteTRZ); \

#define GTE_RTPS2(vn) { \
	if (gteSZ##vn == 0) { \
	FDSZ = 2 << 16; gteFLAG |= 1<<17; \
	} else { \
	FDSZ = ((uint64_t)gteH << 32) / ((uint64_t)gteSZ##vn << 16); \
	if ((uint64_t)FDSZ > (2 << 16)) { FDSZ = 2 << 16; gteFLAG |= 1<<17; } \
	} \
	\
	gteSX##vn = FlimG1((gteOFX + (((int64_t)((int64_t)gteIR1 << 16) * FDSZ) >> 16)) >> 16); \
	gteSY##vn = FlimG2((gteOFY + (((int64_t)((int64_t)gteIR2 << 16) * FDSZ) >> 16)) >> 16); \
}

#define GTE_RTPS3() \
	FDSZ    = (int64_t)((int64_t)gteDQB + (((int64_t)((int64_t)gteDQA << 8) * FDSZ) >> 8)); \
	gteMAC0 = FDSZ; \
	gteIR0  = FlimE(FDSZ >> 12); \

void gteRTPS(void)
{
	int64_t FDSZ;

	gteFLAG = 0;

	GTE_RTPS1(0);

	MAC2IR();

	gteSZx = gteSZ0;
	gteSZ0 = gteSZ1;
	gteSZ1 = gteSZ2;
	gteSZ2 = FlimC(gteMAC3);

	gteSXY0 = gteSXY1;
	gteSXY1 = gteSXY2;

	GTE_RTPS2(2);
	gteSXYP = gteSXY2;

	GTE_RTPS3();

	SUM_FLAG;
}

void gteRTPT() {
	int64_t FDSZ;

	gteFLAG = 0;

	gteSZx = gteSZ2;

	GTE_RTPS1(0);

	gteSZ0 = FlimC(gteMAC3);

	gteIR1 = FlimA1S(gteMAC1);
	gteIR2 = FlimA2S(gteMAC2);
	GTE_RTPS2(0);

	GTE_RTPS1(1);

	gteSZ1 = FlimC(gteMAC3);

	gteIR1 = FlimA1S(gteMAC1);
	gteIR2 = FlimA2S(gteMAC2);
	GTE_RTPS2(1);

	GTE_RTPS1(2);

	MAC2IR();

	gteSZ2 = FlimC(gteMAC3);

	GTE_RTPS2(2);
	gteSXYP = gteSXY2;

	GTE_RTPS3();

	SUM_FLAG;
}

#define gte_C11 gteLR1
#define gte_C12 gteLR2
#define gte_C13 gteLR3
#define gte_C21 gteLG1
#define gte_C22 gteLG2
#define gte_C23 gteLG3
#define gte_C31 gteLB1
#define gte_C32 gteLB2
#define gte_C33 gteLB3

#define _MVMVA_FUNC(_v0, _v1, _v2, mx) { \
	SSX = (_v0) * mx##11 + (_v1) * mx##12 + (_v2) * mx##13; \
	SSY = (_v0) * mx##21 + (_v1) * mx##22 + (_v2) * mx##23; \
	SSZ = (_v0) * mx##31 + (_v1) * mx##32 + (_v2) * mx##33; \
}

void gteMVMVA() {
	int64_t SSX, SSY, SSZ;


	switch (psxRegs.code & 0x78000) {
	case 0x00000: // V0 * R
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gteR); break;
	case 0x08000: // V1 * R
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gteR); break;
	case 0x10000: // V2 * R
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gteR); break;
	case 0x18000: // IR * R
		_MVMVA_FUNC((int16_t)gteIR1, (int16_t)gteIR2, (int16_t)gteIR3, gteR);
		break;
	case 0x20000: // V0 * L
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gteL); break;
	case 0x28000: // V1 * L
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gteL); break;
	case 0x30000: // V2 * L
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gteL); break;
	case 0x38000: // IR * L
		_MVMVA_FUNC((int16_t)gteIR1, (int16_t)gteIR2, (int16_t)gteIR3, gteL); break;
	case 0x40000: // V0 * C
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gte_C); break;
	case 0x48000: // V1 * C
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gte_C); break;
	case 0x50000: // V2 * C
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gte_C); break;
	case 0x58000: // IR * C
		_MVMVA_FUNC((int16_t)gteIR1, (int16_t)gteIR2, (int16_t)gteIR3, gte_C); break;
	default:
		SSX = SSY = SSZ = 0;
	}

	if (psxRegs.code & 0x80000)
	{
		SSX >>= 12;
		SSY >>= 12;
		SSZ >>= 12;
	}

	switch (psxRegs.code & 0x6000)
	{
		case 0x0000: // Add TR
			SSX += gteTRX;
			SSY += gteTRY;
			SSZ += gteTRZ;
			break;
		case 0x2000: // Add BK
			SSX += gteRBK;
			SSY += gteGBK;
			SSZ += gteBBK;
			break;
		case 0x4000: // Add FC
			SSX += gteRFC;
			SSY += gteGFC;
			SSZ += gteBFC;
			break;
	}

	gteFLAG = 0;
	gteMAC1 = FNC_OVERFLOW1(SSX);
	gteMAC2 = FNC_OVERFLOW2(SSY);
	gteMAC3 = FNC_OVERFLOW3(SSZ);
	if (psxRegs.code & 0x400)
		MAC2IR1()
	else MAC2IR()

	SUM_FLAG;
}

void gteNCLIP(void)
{
	gteFLAG = 0;
	gteMAC0 = gteSX0 * (gteSY1 - gteSY2) +
		gteSX1 * (gteSY2 - gteSY0) +
		gteSX2 * (gteSY0 - gteSY1);

	SUM_FLAG;
}

void gteAVSZ3(void)
{
	gteFLAG = 0;
	gteMAC0 = ((gteSZ0 + gteSZ1 + gteSZ2) * (gteZSF3)) >> 12;

	gteOTZ = FlimC(gteMAC0);

	SUM_FLAG
}

void gteAVSZ4(void)
{
	gteFLAG = 0;
	gteMAC0 = ((gteSZx + gteSZ0 + gteSZ1 + gteSZ2) * (gteZSF4)) >> 12;

	gteOTZ = FlimC(gteMAC0);

	SUM_FLAG
}

void gteSQR() {
	gteFLAG = 0;

	if (psxRegs.code & 0x80000) {
		gteMAC1 = FNC_OVERFLOW1((gteIR1 * gteIR1) >> 12);
		gteMAC2 = FNC_OVERFLOW2((gteIR2 * gteIR2) >> 12);
		gteMAC3 = FNC_OVERFLOW3((gteIR3 * gteIR3) >> 12);
	}
	else {
		gteMAC1 = FNC_OVERFLOW1(gteIR1 * gteIR1);
		gteMAC2 = FNC_OVERFLOW2(gteIR2 * gteIR2);
		gteMAC3 = FNC_OVERFLOW3(gteIR3 * gteIR3);
	}
	MAC2IR1();

	SUM_FLAG
}

#define GTE_NCCS(vn) \
	gte_LL1 = F12limA1U((gteL11*gteVX##vn + gteL12*gteVY##vn + gteL13*gteVZ##vn) >> 12); \
	gte_LL2 = F12limA2U((gteL21*gteVX##vn + gteL22*gteVY##vn + gteL23*gteVZ##vn) >> 12); \
	gte_LL3 = F12limA3U((gteL31*gteVX##vn + gteL32*gteVY##vn + gteL33*gteVZ##vn) >> 12); \
	gte_RRLT= F12limA1U(gteRBK + ((gteLR1*gte_LL1 + gteLR2*gte_LL2 + gteLR3*gte_LL3) >> 12)); \
	gte_GGLT= F12limA2U(gteGBK + ((gteLG1*gte_LL1 + gteLG2*gte_LL2 + gteLG3*gte_LL3) >> 12)); \
	gte_BBLT= F12limA3U(gteBBK + ((gteLB1*gte_LL1 + gteLB2*gte_LL2 + gteLB3*gte_LL3) >> 12)); \
	\
	gteMAC1 = (int32_t)(((int64_t)((u32)gteR<<12)*gte_RRLT) >> 20);\
	gteMAC2 = (int32_t)(((int64_t)((u32)gteG<<12)*gte_GGLT) >> 20);\
	gteMAC3 = (int32_t)(((int64_t)((u32)gteB<<12)*gte_BBLT) >> 20);


void gteNCCS() {
	int32_t gte_LL1, gte_LL2, gte_LL3;
	int32_t gte_RRLT, gte_GGLT, gte_BBLT;


	gteFLAG = 0;

	GTE_NCCS(0);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;
	gteR2   = FlimB1(gteMAC1 >> 4);
	gteG2   = FlimB2(gteMAC2 >> 4);
	gteB2   = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG
}

void gteNCCT(void) {
	int32_t gte_LL1, gte_LL2, gte_LL3;
	int32_t gte_RRLT, gte_GGLT, gte_BBLT;



	gteFLAG = 0;

	GTE_NCCS(0);

	gteR0 = FlimB1(gteMAC1 >> 4);
	gteG0 = FlimB2(gteMAC2 >> 4);
	gteB0 = FlimB3(gteMAC3 >> 4);
	gteCODE0 = gteCODE;

	GTE_NCCS(1);

	gteR1 = FlimB1(gteMAC1 >> 4);
	gteG1 = FlimB2(gteMAC2 >> 4);
	gteB1 = FlimB3(gteMAC3 >> 4);
	gteCODE1 = gteCODE;

	GTE_NCCS(2);

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4);
	gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG
}
#define GTE_NCDS(vn) \
	gte_LL1 = F12limA1U((gteL11*gteVX##vn + gteL12*gteVY##vn + gteL13*gteVZ##vn) >> 12); \
	gte_LL2 = F12limA2U((gteL21*gteVX##vn + gteL22*gteVY##vn + gteL23*gteVZ##vn) >> 12); \
	gte_LL3 = F12limA3U((gteL31*gteVX##vn + gteL32*gteVY##vn + gteL33*gteVZ##vn) >> 12); \
	gte_RRLT= F12limA1U(gteRBK + ((gteLR1*gte_LL1 + gteLR2*gte_LL2 + gteLR3*gte_LL3) >> 12)); \
	gte_GGLT= F12limA2U(gteGBK + ((gteLG1*gte_LL1 + gteLG2*gte_LL2 + gteLG3*gte_LL3) >> 12)); \
	gte_BBLT= F12limA3U(gteBBK + ((gteLB1*gte_LL1 + gteLB2*gte_LL2 + gteLB3*gte_LL3) >> 12)); \
	\
	gte_RR0 = (int32_t)(((int64_t)((u32)gteR<<12)*gte_RRLT) >> 12);\
	gte_GG0 = (int32_t)(((int64_t)((u32)gteG<<12)*gte_GGLT) >> 12);\
	gte_BB0 = (int32_t)(((int64_t)((u32)gteB<<12)*gte_BBLT) >> 12);\
	gteMAC1 = (int32_t)((gte_RR0 + (((int64_t)gteIR0 * F12limA1S((int64_t)(gteRFC << 8) - gte_RR0)) >> 12)) >> 8);\
	gteMAC2 = (int32_t)((gte_GG0 + (((int64_t)gteIR0 * F12limA2S((int64_t)(gteGFC << 8) - gte_GG0)) >> 12)) >> 8);\
	gteMAC3 = (int32_t)((gte_BB0 + (((int64_t)gteIR0 * F12limA3S((int64_t)(gteBFC << 8) - gte_BB0)) >> 12)) >> 8);

void gteNCDS(void)
{
	int32_t gte_LL1, gte_LL2, gte_LL3;
	int32_t gte_RRLT, gte_GGLT, gte_BBLT;
	int32_t gte_RR0, gte_GG0, gte_BB0;

	gteFLAG = 0;
	GTE_NCDS(0);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;
	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG;
}

void gteNCDT() {
	int32_t gte_LL1, gte_LL2, gte_LL3;
	int32_t gte_RRLT, gte_GGLT, gte_BBLT;
	int32_t gte_RR0, gte_GG0, gte_BB0;

	gteFLAG = 0;
	GTE_NCDS(0);

	gteR0 = FlimB1(gteMAC1 >> 4);
	gteG0 = FlimB2(gteMAC2 >> 4);
	gteB0 = FlimB3(gteMAC3 >> 4); gteCODE0 = gteCODE;

	GTE_NCDS(1);

	gteR1 = FlimB1(gteMAC1 >> 4);
	gteG1 = FlimB2(gteMAC2 >> 4);
	gteB1 = FlimB3(gteMAC3 >> 4); gteCODE1 = gteCODE;

	GTE_NCDS(2);

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG;
}

#define	gteD1	(*(int16_t *)&gteR11)
#define	gteD2	(*(int16_t *)&gteR22)
#define	gteD3	(*(int16_t *)&gteR33)

void gteOP(void) {
	gteFLAG = 0;

	if (psxRegs.code & 0x80000)
	{
		gteMAC1 = FNC_OVERFLOW1((gteD2 * gteIR3 - gteD3 * gteIR2) >> 12);
		gteMAC2 = FNC_OVERFLOW2((gteD3 * gteIR1 - gteD1 * gteIR3) >> 12);
		gteMAC3 = FNC_OVERFLOW3((gteD1 * gteIR2 - gteD2 * gteIR1) >> 12);
	}
	else
	{
		gteMAC1 = FNC_OVERFLOW1(gteD2 * gteIR3 - gteD3 * gteIR2);
		gteMAC2 = FNC_OVERFLOW2(gteD3 * gteIR1 - gteD1 * gteIR3);
		gteMAC3 = FNC_OVERFLOW3(gteD1 * gteIR2 - gteD2 * gteIR1);
	}

	MAC2IR();

	SUM_FLAG
}

void gteDCPL(void)
{
	gteMAC1 = ((int32_t)(gteR)*gteIR1 + (gteIR0*(int16_t)FlimA1S(gteRFC - ((gteR*gteIR1) >> 12)))) >> 8;
	gteMAC2 = ((int32_t)(gteG)*gteIR2 + (gteIR0*(int16_t)FlimA2S(gteGFC - ((gteG*gteIR2) >> 12)))) >> 8;
	gteMAC3 = ((int32_t)(gteB)*gteIR3 + (gteIR0*(int16_t)FlimA3S(gteBFC - ((gteB*gteIR3) >> 12)))) >> 8;

	gteFLAG = 0;
	MAC2IR();

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

void gteGPF() {
	gteFLAG = 0;

	if (psxRegs.code & 0x80000) {
		gteMAC1 = FNC_OVERFLOW1((gteIR0 * gteIR1) >> 12);
		gteMAC2 = FNC_OVERFLOW2((gteIR0 * gteIR2) >> 12);
		gteMAC3 = FNC_OVERFLOW3((gteIR0 * gteIR3) >> 12);
	}
	else {
		gteMAC1 = FNC_OVERFLOW1(gteIR0 * gteIR1);
		gteMAC2 = FNC_OVERFLOW2(gteIR0 * gteIR2);
		gteMAC3 = FNC_OVERFLOW3(gteIR0 * gteIR3);
	}
	MAC2IR();

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

void gteGPL() {
	gteFLAG = 0;

	if (psxRegs.code & 0x80000) {
		gteMAC1 = FNC_OVERFLOW1(gteMAC1 + ((gteIR0 * gteIR1) >> 12));
		gteMAC2 = FNC_OVERFLOW2(gteMAC2 + ((gteIR0 * gteIR2) >> 12));
		gteMAC3 = FNC_OVERFLOW3(gteMAC3 + ((gteIR0 * gteIR3) >> 12));
	}
	else {
		gteMAC1 = FNC_OVERFLOW1(gteMAC1 + (gteIR0 * gteIR1));
		gteMAC2 = FNC_OVERFLOW2(gteMAC2 + (gteIR0 * gteIR2));
		gteMAC3 = FNC_OVERFLOW3(gteMAC3 + (gteIR0 * gteIR3));
	}
	MAC2IR();

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG

}

void gteDPCS() {
	gteMAC1 = (gteR << 4) + ((gteIR0*(int16_t)FlimA1S(gteRFC - (gteR << 4))) >> 12);
	gteMAC2 = (gteG << 4) + ((gteIR0*(int16_t)FlimA2S(gteGFC - (gteG << 4))) >> 12);
	gteMAC3 = (gteB << 4) + ((gteIR0*(int16_t)FlimA3S(gteBFC - (gteB << 4))) >> 12);

	gteFLAG = 0;
	MAC2IR();

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

void gteDPCT() {
	gteMAC1 = (gteR0 << 4) + ((gteIR0*(int16_t)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(int16_t)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(int16_t)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	gteMAC1 = (gteR0 << 4) + ((gteIR0*(int16_t)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(int16_t)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(int16_t)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	gteMAC1 = (gteR0 << 4) + ((gteIR0*(int16_t)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(int16_t)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(int16_t)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);
	gteFLAG = 0;
	MAC2IR();
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

#define LOW(a) (((a) < 0) ? 0 : (a))

#define	GTE_NCS(vn)  \
	gte_LL1 = F12limA1U((gteL11*gteVX##vn + gteL12*gteVY##vn + gteL13*gteVZ##vn) >> 12); \
	gte_LL2 = F12limA2U((gteL21*gteVX##vn + gteL22*gteVY##vn + gteL23*gteVZ##vn) >> 12); \
	gte_LL3 = F12limA3U((gteL31*gteVX##vn + gteL32*gteVY##vn + gteL33*gteVZ##vn) >> 12); \
	gteMAC1 = F12limA1U(gteRBK + ((gteLR1*gte_LL1 + gteLR2*gte_LL2 + gteLR3*gte_LL3) >> 12)); \
	gteMAC2 = F12limA2U(gteGBK + ((gteLG1*gte_LL1 + gteLG2*gte_LL2 + gteLG3*gte_LL3) >> 12)); \
	gteMAC3 = F12limA3U(gteBBK + ((gteLB1*gte_LL1 + gteLB2*gte_LL2 + gteLB3*gte_LL3) >> 12));

void gteNCS() {
	int32_t gte_LL1, gte_LL2, gte_LL3;
	gteFLAG = 0;

	GTE_NCS(0);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;
	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG
}

void gteNCT() {
	int32_t gte_LL1, gte_LL2, gte_LL3;

	gteFLAG = 0;

	GTE_NCS(0);

	gteR0 = FlimB1(gteMAC1 >> 4);
	gteG0 = FlimB2(gteMAC2 >> 4);
	gteB0 = FlimB3(gteMAC3 >> 4); gteCODE0 = gteCODE;

	GTE_NCS(1);
	gteR1 = FlimB1(gteMAC1 >> 4);
	gteG1 = FlimB2(gteMAC2 >> 4);
	gteB1 = FlimB3(gteMAC3 >> 4); gteCODE1 = gteCODE;

	GTE_NCS(2);
	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG
}

void gteCC() {
	int32_t RR0, GG0, BB0;
	gteFLAG = 0;

	RR0 = FNC_OVERFLOW1(gteRBK + ((gteLR1*gteIR1 + gteLR2*gteIR2 + gteLR3*gteIR3) >> 12));
	GG0 = FNC_OVERFLOW2(gteGBK + ((gteLG1*gteIR1 + gteLG2*gteIR2 + gteLG3*gteIR3) >> 12));
	BB0 = FNC_OVERFLOW3(gteBBK + ((gteLB1*gteIR1 + gteLB2*gteIR2 + gteLB3*gteIR3) >> 12));

	gteMAC1 = (gteR * RR0) >> 8;
	gteMAC2 = (gteG * GG0) >> 8;
	gteMAC3 = (gteB * BB0) >> 8;

	MAC2IR1();

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG

}

void gteINTPL() { //test opcode
	gteMAC1 = gteIR1 + ((gteIR0*(int16_t)FlimA1S(gteRFC - gteIR1)) >> 12);
	gteMAC2 = gteIR2 + ((gteIR0*(int16_t)FlimA2S(gteGFC - gteIR2)) >> 12);
	gteMAC3 = gteIR3 + ((gteIR0*(int16_t)FlimA3S(gteBFC - gteIR3)) >> 12);
	gteFLAG = 0;

	MAC2IR();
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

void gteCDP() { //test opcode
	double RR0, GG0, BB0;
	gteFLAG = 0;

	RR0 = NC_OVERFLOW1(gteRBK + (gteLR1*gteIR1 + gteLR2*gteIR2 + gteLR3*gteIR3));
	GG0 = NC_OVERFLOW2(gteGBK + (gteLG1*gteIR1 + gteLG2*gteIR2 + gteLG3*gteIR3));
	BB0 = NC_OVERFLOW3(gteBBK + (gteLB1*gteIR1 + gteLB2*gteIR2 + gteLB3*gteIR3));
	gteMAC1 = gteR*RR0 + gteIR0*limA1S(gteRFC - gteR*RR0);
	gteMAC2 = gteG*GG0 + gteIR0*limA2S(gteGFC - gteG*GG0);
	gteMAC3 = gteB*BB0 + gteIR0*limA3S(gteBFC - gteB*BB0);

	MAC2IR1();
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	SUM_FLAG
}

//NOTES (TODO):
/*
- 8 and 16 bit access to the PGPU regs is not emulated... is it ever used? Emulating it would be tricky.
------

Much of the code is ("very") unoptimized, because it is a bit cleaner and more complete this way.

All the PS1 GPU info comes from psx-spx: http://problemkaputt.de/psx-spx.htm


*/

//Defines for address labels:

//PGPU_STAT 0x1000F300 The GP1 - Status register, which PS1DRV writes (emulates) for the IOP to read.
#define PGPU_STAT 0x1000F300

//IMM_E2-IMM_E5 - "immediate response registers" - hold the return values for commands that require immediate response.
//They correspond to GP0() E2-E5 commands.
#define IMM_E2 0x1000F310
#define IMM_E3 0x1000F320
#define IMM_E4 0x1000F330
#define IMM_E5 0x1000F340

//PGIF_CTRL 0x1000F380 Main register for PGIF status info & control.
#define PGIF_CTRL 0x1000F380

//PGPU_CMD_FIFO FIFO buffer for GPU GP1 (CMD reg) CMDs IOP->EE only (unknown if reverse direction is possible).
#define PGPU_CMD_FIFO 0x1000F3C0
//PGPU_DAT_FIFO FIFO buffer for GPU GP0 (DATA reg) IOP->EE, but also EE->IOP. Direction is controlled by reg. 0x80/bit4 (most likely).
//Official name is "GFIFO", according to PS1DRV.
#define PGPU_DAT_FIFO 0x1000F3E0

//write to peripheral
#define DMA_LL_END_CODE 0x00FFFFFF

#define PGPU_DMA_MADR 0x1F8010A0
#define PGPU_DMA_BCR 0x1F8010A4
#define PGPU_DMA_CHCR 0x1F8010A8
#define PGPU_DMA_TADR 0x1F8010AC

#define pgpuDmaTadr HW_DMA2_TADR

/***************************************************************************************************
*** Constants here control code that is either not certainly correct or may affect compatibility ***
***************************************************************************************************/

//PGIF_DAT_RB_LEAVE_FREE - How many elements of the FIFO buffer to leave free in DMA.
//Can be 0 and no faults are observed.
//As the buffer has 32 elements, and normal DMA reads are usually done in 4 qwords (16 words),
//this must be less than 16, otherwise the PS1DRV will never read from the FIFO.
//At one point (in Linked-List DMA), PS1DRV will expect at least a certain number of elements, that is sent as argument to the func.
#define PGIF_DAT_RB_LEAVE_FREE 1

static u32 old_gp0_value = 0;

static void ringBufPut(struct ringBuf_t* rb, u32* data)
{
	if (rb->count < rb->size)
	{
		//there is available space
		*(rb->buf + rb->head) = *data;
		if ((++(rb->head)) >= rb->size)
			rb->head = 0; //wrap back when the end is reached
		rb->count++;
	}
}

static void ringBufGet(struct ringBuf_t* rb, u32* data)
{
	if (rb->count > 0)
	{
		//there is available data
		*data = *(rb->buf + rb->tail);
		if ((++(rb->tail)) >= rb->size)
			rb->tail = 0; //wrap back when the end is reached
		rb->count--;
	}
}

static void ringBufferClear(struct ringBuf_t* rb)
{
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
}

//Ring buffers definition and initialization:
//Command (GP1) FIFO, size= 0x8 words:
#define PGIF_CMD_RB_SIZE 0x8
static struct ringBuf_t rb_gp1; //Ring buffer control variables.
static u32 pgif_gp1_buffer[PGIF_CMD_RB_SIZE] = {0}; //Ring buffer data.

//Data (GP0) FIFO - the so-called (in PS1DRV) "GFIFO", (real) size= 0x20 words:
//Using small (the real) FIFO size, disturbs MDEC video (and other stuff),
//because the MDEC does DMA instantly, while this emulation drains the FIFO,
//only when the PS1DRV gets data from it, which depends on IOP-EE sync, among other things.
//The reson it works in the real hardware, is that the MDEC DMA would run in the pauses of GPU DMA,
//thus the GPU DMA would never get data, MDEC hasn't yet written to RAM yet (too early).
#define PGIF_DAT_RB_SIZE 0x20000
static struct ringBuf_t rb_gp0; //Ring buffer control variables.
static u32 pgif_gp0_buffer[PGIF_DAT_RB_SIZE] = {0}; //Ring buffer data.
dma_t dma;


static void fillFifoOnDrain(void);
static void drainPgpuDmaNrToGpu(void);
static void drainPgpuDmaNrToIop(void);

static void pgpuDmaIntr(int trigDma)
{
	//For the IOP GPU DMA channel.
	//trigDma: 1=normal,ToGPU; 2=normal,FromGPU, 3=LinkedList

	// psxmode: 25.09.2016 at this point, the emulator works even when removing this interrupt call. how? why?
	if (trigDma != 1) //Interrupt on ToGPU DMA breaks some games. TODO: Why?
		psxDmaInterrupt(2);
}

static void drainPgpuDmaLl(void)
{
	if (!dma.state.ll_active)
		return;

	//Some games (Breath of Fire 3 US) set-up linked-list DMA, but don't immediatelly have the list correctly set-up,
	//so the result is that this function loops indefinitely, because of some links pointing back to themselves, forming a loop.
	//The solution is to only start DMA once the GP1(04h) - DMA Direction / Data Request command has been set to the value 0x2 (CPU->GPU DMA)

	//Buffer full - needs to be drained first.
	if (rb_gp0.count >= ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE))
		return;

	if (dma.ll_dma.current_word >= dma.ll_dma.total_words)
	{
		if (dma.ll_dma.next_address == DMA_LL_END_CODE)
		{
			//Reached end of linked list
			dma.state.ll_active = 0;
			dmaRegs.madr.address = 0x00FFFFFF;
			dmaRegs.chcr.bits.BUSY = 0; //Transfer completed => clear busy flag
			pgpuDmaIntr(3);
		}
		else
		{
			//Or the beginning of a new one
			u32 data = iopMemRead32(dma.ll_dma.next_address);
			dmaRegs.madr.address = data & 0x00FFFFFF; //Copy the address in MADR.
			dma.ll_dma.data_read_address = dma.ll_dma.next_address + 4; //start of data section of packet
			dma.ll_dma.current_word = 0;
			dma.ll_dma.total_words = (data >> 24) & 0xFF; // Current length of packet and future address of header word.
			dma.ll_dma.next_address = dmaRegs.madr.address;
		}
	}
	else
	{
		//We are in the middle of linked list transfer
		u32 data = iopMemRead32(dma.ll_dma.data_read_address);
		ringBufPut(&rb_gp0, &data);
		dma.ll_dma.data_read_address += 4;
		dma.ll_dma.current_word++;
	}
}


//TODO: Make this be called by IopHw.cpp / psxHwReset()... but maybe it should be called by the EE reset func,
//given that the PGIF is in the EE ASIC, on the other side of the SBUS.
void pgifInit(void)
{
	rb_gp1.buf = pgif_gp1_buffer;
	rb_gp1.size = PGIF_CMD_RB_SIZE;
	ringBufferClear(&rb_gp1);

	rb_gp0.buf = pgif_gp0_buffer;
	rb_gp0.size = PGIF_DAT_RB_SIZE;
	ringBufferClear(&rb_gp0);

	pgpu.stat._u32 = 0;
	pgif.ctrl._u32 = 0;
	old_gp0_value  = 0;


	dmaRegs.madr.address = 0;
	dmaRegs.bcr._u32     = 0;
	dmaRegs.chcr._u32    = 0;

	dma.state.ll_active = 0;
	dma.state.to_gpu_active = 0;
	dma.state.to_iop_active = 0;

	dma.ll_dma.data_read_address = 0;
	dma.ll_dma.current_word = 0;
	dma.ll_dma.total_words = 0;
	dma.ll_dma.next_address = 0;

	dma.normal.total_words = 0;
	dma.normal.current_word = 0;
	dma.normal.address = 0;
}

//Interrupt-related (IOP, EE and DMA):

static void getIrqCmd(u32 data)
{
	//For the IOP - GPU. This is triggered by the GP0(1Fh) - Interrupt Request (IRQ1) command.
	//This may break stuff, because it doesn't detect whether this is really a GP0() command or data...
	//Since PS1 HW also didn't recognize that is data or not, we should left it enabled.
	if ((data & 0xFF000000) == 0x1F000000)
	{
		pgpu.stat.bits.IRQ1 = 1;
		iopIntcIrq(1);
	}
}

//Pass-through & intercepting functions:

static u32 immRespHndl(u32 cmd, u32 data)
{
	//Handles the GP1(10h) command, that requires immediate response.
	//The data argument is the old data of the register 
	//(shouldn't be critical what it contains).
	switch ((cmd & 0x7))
	{
		case 0:
		case 1:
		case 6:
		case 7:
			break; //Returns Nothing (old value in GPUREAD remains unchanged)
		case 2: // Read Texture Window setting  ;GP0(E2h) ;20bit/MSBs=Nothing
			return pgif.imm_response.reg.e2 & 0x000FFFFF;
			
		case 3: // Read Draw area top left      ;GP0(E3h) ;19bit/MSBs=Nothing
			return pgif.imm_response.reg.e3 & 0x0007FFFF;
		case 4: // Read Draw area bottom right  ;GP0(E4h) ;19bit/MSBs=Nothing
			return pgif.imm_response.reg.e4 & 0x0007FFFF;
		case 5: // Read Draw offset             ;GP0(E5h) ;22bit
			return pgif.imm_response.reg.e5 & 0x003FFFFF;
			
	}
	return data;
}

static void handleGp1Command(u32 cmd)
{
	//Check GP1() command and configure PGIF accordingly.
	//Commands 0x00 - 0x01, 0x03, 0x05 - 0x08 are fully handled in ps1drv.
	const u32 cmdNr = ((cmd >> 24) & 0xFF) & 0x3F;
	switch (cmdNr)
	{
		case 2: //Acknowledge GPU IRQ
			//Acknowledge for the IOP GPU interrupt.
			pgpu.stat.bits.IRQ1 = 0;
			break;
		case 4: //DMA Direction / Data Request. The PS1DRV doesn't care of this... Should we do something on pgif ctrl?
			pgpu.stat.bits.DDIR = cmd & 0x3;
			//Since DREQ bit is dependent on DDIR bits, we should 
			//set it as soon as command is processed.
			switch (pgpu.stat.bits.DDIR) //29-30 bit (0=Off, 1=FIFO, 2=CPUtoGP0, 3=GPUREADtoCPU)    ;GP1(04h).0-1
			{
				case 0x00: // When GP1(04h)=0 ---> Always zero (0)
					pgpu.stat.bits.DREQ = 0;
					break;
				case 0x01: // When GP1(04h)=1 ---> FIFO State  (0=Full, 1=Not Full)
					if (rb_gp0.count < (rb_gp0.size - PGIF_DAT_RB_LEAVE_FREE))
						pgpu.stat.bits.DREQ = 1;
					else
						pgpu.stat.bits.DREQ = 0;
					break;
				case 0x02: // When GP1(04h)=2 ---> Same as GPUSTAT.28
					pgpu.stat.bits.DREQ = pgpu.stat.bits.RDMA;
					drainPgpuDmaLl(); //See comment in this function.
					break;
				case 0x03: //When GP1(04h)=3 ---> Same as GPUSTAT.27
					pgpu.stat.bits.DREQ = pgpu.stat.bits.RSEND;
					break;
			}
			break;
		default:
			break;
	}
}

static void rb_gp0_Get(u32* data)
{
	if (rb_gp0.count > 0)
	{
		ringBufGet(&rb_gp0, data);
		getIrqCmd(*data); //Checks if an IRQ CMD passes through here and triggers IRQ when it does.
	}
	else
		*data = old_gp0_value;
}

//PS1 GPU registers I/O handlers:

void psxGPUw(int addr, u32 data)
{
	if (addr == HW_PS1_GPU_DATA)
	{
		ringBufPut(&rb_gp0, &data);
	}
	else if (addr == HW_PS1_GPU_STATUS)
	{
		// Check for Cmd 0x10-0x1F
		u8 imm_check = (data >> 28);
		imm_check &= 0x3;
		//Handle immediate-response command. Commands are NOT sent to the Fifo apparently (PS1DRV).
		if (imm_check == 1)
			old_gp0_value = immRespHndl(data, old_gp0_value);
		else
		{
			// Probably we should try to mess with delta here.
			hwIntcIrq(15);
			cpuSetEvent();
			ringBufPut(&rb_gp1, &data);
		}
	}
}

u32 psxGPUr(int addr)
{
	u32 data = 0;
	if (addr == HW_PS1_GPU_DATA)
		rb_gp0_Get(&data);
	else if (addr == HW_PS1_GPU_STATUS)
	{
		//PS1DRV does set bit RSEND on (probably - took from command print) GP0(C0h), 
		//should we do something?
		//The PS1 program pools this bit to determine if there is data in the FIFO, 
		//it can get. Then starts DMA to get it.

		//The PS1 program will not send the DMA direction command (GP1(04h)) 
		//and will not start DMA until this bit (27) becomes set.
		pgpu.stat.bits.RSEND = pgif.ctrl.bits.data_from_gpu_ready;
		return pgpu.stat._u32;
	}

	return data;
}

// PGIF registers I/O handlers:

void PGIFw(int addr, u32 data)
{
	switch (addr)
	{
		case PGPU_STAT:
			pgpu.stat._u32 = data; //Should all bits be writable?
			break;
		case PGIF_CTRL:
			pgif.ctrl._u32 = data;
			fillFifoOnDrain(); // Now this checks the 0x8 bit of the PGIF_CTRL reg, 
					   // so it here too,
			break; 		   //so that it gets updated immediately once it is set.
		case IMM_E2:
			pgif.imm_response.reg.e2 = data;
			break;
		case IMM_E3:
			pgif.imm_response.reg.e3 = data;
			break;
		case IMM_E4:
			pgif.imm_response.reg.e4 = data;
			break;
		case IMM_E5:
			pgif.imm_response.reg.e5 = data;
			break;
		case PGPU_CMD_FIFO:
			break;
		case PGPU_DAT_FIFO:
			ringBufPut(&rb_gp0, &data);
			drainPgpuDmaNrToIop();
			break;
		default:
			break;
	}
}

// Read PGIF Hardware Registers.
u32 PGIFr(int addr)
{
	switch (addr)
	{
		case PGPU_STAT:
			return pgpu.stat._u32;
		case PGIF_CTRL:
			//Update fifo counts before returning register value
			pgif.ctrl.bits.GP0_fifo_count = std::min(rb_gp0.count, 0x1F);
			pgif.ctrl.bits.GP1_fifo_count = rb_gp1.count;
			return pgif.ctrl._u32;
		case IMM_E2:
			return pgif.imm_response.reg.e2;
		case IMM_E3:
			return pgif.imm_response.reg.e3;
		case IMM_E4:
			return pgif.imm_response.reg.e4;
		case IMM_E5:
			return pgif.imm_response.reg.e5;
		case PGPU_CMD_FIFO:
			{
				u32 data = 0;
				ringBufGet(&rb_gp1, &data);
				handleGp1Command(data); //Setup GP1 right after reading command from FIFO.
				return data;
			}
		case PGPU_DAT_FIFO:
			{
				u32 data = 0;
				fillFifoOnDrain();
				rb_gp0_Get(&data);
				return data;
			}
		default:
			break;
	}

	return 0;
}

void PGIFrQword(u32 addr, void* dat)
{
	if (addr == PGPU_DAT_FIFO)
	{
		u32* data = (u32*)dat;
		fillFifoOnDrain();
		rb_gp0_Get(data + 0);
		rb_gp0_Get(data + 1);
		rb_gp0_Get(data + 2);
		rb_gp0_Get(data + 3);

		fillFifoOnDrain();
	}
}

void PGIFwQword(u32 addr, void* dat)
{
	if (addr == PGPU_DAT_FIFO)
	{
		u32* data = (u32*)dat;
		ringBufPut(&rb_gp0, (u32*)(data + 0));
		ringBufPut(&rb_gp0, (u32*)(data + 1));
		ringBufPut(&rb_gp0, (u32*)(data + 2));
		ringBufPut(&rb_gp0, (u32*)(data + 3));
		drainPgpuDmaNrToIop();
	}
}

//DMA-emulating functions:

//This function is used as a global FIFO-DMA-fill function and both Linked-list normal DMA call it,
static void fillFifoOnDrain(void)
{
	//Skip filing FIFO with elements, if PS1DRV hasn't set this bit.
	//Maybe it could be cleared once FIFO has data?
	if (!pgif.ctrl.bits.fifo_GP0_ready_for_data)
		return;

	//This is done here in a loop, rather than recursively in each function, 
	//because a very large buffer causes stack oveflow.
	while ((rb_gp0.count < ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE)) && ((dma.state.to_gpu_active) 
	    || (dma.state.ll_active)))
	{
		drainPgpuDmaLl();
		drainPgpuDmaNrToGpu();
	}

	//Clear bit as DMA will be run - normally it should be cleared only once 
	//the current request finishes, but the IOP won't notice anything anyway.
	//WARNING: Current implementation assume that GPU->IOP DMA uses this flag, 
	//so we only clear it here if the mode is not GPU->IOP.
	if (((dma.state.ll_active) || (dma.state.to_gpu_active)) && (!dma.state.to_iop_active))
		pgif.ctrl.bits.fifo_GP0_ready_for_data = 0;
}

static void drainPgpuDmaNrToGpu(void)
{
	if (!dma.state.to_gpu_active)
		return;

	//Buffer full - needs to be drained first.
	if (rb_gp0.count >= ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE))
		return;

	if (dma.normal.current_word < dma.normal.total_words)
	{
		u32 data = iopMemRead32(dma.normal.address);

		ringBufPut(&rb_gp0, &data);
		dmaRegs.madr.address += 4;
		dma.normal.address += 4;
		dma.normal.current_word++;

		// decrease block amount only if full block size were drained.
		if ((dma.normal.current_word % dmaRegs.bcr.bit.block_size) == 0)
			dmaRegs.bcr.bit.block_amount -= 1;
	}
	if (dma.normal.current_word >= dma.normal.total_words)
	{
		//Reached end of sequence = complete
		dma.state.to_gpu_active = 0;
		dmaRegs.chcr.bits.BUSY = 0;
		pgpuDmaIntr(1);
	}
}

static void drainPgpuDmaNrToIop(void)
{
	if (!dma.state.to_iop_active || rb_gp0.count <= 0)
		return;

	if (dma.normal.current_word < dma.normal.total_words)
	{
		u32 data = 0;
		//This is not the best way, but... is there another?
		ringBufGet(&rb_gp0, &data);
		iopMemWrite32(dma.normal.address, data);
		dmaRegs.madr.address += 4;
		dma.normal.address += 4;
		dma.normal.current_word++;
		// decrease block amount only if full block size were drained.
		if ((dma.normal.current_word % dmaRegs.bcr.bit.block_size) == 0)
			dmaRegs.bcr.bit.block_amount -= 1;
	}
	if (dma.normal.current_word >= dma.normal.total_words)
	{
		dma.state.to_iop_active = 0;
		dmaRegs.chcr.bits.BUSY = 0;
		pgpuDmaIntr(2);
	}

	if (rb_gp0.count > 0)
		drainPgpuDmaNrToIop();
}

static void processPgpuDma(void)
{
	if (dmaRegs.chcr.bits.TSM == 3)
		dmaRegs.chcr.bits.TSM = 1;

	//Linked List Mode
	if (dmaRegs.chcr.bits.TSM == 2)
	{
		//To GPU
		if (dmaRegs.chcr.bits.DIR)
		{
			dma.state.ll_active = 1;
			dma.ll_dma.next_address = (dmaRegs.madr.address & 0x00FFFFFF); //The address in IOP RAM where to load the first header word from
			dma.ll_dma.current_word = 0;
			dma.ll_dma.total_words = 0;

			//fill a single word in fifo now, because otherwise PS1DRV won't know that a transfer is pending.
			fillFifoOnDrain();
		}
		return;
	}
	dma.normal.current_word = 0;
	dma.normal.address      = dmaRegs.madr.address & 0x1FFFFFFF; // Sould we allow whole range? Maybe for psx SPR?
	u32 block_amt           = dmaRegs.bcr.bit.block_amount ? dmaRegs.bcr.bit.block_amount : 0x10000;
	dma.normal.total_words  = block_amt;

	if (dmaRegs.chcr.bits.DIR) // to gpu
	{
		dma.state.to_gpu_active = 1;
		fillFifoOnDrain();
	}
	else
	{
		dma.state.to_iop_active = 1;
		drainPgpuDmaNrToIop();
	}
}

u32 psxDma2GpuR(u32 addr)
{
	addr &= 0x1FFFFFFF;
	switch (addr)
	{
		case PGPU_DMA_MADR:
			return dmaRegs.madr.address;
		case PGPU_DMA_BCR:
			return dmaRegs.bcr._u32;
		case PGPU_DMA_CHCR:
			return dmaRegs.chcr._u32;
		case PGPU_DMA_TADR:
			return pgpuDmaTadr;
		default:
			break;
	}
	return 0;
}

void psxDma2GpuW(u32 addr, u32 data)
{
	addr &= 0x1FFFFFFF;
	switch (addr)
	{
		case PGPU_DMA_MADR:
			dmaRegs.madr.address = (data & 0x00FFFFFF);
			break;
		case PGPU_DMA_BCR:
			dmaRegs.bcr._u32     = data;
			break;
		case PGPU_DMA_CHCR:
			dmaRegs.chcr._u32    = data;
			if (dmaRegs.chcr.bits.BUSY)
				processPgpuDma();
			break;
		case PGPU_DMA_TADR:
			pgpuDmaTadr = data;
			break;
		default:
			break;
	}
}

static std::string psxout_buf;

// This filtering should almost certainly be done in the console classes instead
static std::string psxout_last;
static unsigned psxout_repeat;

static void flush_stdout(bool closing = false)
{
    while (!psxout_buf.empty())
    {
        size_t linelen = psxout_buf.find_first_of("\n\0", 0, 2);
        if (linelen == std::string::npos)
	{
            if (!closing)
                return;
        }
	else
            psxout_buf[linelen++] = '\n';
        if (linelen != 1)
	{
            if (!psxout_buf.compare(0, linelen, psxout_last))
                psxout_repeat++;
            else {
                if (psxout_repeat)
                    psxout_repeat = 0;
                psxout_last = psxout_buf.substr(0, linelen);
            }
        }
        psxout_buf.erase(0, linelen);
    }
    if (closing && psxout_repeat)
        psxout_repeat = 0;
}

void psxBiosReset(void)
{
    flush_stdout(true);
}

// Called for PlayStation BIOS calls at 0xA0, 0xB0 and 0xC0 in kernel reserved memory (seemingly by actually calling those addresses)
// Returns true if we internally process the call, not that we're likely to do any such thing
bool psxBiosCall(void)
{
    // TODO: Tracing
    // TODO (maybe, psx is hardly a priority): HLE framework

    switch (((psxRegs.pc << 4) & 0xf00) | (psxRegs.GPR.n.t1 & 0xff))
    {
        case 0xa03:
        case 0xb35:
            // write(fd, data, size)
            {
                int fd = psxRegs.GPR.n.a0;
                if (fd != 1)
                    return false;

                u32 data = psxRegs.GPR.n.a1;
                u32 size = psxRegs.GPR.n.a2;
                while (size--)
                    psxout_buf.push_back(iopMemRead8(data++));
                flush_stdout(false);
                return false;
            }
        case 0xa09:
        case 0xb3b:
            // putc(c, fd)
            if (psxRegs.GPR.n.a1 != 1)
                return false;
	    // fallthrough 
	    // fd=1, fall through to putchar
        case 0xa3c:
        case 0xb3d:
            // putchar(c)
            psxout_buf.push_back((char)psxRegs.GPR.n.a0);
            flush_stdout(false);
            return false;
        case 0xa3e:
        case 0xb3f:
            // puts(s)
            {
                u32 str = psxRegs.GPR.n.a0;
                while (char c = iopMemRead8(str++))
                    psxout_buf.push_back(c);
                psxout_buf.push_back('\n');
                flush_stdout(false);
                return false;
            }
    }

    return false;
}

namespace IopMemory
{
	namespace Internal {
//////////////////////////////////////////////////////////////////////////////////////////
// Masking helper so that I can use the fully qualified address for case statements.
// Switches are based on the bottom 12 bits only, since MSVC tends to optimize switches
// better when it has a limited width operand to work with. :)
//
#define pgmsk( src ) ( (src) & 0x0fff )
#define mcase( src ) case pgmsk(src)

// Template-compatible version of the psxHu macro.  Used for writing.
#define psxHu(mem)	(*(u32*)&iopHw[(mem) & 0xffff])
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	template< typename T >
		static __fi void _generic_write( u32 addr, T val )
		{
			psxHu(addr) = val;
		}

	void iopHwWrite8_generic( u32 addr, mem8_t val )	{ _generic_write<mem8_t>( addr, val ); }
	void iopHwWrite16_generic( u32 addr, mem16_t val )	{ _generic_write<mem16_t>( addr, val ); }
	void iopHwWrite32_generic( u32 addr, mem32_t val )	{ _generic_write<mem32_t>( addr, val ); }

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	template< typename T >
		static __fi T _generic_read( u32 addr )
		{
			return psxHu(addr);
		}

	mem8_t iopHwRead8_generic( u32 addr )	{ return _generic_read<mem8_t>( addr ); }
	mem16_t iopHwRead16_generic( u32 addr )	{ return _generic_read<mem16_t>( addr ); }
	mem32_t iopHwRead32_generic( u32 addr )	{ return _generic_read<mem32_t>( addr ); }


	//////////////////////////////////////////////////////////////////////////////////////////
	//
	void iopHwWrite8_Page1( u32 addr, mem8_t val )
	{
		u32 masked_addr = pgmsk( addr );

		switch( masked_addr )
		{
			case (HW_SIO_DATA & 0x0fff):
				sio0.SetTxData(val);
				break;
			case (HW_SIO_STAT & 0x0fff):
			case (HW_SIO_MODE & 0x0fff):
			case (HW_SIO_CTRL & 0x0fff):
			case (HW_SIO_BAUD & 0x0fff):
				break;
				// for use of serial port ignore for now
				//case 0x50: serial_write8( val ); break;

				mcase(HW_DEV9_DATA): DEV9write8( addr, val ); break;

				mcase(HW_CDR_DATA0): cdrWrite0( val ); break;
				mcase(HW_CDR_DATA1): cdrWrite1( val ); break;
				mcase(HW_CDR_DATA2): cdrWrite2( val ); break;
				mcase(HW_CDR_DATA3): cdrWrite3( val ); break;

			default:
				if( masked_addr >= 0x100 && masked_addr < 0x130 )
					psxHu8( addr ) = val;
				else if( masked_addr >= 0x480 && masked_addr < 0x4a0 )
					psxHu8( addr ) = val;
				else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
				{
					USBwrite8( addr, val );
				}
				else
				{
					psxHu8(addr) = val;
				}
				break;
		}
	}

	void iopHwWrite8_Page3( u32 addr, mem8_t val )
	{
		psxHu8( addr ) = val;
	}

	void iopHwWrite8_Page8( u32 addr, mem8_t val )
	{
		if (addr == HW_SIO2_DATAIN)
			sio2.Write(val);
		else
			psxHu8(addr) = val;

	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// Templated handler for both 32 and 16 bit write operations, to Page 1 registers.
	//
	template< typename T >
		static __fi void _HwWrite_16or32_Page1( u32 addr, T val )
		{
			u32 masked_addr = addr & 0x0fff;

			// ------------------------------------------------------------------------
			// Counters, 16-bit varieties!
			//
			if( masked_addr >= 0x100 && masked_addr < 0x130 )
			{
				int cntidx = ( masked_addr >> 4 ) & 0xf;
				switch( masked_addr & 0xf )
				{
					case 0x0:
						psxRcntWcount16( cntidx, val );
						break;

					case 0x4:
						psxRcntWmode16( cntidx, val );
						break;

					case 0x8:
						psxRcntWtarget16( cntidx, val );
						break;

					default:
						psxHu(addr) = val;
						break;
				}
			}
			// ------------------------------------------------------------------------
			// Counters, 32-bit varieties!
			//
			else if( masked_addr >= 0x480 && masked_addr < 0x4b0 )
			{
				int cntidx = (( masked_addr >> 4 ) & 0xf) - 5;
				switch( masked_addr & 0xf )
				{
					case 0x0:
						psxRcntWcount32( cntidx, val );
						break;

					case 0x2:	// Count HiWord
						psxRcntWcount32( cntidx, (u32)val << 16 );
						break;

					case 0x4:
						psxRcntWmode32( cntidx, val );
						break;

					case 0x8:
						psxRcntWtarget32( cntidx, val );
						break;

					case 0xa:	// Target HiWord
						psxRcntWtarget32( cntidx, (u32)val << 16);
						break;

					default:
						psxHu(addr) = val;
						break;
				}
			}
			// ------------------------------------------------------------------------
			// USB, with both 16 and 32 bit interfaces
			//
			else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
			{
				if( sizeof(T) == 2 ) USBwrite16( addr, val ); else USBwrite32( addr, val );
			}
			// ------------------------------------------------------------------------
			// SPU2, accessible in 16 bit mode only!
			//
			else if( (masked_addr >= pgmsk(HW_SPU2_START)) && (masked_addr < pgmsk(HW_SPU2_END)) )
			{
				if( sizeof(T) == 2 )
					SPU2write( addr, val );
			}
			// ------------------------------------------------------------------------
			// PS1 GPU access
			//
			else if( (masked_addr >= pgmsk(HW_PS1_GPU_START)) && (masked_addr < pgmsk(HW_PS1_GPU_END)) )
			{
				psxDma2GpuW(addr, val);
			}
			else
			{
				switch( masked_addr )
				{
					// ------------------------------------------------------------------------
					case (HW_SIO_DATA & 0x0fff):
						break;
					case (HW_SIO_STAT & 0x0fff):
						break;
					case (HW_SIO_MODE & 0x0fff):
						sio0.mode = static_cast<u16>(val);
						break;

					case (HW_SIO_CTRL & 0x0fff):
						sio0.SetCtrl(static_cast<u16>(val));
						break;

					case (HW_SIO_BAUD & 0x0fff):
						sio0.baud = static_cast<u16>(val);
						break;

						// ------------------------------------------------------------------------
						//Serial port stuff not support now ;P
						// case 0x050: serial_write16( val ); break;
						//	case 0x054: serial_status_write( val ); break;
						//	case 0x05a: serial_control_write( val ); break;
						//	case 0x05e: serial_baud_write( val ); break;

						mcase(HW_IREG):
							psxHu(addr) &= val;
						if (val == 0xffffffff) {
							psxHu32(addr) |= 1 << 2;
							psxHu32(addr) |= 1 << 3;
						}
						break;

						mcase(HW_IREG+2):
							psxHu(addr) &= val;
						break;

						mcase(HW_IMASK):
							psxHu(addr) = val;
						iopTestIntc();
						break;

						mcase(HW_IMASK+2):
							psxHu(addr) = val;
						iopTestIntc();
						break;

						mcase(HW_ICTRL):
							psxHu(addr) = val;
						iopTestIntc();
						break;

						mcase(HW_ICTRL+2):
							psxHu(addr) = val;
						iopTestIntc();
						break;

						// ------------------------------------------------------------------------
						//

						mcase(0x1f801088) :	// DMA0 CHCR -- MDEC IN
									// psx mode
							HW_DMA0_CHCR = val;
						psxDma0(HW_DMA0_MADR, HW_DMA0_BCR, HW_DMA0_CHCR);
						break;

						mcase(0x1f801098):	// DMA1 CHCR -- MDEC OUT
									// psx mode
							HW_DMA1_CHCR = val;
						psxDma1(HW_DMA1_MADR, HW_DMA1_BCR, HW_DMA1_CHCR);
						break;
						mcase(0x1f8010ac):
							psxHu(addr) = val;
						break;

						mcase(0x1f8010a8) :	// DMA2 CHCR -- GPU
									// BIOS functions
									// send_gpu_linked_list: [1F8010A8h]=1000401h
									// gpu_abort_dma: [1F8010A8h]=401h
									// gpu_send_dma: [1F8010A8h]=1000201h
							psxHu(addr) = val;
						DmaExec(2);
						break;

						mcase(0x1f8010b8):	// DMA3 CHCR -- CDROM
							psxHu(addr) = val;
						DmaExec(3);
						break;

						mcase(0x1f8010c8):	// DMA4 CHCR -- SPU2 Core 1
							psxHu(addr) = val;
						DmaExec(4);
						break;

						mcase(0x1f8010e8):	// DMA6 CHCR -- OT clear
							psxHu(addr) = val;
						DmaExec(6);
						break;

						mcase(0x1f801508):	// DMA7 CHCR -- SPU2 core 2
							psxHu(addr) = val;
						DmaExec2(7);
						break;

						mcase(0x1f801518):	// DMA8 CHCR -- DEV9
							psxHu(addr) = val;
						DmaExec2(8);
						break;

						mcase(0x1f801528):	// DMA9 CHCR -- SIF0
							psxHu(addr) = val;
						DmaExec2(9);
						break;

						mcase(0x1f801538):	// DMA10 CHCR -- SIF1
							psxHu(addr) = val;
						DmaExec2(10);
						break;


						mcase(0x1f801548):	// DMA11 CHCR -- SIO2 IN
							psxHu(addr) = val;
						DmaExec2(11);
						break;

						mcase(0x1f801558):	// DMA12 CHCR -- SIO2 OUT
							psxHu(addr) = val;
						DmaExec2(12);
						break;

						// ------------------------------------------------------------------------
						// DMA ICR handlers -- General XOR behavior!

						mcase(0x1f8010f4):
					{
						//u32 tmp = (~val) & HW_DMA_ICR;
						//u32 old = ((tmp ^ val) & 0xffffff) ^ tmp;
						///psxHu(addr) = ((tmp ^ val) & 0xffffff) ^ tmp;
						u32 newtmp = (HW_DMA_ICR & 0xff000000) | (val & 0xffffff);
						newtmp &= ~(val & 0x7F000000);
						if (((newtmp >> 15) & 0x1) || (((newtmp >> 23) & 0x1) == 0x1 && (((newtmp & 0x7F000000) >> 8) & (newtmp & 0x7F0000)) != 0)) {
							newtmp |= 0x80000000;
						}
						else {
							newtmp &= ~0x80000000;
						}
						psxHu(addr) = newtmp;
						if ((HW_DMA_ICR >> 15) & 0x1) {
							psxRegs.CP0.n.Cause &= ~0x7C;
							iopIntcIrq(3);
						}
						else {
							psxDmaInterrupt(33);
						}
					}
						break;

						mcase(0x1f8010f6):		// ICR_hi (16 bit?) [dunno if it ever happens]
					{
						const u32 val2 = (u32)val << 16;
						const u32 tmp = (~val2) & HW_DMA_ICR;
						psxHu(addr) = (((tmp ^ val2) & 0xffffff) ^ tmp) >> 16;
					}
						break;

						mcase(0x1f801574):
					{
						/*u32 tmp = (~val) & HW_DMA_ICR2;
						  psxHu(addr) = ((tmp ^ val) & 0xffffff) ^ tmp;*/
						//u32 tmp = (~val) & HW_DMA_ICR2;
						//u32 old = ((tmp ^ val) & 0xffffff) ^ tmp;
						///psxHu(addr) = ((tmp ^ val) & 0xffffff) ^ tmp;
						u32 newtmp = (HW_DMA_ICR2 & 0xff000000) | (val & 0xffffff);
						newtmp &= ~(val & 0x7F000000);
						if (((newtmp >> 15) & 0x1) || (((newtmp >> 23) & 0x1) == 0x1 && (((newtmp & 0x7F000000) >> 8) & (newtmp & 0x7F0000)) != 0)) {
							newtmp |= 0x80000000;
						}
						else {
							newtmp &= ~0x80000000;
						}
						psxHu(addr) = newtmp;
						if ((HW_DMA_ICR2 >> 15) & 0x1) {
							psxRegs.CP0.n.Cause &= ~0x7C;
							iopIntcIrq(3);
						}
						else {
							psxDmaInterrupt2(33);
						}
					}
						break;

						mcase(0x1f801576):		// ICR2_hi (16 bit?) [dunno if it ever happens]
					{
						const u32 val2 = (u32)val << 16;
						const u32 tmp = (~val2) & HW_DMA_ICR2;
						psxHu(addr) = (((tmp ^ val2) & 0xffffff) ^ tmp) >> 16;
					}
						break;

						// ------------------------------------------------------------------------
						// Legacy GPU  emulation
						//

						mcase(HW_PS1_GPU_DATA) : // HW_PS1_GPU_DATA = 0x1f801810
							psxHu(addr) = val; // guess
						psxGPUw(addr, val);
						break;
						mcase (HW_PS1_GPU_STATUS): // HW_PS1_GPU_STATUS = 0x1f801814
							psxHu(addr) = val; // guess
						psxGPUw(addr, val);
						break;
						mcase (0x1f801820): // MDEC
							psxHu(addr) = val; // guess
						mdecWrite0(val);
						break;
						mcase (0x1f801824): // MDEC
							psxHu(addr) = val; // guess
						mdecWrite1(val);
						break;

						// ------------------------------------------------------------------------

						mcase(HW_DEV9_DATA):
							DEV9write16( addr, val );
						psxHu(addr) = val;
						break;

					default:
						psxHu(addr) = val;
						break;
				}
			}
		}


	//////////////////////////////////////////////////////////////////////////////////////////
	//
	void iopHwWrite16_Page1( u32 addr, mem16_t val )
	{
		_HwWrite_16or32_Page1<mem16_t>( addr, val );
	}

	void iopHwWrite16_Page3( u32 addr, mem16_t val )
	{
		psxHu16(addr) = val;
	}

	void iopHwWrite16_Page8( u32 addr, mem16_t val )
	{
		psxHu16(addr) = val;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	void iopHwWrite32_Page1( u32 addr, mem32_t val )
	{
		_HwWrite_16or32_Page1<mem32_t >( addr, val );
	}

	void iopHwWrite32_Page3( u32 addr, mem32_t val )
	{
		psxHu16(addr) = val;
	}

	void iopHwWrite32_Page8( u32 addr, mem32_t val )
	{
		u32 masked_addr = addr & 0x0fff;

		if( masked_addr >= 0x200 )
		{
			if( masked_addr < 0x240 )
			{
				const int parm = (masked_addr - 0x200) / 4;
				sio2.SetSend3(parm, val);
			}
			else if( masked_addr < 0x260 )
			{
				// SIO2 Send commands alternate registers.  First reg maps to Send1, second
				// to Send2, third to Send1, etc.  And the following clever code does this:

				const int parm = (masked_addr - 0x240) / 8;

				if (masked_addr & 4)
					sio2.send2[parm] = val;
				else
					sio2.send1[parm] = val;
			}
			else if( masked_addr <= 0x280 )
			{
				switch( masked_addr )
				{
					case (HW_SIO2_DATAIN & 0x0fff):
						break;
					case (HW_SIO2_FIFO & 0x0fff):
						break;
					case (HW_SIO2_CTRL & 0x0fff):
						sio2.SetCtrl(val);
						break;
					case (HW_SIO2_RECV1 & 0x0fff):
						sio2.recv1 = val;
						break;
					case (HW_SIO2_RECV2 & 0x0fff):
						sio2.recv2 = val;
						break;
					case (HW_SIO2_RECV3 & 0x0fff):
						sio2.recv3 = val;
						break;
					case (HW_SIO2_8278 & 0x0fff):
						sio2.unknown1 = val;
						break;
					case (HW_SIO2_827C & 0x0fff):
						sio2.unknown2 = val;
						break;
					case (HW_SIO2_INTR & 0x0fff):
						sio2.iStat = val;
						break;
						// Other SIO2 registers are read-only, no-ops on write.
					default:
						psxHu32(addr) = val;
						break;
				}
			}
			else if( masked_addr >= pgmsk(HW_FW_START) && masked_addr <= pgmsk(HW_FW_END) )
			{
				FWwrite32( addr, val );
			}
		}
		else psxHu32(addr) = val;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	mem8_t iopHwRead8_Page1( u32 addr )
	{
		const u32 masked_addr = addr & 0x0fff;

		mem8_t ret = 0; // using a return var can be helpful in debugging.
		switch( masked_addr )
		{
			case (HW_SIO_DATA & 0x0fff):
				ret = sio0.GetRxData();
				break;
			case (HW_SIO_STAT & 0x0fff):
			case (HW_SIO_MODE & 0x0fff):
			case (HW_SIO_CTRL & 0x0fff):
			case (HW_SIO_BAUD & 0x0fff):
				break;

				// for use of serial port ignore for now
				//case 0x50: ret = serial_read8(); break;

				mcase(HW_DEV9_DATA): ret = DEV9read8( addr ); break;

				mcase(HW_CDR_DATA0): ret = cdrRead0(); break;
				mcase(HW_CDR_DATA1): ret = cdrRead1(); break;
				mcase(HW_CDR_DATA2): ret = cdrRead2(); break;
				mcase(HW_CDR_DATA3): ret = cdrRead3(); break;

			default:
				if( masked_addr >= 0x100 && masked_addr < 0x130 )
					ret = psxHu8( addr );
				else if( masked_addr >= 0x480 && masked_addr < 0x4a0 )
					ret = psxHu8( addr );
				else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
					ret = USBread8( addr );
				else
					ret = psxHu8(addr);
				return ret;
		}
		return ret;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	mem8_t iopHwRead8_Page3( u32 addr )
	{
		mem8_t ret;
		if( addr == 0x1f803100 )	// PS/EE/IOP conf related
						//ret = 0x10; // Dram 2M
			ret = 0xFF; //all high bus is the corect default state for CEX PS2!
		else
			ret = psxHu8( addr );
		return ret;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	mem8_t iopHwRead8_Page8( u32 addr )
	{
		mem8_t ret;

		if (addr == HW_SIO2_FIFO)
			ret = sio2.Read();
		else
			ret = psxHu8(addr);
		return ret;
	}
	//////////////////////////////////////////////////////////////////////////////////////////
	//
	template< typename T >
		static __fi T _HwRead_16or32_Page1( u32 addr )
		{
			u32 masked_addr = pgmsk( addr );
			T ret = 0;

			// ------------------------------------------------------------------------
			// Counters, 16-bit varieties!
			//
			if( masked_addr >= 0x100 && masked_addr < 0x130 )
			{
				int cntidx = ( masked_addr >> 4 ) & 0xf;
				switch( masked_addr & 0xf )
				{
					case 0x0:
						ret = (T)psxRcntRcount16( cntidx );
						break;

					case 0x4:
						ret = psxCounters[cntidx].mode;

						// hmm!  The old code only did this bitwise math for 16 bit reads.
						// Logic indicates it should do the math consistently.  Question is,
						// should it do the logic for both 16 and 32, or not do logic at all?

						psxCounters[cntidx].mode &= ~0x1800;
						break;

					case 0x8:
						ret = psxCounters[cntidx].target;
						break;

					default:
						ret = psxHu32(addr);
						break;
				}
			}
			// ------------------------------------------------------------------------
			// Counters, 32-bit varieties!
			//
			else if( masked_addr >= 0x480 && masked_addr < 0x4b0 )
			{
				int cntidx = (( masked_addr >> 4 ) & 0xf) - 5;
				switch( masked_addr & 0xf )
				{
					case 0x0:
						ret = (T)psxRcntRcount32( cntidx );
						break;

					case 0x2:
						ret = (T)(psxRcntRcount32( cntidx ) >> 16);
						break;

					case 0x4:
						ret = psxCounters[cntidx].mode;
						// hmm!  The old code only did the following bitwise math for 16 bit reads.
						// Logic indicates it should do the math consistently.  Question is,
						// should it do the logic for both 16 and 32, or not do logic at all?

						psxCounters[cntidx].mode &= ~0x1800;
						break;

					case 0x8:
						ret = psxCounters[cntidx].target;
						break;

					case 0xa:
						ret = psxCounters[cntidx].target >> 16;
						break;

					default:
						ret = psxHu32(addr);
						break;
				}
			}
			// ------------------------------------------------------------------------
			// USB, with both 16 and 32 bit interfaces
			//
			else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
				ret = (sizeof(T) == 2) ? USBread16( addr ) : USBread32( addr );
			// ------------------------------------------------------------------------
			// SPU2, accessible in 16 bit mode only!
			//
			else if( masked_addr >= pgmsk(HW_SPU2_START) && masked_addr < pgmsk(HW_SPU2_END) )
			{
				if( sizeof(T) == 2 )
					ret = SPU2read( addr );
				else
					ret = psxHu32(addr);
			}
			// ------------------------------------------------------------------------
			// PS1 GPU access
			//
			else if( (masked_addr >= pgmsk(HW_PS1_GPU_START)) && (masked_addr < pgmsk(HW_PS1_GPU_END)) )
				ret = psxDma2GpuR(addr);
			else
			{
				switch( masked_addr )
				{
					// ------------------------------------------------------------------------
					case (HW_SIO_DATA & 0x0fff):
						ret = sio0.GetRxData();
						ret |= sio0.GetRxData() << 8;
						if (sizeof(T) == 4)
						{
							ret |= sio0.GetRxData() << 16;
							ret |= sio0.GetRxData() << 24;
						}
						break;
					case (HW_SIO_STAT & 0x0fff):
						ret = sio0.GetStat();
						break;
					case (HW_SIO_MODE & 0x0fff):
						ret = sio0.mode;
						break;
					case (HW_SIO_CTRL & 0x0fff):
						ret = sio0.ctrl;
						break;
					case (HW_SIO_BAUD & 0x0fff):
						ret = sio0.baud;
						break;

						// ------------------------------------------------------------------------
						//Serial port stuff not support now ;P
						// case 0x050: hard = serial_read32(); break;
						//	case 0x054: hard = serial_status_read(); break;
						//	case 0x05a: hard = serial_control_read(); break;
						//	case 0x05e: hard = serial_baud_read(); break;

						mcase(HW_ICTRL):
							ret = psxHu32(0x1078);
						psxHu32(0x1078) = 0;
						break;

						mcase(HW_ICTRL+2):
							ret = psxHu16(0x107a);
						psxHu32(0x1078) = 0;	// most likely should clear all 32 bits here.
						break;

						// ------------------------------------------------------------------------
						// Legacy GPU  emulation
						//
						mcase(0x1f8010ac) :
							ret = psxHu32(addr);
						break;

						mcase(HW_PS1_GPU_DATA) :
							ret = psxGPUr(addr);
						break;

						mcase(HW_PS1_GPU_STATUS) :
							ret = psxGPUr(addr);
						break;

						mcase (0x1f801820): // MDEC
							ret = mdecRead0();
						break;

						mcase (0x1f801824): // MDEC
							ret = mdecRead1();
						break;

						// ------------------------------------------------------------------------

						mcase(0x1f80146e):
							ret = DEV9read16( addr );
						break;

					default:
						ret = psxHu32(addr);
						break;
				}
			}

			return ret;
		}

	// Some Page 2 mess?  I love random question marks for comments!
	//case 0x1f802030: hard =   //int_2000????
	//case 0x1f802040: hard =//dip switches...??

	mem16_t iopHwRead16_Page1( u32 addr )
	{
		return _HwRead_16or32_Page1<mem16_t>( addr );
	}

	mem16_t iopHwRead16_Page3( u32 addr )
	{
		return psxHu16(addr);
	}

	mem16_t iopHwRead16_Page8( u32 addr )
	{
		return psxHu16(addr);
	}

	mem32_t iopHwRead32_Page1( u32 addr )
	{
		return _HwRead_16or32_Page1<mem32_t>( addr );
	}

	mem32_t iopHwRead32_Page3( u32 addr )
	{
		return psxHu32(addr);
	}

	mem32_t iopHwRead32_Page8( u32 addr )
	{

		u32 masked_addr = addr & 0x0fff;
		mem32_t ret;

		if( masked_addr >= 0x200 )
		{
			if( masked_addr < 0x240 )
			{
				const int parm = (masked_addr-0x200) / 4;
				ret = sio2.send3[parm];
			}
			else if( masked_addr < 0x260 )
			{
				// SIO2 Send commands alternate registers.  First reg maps to Send1, second
				// to Send2, third to Send1, etc.  And the following clever code does this:
				const int parm = (masked_addr-0x240) / 8;
				ret = (masked_addr & 4) ? sio2.send2[parm] : sio2.send1[parm];
			}
			else if( masked_addr <= 0x280 )
			{
				switch( masked_addr )
				{
					case (HW_SIO2_DATAIN & 0x0fff):
						ret = psxHu32(addr);
						break;
					case (HW_SIO2_FIFO & 0x0fff):
						ret = psxHu32(addr);
						break;
					case (HW_SIO2_CTRL & 0x0fff):
						ret = sio2.ctrl;
						break;
					case (HW_SIO2_RECV1 & 0xfff):
						ret = sio2.recv1;
						break;
					case (HW_SIO2_RECV2 & 0x0fff):
						ret = sio2.recv2;
						break;
					case (HW_SIO2_RECV3 & 0x0fff):
						ret = sio2.recv3;
						break;
					case (0x1f808278 & 0x0fff):
						ret = sio2.unknown1;
						break;
					case (0x1f80827C & 0x0fff):
						ret = sio2.unknown2;
						break;
					case (HW_SIO2_INTR & 0x0fff):
						ret = sio2.iStat;
						break;
					default:
						ret = psxHu32(addr);
						break;
				}
			}
			else if( masked_addr >= pgmsk(HW_FW_START) && masked_addr <= pgmsk(HW_FW_END) )
				ret = FWread32( addr );
			else
				ret = psxHu32(addr);
		}
		else ret = psxHu32(addr);
		return ret;
	}
}
