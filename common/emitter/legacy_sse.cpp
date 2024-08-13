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

#include "common/emitter/legacy_internal.h"

using namespace x86Emitter;

// ------------------------------------------------------------------------
//                         Begin SSE-Only Part!
// ------------------------------------------------------------------------

emitterT void SSE_SUBSS_XMM_to_XMM (int to, int from) { xSUB.SS(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE_ADDSS_XMM_to_XMM (int to, int from) { xADD.SS(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE_MINSS_XMM_to_XMM (int to, int from) { xMIN.SS(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE_MAXSS_XMM_to_XMM (int to, int from) { xMAX.SS(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE2_SUBSD_XMM_to_XMM(int to, int from) { xSUB.SD(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE2_ADDSD_XMM_to_XMM(int to, int from) { xADD.SD(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE2_MINSD_XMM_to_XMM(int to, int from) { xMIN.SD(xRegisterSSE(to), xRegisterSSE(from)); }
emitterT void SSE2_MAXSD_XMM_to_XMM(int to, int from) { xMAX.SD(xRegisterSSE(to), xRegisterSSE(from)); }
