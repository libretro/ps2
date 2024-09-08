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

// Test if Vector is +/- Zero
static __fi void testZero(const xmm& xmmReg, const xmm& xmmTemp, const x32& gprTemp)
{
	xXOR.PS(xmmTemp, xmmTemp);
	xCMPEQ.SS(xmmTemp, xmmReg);
	xPTEST(xmmTemp, xmmTemp);
}

// Test if Vector is Negative (Set Flags and Makes Positive)
static __fi void testNeg(mV, const xmm& xmmReg, const x32& gprTemp)
{
	xMOVMSKPS(gprTemp, xmmReg);
	xTEST(gprTemp, 1);
	xForwardJZ8 skip;
		xMOV(ptr32[&mVU.divFlag], divI);
		xAND.PS(xmmReg, ptr128[mVUglob.absclip]);
	skip.SetTarget();
}

mVUop(mVU_DIV)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7); }
	pass2
	{
		xmm Ft;
		if (_Ftf_) Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		else       Ft = mVU.regAlloc->allocReg(_Ft_);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 cjmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 ajmp;
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 bjmp;
			ajmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide (only when not 0/0)
			bjmp.SetTarget();

			xXOR.PS(Fs, Ft);
			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS (Fs, ptr128[mVUglob.maxvals]); // If division by zero, then xmmFs = +/- fmax

			xForwardJump8 djmp;
		cjmp.SetTarget();
			xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
	}
}

mVUop(mVU_SQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7); }
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		if (CHECK_VU_OVERFLOW(mVU.index)) // Clamp infinities (only need to do positive clamp since xmmFt is positive)
			xMIN.SS(Ft, ptr32[mVUglob.maxvals]);
		xSQRT.SS(Ft, Ft);

		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RSQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		xSQRT.SS(Ft, Ft);
		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 ajmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 bjmp; // Skip if none are
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 cjmp;
			bjmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide flag (only when not 0/0)
			cjmp.SetTarget();

			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS(Fs, ptr128[mVUglob.maxvals]); // xmmFs = +/-Max

			xForwardJump8 djmp;
		ajmp.SetTarget();
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
	}
}

//------------------------------------------------------------------
// EATAN/EEXP/ELENG/ERCPR/ERLENG/ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM
//------------------------------------------------------------------

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, PQ, t1); \
	}

// ToDo: Can Be Optimized Further? (takes approximately (~115 cycles + mem access time) on a c2d)
static __fi void mVU_EATAN_(mV, const xmm& PQ, const xmm& Fs, const xmm& t1, const xmm& t2)
{
	xMOVSS(PQ, Fs);
	xMUL.SS(PQ, ptr32[mVUglob.T1]);
	xMOVAPS(t2, Fs);
	EATANhelper(mVUglob.T2);
	EATANhelper(mVUglob.T3);
	EATANhelper(mVUglob.T4);
	EATANhelper(mVUglob.T5);
	EATANhelper(mVUglob.T6);
	EATANhelper(mVUglob.T7);
	EATANhelper(mVUglob.T8);
	xADD.SS(PQ, ptr32[mVUglob.Pi4]);
	xPSHUF.D(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS (xmmPQ, Fs);
		xSUB.SS(Fs,    ptr32[mVUglob.one]);
		xADD.SS(xmmPQ, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
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
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x01);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1); // y-x, not y-1? ><
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
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
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x02);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		xMUL.SS (xmmPQ, ptr32[mVUglob.E1]);
		xADD.SS (xmmPQ, ptr32[mVUglob.one]);
		xMOVAPS(t1, Fs);
		SSE_MULSS(mVU, t1, Fs);
		xMOVAPS(t2, t1);
		xMUL.SS(t1, ptr32[mVUglob.E2]);
		SSE_ADDSS(mVU, xmmPQ, t1);
		eexpHelper(&mVUglob.E3);
		eexpHelper(&mVUglob.E4);
		eexpHelper(&mVUglob.E5);
		SSE_MULSS(mVU, t2, Fs);
		xMUL.SS(t2, ptr32[mVUglob.E6]);
		SSE_ADDSS(mVU, xmmPQ, t2);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		xMOVSSZX(t2, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, t2, xmmPQ);
		xMOVSS(xmmPQ, t2);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

// sumXYZ(): PQ.x = x ^ 2 + y ^ 2 + z ^ 2
static __fi void mVU_sumXYZ(mV, const xmm& PQ, const xmm& Fs)
{
	xDP.PS(Fs, Fs, 0x71);
	xMOVSS(PQ, Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS        (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xAND.PS       (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS      (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS        (xmmPQ, Fs); // pq = X
		SSE_MULSS(mVU, Fs, Fs);    // fs = X^2
		xMOVAPS       (t1, Fs);    // t1 = X^2
		SSE_MULSS(mVU, Fs, xmmPQ); // fs = X^3
		xMOVAPS       (t2, Fs);    // t2 = X^3
		xMUL.SS       (Fs, ptr32[mVUglob.S2]); // fs = s2 * X^3
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3

		SSE_MULSS(mVU, t2, t1);    // t2 = X^3 * X^2
		xMOVAPS       (Fs, t2);    // fs = X^5
		xMUL.SS       (Fs, ptr32[mVUglob.S3]); // ps = s3 * X^5
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5

		SSE_MULSS(mVU, t2, t1);    // t2 = X^5 * X^2
		xMOVAPS       (Fs, t2);    // fs = X^7
		xMUL.SS       (Fs, ptr32[mVUglob.S4]); // fs = s4 * X^7
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7

		SSE_MULSS(mVU, t2, t1);    // t2 = X^7 * X^2
		xMUL.SS       (t2, ptr32[mVUglob.S5]); // t2 = s5 * X^9
		SSE_ADDSS(mVU, xmmPQ, t2); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 + s5 * X^9
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xAND.PS (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		const xmm& t1 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xPSHUF.D(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1);
		xPSHUF.D(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1);
		xMOVSS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
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
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xAND(dst, _Imm24_);
		xADD(dst, 0xffffff);
		xSHR(dst, 24);
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xXOR(dst, _Imm24_);
		xSUB(dst, 1);
		xSHR(dst, 31);
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		xAND(regT, 0xfff);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xOR(dst, _Imm24_);
		xADD(dst, 1);  // If 24 1's will make 25th bit 1, else 0
		xSHR(dst, 24); // Get the 25th bit (also clears the rest of the garbage in the reg)
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		xMOV(gprT1, _Imm24_);
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
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xAND(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xXOR(regT, gprT1);
		xSUB(regT, 1);
		xSHR(regT, 31);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xOR(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
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
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		xAND(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		xOR(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
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

		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00); // Z  bit
		setBitFSEQ(reg, 0xf000); // S  bit
		setBitFSEQ(reg, 0x000f); // ZS bit
		setBitFSEQ(reg, 0x00f0); // SS bit
		xXOR(reg, imm);
		xSUB(reg, 1);
		xSHR(reg, 31);
		mVU.regAlloc->clearNeeded(reg);
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
		xAND(getFlagReg(sFLAG.write), 0xfff00); // Keep Non-Sticky Bits
		if (imm)
			xOR(getFlagReg(sFLAG.write), imm);
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
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_ ? _Is_ : _It_, -1);
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xMOV(regD, regS);
			mVU.regAlloc->clearNeeded(regD);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xADD(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
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
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (_Imm5_ != 0)
				xMOV(regT, _Imm5_);
			else
				xXOR(regT, regT);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (_Imm5_ != 0)
				xADD(regS, _Imm5_);
			mVU.regAlloc->clearNeeded(regS);
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
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (_Imm15_ != 0)
				xMOV(regT, _Imm15_);
			else
				xXOR(regT, regT);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (_Imm15_ != 0)
				xADD(regS, _Imm15_);
			mVU.regAlloc->clearNeeded(regS);
		}
	}
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xAND(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xOR(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xSUB(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xXOR(regD, regD);
			mVU.regAlloc->clearNeeded(regD);
		}
	}
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		if (_Imm15_ != 0)
			xSUB(regS, _Imm15_);
		mVU.regAlloc->clearNeeded(regS);
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
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_Is_ != 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
			xMOVSX(xRegister32(regS), xRegister16(regS));
			// TODO: Broadcast instead
			xMOVDZX(Ft, regS);
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			xPXOR(Ft, Ft);
		}
		mVU.regAlloc->clearNeeded(Ft);
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
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUunpack_xyzw(Ft, xmmPQ, (2 + mVUinfo.readP));
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_);
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			xPSHUF.D(Ft, Fs, 0x39);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
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
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVD(regT, Fs);
		mVU.regAlloc->clearNeeded(regT);
		mVU.regAlloc->clearNeeded(Fs);
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
		std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, ptr, gprT1q)]);
		mVU.regAlloc->clearNeeded(regT);
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

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[xComplexAddress(gprT2q, ptr, gprT1q)]);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[ptr]);
			mVU.regAlloc->clearNeeded(regT);
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
		std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		// If regT is dirty, the high bits might not be zero.
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		const xAddressVoid ptr(optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q));
		if (_X) xMOV(ptr32[ptr], regT);
		if (_Y) xMOV(ptr32[ptr + 4], regT);
		if (_Z) xMOV(ptr32[ptr + 8], regT);
		if (_W) xMOV(ptr32[ptr + 12], regT);
		mVU.regAlloc->clearNeeded(regT);
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
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		if (!is.IsEmpty() && (sptr)base != (s32)(sptr)base)
		{
			int register_offset = -1;
			auto writeBackAt = [&](int offset) {
				if (register_offset == -1)
				{
					xLEA(gprT2q, ptr[(void*)((sptr)base + offset)]);
					register_offset = offset;
				}
				xMOV(ptr32[gprT2q + is + (offset - register_offset)], regT);
			};
			if (_X) writeBackAt(0);
			if (_Y) writeBackAt(4);
			if (_Z) writeBackAt(8);
			if (_W) writeBackAt(12);
		}
		else if (is.IsEmpty())
		{
			if (_X) xMOV(ptr32[(void*)((uptr)base)], regT);
			if (_Y) xMOV(ptr32[(void*)((uptr)base + 4)], regT);
			if (_Z) xMOV(ptr32[(void*)((uptr)base + 8)], regT);
			if (_W) xMOV(ptr32[(void*)((uptr)base + 12)], regT);
		}
		else
		{
			if (_X) xMOV(ptr32[base + is], regT);
			if (_Y) xMOV(ptr32[base + is + 4], regT);
			if (_Z) xMOV(ptr32[base + is + 8], regT);
			if (_W) xMOV(ptr32[base + is + 12], regT);
		}
		mVU.regAlloc->clearNeeded(regT);
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
		const std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUloadReg(Ft, optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q), _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_ || isVU0) // Access VU1 regs mem-map in !_Is_ case
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xDEC(regS);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			xAddressVoid _ptr = (is.IsEmpty()) ? xAddressVoid(ptr) : xComplexAddress(gprT2q, ptr, is);
			mVUloadReg(Ft, _ptr, _X_Y_Z_W);
			mVU.regAlloc->clearNeeded(Ft);
		}
	}
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			xINC(regS);
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			xAddressVoid _ptr = (is.IsEmpty()) ? xAddressVoid(ptr) : xComplexAddress(gprT2q, ptr, is);
			mVUloadReg(Ft, _ptr, _X_Y_Z_W);
			mVU.regAlloc->clearNeeded(Ft);
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
		const std::optional<xAddressVoid> optptr(mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0));
		if (!optptr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _It_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		mVUsaveReg(Fs, optptr.has_value() ? optptr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg it = xEmptyReg;
		if (_It_ || isVU0) // Access VU1 regs mem-map in !_It_ case
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xDEC(regT);
			xMOVZX(gprT1, xRegister16(regT));
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q);
			it = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		xAddressVoid _ptr = (it.IsEmpty()) ? xAddressVoid(ptr) : xComplexAddress(gprT2q, ptr, it);
		mVUsaveReg(Fs, _ptr, _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
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
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xMOVZX(gprT1, xRegister16(regT));
			xINC(regT);
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q);
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		xAddressVoid _ptr = (_It_) ? xComplexAddress(gprT2q, ptr, gprT1q) : xAddressVoid(ptr);
		mVUsaveReg(Fs, _ptr, _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
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
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x007fffff);
			xOR (gprT1, 0x3f800000);
			xMOV(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		else
			xMOV(ptr32[Rmem], 0x3f800000);
	}
}

static __fi void mVU_RGET_(mV, const x32& Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		xMOVDZX(Ft, Rreg);
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		xMOV(gprT1, ptr32[Rmem]);
		mVU_RGET_(mVU, gprT1);
	}
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		// algorithm from www.project-fao.org
		const xRegister32& temp3 = mVU.regAlloc->allocGPR();
		xMOV(temp3, ptr32[Rmem]);
		xMOV(gprT1, temp3);
		xSHR(gprT1, 4);
		xAND(gprT1, 1);

		xMOV(gprT2, temp3);
		xSHR(gprT2, 22);
		xAND(gprT2, 1);

		xSHL(temp3, 1);
		xXOR(gprT1, gprT2);
		xXOR(temp3, gprT1);
		xAND(temp3, 0x007fffff);
		xOR (temp3, 0x3f800000);
		xMOV(ptr32[Rmem], temp3);
		mVU_RGET_(mVU, temp3);
		mVU.regAlloc->clearNeeded(temp3);
	}
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x7fffff);
			xXOR(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
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
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xMOVZX(regT, ptr16[&vu1Thread.vifRegs.top]);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xMOVZX(regT, ptr16[&vif1Regs.top]);
			else
				xMOVZX(regT, ptr16[&vif0Regs.top]);
		}
		mVU.regAlloc->clearNeeded(regT);
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
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xMOVZX(regT, ptr16[&vu1Thread.vifRegs.itop]);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xMOVZX(regT, ptr16[&vif1Regs.itop]);
			else
				xMOVZX(regT, ptr16[&vif0Regs.itop]);
		}
		xAND(regT, isVU1 ? 0x3ff : 0xff);
		mVU.regAlloc->clearNeeded(regT);
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
	xTEST(ptr32[&vuRegs[1].xgkickenable], 0x1);
	xForwardJZ32 skipxgkick;
	xADD(ptr32[&vuRegs[1].xgkickcyclecount], mVUlow.kickcycles-1);
	xCMP(ptr32[&vuRegs[1].xgkickcyclecount], 2);
	xForwardJL32 needcycles;
	mVUbackupRegs(mVU, true, true);
	xFastCall(_vuXGKICKTransfermVU, flush);
	mVUrestoreRegs(mVU, true, true);
	needcycles.SetTarget();
	xADD(ptr32[&vuRegs[1].xgkickcyclecount], 1);
	skipxgkick.SetTarget();
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	mVUbackupRegs(mVU, true, true);
#if 0 // XGkick Break - ToDo: Change "SomeGifPathValue" to w/e needs to be tested
	xTEST (ptr32[&SomeGifPathValue], 1); // If '1', breaks execution
	xMOV  (ptr32[&mVU.resumePtrXG], (uptr)xGetPtr() + 10 + 6);
	xJcc32(Jcc_NotZero, (uptr)mVU.exitFunctXG - ((uptr)xGetPtr()+6));
#endif
	xFastCall(mVU_XGKICK_, ptr32[&mVU.VIxgkick]);
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

		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
		if (!CHECK_XGKICKHACK)
		{
			xMOV(ptr32[&mVU.VIxgkick], regS);
		}
		else
		{
			xMOV(ptr32[&vuRegs[1].xgkickenable], 1);
			xMOV(ptr32[&vuRegs[1].xgkickendpacket], 0);
			xMOV(ptr32[&vuRegs[1].xgkicksizeremaining], 0);
			xMOV(ptr32[&vuRegs[1].xgkickcyclecount], 0);
			xMOV(gprT2, ptr32[&mVU.totalCycles]);
			xSUB(gprT2, ptr32[&mVU.cycles]);
			xADD(gprT2, ptr32[&vuRegs[1].cycle]);
			xMOV(ptr32[&vuRegs[1].xgkicklastcycle], gprT2);
			xMOV(gprT1, regS);
			xAND(gprT1, 0x3FF);
			xSHL(gprT1, 4);
			xMOV(ptr32[&vuRegs[1].xgkickaddr], gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
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
		xMOV(ptr32[&mVU.branch], gprT1);
		xMOV(ptr32[&mVU.badBranch], branchAddr(mVU));

		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
			incPC(4); // Branch Not Taken Addr
			xMOV(ptr32[&mVU.badBranch], xPC);
			incPC(-4);
		cJMP.SetTarget();
		return;
	}
	if (isEvilBlock)
	{
		xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.evilBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilevilBranch], gprT1);
		cJMP.SetTarget();
	}
	else
	{
		xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.badBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilBranch], gprT1);
		cJMP.SetTarget();
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
		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if(isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
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
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			incPC(-2);
			incPC(2);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
				xMOV(regT, ptr32[&mVU.evilBranch]);
			else
				xMOV(regT, ptr32[&mVU.badBranch]);

			xADD(regT, 8);
			xSHR(regT, 3);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
	}
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
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
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
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
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
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
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
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
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
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
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_NotEqual);
	}
}

void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		xSHL(gprT1, 3);
		xAND(gprT1, mVU.microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			xMOV(ptr32[&mVU.branch], gprT1);
		}
		else
		{
			if(isEvilBlock)
				xMOV(ptr32[&mVU.evilevilBranch], gprT1);
			else
				xMOV(ptr32[&mVU.evilBranch], gprT1);
		}
		//If delay slot is conditional, it uses badBranch to go to its target
		if (mVUlow.badBranch)
		{
			xMOV(ptr32[&mVU.badBranch], gprT1);
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
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		if (mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
			{
				xMOV(regT, ptr32[&mVU.evilBranch]);
			}
			else
			{
				incPC(-2);
				incPC(2);

				xMOV(regT, ptr32[&mVU.badBranch]);
			}
			xADD(regT, 8);
			xSHR(regT, 3);
			mVU.regAlloc->clearNeeded(regT);
		}
	}
}
