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
#include "R3000A.h"
#include "IopMem.h"

#include "../common/MathUtils.h"

#define SUM_FLAG if(gteFLAG & 0x7F87E000) gteFLAG |= 0x80000000;

#ifdef _MSC_VER_
#pragma warning(disable:4244)
#pragma warning(disable:4761)
#endif

#define gteVX0     ((s16*)psxRegs.CP2D.r)[0]
#define gteVY0     ((s16*)psxRegs.CP2D.r)[1]
#define gteVZ0     ((s16*)psxRegs.CP2D.r)[2]
#define gteVX1     ((s16*)psxRegs.CP2D.r)[4]
#define gteVY1     ((s16*)psxRegs.CP2D.r)[5]
#define gteVZ1     ((s16*)psxRegs.CP2D.r)[6]
#define gteVX2     ((s16*)psxRegs.CP2D.r)[8]
#define gteVY2     ((s16*)psxRegs.CP2D.r)[9]
#define gteVZ2     ((s16*)psxRegs.CP2D.r)[10]
#define gteRGB     psxRegs.CP2D.r[6]
#define gteOTZ     ((s16*)psxRegs.CP2D.r)[7*2]
#define gteIR0     ((s32*)psxRegs.CP2D.r)[8]
#define gteIR1     ((s32*)psxRegs.CP2D.r)[9]
#define gteIR2     ((s32*)psxRegs.CP2D.r)[10]
#define gteIR3     ((s32*)psxRegs.CP2D.r)[11]
#define gteSXY0    ((s32*)psxRegs.CP2D.r)[12]
#define gteSXY1    ((s32*)psxRegs.CP2D.r)[13]
#define gteSXY2    ((s32*)psxRegs.CP2D.r)[14]
#define gteSXYP    ((s32*)psxRegs.CP2D.r)[15]
#define gteSX0     ((s16*)psxRegs.CP2D.r)[12*2]
#define gteSY0     ((s16*)psxRegs.CP2D.r)[12*2+1]
#define gteSX1     ((s16*)psxRegs.CP2D.r)[13*2]
#define gteSY1     ((s16*)psxRegs.CP2D.r)[13*2+1]
#define gteSX2     ((s16*)psxRegs.CP2D.r)[14*2]
#define gteSY2     ((s16*)psxRegs.CP2D.r)[14*2+1]
#define gteSXP     ((s16*)psxRegs.CP2D.r)[15*2]
#define gteSYP     ((s16*)psxRegs.CP2D.r)[15*2+1]
#define gteSZx     ((u16*)psxRegs.CP2D.r)[16*2]
#define gteSZ0     ((u16*)psxRegs.CP2D.r)[17*2]
#define gteSZ1     ((u16*)psxRegs.CP2D.r)[18*2]
#define gteSZ2     ((u16*)psxRegs.CP2D.r)[19*2]
#define gteRGB0    psxRegs.CP2D.r[20]
#define gteRGB1    psxRegs.CP2D.r[21]
#define gteRGB2    psxRegs.CP2D.r[22]
#define gteMAC0    psxRegs.CP2D.r[24]
#define gteMAC1    ((s32*)psxRegs.CP2D.r)[25]
#define gteMAC2    ((s32*)psxRegs.CP2D.r)[26]
#define gteMAC3    ((s32*)psxRegs.CP2D.r)[27]
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



#define gteR11  ((s16*)psxRegs.CP2C.r)[0]
#define gteR12  ((s16*)psxRegs.CP2C.r)[1]
#define gteR13  ((s16*)psxRegs.CP2C.r)[2]
#define gteR21  ((s16*)psxRegs.CP2C.r)[3]
#define gteR22  ((s16*)psxRegs.CP2C.r)[4]
#define gteR23  ((s16*)psxRegs.CP2C.r)[5]
#define gteR31  ((s16*)psxRegs.CP2C.r)[6]
#define gteR32  ((s16*)psxRegs.CP2C.r)[7]
#define gteR33  ((s16*)psxRegs.CP2C.r)[8]
#define gteTRX  ((s32*)psxRegs.CP2C.r)[5]
#define gteTRY  ((s32*)psxRegs.CP2C.r)[6]
#define gteTRZ  ((s32*)psxRegs.CP2C.r)[7]
#define gteL11  ((s16*)psxRegs.CP2C.r)[16]
#define gteL12  ((s16*)psxRegs.CP2C.r)[17]
#define gteL13  ((s16*)psxRegs.CP2C.r)[18]
#define gteL21  ((s16*)psxRegs.CP2C.r)[19]
#define gteL22  ((s16*)psxRegs.CP2C.r)[20]
#define gteL23  ((s16*)psxRegs.CP2C.r)[21]
#define gteL31  ((s16*)psxRegs.CP2C.r)[22]
#define gteL32  ((s16*)psxRegs.CP2C.r)[23]
#define gteL33  ((s16*)psxRegs.CP2C.r)[24]
#define gteRBK  ((s32*)psxRegs.CP2C.r)[13]
#define gteGBK  ((s32*)psxRegs.CP2C.r)[14]
#define gteBBK  ((s32*)psxRegs.CP2C.r)[15]
#define gteLR1  ((s16*)psxRegs.CP2C.r)[32]
#define gteLR2  ((s16*)psxRegs.CP2C.r)[33]
#define gteLR3  ((s16*)psxRegs.CP2C.r)[34]
#define gteLG1  ((s16*)psxRegs.CP2C.r)[35]
#define gteLG2  ((s16*)psxRegs.CP2C.r)[36]
#define gteLG3  ((s16*)psxRegs.CP2C.r)[37]
#define gteLB1  ((s16*)psxRegs.CP2C.r)[38]
#define gteLB2  ((s16*)psxRegs.CP2C.r)[39]
#define gteLB3  ((s16*)psxRegs.CP2C.r)[40]
#define gteRFC  ((s32*)psxRegs.CP2C.r)[21]
#define gteGFC  ((s32*)psxRegs.CP2C.r)[22]
#define gteBFC  ((s32*)psxRegs.CP2C.r)[23]
#define gteOFX  ((s32*)psxRegs.CP2C.r)[24]
#define gteOFY  ((s32*)psxRegs.CP2C.r)[25]
#define gteH    ((u16*)psxRegs.CP2C.r)[52]
#define gteDQA  ((s16*)psxRegs.CP2C.r)[54]
#define gteDQB  ((s32*)psxRegs.CP2C.r)[28]
#define gteZSF3 ((s16*)psxRegs.CP2C.r)[58]
#define gteZSF4 ((s16*)psxRegs.CP2C.r)[60]
#define gteFLAG psxRegs.CP2C.r[31]

__inline unsigned long MFC2(int reg) {
	switch (reg) {
	case 29:
		gteORGB = (((gteIR1 >> 7) & 0x1f)) |
			(((gteIR2 >> 7) & 0x1f) << 5) |
			(((gteIR3 >> 7) & 0x1f) << 10);
		return gteORGB;

	default:
		return psxRegs.CP2D.r[reg];
	}
}

__inline void MTC2(unsigned long value, int reg) {
	switch (reg) {
	case 8: case 9: case 10: case 11:
		psxRegs.CP2D.r[reg] = (short)value;
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
		//			gteIR1 = (value      ) & 0x1f;
		//			gteIR2 = (value >>  5) & 0x1f;
		//			gteIR3 = (value >> 10) & 0x1f;
		//			gteIR1 = ((value      ) & 0x1f) << 4;
		//			gteIR2 = ((value >>  5) & 0x1f) << 4;
		//			gteIR3 = ((value >> 10) & 0x1f) << 4;
		break;

	case 30:
		psxRegs.CP2D.r[30] = value;
		psxRegs.CP2D.r[31] = count_leading_sign_bits(value);
		break;

	default:
		psxRegs.CP2D.r[reg] = value;
	}
}

void gteMFC2() {
	if (!_Rt_) return;
	psxRegs.GPR.r[_Rt_] = MFC2(_Rd_);
}

void gteCFC2() {
	if (!_Rt_) return;
	psxRegs.GPR.r[_Rt_] = psxRegs.CP2C.r[_Rd_];
}

void gteMTC2() {
	MTC2(psxRegs.GPR.r[_Rt_], _Rd_);
}

void gteCTC2() {
	psxRegs.CP2C.r[_Rd_] = psxRegs.GPR.r[_Rt_];
}

#define _oB_ (psxRegs.GPR.r[_Rs_] + _Imm_)

void gteLWC2() {
	MTC2(iopMemRead32(_oB_), _Rt_);
}

void gteSWC2() {
	iopMemWrite32(_oB_, MFC2(_Rt_));
}

/////LIMITATIONS AND OTHER STUFF************************************

__inline double NC_OVERFLOW1(double x) {
	if (x<-2147483648.0) { gteFLAG |= 1 << 29; }
	else if (x> 2147483647.0) { gteFLAG |= 1 << 26; }

	return x;
}

__inline double NC_OVERFLOW2(double x) {
	if (x<-2147483648.0) { gteFLAG |= 1 << 28; }
	else if (x> 2147483647.0) { gteFLAG |= 1 << 25; }

	return x;
}

__inline double NC_OVERFLOW3(double x) {
	if (x<-2147483648.0) { gteFLAG |= 1 << 27; }
	else if (x> 2147483647.0) { gteFLAG |= 1 << 24; }

	return x;
}

__inline double NC_OVERFLOW4(double x) {
	if (x<-2147483648.0) { gteFLAG |= 1 << 16; }
	else if (x> 2147483647.0) { gteFLAG |= 1 << 15; }

	return x;
}

__inline s32 FNC_OVERFLOW1(s64 x) {
	if (x< (s64)0xffffffff80000000) { gteFLAG |= 1 << 29; }
	else if (x> 2147483647) { gteFLAG |= 1 << 26; }

	return (s32)x;
}

__inline s32 FNC_OVERFLOW2(s64 x) {
	if (x< (s64)0xffffffff80000000) { gteFLAG |= 1 << 28; }
	else if (x> 2147483647) { gteFLAG |= 1 << 25; }

	return (s32)x;
}

__inline s32 FNC_OVERFLOW3(s64 x) {
	if (x< (s64)0xffffffff80000000) { gteFLAG |= 1 << 27; }
	else if (x> 2147483647) { gteFLAG |= 1 << 24; }

	return (s32)x;
}

__inline s32 FNC_OVERFLOW4(s64 x) {
	if (x< (s64)0xffffffff80000000) { gteFLAG |= 1 << 16; }
	else if (x> 2147483647) { gteFLAG |= 1 << 15; }

	return (s32)x;
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
	if (x > 2147483647.0) { gteFLAG |= (1 << 16); }
	else
		if (x <-2147483648.0) { gteFLAG |= (1 << 15); }

	if (x >       1023.0) { x = 1023.0; gteFLAG |= (1 << 14); }
	else
		if (x <      -1024.0) { x = -1024.0; gteFLAG |= (1 << 14); }

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

__inline s32 F12limA1S(s64 x) { _LIMX(-(32768 << 12), 32767 << 12, 24); }
__inline s32 F12limA2S(s64 x) { _LIMX(-(32768 << 12), 32767 << 12, 23); }
__inline s32 F12limA3S(s64 x) { _LIMX(-(32768 << 12), 32767 << 12, 22); }
__inline s32 F12limA1U(s64 x) { _LIMX(0, 32767 << 12, 24); }
__inline s32 F12limA2U(s64 x) { _LIMX(0, 32767 << 12, 23); }
__inline s32 F12limA3U(s64 x) { _LIMX(0, 32767 << 12, 22); }

__inline s16 FlimA1S(s32 x) { _LIMX(-32768, 32767, 24); }
__inline s16 FlimA2S(s32 x) { _LIMX(-32768, 32767, 23); }
__inline s16 FlimA3S(s32 x) { _LIMX(-32768, 32767, 22); }
__inline s16 FlimA1U(s32 x) { _LIMX(0, 32767, 24); }
__inline s16 FlimA2U(s32 x) { _LIMX(0, 32767, 23); }
__inline s16 FlimA3U(s32 x) { _LIMX(0, 32767, 22); }
__inline u8  FlimB1(s32 x) { _LIMX(0, 255, 21); }
__inline u8  FlimB2(s32 x) { _LIMX(0, 255, 20); }
__inline u8  FlimB3(s32 x) { _LIMX(0, 255, 19); }
__inline u16 FlimC(s32 x) { _LIMX(0, 65535, 18); }
__inline s32 FlimD1(s32 x) { _LIMX(-1024, 1023, 14); }
__inline s32 FlimD2(s32 x) { _LIMX(-1024, 1023, 13); }
__inline s32 FlimE(s32 x) { _LIMX(0, 65535, 12); }

__inline s32 FlimG1(s64 x) {
	if (x > 2147483647) { gteFLAG |= (1 << 16); }
	else if (x < (s64)0xffffffff80000000) { gteFLAG |= (1 << 15); }

	if (x >       1023) { x = 1023; gteFLAG |= (1 << 14); }
	else if (x <      -1024) { x = -1024; gteFLAG |= (1 << 14); }

	return (x);
}

__inline s32 FlimG2(s64 x) {
	if (x > 2147483647) { gteFLAG |= (1 << 16); }
	else
		if (x < (s64)0xffffffff80000000) { gteFLAG |= (1 << 15); }

	if (x >       1023) { x = 1023; gteFLAG |= (1 << 13); }
	else
		if (x < -1024) { x = -1024; gteFLAG |= (1 << 13); }

	return (x);
}

#define MAC2IR() { \
	if (gteMAC1 < (long)(-32768)) { gteIR1=(long)(-32768); gteFLAG|=1<<24;} \
else \
	if (gteMAC1 > (long)( 32767)) { gteIR1=(long)( 32767); gteFLAG|=1<<24;} \
else gteIR1=(long)gteMAC1; \
	if (gteMAC2 < (long)(-32768)) { gteIR2=(long)(-32768); gteFLAG|=1<<23;} \
else \
	if (gteMAC2 > (long)( 32767)) { gteIR2=(long)( 32767); gteFLAG|=1<<23;} \
else gteIR2=(long)gteMAC2; \
	if (gteMAC3 < (long)(-32768)) { gteIR3=(long)(-32768); gteFLAG|=1<<22;} \
else \
	if (gteMAC3 > (long)( 32767)) { gteIR3=(long)( 32767); gteFLAG|=1<<22;} \
else gteIR3=(long)gteMAC3; \
}


#define MAC2IR1() {           \
	if (gteMAC1 < (long)0) { gteIR1=(long)0; gteFLAG|=1<<24;}  \
else if (gteMAC1 > (long)(32767)) { gteIR1=(long)(32767); gteFLAG|=1<<24;} \
else gteIR1=(long)gteMAC1;                                                         \
	if (gteMAC2 < (long)0) { gteIR2=(long)0; gteFLAG|=1<<23;}      \
else if (gteMAC2 > (long)(32767)) { gteIR2=(long)(32767); gteFLAG|=1<<23;}    \
else gteIR2=(long)gteMAC2;                                                            \
	if (gteMAC3 < (long)0) { gteIR3=(long)0; gteFLAG|=1<<22;}         \
else if (gteMAC3 > (long)(32767)) { gteIR3=(long)(32767); gteFLAG|=1<<22;}       \
else gteIR3=(long)gteMAC3; \
}

//********END OF LIMITATIONS**********************************/

#define GTE_RTPS1(vn) { \
	gteMAC1 = FNC_OVERFLOW1(((signed long)(gteR11*gteVX##vn + gteR12*gteVY##vn + gteR13*gteVZ##vn)>>12) + gteTRX); \
	gteMAC2 = FNC_OVERFLOW2(((signed long)(gteR21*gteVX##vn + gteR22*gteVY##vn + gteR23*gteVZ##vn)>>12) + gteTRY); \
	gteMAC3 = FNC_OVERFLOW3(((signed long)(gteR31*gteVX##vn + gteR32*gteVY##vn + gteR33*gteVZ##vn)>>12) + gteTRZ); \
}

#define GTE_RTPS2(vn) { \
	if (gteSZ##vn == 0) { \
	FDSZ = 2 << 16; gteFLAG |= 1<<17; \
	} else { \
	FDSZ = ((u64)gteH << 32) / ((u64)gteSZ##vn << 16); \
	if ((u64)FDSZ > (2 << 16)) { FDSZ = 2 << 16; gteFLAG |= 1<<17; } \
	} \
	\
	gteSX##vn = FlimG1((gteOFX + (((s64)((s64)gteIR1 << 16) * FDSZ) >> 16)) >> 16); \
	gteSY##vn = FlimG2((gteOFY + (((s64)((s64)gteIR2 << 16) * FDSZ) >> 16)) >> 16); \
}

#define GTE_RTPS3() { \
	FDSZ = (s64)((s64)gteDQB + (((s64)((s64)gteDQA << 8) * FDSZ) >> 8)); \
	gteMAC0 = FDSZ; \
	gteIR0  = FlimE(FDSZ >> 12); \
}

void gteRTPS(void) {
	s64 FDSZ;

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
	s64 FDSZ;

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
	s64 SSX, SSY, SSZ;


	switch (psxRegs.code & 0x78000) {
	case 0x00000: // V0 * R
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gteR); break;
	case 0x08000: // V1 * R
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gteR); break;
	case 0x10000: // V2 * R
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gteR); break;
	case 0x18000: // IR * R
		_MVMVA_FUNC((short)gteIR1, (short)gteIR2, (short)gteIR3, gteR);
		break;
	case 0x20000: // V0 * L
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gteL); break;
	case 0x28000: // V1 * L
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gteL); break;
	case 0x30000: // V2 * L
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gteL); break;
	case 0x38000: // IR * L
		_MVMVA_FUNC((short)gteIR1, (short)gteIR2, (short)gteIR3, gteL); break;
	case 0x40000: // V0 * C
		_MVMVA_FUNC(gteVX0, gteVY0, gteVZ0, gte_C); break;
	case 0x48000: // V1 * C
		_MVMVA_FUNC(gteVX1, gteVY1, gteVZ1, gte_C); break;
	case 0x50000: // V2 * C
		_MVMVA_FUNC(gteVX2, gteVY2, gteVZ2, gte_C); break;
	case 0x58000: // IR * C
		_MVMVA_FUNC((short)gteIR1, (short)gteIR2, (short)gteIR3, gte_C); break;
	default:
		SSX = SSY = SSZ = 0;
	}

	if (psxRegs.code & 0x80000) {
		//		SSX /= 4096.0; SSY /= 4096.0; SSZ /= 4096.0;
		SSX >>= 12; SSY >>= 12; SSZ >>= 12;
	}

	switch (psxRegs.code & 0x6000) {
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

void gteNCLIP(void) {
	gteFLAG = 0;


	gteMAC0 = gteSX0 * (gteSY1 - gteSY2) +
		gteSX1 * (gteSY2 - gteSY0) +
		gteSX2 * (gteSY0 - gteSY1);

	SUM_FLAG;
}

void gteAVSZ3() {
	gteFLAG = 0;

	gteMAC0 = ((gteSZ0 + gteSZ1 + gteSZ2) * (gteZSF3)) >> 12;

	gteOTZ = FlimC(gteMAC0);

	SUM_FLAG
}

void gteAVSZ4() {
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
	gteMAC1 = (long)(((s64)((u32)gteR<<12)*gte_RRLT) >> 20);\
	gteMAC2 = (long)(((s64)((u32)gteG<<12)*gte_GGLT) >> 20);\
	gteMAC3 = (long)(((s64)((u32)gteB<<12)*gte_BBLT) >> 20);


void gteNCCS() {
	s32 gte_LL1, gte_LL2, gte_LL3;
	s32 gte_RRLT, gte_GGLT, gte_BBLT;


	gteFLAG = 0;

	GTE_NCCS(0);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;
	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	MAC2IR1();

	SUM_FLAG
}

void gteNCCT(void) {
	s32 gte_LL1, gte_LL2, gte_LL3;
	s32 gte_RRLT, gte_GGLT, gte_BBLT;



	gteFLAG = 0;

	GTE_NCCS(0);

	gteR0 = FlimB1(gteMAC1 >> 4);
	gteG0 = FlimB2(gteMAC2 >> 4);
	gteB0 = FlimB3(gteMAC3 >> 4); gteCODE0 = gteCODE;

	GTE_NCCS(1);

	gteR1 = FlimB1(gteMAC1 >> 4);
	gteG1 = FlimB2(gteMAC2 >> 4);
	gteB1 = FlimB3(gteMAC3 >> 4); gteCODE1 = gteCODE;

	GTE_NCCS(2);

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

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
	gte_RR0 = (long)(((s64)((u32)gteR<<12)*gte_RRLT) >> 12);\
	gte_GG0 = (long)(((s64)((u32)gteG<<12)*gte_GGLT) >> 12);\
	gte_BB0 = (long)(((s64)((u32)gteB<<12)*gte_BBLT) >> 12);\
	gteMAC1 = (long)((gte_RR0 + (((s64)gteIR0 * F12limA1S((s64)(gteRFC << 8) - gte_RR0)) >> 12)) >> 8);\
	gteMAC2 = (long)((gte_GG0 + (((s64)gteIR0 * F12limA2S((s64)(gteGFC << 8) - gte_GG0)) >> 12)) >> 8);\
	gteMAC3 = (long)((gte_BB0 + (((s64)gteIR0 * F12limA3S((s64)(gteBFC << 8) - gte_BB0)) >> 12)) >> 8);

void gteNCDS() {
	s32 gte_LL1, gte_LL2, gte_LL3;
	s32 gte_RRLT, gte_GGLT, gte_BBLT;
	s32 gte_RR0, gte_GG0, gte_BB0;

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
	s32 gte_LL1, gte_LL2, gte_LL3;
	s32 gte_RRLT, gte_GGLT, gte_BBLT;
	s32 gte_RR0, gte_GG0, gte_BB0;

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

#define	gteD1	(*(short *)&gteR11)
#define	gteD2	(*(short *)&gteR22)
#define	gteD3	(*(short *)&gteR33)

void gteOP() {
	gteFLAG = 0;

	if (psxRegs.code & 0x80000) {
		gteMAC1 = FNC_OVERFLOW1((gteD2 * gteIR3 - gteD3 * gteIR2) >> 12);
		gteMAC2 = FNC_OVERFLOW2((gteD3 * gteIR1 - gteD1 * gteIR3) >> 12);
		gteMAC3 = FNC_OVERFLOW3((gteD1 * gteIR2 - gteD2 * gteIR1) >> 12);
	}
	else {
		gteMAC1 = FNC_OVERFLOW1(gteD2 * gteIR3 - gteD3 * gteIR2);
		gteMAC2 = FNC_OVERFLOW2(gteD3 * gteIR1 - gteD1 * gteIR3);
		gteMAC3 = FNC_OVERFLOW3(gteD1 * gteIR2 - gteD2 * gteIR1);
	}

	MAC2IR();

	SUM_FLAG
}

void gteDCPL() {
	gteMAC1 = ((signed long)(gteR)*gteIR1 + (gteIR0*(signed short)FlimA1S(gteRFC - ((gteR*gteIR1) >> 12)))) >> 8;
	gteMAC2 = ((signed long)(gteG)*gteIR2 + (gteIR0*(signed short)FlimA2S(gteGFC - ((gteG*gteIR2) >> 12)))) >> 8;
	gteMAC3 = ((signed long)(gteB)*gteIR3 + (gteIR0*(signed short)FlimA3S(gteBFC - ((gteB*gteIR3) >> 12)))) >> 8;

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
	gteMAC1 = (gteR << 4) + ((gteIR0*(signed short)FlimA1S(gteRFC - (gteR << 4))) >> 12);
	gteMAC2 = (gteG << 4) + ((gteIR0*(signed short)FlimA2S(gteGFC - (gteG << 4))) >> 12);
	gteMAC3 = (gteB << 4) + ((gteIR0*(signed short)FlimA3S(gteBFC - (gteB << 4))) >> 12);

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
	gteMAC1 = (gteR0 << 4) + ((gteIR0*(signed short)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(signed short)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(signed short)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);

	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	gteMAC1 = (gteR0 << 4) + ((gteIR0*(signed short)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(signed short)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(signed short)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);
	gteRGB0 = gteRGB1;
	gteRGB1 = gteRGB2;

	gteR2 = FlimB1(gteMAC1 >> 4);
	gteG2 = FlimB2(gteMAC2 >> 4);
	gteB2 = FlimB3(gteMAC3 >> 4); gteCODE2 = gteCODE;

	gteMAC1 = (gteR0 << 4) + ((gteIR0*(signed short)FlimA1S(gteRFC - (gteR0 << 4))) >> 12);
	gteMAC2 = (gteG0 << 4) + ((gteIR0*(signed short)FlimA2S(gteGFC - (gteG0 << 4))) >> 12);
	gteMAC3 = (gteB0 << 4) + ((gteIR0*(signed short)FlimA3S(gteBFC - (gteB0 << 4))) >> 12);
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
	s32 gte_LL1, gte_LL2, gte_LL3;
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
	s32 gte_LL1, gte_LL2, gte_LL3;

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
	s32 RR0, GG0, BB0;
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
	gteMAC1 = gteIR1 + ((gteIR0*(signed short)FlimA1S(gteRFC - gteIR1)) >> 12);
	gteMAC2 = gteIR2 + ((gteIR0*(signed short)FlimA2S(gteGFC - gteIR2)) >> 12);
	gteMAC3 = gteIR3 + ((gteIR0*(signed short)FlimA3S(gteBFC - gteIR3)) >> 12);
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
