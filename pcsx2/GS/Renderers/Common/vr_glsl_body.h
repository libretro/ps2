/*  PCSX2 - PS2 Emulator for PCs
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 */

#pragma once

/* The two VR passes (GSDevice::VRCompose), as GLSL shared by the Vulkan
 * and the OpenGL/GLES backends. Each backend's vr.glsl puts its own
 * interface in front: samp0, the atlas; v_tex, the position in the
 * rectangle drawn, (0, 0) at its top left as displayed; o_col0, the
 * output; and the constants (VRRangeConstants, VREyeConstants).
 *
 * The atlas, W wide and 2H + 1 high: the picture in rows [0, H); under
 * each of its pixels in rows [H, 2H) the bytes of the GS's Z there, low
 * byte in red; and in texel (0, 2H) the frame's range of Z, as two
 * 16-bit fractions of the Z format's range: the least in red and green,
 * the greatest in blue and alpha. A range
 * whose min is not below its max is no range, which is what a texel of
 * zeros is.
 *
 * The eye: the picture is a window, screen.x by screen.y, centred on
 * -z at screen.z. Behind it each pixel has a depth, the inverse of its
 * distance in units of the window's: 1 on the window, larger in front,
 * 0 at infinity. The frame's farthest Z is depth.y and its nearest
 * depth.x, and Z between them is depth between them. A PS2 game's Z is
 * mostly its 1/w, scaled and offset to suit the buffer, so a Z that is
 * linear in inverse distance is right for most of them. The relief this
 * makes is seen from the eye: the ray through each pixel is marched
 * from the nearest depth to the farthest, in equal steps of inverse
 * distance, which are equal steps across the window, until it is behind
 * the relief; a few halvings find where it crossed. */
#define VR_GLSL_BODY \
"vec4 vr_bytes(vec4 c)\n" \
"{\n" \
"	return floor(c * 255.0 + 0.5);\n" \
"}\n" \
"\n" \
"float vr_z(vec4 c, float scale)\n" \
"{\n" \
"	return dot(vr_bytes(c), vec4(1.0, 256.0, 65536.0, 16777216.0)) * scale;\n" \
"}\n" \
"\n" \
"vec2 vr_range_decode(vec4 c)\n" \
"{\n" \
"	vec4 b = vr_bytes(c);\n" \
"	return vec2(b.x + b.y * 256.0, b.z + b.w * 256.0) / 65535.0;\n" \
"}\n" \
"\n" \
"vec4 vr_range_encode(vec2 r)\n" \
"{\n" \
"	uvec2 v = uvec2(clamp(r, 0.0, 1.0) * 65535.0 + 0.5);\n" \
"	return vec4(uvec4(v.x & 255u, v.x >> 8, v.y & 255u, v.y >> 8)) / 255.0;\n" \
"}\n" \
"\n" \
"#ifdef ps_vr_range\n" \
"// One output texel: the range of the Params.x square block of SrcRect\n" \
"// under it, from Z bytes or from ranges. Z 0 is a cleared buffer and no\n" \
"// part of the range.\n" \
"void ps_vr_range()\n" \
"{\n" \
"	ivec2 base = ivec2(gl_FragCoord.xy) * Params.x;\n" \
"	vec2 r = vec2(1.0, 0.0);\n" \
"	for (int y = 0; y < Params.x; y++)\n" \
"	{\n" \
"		for (int x = 0; x < Params.x; x++)\n" \
"		{\n" \
"			ivec2 p = base + ivec2(x, y);\n" \
"			if (p.x >= SrcRect.z || p.y >= SrcRect.w)\n" \
"				continue;\n" \
"			vec4 c = texelFetch(samp0, SrcRect.xy + p, 0);\n" \
"			if ((Params.y & 1) != 0)\n" \
"			{\n" \
"				float z = vr_z(c, FParams.x);\n" \
"				if (z > 0.0)\n" \
"					r = vec2(min(r.x, z), max(r.y, z));\n" \
"			}\n" \
"			else\n" \
"			{\n" \
"				vec2 s = vr_range_decode(c);\n" \
"				if (s.x <= s.y)\n" \
"					r = vec2(min(r.x, s.x), max(r.y, s.y));\n" \
"			}\n" \
"		}\n" \
"	}\n" \
"\n" \
"	// Eased towards from last frame's, kept where this frame has none.\n" \
"	if ((Params.y & 2) != 0)\n" \
"	{\n" \
"		vec2 prev = vr_range_decode(texelFetch(samp0, Params.zw, 0));\n" \
"		if (prev.x < prev.y)\n" \
"			r = (r.x <= r.y) ? mix(prev, r, FParams.y) : prev;\n" \
"	}\n" \
"\n" \
"	o_col0 = vr_range_encode(r);\n" \
"}\n" \
"#endif\n" \
"\n" \
"#ifdef ps_vr_eye\n" \
"// What every step needs: the picture's size, the frame's range of Z, and\n" \
"// the depths of its nearest and farthest.\n" \
"struct VRRelief\n" \
"{\n" \
"	ivec2 size;\n" \
"	vec2 range;\n" \
"	float near_w;\n" \
"	float far_w;\n" \
"};\n" \
"\n" \
"vec3 vr_rotate(vec4 q, vec3 v)\n" \
"{\n" \
"	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);\n" \
"}\n" \
"\n" \
"// Where the ray from e along d is at depth w, as the place on the window\n" \
"// it is seen through from the centre. False behind the eye.\n" \
"bool vr_point(vec3 e, vec3 d, float w, out vec2 uv)\n" \
"{\n" \
"	float t = (Screen.z / w + e.z) / -d.z;\n" \
"	vec3 x = e + t * d;\n" \
"	uv = vec2(0.5 + x.x * w / Screen.x, 0.5 - x.y * w / Screen.y);\n" \
"	return t > 0.0;\n" \
"}\n" \
"\n" \
"bool vr_inside(vec2 uv)\n" \
"{\n" \
"	return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));\n" \
"}\n" \
"\n" \
"// The depth of the relief at uv.\n" \
"float vr_surface(VRRelief r, vec2 uv)\n" \
"{\n" \
"	ivec2 p = clamp(ivec2(uv * vec2(r.size)), ivec2(0), r.size - 1);\n" \
"	float z = vr_z(texelFetch(samp0, ivec2(p.x, r.size.y + p.y), 0), DepthMap.z);\n" \
"	float n = (z > 0.0) ? clamp((z - r.range.x) / (r.range.y - r.range.x), 0.0, 1.0) : 0.0;\n" \
"	return mix(r.far_w, r.near_w, n);\n" \
"}\n" \
"\n" \
"bool vr_behind(VRRelief r, vec3 e, vec3 d, float w, out vec2 uv)\n" \
"{\n" \
"	return vr_point(e, d, w, uv) && vr_inside(uv) && w <= vr_surface(r, uv);\n" \
"}\n" \
"\n" \
"void ps_vr_eye()\n" \
"{\n" \
"	ivec2 atlas = textureSize(samp0, 0);\n" \
"	VRRelief r;\n" \
"	r.size = ivec2(atlas.x, (atlas.y - 1) / 2);\n" \
"	r.range = vr_range_decode(texelFetch(samp0, ivec2(0, 2 * r.size.y), 0));\n" \
"	bool flat_window = !(r.range.x < r.range.y);\n" \
"	r.near_w = flat_window ? 1.0 : DepthMap.x;\n" \
"	r.far_w = flat_window ? 1.0 : DepthMap.y;\n" \
"\n" \
"	vec3 e = EyePos.xyz;\n" \
"	vec3 d = vr_rotate(EyeQuat, vec3(mix(-FovTan.x, FovTan.y, v_tex.x), mix(FovTan.z, -FovTan.w, v_tex.y), -1.0));\n" \
"\n" \
"	o_col0 = vec4(0.0, 0.0, 0.0, 1.0);\n" \
"	if (d.z > -1e-6)\n" \
"		return;\n" \
"\n" \
"	// From just in front of the nearest depth to the farthest, in about\n" \
"	// two pixels of the picture a step.\n" \
"	float w_hi = flat_window ? 1.0 : r.near_w * 1.001 + 0.001;\n" \
"	float w_lo = r.far_w;\n" \
"	vec2 uv_hi, uv_lo;\n" \
"	vr_point(e, d, w_hi, uv_hi);\n" \
"	vr_point(e, d, w_lo, uv_lo);\n" \
"	float span = length((uv_lo - uv_hi) * vec2(r.size));\n" \
"	int steps = clamp(int(span * 0.5) + 1, 1, int(Screen.w));\n" \
"\n" \
"	vec2 uv = vec2(0.0);\n" \
"	float prev = w_hi;\n" \
"	bool hit = false;\n" \
"	for (int i = 0; i <= steps; i++)\n" \
"	{\n" \
"		float w = mix(w_hi, w_lo, float(i) / float(steps));\n" \
"		if (vr_behind(r, e, d, w, uv))\n" \
"		{\n" \
"			// Between the last step in front and this one behind.\n" \
"			float lo = prev;\n" \
"			float hi = w;\n" \
"			for (int k = 0; k < 6; k++)\n" \
"			{\n" \
"				float m = 0.5 * (lo + hi);\n" \
"				vec2 q;\n" \
"				if (vr_behind(r, e, d, m, q))\n" \
"				{\n" \
"					hi = m;\n" \
"					uv = q;\n" \
"				}\n" \
"				else\n" \
"				{\n" \
"					lo = m;\n" \
"				}\n" \
"			}\n" \
"			hit = true;\n" \
"			break;\n" \
"		}\n" \
"		prev = w;\n" \
"	}\n" \
"	if (!hit)\n" \
"		return;\n" \
"\n" \
"	// The picture's rows only: filtering is not to reach the depth.\n" \
"	float y = clamp(uv.y * float(r.size.y), 0.5, float(r.size.y) - 0.5);\n" \
"	o_col0 = vec4(texture(samp0, vec2(uv.x, y / float(atlas.y))).rgb, 1.0);\n" \
"}\n" \
"#endif\n"
