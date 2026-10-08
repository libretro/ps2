/* The ABI frame generated code that is entered by a call keeps around
 * itself. C; the instruction macros are c89ops.h's.
 *
 * XE_FRAME_WIN64 picks the Win64 frame, and follows _WIN32 unless set
 * before this is included: tests/emitter/frame.c sets it to run the
 * Win64 frame on any x86-64 host. */
#ifndef XE_C89FRAME_H
#define XE_C89FRAME_H

#include "c89ops.h"

#ifndef XE_FRAME_WIN64
#ifdef _WIN32
#define XE_FRAME_WIN64 1
#else
#define XE_FRAME_WIN64 0
#endif
#endif

/* Every x86-64 calling convention wants the stack 16-byte aligned at a
 * call; the frame pads to that. */
/* Shadow space and Win64's extra callee-saved registers - rsi, rdi and
 * xmm6-xmm15 - are the only Win64/SysV differences; the shared skeleton
 * lives once and the platform inserts its extra chunk at the marked
 * points. The generated code inside uses every xmm register, and its
 * caller is compiled C that may hold values in xmm6-xmm15 across the
 * call. */
#if XE_FRAME_WIN64
#define SCOPED_STACK_FRAME_WIN_BEGIN(m_offset) \
	xe_push64_r(7); \
	xe_push64_r(6); \
	xe_sub64_ri(4, 32 + 160); \
	xe_win64_save_xmm(32); \
	(m_offset) += 48 + 160;
#define SCOPED_STACK_FRAME_WIN_END() \
	xe_win64_restore_xmm(32); \
	xe_add64_ri(4, 32 + 160); \
	xe_pop64_r(6); \
	xe_pop64_r(7);
#else
#define SCOPED_STACK_FRAME_WIN_BEGIN(m_offset)
#define SCOPED_STACK_FRAME_WIN_END()
#endif

#define SCOPED_STACK_FRAME_BEGIN(m_offset) \
	(m_offset) = sizeof(void*); \
	xe_push64_r(5); \
	(m_offset) += sizeof(void*); \
	xe_push64_r(3); \
	xe_push64_r(12); \
	xe_push64_r(13); \
	xe_push64_r(14); \
	xe_push64_r(15); \
	(m_offset) += 40; \
	SCOPED_STACK_FRAME_WIN_BEGIN(m_offset) \
	xe_add64_ri(4, (-((16 - ((m_offset) % 16)) % 16)))

#define SCOPED_STACK_FRAME_END(m_offset) \
	xe_add64_ri(4, ((16 - ((m_offset) % 16)) % 16)); \
	SCOPED_STACK_FRAME_WIN_END() \
	xe_pop64_r(15); \
	xe_pop64_r(14); \
	xe_pop64_r(13); \
	xe_pop64_r(12); \
	xe_pop64_r(3); \
	xe_pop64_r(5)

#endif
