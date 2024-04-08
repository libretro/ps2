//#version 420 // Keep it for editor detection


#ifdef VERTEX_SHADER

layout(location = 0) in vec2 POSITION;
layout(location = 1) in vec2 TEXCOORD0;
layout(location = 7) in vec4 COLOR;

// FIXME set the interpolation (don't know what dx do)
// flat means that there is no interpolation. The value given to the fragment shader is based on the provoking vertex conventions.
//
// noperspective means that there will be linear interpolation in window-space. This is usually not what you want, but it can have its uses.
//
// smooth, the default, means to do perspective-correct interpolation.
//
// The centroid qualifier only matters when multisampling. If this qualifier is not present, then the value is interpolated to the pixel's center, anywhere in the pixel, or to one of the pixel's samples. This sample may lie outside of the actual primitive being rendered, since a primitive can cover only part of a pixel's area. The centroid qualifier is used to prevent this; the interpolation point must fall within both the pixel's area and the primitive's area.
out vec4 PSin_p;
out vec2 PSin_t;
out vec4 PSin_c;

void vs_main()
{
	PSin_p = vec4(POSITION, 0.5f, 1.0f);
	PSin_t = TEXCOORD0;
	PSin_c = COLOR;
	gl_Position = vec4(POSITION, 0.5f, 1.0f); // NOTE I don't know if it is possible to merge POSITION_OUT and gl_Position
}

#endif

#ifdef FRAGMENT_SHADER

uniform vec4 u_source_rect;
uniform vec4 u_target_rect;
uniform vec2 u_source_size;
uniform vec2 u_target_size;
uniform vec2 u_target_resolution;
uniform vec2 u_rcp_target_resolution; // 1 / u_target_resolution
uniform vec2 u_source_resolution;
uniform vec2 u_rcp_source_resolution; // 1 / u_source_resolution
uniform vec3 cb0_pad0;

in vec4 PSin_p;
in vec2 PSin_t;
in vec4 PSin_c;

layout(binding = 0) uniform sampler2D TextureSampler;

layout(location = 0) out vec4 SV_Target0;

vec4 sample_c()
{
	return texture(TextureSampler, PSin_t);
}

void ps_copy()
{
	SV_Target0 = sample_c();
}

#endif
