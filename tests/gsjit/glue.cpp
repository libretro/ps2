/* The software renderer's code-generator map over the real GSCodeReserve,
 * with a stand-in generator, for main.c. A key's low 32 bits name the
 * function and its high 32 bits the bytes it takes; the generator writes
 * the name's low byte over them, and like the real generator's emitter
 * stops writing at the end of the room it is given. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSFunctionMap.h"

static void gsjit_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
extern "C" retro_log_printf_t log_cb = gsjit_log;

typedef const u8* FakeFn;

struct FakeCG
{
	u8* m_code;
	size_t m_size;

	FakeCG(u64 key, void* code, size_t room)
		: m_code(static_cast<u8*>(code))
	{
		const size_t want = (size_t)(key >> 32);
		m_size = want < room ? want : room;
		memset(m_code, (int)(key & 0xff), m_size);
	}
	size_t getSize() const { return m_size; }
	const u8* getCode() const { return m_code; }
};

static GSCodeGeneratorFunctionMap<FakeCG, u64, FakeFn> s_map;

extern "C" void gsjit_assign(unsigned char* base, size_t size)
{
	GSCodeReserve::GetInstance().VirtualMemoryReserve::Assign(nullptr, base, size);
	GSCodeReserve::GetInstance().Reset();
}

extern "C" const unsigned char* gsjit_get(unsigned long long key)
{
	return s_map[key];
}

/* What GSDrawScanline::ResetCodeCache does. */
extern "C" void gsjit_reset(void)
{
	s_map.Clear();
	GSCodeReserve::GetInstance().Reset();
}

extern "C" void gsjit_release(void)
{
	s_map.Clear();
	GSCodeReserve::GetInstance().VirtualMemoryReserve::Assign(nullptr, nullptr, 0);
}
