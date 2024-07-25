static const char present_glsl_shader_raw[] = 
"#ifdef VERTEX_SHADER\n"
"\n"
"layout(location = 0) in vec2 POSITION;\n"
"layout(location = 1) in vec2 TEXCOORD0;\n"
"layout(location = 7) in vec4 COLOR;\n"
"\n"
"// FIXME set the interpolation (don't know what dx do)\n"
"// flat means that there is no interpolation. The value given to the fragment shader is based on the provoking vertex conventions.\n"
"//\n"
"// noperspective means that there will be linear interpolation in window-space. This is usually not what you want, but it can have its uses.\n"
"//\n"
"// smooth, the default, means to do perspective-correct interpolation.\n"
"//\n"
"// The centroid qualifier only matters when multisampling. If this qualifier is not present, then the value is interpolated to the pixel's center, anywhere in the pixel, or to one of the pixel's samples. This sample may lie outside of the actual primitive being rendered, since a primitive can cover only part of a pixel's area. The centroid qualifier is used to prevent this; the interpolation point must fall within both the pixel's area and the primitive's area.\n"
"out vec4 PSin_p;\n"
"out vec2 PSin_t;\n"
"out vec4 PSin_c;\n"
"\n"
"void vs_main()\n"
"{\n"
"	PSin_p = vec4(POSITION, 0.5f, 1.0f);\n"
"	PSin_t = TEXCOORD0;\n"
"	PSin_c = COLOR;\n"
"	gl_Position = vec4(POSITION, 0.5f, 1.0f); // NOTE I don't know if it is possible to merge POSITION_OUT and gl_Position\n"
"}\n"
"\n"
"#endif\n"
"\n"
"#ifdef FRAGMENT_SHADER\n"
"\n"
"uniform vec4 u_source_rect;\n"
"uniform vec4 u_target_rect;\n"
"uniform vec2 u_source_size;\n"
"uniform vec2 u_target_size;\n"
"uniform vec2 u_target_resolution;\n"
"uniform vec2 u_rcp_target_resolution; // 1 / u_target_resolution\n"
"uniform vec2 u_source_resolution;\n"
"uniform vec2 u_rcp_source_resolution; // 1 / u_source_resolution\n"
"\n"
"in vec4 PSin_p;\n"
"in vec2 PSin_t;\n"
"in vec4 PSin_c;\n"
"\n"
"layout(binding = 0) uniform sampler2D TextureSampler;\n"
"\n"
"layout(location = 0) out vec4 SV_Target0;\n"
"\n"
"vec4 sample_c()\n"
"{\n"
"	return texture(TextureSampler, PSin_t);\n"
"}\n"
"\n"
"void ps_copy()\n"
"{\n"
"	SV_Target0 = sample_c();\n"
"}\n"
"\n"
"#endif\n"
;