/*  Hardware-context request audit.
 *
 *  The GL renderer is desktop OpenGL only: its shaders are GLSL 330/430
 *  core and GSDeviceOGL requires GL_ARB_shading_language_420pack, neither
 *  of which an OpenGL ES context has. A frontend offered an ES context
 *  accepts it, the GL device then refuses to come up, and there is no
 *  picture. So the core never asks for an ES context, and when no context
 *  it can draw into is accepted it asks for none and runs the software
 *  renderer.
 *
 *  This checks that shape in the sources: libretro_set_hw_render and
 *  libretro_select_hw_render (libretro/main.cpp) and the GS context
 *  switches (pcsx2/GS/GS.cpp) name no RETRO_HW_CONTEXT_OPENGLES* type, and
 *  libretro_select_hw_render ends by asking for RETRO_HW_CONTEXT_NONE.
 *
 *  Usage: tests/hwctx/ctxaudit <libretro/main.cpp> <pcsx2/GS/GS.cpp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Body of the function whose definition starts with sig, as [*s, *e). */
static int body(const char* src, const char* sig, const char** s, const char** e)
{
	*s = strstr(src, sig);
	*e = *s ? strstr(*s, "\n}\n") : NULL;
	return *s && *e;
}

static int names_es(const char* s, const char* e)
{
	const char* p = strstr(s, "RETRO_HW_CONTEXT_OPENGLES");
	return p && p < e;
}

static int verdict(const char* what, int ok)
{
	printf("  %-48s %s\n", what, ok ? "ok" : "WRONG");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	static const char kNone[] = "return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);\n}\n";
	char* m;
	char* g;
	const char *s, *e;
	int bad = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <libretro/main.cpp> <pcsx2/GS/GS.cpp>\n", argv[0]);
		return 2;
	}
	m = read_file(argv[1]);
	g = read_file(argv[2]);
	if (!m || !g)
	{
		fprintf(stderr, "cannot read the inputs\n");
		free(m);
		free(g);
		return 2;
	}

	if (!body(m, "static bool libretro_set_hw_render(", &s, &e))
		bad += verdict("libretro_set_hw_render found", 0);
	else
		bad += verdict("libretro_set_hw_render accepts no ES type", !names_es(s, e));

	if (!body(m, "static bool libretro_select_hw_render(", &s, &e))
		bad += verdict("libretro_select_hw_render found", 0);
	else
	{
		bad += verdict("libretro_select_hw_render asks for no ES type", !names_es(s, e));
		/* e points at the "\n}\n" that closes it */
		bad += verdict("libretro_select_hw_render ends on CONTEXT_NONE",
			(size_t)(e + 3 - s) >= strlen(kNone) - 1 && !strncmp(e + 3 - (strlen(kNone) - 1), kNone + 1, strlen(kNone) - 1));
	}

	bad += verdict("GS.cpp maps no ES type to a device", !names_es(g, g + strlen(g)));

	free(m);
	free(g);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
