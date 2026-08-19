#include "common/emitter/c89ops.h"
/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

//------------------------------------------------------------------
// Micro VU Micromode Lower instructions
//------------------------------------------------------------------

//------------------------------------------------------------------
// DIV/SQRT/RSQRT
//------------------------------------------------------------------

/* Test if Vector is +/- Zero */
static __fi void testZero(int xmmReg, int xmmTemp, int gprTemp)
{
	xe_xorps_xx(xmmTemp, xmmTemp);
	xe_cmpeqss_xx(xmmTemp, xmmReg);
	xe_ptest_xx(xmmTemp, xmmTemp);
}

// Test if Vector is Negative (Set Flags and Makes Positive)
static __fi void testNeg(mV, int xmmReg, int gprTemp)
{
	xe_movmskps_rx(gprTemp, xmmReg);
	xe_test32_ri(gprTemp, 1);
	e_u8* skip; xe_fwd_jcc8(Jcc_Zero, skip);
		xe_mov32_mi(&mVU.divFlag, divI);
		xe_andps_xm(xmmReg, mVUglob.absclip);
	xe_fwd_set8(skip);
}

mVUop(mVU_DIV)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7); }
	pass2
	{
		int Ft;
		if (_Ftf_) Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		else       Ft = mVU.regAlloc->allocReg(_Ft_);
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int t1 = mVU.regAlloc->allocReg();

		testZero(Ft, t1, gprT1); // Test if Ft is zero
		e_u8* cjmp; xe_fwd_jcc8(Jcc_Zero, cjmp); // Skip if not zero

		testZero(Fs, t1, gprT1); // Test if Fs is zero
		e_u8* ajmp; xe_fwd_jcc8(Jcc_Zero, ajmp);
		xe_mov32_mi(&mVU.divFlag, divI); // Set invalid flag (0/0)
		e_u8* bjmp; xe_fwd_jcc8(Jcc_Unconditional, bjmp);
		xe_fwd_set8(ajmp);
		xe_mov32_mi(&mVU.divFlag, divD); // Zero divide (only when not 0/0)
		xe_fwd_set8(bjmp);

		xe_xorps_xx(Fs, Ft);
		xe_andps_xm(Fs, mVUglob.signbit);
		xe_orps_xm(Fs, mVUglob.maxvals); // If division by zero, then xmmFs = +/- fmax

		e_u8* djmp; xe_fwd_jcc8(Jcc_Unconditional, djmp);

		xe_fwd_set8(cjmp);

		xe_mov32_mi(&mVU.divFlag, 0); // Clear I/D flags
		SSE_DIVSS(mVU, Fs, Ft);
		mVUclamp1(mVU, Fs, t1, 8, true);

		xe_fwd_set8(djmp);

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xe_and32_ri(gprF0, ~0xc0000);
			xe_or32_rm(gprF0, &mVU.divFlag);
		}

		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(Ft);
		mVU.regAlloc->clearNeededXMM(t1);
	}
}

mVUop(mVU_SQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7); }
	pass2
	{
		const int Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));

		xe_mov32_mi(&mVU.divFlag, 0); /* Clear I/D flags */
		testNeg(mVU, Ft, gprT1); /* Check for negative sqrt */
		
		/* Clamp infinities (only need to do positive clamp since xmmFt is positive) */
		if (CHECK_VU_OVERFLOW(mVU.index)) 
			xe_minss_xm(Ft, mVUglob.maxvals);
		xe_sqrtss_xx(Ft, Ft);

		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xe_and32_ri(gprF0, ~0xc0000);
			xe_or32_rm(gprF0, &mVU.divFlag);
		}

		mVU.regAlloc->clearNeededXMM(Ft);
	}
}

mVUop(mVU_RSQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13); }
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		const int t1 = mVU.regAlloc->allocReg();

		xe_mov32_mi(&mVU.divFlag, 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		xe_sqrtss_xx(Ft, Ft);
		testZero(Ft, t1, gprT1); // Test if Ft is zero
		e_u8* ajmp; xe_fwd_jcc8(Jcc_Zero, ajmp); // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			e_u8* bjmp; xe_fwd_jcc8(Jcc_Zero, bjmp); // Skip if none are
				xe_mov32_mi(&mVU.divFlag, divI); // Set invalid flag (0/0)
				e_u8* cjmp; xe_fwd_jcc8(Jcc_Unconditional, cjmp);
			xe_fwd_set8(bjmp);
				xe_mov32_mi(&mVU.divFlag, divD); // Zero divide flag (only when not 0/0)
			xe_fwd_set8(cjmp);

			xe_andps_xm(Fs, mVUglob.signbit);
			xe_orps_xm(Fs, mVUglob.maxvals); // xmmFs = +/-Max

			e_u8* djmp; xe_fwd_jcc8(Jcc_Unconditional, djmp);
		xe_fwd_set8(ajmp);
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		xe_fwd_set8(djmp);

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xe_and32_ri(gprF0, ~0xc0000);
			xe_or32_rm(gprF0, &mVU.divFlag);
		}

		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(Ft);
		mVU.regAlloc->clearNeededXMM(t1);
	}
}

//------------------------------------------------------------------
// EATAN/EEXP/ELENG/ERCPR/ERLENG/ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM
//------------------------------------------------------------------

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		SSE_MULSS(mVU, t2, Fs); \
		xe_movaps_xx(t1, t2); \
		xe_mulss_xm(t1, addr); \
		SSE_ADDSS(mVU, PQ, t1); \
	}

// ToDo: Can Be Optimized Further? (takes approximately (~115 cycles + mem access time) on a c2d)
static __fi void mVU_EATAN_(mV, int PQ, int Fs, int t1, int t2)
{
	xe_movss_xx(PQ, Fs);
	xe_mulss_xm(PQ, mVUglob.T1);
	xe_movaps_xx(t2, Fs);
	EATANhelper(mVUglob.T2);
	EATANhelper(mVUglob.T3);
	EATANhelper(mVUglob.T4);
	EATANhelper(mVUglob.T5);
	EATANhelper(mVUglob.T6);
	EATANhelper(mVUglob.T7);
	EATANhelper(mVUglob.T8);
	xe_addss_xm(PQ, mVUglob.Pi4);
	xe_pshufd_xxi(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

mVUop(mVU_EATAN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 54);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int t1 = mVU.regAlloc->allocReg();
		const int t2 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs);
		xe_subss_xm(Fs, mVUglob.one);
		xe_addss_xm(xmmPQ, mVUglob.one);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
		mVU.regAlloc->clearNeededXMM(t2);
	}
}

mVUop(mVU_EATANxy)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const int t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const int Fs = mVU.regAlloc->allocReg();
		const int t2 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(Fs, t1, 0x01);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1); // y-x, not y-1? ><
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
		mVU.regAlloc->clearNeededXMM(t2);
	}
}

mVUop(mVU_EATANxz)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const int t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const int Fs = mVU.regAlloc->allocReg();
		const int t2 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(Fs, t1, 0x02);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
		mVU.regAlloc->clearNeededXMM(t2);
	}
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		xe_movaps_xx(t1, t2); \
		xe_mulss_xm(t1, addr); \
		SSE_ADDSS(mVU, xmmPQ, t1); \
	}

mVUop(mVU_EEXP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 44);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int t1 = mVU.regAlloc->allocReg();
		const int t2 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs);
		xe_mulss_xm(xmmPQ, mVUglob.E1);
		xe_addss_xm(xmmPQ, mVUglob.one);
		xe_movaps_xx(t1, Fs);
		SSE_MULSS(mVU, t1, Fs);
		xe_movaps_xx(t2, t1);
		xe_mulss_xm(t1, mVUglob.E2);
		SSE_ADDSS(mVU, xmmPQ, t1);
		eexpHelper(&mVUglob.E3);
		eexpHelper(&mVUglob.E4);
		eexpHelper(&mVUglob.E5);
		SSE_MULSS(mVU, t2, Fs);
		xe_mulss_xm(t2, mVUglob.E6);
		SSE_ADDSS(mVU, xmmPQ, t2);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		xe_movss_xm(t2, mVUglob.one);
		SSE_DIVSS(mVU, t2, xmmPQ);
		xe_movss_xx(xmmPQ, t2);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
		mVU.regAlloc->clearNeededXMM(t2);
	}
}

// sumXYZ(): PQ.x = x ^ 2 + y ^ 2 + z ^ 2
static __fi void mVU_sumXYZ(mV, int PQ, int Fs)
{
	xe_dpps_xxi(Fs, Fs, 0x71);
	xe_movss_xx(PQ, Fs);
}

mVUop(mVU_ELENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xe_sqrtss_xx(xmmPQ, xmmPQ);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ERCPR)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs);
		xe_movss_xm(Fs, mVUglob.one);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xe_movss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ERLENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 24);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xe_sqrtss_xx(xmmPQ, xmmPQ);
		xe_movss_xm(Fs, mVUglob.one);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xe_movss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ERSADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xe_movss_xm(Fs, mVUglob.one);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xe_movss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ERSQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 18);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_andps_xm(Fs, mVUglob.absclip);
		xe_sqrtss_xx(xmmPQ, Fs);
		xe_movss_xm(Fs, mVUglob.one);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xe_movss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ESADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 11);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ESIN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 29);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int t1 = mVU.regAlloc->allocReg();
		const int t2 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_movss_xx(xmmPQ, Fs); // pq = X
		SSE_MULSS(mVU, Fs, Fs);    // fs = X^2
		xe_movaps_xx(t1, Fs);    // t1 = X^2
		SSE_MULSS(mVU, Fs, xmmPQ); // fs = X^3
		xe_movaps_xx(t2, Fs);    // t2 = X^3
		xe_mulss_xm(Fs, mVUglob.S2); // fs = s2 * X^3
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3

		SSE_MULSS(mVU, t2, t1);    // t2 = X^3 * X^2
		xe_movaps_xx(Fs, t2);    // fs = X^5
		xe_mulss_xm(Fs, mVUglob.S3); // ps = s3 * X^5
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5

		SSE_MULSS(mVU, t2, t1);    // t2 = X^5 * X^2
		xe_movaps_xx(Fs, t2);    // fs = X^7
		xe_mulss_xm(Fs, mVUglob.S4); // fs = s4 * X^7
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7

		SSE_MULSS(mVU, t2, t1);    // t2 = X^7 * X^2
		xe_mulss_xm(t2, mVUglob.S5); // t2 = s5 * X^9
		SSE_ADDSS(mVU, xmmPQ, t2); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 + s5 * X^9
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
		mVU.regAlloc->clearNeededXMM(t2);
	}
}

mVUop(mVU_ESQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_andps_xm(Fs, mVUglob.absclip);
		xe_sqrtss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_ESUM)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 12);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		const int t1 = mVU.regAlloc->allocReg();
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xe_pshufd_xxi(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1);
		xe_pshufd_xxi(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1);
		xe_movss_xx(xmmPQ, Fs);
		xe_pshufd_xxi(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeededXMM(Fs);
		mVU.regAlloc->clearNeededXMM(t1);
	}
}

//------------------------------------------------------------------
// FCAND/FCEQ/FCGET/FCOR/FCSET
//------------------------------------------------------------------

mVUop(mVU_FCAND)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const int dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xe_and32_ri(dst, _Imm24_);
		xe_add32_ri(dst, 0xffffff);
		xe_shr32_ri(dst, 24);
		mVU.regAlloc->clearNeededGPR(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const int dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xe_xor32_ri(dst, _Imm24_);
		xe_sub32_ri(dst, 1);
		xe_shr32_ri(dst, 31);
		mVU.regAlloc->clearNeededGPR(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		xe_and32_ri(regT, 0xfff);
		mVU.regAlloc->clearNeededGPR(regT);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const int dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xe_or32_ri(dst, _Imm24_);
		xe_add32_ri(dst, 1);  // If 24 1's will make 25th bit 1, else 0
		xe_shr32_ri(dst, 24); // Get the 25th bit (also clears the rest of the garbage in the reg)
		mVU.regAlloc->clearNeededGPR(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		xe_mov32_ri(gprT1, _Imm24_);
		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
	}
}

//------------------------------------------------------------------
// FMAND/FMEQ/FMOR
//------------------------------------------------------------------

mVUop(mVU_FMAND)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const int regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xe_and32_rr(regT, gprT1);
		mVU.regAlloc->clearNeededGPR(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const int regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xe_xor32_rr(regT, gprT1);
		xe_sub32_ri(regT, 1);
		xe_shr32_ri(regT, 31);
		mVU.regAlloc->clearNeededGPR(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const int regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xe_or32_rr(regT, gprT1);
		mVU.regAlloc->clearNeededGPR(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

//------------------------------------------------------------------
// FSAND/FSEQ/FSOR/FSSET
//------------------------------------------------------------------

mVUop(mVU_FSAND)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const int reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		xe_and32_ri(reg, _Imm12_);
		mVU.regAlloc->clearNeededGPR(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const int reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		xe_or32_ri(reg, _Imm12_);
		mVU.regAlloc->clearNeededGPR(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSEQ)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0001) imm |= 0x0000f00; // Z
		if (_Imm12_ & 0x0002) imm |= 0x000f000; // S
		if (_Imm12_ & 0x0004) imm |= 0x0010000; // U
		if (_Imm12_ & 0x0008) imm |= 0x0020000; // O
		if (_Imm12_ & 0x0010) imm |= 0x0040000; // I
		if (_Imm12_ & 0x0020) imm |= 0x0080000; // D
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS

		const int reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00); // Z  bit
		setBitFSEQ(reg, 0xf000); // S  bit
		setBitFSEQ(reg, 0x000f); // ZS bit
		setBitFSEQ(reg, 0x00f0); // SS bit
		xe_xor32_ri(reg, imm);
		xe_sub32_ri(reg, 1);
		xe_shr32_ri(reg, 31);
		mVU.regAlloc->clearNeededGPR(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSSET)
{
	pass1 { mVUanalyzeFSSET(mVU); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS
		if (!(sFLAG.doFlag || mVUinfo.doDivFlag))
		{
			mVUallocSFLAGa(getFlagReg(sFLAG.write), sFLAG.lastWrite); // Get Prev Status Flag
		}
		xe_and32_ri(getFlagReg(sFLAG.write), 0xfff00); // Keep Non-Sticky Bits
		if (imm)
			xe_or32_ri(getFlagReg(sFLAG.write), imm);
	}
}

//------------------------------------------------------------------
// IADD/IADDI/IADDIU/IAND/IOR/ISUB/ISUBIU
//------------------------------------------------------------------

mVUop(mVU_IADD)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_Is_ == 0 || _It_ == 0)
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_ ? _Is_ : _It_, -1);
			const int regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xe_mov32_rr(regD, regS);
			mVU.regAlloc->clearNeededGPR(regD);
			mVU.regAlloc->clearNeededGPR(regS);
		}
		else
		{
			const int regT = mVU.regAlloc->allocGPR(_It_, -1);
			const int regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xe_add32_rr(regS, regT);
			mVU.regAlloc->clearNeededGPR(regS);
			mVU.regAlloc->clearNeededGPR(regT);
		}
	}
}

mVUop(mVU_IADDI)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm5_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xe_mov32_ri(regT, _Imm5_);
				else
					xe_xor32_rr(regT, regT);
			}
			else
			{
				xe_mov32_rm(regT, &curI);
				xe_shl32_ri(regT, 21);
				xe_sar32_ri(regT, 27);
			}
			mVU.regAlloc->clearNeededGPR(regT);
		}
		else
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xe_add32_ri(regS, _Imm5_);
			}
			else
			{
				xe_mov32_rm(gprT1, &curI);
				xe_shl32_ri(gprT1, 21);
				xe_sar32_ri(gprT1, 27);

				xe_add32_rr(regS, gprT1);
			}
			mVU.regAlloc->clearNeededGPR(regS);
		}
	}
}

mVUop(mVU_IADDIU)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm15_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xe_mov32_ri(regT, _Imm15_);
				else
					xe_xor32_rr(regT, regT);
			}
			else
			{
				xe_mov32_rm(regT, &curI);
				xe_mov32_rr(gprT1, regT);
				xe_shr32_ri(gprT1, 10);
				xe_and32_ri(gprT1, 0x7800);
				xe_and32_ri(regT, 0x7FF);
				xe_or32_rr(regT, gprT1);
			}
			mVU.regAlloc->clearNeededGPR(regT);
		}
		else
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xe_add32_ri(regS, _Imm15_);
			}
			else
			{
				xe_mov32_rm(gprT1, &curI);
				xe_mov32_rr(gprT2, gprT1);
				xe_shr32_ri(gprT2, 10);
				xe_and32_ri(gprT2, 0x7800);
				xe_and32_ri(gprT1, 0x7FF);
				xe_or32_rr(gprT1, gprT2);

				xe_add32_rr(regS, gprT1);
			}
			mVU.regAlloc->clearNeededGPR(regS);
		}
	}
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const int regT = mVU.regAlloc->allocGPR(_It_, -1);
		const int regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xe_and32_rr(regS, regT);
		mVU.regAlloc->clearNeededGPR(regS);
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const int regT = mVU.regAlloc->allocGPR(_It_, -1);
		const int regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xe_or32_rr(regS, regT);
		mVU.regAlloc->clearNeededGPR(regS);
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const int regT = mVU.regAlloc->allocGPR(_It_, -1);
			const int regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xe_sub32_rr(regS, regT);
			mVU.regAlloc->clearNeededGPR(regS);
			mVU.regAlloc->clearNeededGPR(regT);
		}
		else
		{
			const int regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xe_xor32_rr(regD, regD);
			mVU.regAlloc->clearNeededGPR(regD);
		}
	}
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const int regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		if (!EmuConfig.Gamefixes.IbitHack)
		{
			if (_Imm15_ != 0)
				xe_sub32_ri(regS, _Imm15_);
		}
		else
		{
			xe_mov32_rm(gprT1, &curI);
			xe_mov32_rr(gprT2, gprT1);
			xe_shr32_ri(gprT2, 10);
			xe_and32_ri(gprT2, 0x7800);
			xe_and32_ri(gprT1, 0x7FF);
			xe_or32_rr(gprT1, gprT2);

			xe_sub32_rr(regS, gprT1);
		}
		mVU.regAlloc->clearNeededGPR(regS);
	}
}

//------------------------------------------------------------------
// MFIR/MFP/MOVE/MR32/MTIR
//------------------------------------------------------------------

mVUop(mVU_MFIR)
{
	pass1
	{
		if (!_Ft_)
		{
			mVUlow.isNOP = true;
		}
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeReg2  (mVU, _Ft_, mVUlow.VF_write, 1);
	}
	pass2
	{
		const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_Is_ != 0)
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_, -1);
			xe_movsx32_rr16(regS, regS);
			// TODO: Broadcast instead
			xe_movdzx_xr(Ft, regS);
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVU.regAlloc->clearNeededGPR(regS);
		}
		else
		{
			xe_pxor_xx(Ft, Ft);
		}
		mVU.regAlloc->clearNeededXMM(Ft);
	}
}

mVUop(mVU_MFP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeMFP(mVU, _Ft_);
	}
	pass2
	{
		const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUunpack_xyzw(Ft, xmmPQ, (2 + mVUinfo.readP));
		mVU.regAlloc->clearNeededXMM(Ft);
	}
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_);
		const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			xe_pshufd_xxi(Ft, Fs, 0x39);
		mVU.regAlloc->clearNeededXMM(Ft);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_MTIR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeReg5(mVU, _Fs_, _Fsf_, mVUlow.VF_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xe_movd_rx(regT, Fs);
		mVU.regAlloc->clearNeededGPR(regT);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

//------------------------------------------------------------------
// ILW/ILWR
//------------------------------------------------------------------

mVUop(mVU_ILW)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem + offsetSS;
		std::optional<struct e_mem> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xe_add32_ri(gprT1, _Imm11_);
			}
			else
			{
				xe_mov32_rm(gprT2, &curI);
				xe_shl32_ri(gprT2, 21);
				xe_sar32_ri(gprT2, 21);

				xe_add32_rr(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q);
		}

		const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		{ struct e_mem xm; if (optaddr.has_value()) xm = optaddr.value(); else xe_complexaddr_si(xm, gprT2q, ptr, gprT1q, 1); xe_movzx32_rmemg16(regT, xm); }
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

mVUop(mVU_ILWR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem + offsetSS;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix (mVU, gprT1q);

			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			{ struct e_mem xm; xe_complexaddr_si(xm, gprT2q, ptr, gprT1q, 1); xe_movzx32_rmemg16(regT, xm); }
			mVU.regAlloc->clearNeededGPR(regT);
		}
		else
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xe_movzx32_rm16(regT, ptr);
			mVU.regAlloc->clearNeededGPR(regT);
		}
	}
}

//------------------------------------------------------------------
// ISW/ISWR
//------------------------------------------------------------------

mVUop(mVU_ISW)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		std::optional<struct e_mem> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xe_add32_ri(gprT1, _Imm11_);
			}
			else
			{
				xe_mov32_rm(gprT2, &curI);
				xe_shl32_ri(gprT2, 21);
				xe_sar32_ri(gprT2, 21);

				xe_add32_rr(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q);
		}

		// If regT is dirty, the high bits might not be zero.
		const int regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		struct e_mem mptr; if (optaddr.has_value()) mptr = optaddr.value(); else xe_complexaddr_si(mptr, gprT2q, vuRegs[mVU.index].Mem, gprT1q, 1);
		if (_X) xe_mov32_memgr(mptr, regT);
		if (_Y) xe_mov32_memgr(e_mem_off(mptr, 4), regT);
		if (_Z) xe_mov32_memgr(e_mem_off(mptr, 8), regT);
		if (_W) xe_mov32_memgr(e_mem_off(mptr, 12), regT);
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

mVUop(mVU_ISWR)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		void* base = vuRegs[mVU.index].Mem;
		int is = -1;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		const int regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		if (!(is < 0) && (sptr)base != (s32)(sptr)base)
		{
			int register_offset = -1;
			auto writeBackAt = [&](int offset) {
				if (register_offset == -1)
				{
					xe_lea64_m(gprT2q, (void*)((sptr)base + offset));
					register_offset = offset;
				}
				{ struct e_mem xm; E_MEM(xm, gprT2q, is, 1, (e_sptr)(offset - register_offset)); xe_mov32_memgr(xm, regT); }
			};
			if (_X) writeBackAt(0);
			if (_Y) writeBackAt(4);
			if (_Z) writeBackAt(8);
			if (_W) writeBackAt(12);
		}
		else if ((is < 0))
		{
			if (_X) xe_mov32_mr((void*)((uptr)base), regT);
			if (_Y) xe_mov32_mr((void*)((uptr)base + 4), regT);
			if (_Z) xe_mov32_mr((void*)((uptr)base + 8), regT);
			if (_W) xe_mov32_mr((void*)((uptr)base + 12), regT);
		}
		else
		{
			if (_X) { struct e_mem xm; E_MEM(xm, E_NOREG, is, 1, (e_sptr)(uptr)base + 0); xe_mov32_memgr(xm, regT); }
			if (_Y) { struct e_mem xm; E_MEM(xm, E_NOREG, is, 1, (e_sptr)(uptr)base + 4); xe_mov32_memgr(xm, regT); }
			if (_Z) { struct e_mem xm; E_MEM(xm, E_NOREG, is, 1, (e_sptr)(uptr)base + 8); xe_mov32_memgr(xm, regT); }
			if (_W) { struct e_mem xm; E_MEM(xm, E_NOREG, is, 1, (e_sptr)(uptr)base + 12); xe_mov32_memgr(xm, regT); }
		}
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

//------------------------------------------------------------------
// LQ/LQD/LQI
//------------------------------------------------------------------

mVUop(mVU_LQ)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, false); }
	pass2
	{
		const std::optional<struct e_mem> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xe_add32_ri(gprT1, _Imm11_);
			}
			else
			{
				xe_mov32_rm(gprT2, &curI);
				xe_shl32_ri(gprT2, 21);
				xe_sar32_ri(gprT2, 21);

				xe_add32_rr(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q);
		}

		const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		struct e_mem _mp; if (optaddr.has_value()) _mp = optaddr.value(); else xe_complexaddr_si(_mp, gprT2q, vuRegs[mVU.index].Mem, gprT1q, 1);
		mVUloadReg(Ft, _mp, _X_Y_Z_W);
		mVU.regAlloc->clearNeededXMM(Ft);
	}
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		int is = -1;
		if (_Is_ || isVU0) // Access VU1 regs mem-map in !_Is_ case
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xe_dec32_r(regS);
			xe_movsx32_rr16(gprT1, regS); // TODO: Confirm
			mVU.regAlloc->clearNeededGPR(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		if (!mVUlow.noWriteVF)
		{
			const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			struct e_mem _mp; if ((is < 0)) _mp = e_mem_abs(ptr); else xe_complexaddr_si(_mp, gprT2q, ptr, is, 1);
			mVUloadReg(Ft, _mp, _X_Y_Z_W);
			mVU.regAlloc->clearNeededXMM(Ft);
		}
	}
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		int is = -1;
		if (_Is_)
		{
			const int regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xe_movsx32_rr16(gprT1, regS); // TODO: Confirm
			xe_inc32_r(regS);
			mVU.regAlloc->clearNeededGPR(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		if (!mVUlow.noWriteVF)
		{
			const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			struct e_mem _mp; if ((is < 0)) _mp = e_mem_abs(ptr); else xe_complexaddr_si(_mp, gprT2q, ptr, is, 1);
			mVUloadReg(Ft, _mp, _X_Y_Z_W);
			mVU.regAlloc->clearNeededXMM(Ft);
		}
	}
}

//------------------------------------------------------------------
// SQ/SQD/SQI
//------------------------------------------------------------------

mVUop(mVU_SQ)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, false); }
	pass2
	{
		const std::optional<struct e_mem> optptr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0));
		if (!optptr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _It_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xe_add32_ri(gprT1, _Imm11_);
			}
			else
			{
				xe_mov32_rm(gprT2, &curI);
				xe_shl32_ri(gprT2, 21);
				xe_sar32_ri(gprT2, 21);

				xe_add32_rr(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q);
		}

		const int Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		struct e_mem _mp; if (optptr.has_value()) _mp = optptr.value(); else xe_complexaddr_si(_mp, gprT2q, vuRegs[mVU.index].Mem, gprT1q, 1);
		mVUsaveReg(Fs, _mp, _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		int it = -1;
		if (_It_ || isVU0) // Access VU1 regs mem-map in !_It_ case
		{
			const int regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xe_dec32_r(regT);
			xe_movzx32_rr16(gprT1, regT);
			mVU.regAlloc->clearNeededGPR(regT);
			mVUaddrFix(mVU, gprT1q);
			it = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		const int Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		struct e_mem _mp; if ((it < 0)) _mp = e_mem_abs(ptr); else xe_complexaddr_si(_mp, gprT2q, ptr, it, 1);
		mVUsaveReg(Fs, _mp, _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

mVUop(mVU_SQI)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		if (_It_)
		{
			const int regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xe_movzx32_rr16(gprT1, regT);
			xe_inc32_r(regT);
			mVU.regAlloc->clearNeededGPR(regT);
			mVUaddrFix(mVU, gprT1q);
		}
		const int Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		struct e_mem _mp; if (_It_) xe_complexaddr_si(_mp, gprT2q, ptr, gprT1q, 1); else _mp = e_mem_abs(ptr);
		mVUsaveReg(Fs, _mp, _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeededXMM(Fs);
	}
}

//------------------------------------------------------------------
// RINIT/RGET/RNEXT/RXOR
//------------------------------------------------------------------

mVUop(mVU_RINIT)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xe_movd_rx(gprT1, Fs);
			xe_and32_ri(gprT1, 0x007fffff);
			xe_or32_ri(gprT1, 0x3f800000);
			xe_mov32_mr(Rmem, gprT1);
			mVU.regAlloc->clearNeededXMM(Fs);
		}
		else
			xe_mov32_mi(Rmem, 0x3f800000);
	}
}

static __fi void mVU_RGET_(mV, int Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const int Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		xe_movdzx_xr(Ft, Rreg);
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVU.regAlloc->clearNeededXMM(Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		xe_mov32_rm(gprT1, Rmem);
		mVU_RGET_(mVU, gprT1);
	}
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		// algorithm from www.project-fao.org
		const int temp3 = mVU.regAlloc->allocGPR();
		xe_mov32_rm(temp3, Rmem);
		xe_mov32_rr(gprT1, temp3);
		xe_shr32_ri(gprT1, 4);
		xe_and32_ri(gprT1, 1);

		xe_mov32_rr(gprT2, temp3);
		xe_shr32_ri(gprT2, 22);
		xe_and32_ri(gprT2, 1);

		xe_shl32_ri(temp3, 1);
		xe_xor32_rr(gprT1, gprT2);
		xe_xor32_rr(temp3, gprT1);
		xe_and32_ri(temp3, 0x007fffff);
		xe_or32_ri(temp3, 0x3f800000);
		xe_mov32_mr(Rmem, temp3);
		mVU_RGET_(mVU, temp3);
		mVU.regAlloc->clearNeededGPR(temp3);
	}
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const int Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xe_movd_rx(gprT1, Fs);
			xe_and32_ri(gprT1, 0x7fffff);
			xe_xor32_mr(Rmem, gprT1);
			mVU.regAlloc->clearNeededXMM(Fs);
		}
	}
}

//------------------------------------------------------------------
// WaitP/WaitQ
//------------------------------------------------------------------

mVUop(mVU_WAITP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0));
	}
}

mVUop(mVU_WAITQ)
{
	pass1 { mVUstall = std::max(mVUstall, mVUregs.q); }
}

//------------------------------------------------------------------
// XTOP/XITOP
//------------------------------------------------------------------

mVUop(mVU_XTOP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}

		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xe_movzx32_rm16(regT, &vu1Thread.vifRegs.top);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xe_movzx32_rm16(regT, &vif1Regs.top);
			else
				xe_movzx32_rm16(regT, &vif0Regs.top);
		}
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

mVUop(mVU_XITOP)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xe_movzx32_rm16(regT, &vu1Thread.vifRegs.itop);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xe_movzx32_rm16(regT, &vif1Regs.itop);
			else
				xe_movzx32_rm16(regT, &vif0Regs.itop);
		}
		xe_and32_ri(regT, isVU1 ? 0x3ff : 0xff);
		mVU.regAlloc->clearNeededGPR(regT);
	}
}

//------------------------------------------------------------------
// XGkick
//------------------------------------------------------------------

void mVU_XGKICK_(u32 addr)
{
	addr = (addr & 0x3ff) * 16;
	u32 diff = 0x4000 - addr;
	u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, addr, ~0u, true);

	if (size > diff)
	{
		gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[addr], diff, true);
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[0], size - diff, true);
	}
	else
	{
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[addr], size, true);
	}
}

void _vuXGKICKTransfermVU(bool flush)
{
	while (vuRegs[1].xgkickenable && (flush || vuRegs[1].xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (vuRegs[1].xgkicksizeremaining == 0)
		{
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, vuRegs[1].xgkickaddr, ~0u, flush);
			vuRegs[1].xgkicksizeremaining = size & 0xFFFF;
			vuRegs[1].xgkickendpacket = size >> 31;
			vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

			if (vuRegs[1].xgkicksizeremaining == 0)
			{
				vuRegs[1].xgkickenable = false;
				break;
			}
		}

		if (!flush)
			transfersize = std::min(vuRegs[1].xgkicksizeremaining, vuRegs[1].xgkickcyclecount * 8);
		else
			transfersize = vuRegs[1].xgkicksizeremaining;
		transfersize = std::min(transfersize, vuRegs[1].xgkickdiff);

		// Would be "nicer" to do the copy until it's all up, however this really screws up PATH3 masking stuff
		// So lets just do it the other way :)
		if (THREAD_VU1 && (transfersize < vuRegs[1].xgkicksizeremaining))
			gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize, true);
		else
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize, true);

		if (flush)
			vuRegs[1].cycle += transfersize / 8;

		vuRegs[1].xgkickcyclecount -= transfersize / 8;

		vuRegs[1].xgkickaddr = (vuRegs[1].xgkickaddr + transfersize) & 0x3FFF;
		vuRegs[1].xgkicksizeremaining -= transfersize;
		vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

		if (vuRegs[1].xgkickendpacket && !vuRegs[1].xgkicksizeremaining)
			vuRegs[1].xgkickenable = false;
	}
}

static __fi void mVU_XGKICK_SYNC(mV, bool flush)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	// Add the single cycle remainder after this instruction, some games do the store
	// on the second instruction after the kick and that needs to go through first
	// but that's VERY close..
	xe_test32_mi(&vuRegs[1].xgkickenable, 0x1);
	e_u8* skipxgkick; xe_fwd_jcc32(Jcc_Zero, skipxgkick);
	xe_add32_mi(&vuRegs[1].xgkickcyclecount, mVUlow.kickcycles-1);
	xe_cmp32_mi(&vuRegs[1].xgkickcyclecount, 2);
	e_u8* needcycles; xe_fwd_jcc32(Jcc_Less, needcycles);
	mVUbackupRegs(mVU, true, true);
	xe_fastcall1_i(_vuXGKICKTransfermVU, flush);
	mVUrestoreRegs(mVU, true, true);
	xe_fwd_set32(needcycles);
	xe_add32_mi(&vuRegs[1].xgkickcyclecount, 1);
	xe_fwd_set32(skipxgkick);
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	mVUbackupRegs(mVU, true, true);
#if 0 // XGkick Break - ToDo: Change "SomeGifPathValue" to w/e needs to be tested
	xTEST(ptr32[&SomeGifPathValue], 1); // If '1', breaks execution
	xMOV(ptr32[&mVU.resumePtrXG], (uptr)xGetPtr() + 10 + 6);
	xJcc32(Jcc_NotZero, (uptr)mVU.exitFunctXG - ((uptr)xGetPtr()+6));
#endif
	xe_fastcall1_m32(mVU_XGKICK_, &mVU.VIxgkick);
	mVUrestoreRegs(mVU, true, true);
}

mVUop(mVU_XGKICK)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeXGkick(mVU, _Is_, 1);
	}
		pass2
	{
		if (CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
			mVUlow.kickcycles = 0;
		}
		if (mVUinfo.doXGKICK) // check for XGkick Transfer
		{
			mVU_XGKICK_DELAY(mVU);
			mVUinfo.doXGKICK = false;
		}

		const int regS = mVU.regAlloc->allocGPR(_Is_, -1);
		if (!CHECK_XGKICKHACK)
		{
			xe_mov32_mr(&mVU.VIxgkick, regS);
		}
		else
		{
			xe_mov32_mi(&vuRegs[1].xgkickenable, 1);
			xe_mov32_mi(&vuRegs[1].xgkickendpacket, 0);
			xe_mov32_mi(&vuRegs[1].xgkicksizeremaining, 0);
			xe_mov32_mi(&vuRegs[1].xgkickcyclecount, 0);
			xe_mov32_rm(gprT2, &mVU.totalCycles);
			xe_sub32_rm(gprT2, &mVU.cycles);
			xe_add64_rm(gprT2q, &vuRegs[1].cycle);
			xe_mov64_mr(&vuRegs[1].xgkicklastcycle, gprT2q);
			xe_mov32_rr(gprT1, regS);
			xe_and32_ri(gprT1, 0x3FF);
			xe_shl32_ri(gprT1, 4);
			xe_mov32_mr(&vuRegs[1].xgkickaddr, gprT1);
		}
		mVU.regAlloc->clearNeededGPR(regS);
	}
}

//------------------------------------------------------------------
// Branches/Jumps
//------------------------------------------------------------------

void setBranchA(mP, int x, int _x_)
{
	bool isBranchDelaySlot = false;

	incPC(-2);
	if (mVUlow.branch)
		isBranchDelaySlot = true;
	incPC(2);

	pass1
	{
		if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUbranch     = x;
		mVUlow.branch = x;
	}
	pass2 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
	pass4 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
}

void condEvilBranch(mV, int JMPcc)
{
	if (mVUlow.badBranch)
	{
		xe_mov32_mr(&mVU.branch, gprT1);
		xe_mov32_mi(&mVU.badBranch, branchAddr(mVU));

		xe_cmp8_ri(gprT1b, 0);
		e_u8* cJMP; xe_fwd_jcc8((JccComparisonType)JMPcc, cJMP);
			incPC(4); // Branch Not Taken Addr
			xe_mov32_mi(&mVU.badBranch, xPC);
			incPC(-4);
		xe_fwd_set8(cJMP);
		return;
	}
	if (isEvilBlock)
	{
		xe_mov32_mi(&mVU.evilevilBranch, branchAddr(mVU));
		xe_cmp8_ri(gprT1b, 0);
		e_u8* cJMP; xe_fwd_jcc8((JccComparisonType)JMPcc, cJMP);
		xe_mov32_rm(gprT1, &mVU.evilBranch); // Branch Not Taken
		xe_add32_ri(gprT1, 8); // We have already executed 1 instruction from the original branch
		xe_mov32_mr(&mVU.evilevilBranch, gprT1);
		xe_fwd_set8(cJMP);
	}
	else
	{
		xe_mov32_mi(&mVU.evilBranch, branchAddr(mVU));
		xe_cmp8_ri(gprT1b, 0);
		e_u8* cJMP; xe_fwd_jcc8((JccComparisonType)JMPcc, cJMP);
		xe_mov32_rm(gprT1, &mVU.badBranch); // Branch Not Taken
		xe_add32_ri(gprT1, 8); // We have already executed 1 instruction from the original branch
		xe_mov32_mr(&mVU.evilBranch, gprT1);
		xe_fwd_set8(cJMP);
		incPC(-2);
		incPC(2);
	}
}

mVUop(mVU_B)
{
	setBranchA(mX, 1, 0);
	pass1 { mVUanalyzeNormBranch(mVU, 0, false); }
	pass2
	{
		if (mVUlow.badBranch)  { xe_mov32_mi(&mVU.badBranch,  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if(isEvilBlock) xe_mov32_mi(&mVU.evilevilBranch, branchAddr(mVU)); else xe_mov32_mi(&mVU.evilBranch, branchAddr(mVU)); }
	}
}

mVUop(mVU_BAL)
{
	setBranchA(mX, 2, _It_);
	pass1 { mVUanalyzeNormBranch(mVU, _It_, true); }
	pass2
	{
		if (!mVUlow.evilBranch)
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xe_mov32_ri(regT, bSaveAddr);
			mVU.regAlloc->clearNeededGPR(regT);
		}
		else
		{
			incPC(-2);
			incPC(2);

			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
				xe_mov32_rm(regT, &mVU.evilBranch);
			else
				xe_mov32_rm(regT, &mVU.badBranch);

			xe_add32_ri(regT, 8);
			xe_shr32_ri(regT, 3);
			mVU.regAlloc->clearNeededGPR(regT);
		}

		if (mVUlow.badBranch)  { xe_mov32_mi(&mVU.badBranch,  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xe_mov32_mi(&mVU.evilevilBranch, branchAddr(mVU)); else xe_mov32_mi(&mVU.evilBranch, branchAddr(mVU)); }
	}
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xe_xor32_rm(gprT1, &mVU.VIbackup);
		else
		{
			const int regT = mVU.regAlloc->allocGPR(_It_);
			xe_xor32_rr(gprT1, regT);
			mVU.regAlloc->clearNeededGPR(regT);
		}

		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_Equal);
	}
}

mVUop(mVU_IBGEZ)
{
	setBranchA(mX, 4, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_GreaterOrEqual);
	}
}

mVUop(mVU_IBGTZ)
{
	setBranchA(mX, 5, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_Greater);
	}
}

mVUop(mVU_IBLEZ)
{
	setBranchA(mX, 6, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_LessOrEqual);
	}
}

mVUop(mVU_IBLTZ)
{
	setBranchA(mX, 7, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_Less);
	}
}

mVUop(mVU_IBNE)
{
	setBranchA(mX, 8, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xe_mov32_rm(gprT1, &mVU.VIbackup);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xe_xor32_rm(gprT1, &mVU.VIbackup);
		else
		{
			const int regT = mVU.regAlloc->allocGPR(_It_);
			xe_xor32_rr(gprT1, regT);
			mVU.regAlloc->clearNeededGPR(regT);
		}

		if (!(isBadOrEvil))
			xe_mov32_mr(&mVU.branch, gprT1);
		else
			condEvilBranch(mVU, Jcc_NotEqual);
	}
}

void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		xe_shl32_ri(gprT1, 3);
		xe_and32_ri(gprT1, mVU.microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			xe_mov32_mr(&mVU.branch, gprT1);
		}
		else
		{
			if(isEvilBlock)
				xe_mov32_mr(&mVU.evilevilBranch, gprT1);
			else
				xe_mov32_mr(&mVU.evilBranch, gprT1);
		}
		//If delay slot is conditional, it uses badBranch to go to its target
		if (mVUlow.badBranch)
		{
			xe_mov32_mr(&mVU.badBranch, gprT1);
		}
	}
}

mVUop(mVU_JR)
{
	mVUbranch = 9;
	pass1 { mVUanalyzeJump(mVU, _Is_, 0, false); }
	pass2
	{
		normJumpPass2(mVU);
	}
}

mVUop(mVU_JALR)
{
	mVUbranch = 10;
	pass1 { mVUanalyzeJump(mVU, _Is_, _It_, 1); }
	pass2
	{
		normJumpPass2(mVU);
		if (!mVUlow.evilBranch)
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xe_mov32_ri(regT, bSaveAddr);
			mVU.regAlloc->clearNeededGPR(regT);
		}
		if (mVUlow.evilBranch)
		{
			const int regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
			{
				xe_mov32_rm(regT, &mVU.evilBranch);
			}
			else
			{
				incPC(-2);
				incPC(2);

				xe_mov32_rm(regT, &mVU.badBranch);
			}
			xe_add32_ri(regT, 8);
			xe_shr32_ri(regT, 3);
			mVU.regAlloc->clearNeededGPR(regT);
		}
	}
}
