/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

class alignas(32) GSDrawingEnvironment
{
public:
	GIFRegPRIM       PRIM;
	GIFRegPRMODE     PRMODE;
	GIFRegPRMODECONT PRMODECONT;
	GIFRegTEXCLUT    TEXCLUT;
	GIFRegSCANMSK    SCANMSK;
	GIFRegTEXA       TEXA;
	GIFRegFOGCOL     FOGCOL;
	GIFRegDIMX       DIMX;
	GIFRegDTHE       DTHE;
	GIFRegCOLCLAMP   COLCLAMP;
	GIFRegPABE       PABE;
	GIFRegBITBLTBUF  BITBLTBUF;
	GIFRegTRXDIR     TRXDIR;
	GIFRegTRXPOS     TRXPOS;
	GIFRegTRXREG     TRXREG;
	GSDrawingContext CTXT[2];

	GSDrawingEnvironment() { }

	void Reset()
	{
		memset(&PRIM, 0, sizeof(PRIM));
		memset(&PRMODE, 0, sizeof(PRMODE));
		memset(&PRMODECONT, 0, sizeof(PRMODECONT));
		memset(&TEXCLUT, 0, sizeof(TEXCLUT));
		memset(&SCANMSK, 0, sizeof(SCANMSK));
		memset(&TEXA, 0, sizeof(TEXA));
		memset(&FOGCOL, 0, sizeof(FOGCOL));
		memset(&DIMX, 0, sizeof(DIMX));
		memset(&DTHE, 0, sizeof(DTHE));
		memset(&COLCLAMP, 0, sizeof(COLCLAMP));
		memset(&PABE, 0, sizeof(PABE));
		memset(&BITBLTBUF, 0, sizeof(BITBLTBUF));
		memset(&TRXDIR, 0, sizeof(TRXDIR));
		memset(&TRXPOS, 0, sizeof(TRXPOS));
		memset(&TRXREG, 0, sizeof(TRXREG));

		CTXT[0].Reset();
		CTXT[1].Reset();
	}
};
