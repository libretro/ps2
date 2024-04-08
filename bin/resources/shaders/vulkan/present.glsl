#ifdef VERTEX_SHADER

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_tex;

layout(location = 0) out vec2 v_tex;

void main()
{
	gl_Position = vec4(a_pos.x, -a_pos.y, a_pos.z, a_pos.w);
	v_tex = a_tex;
}

#endif

#ifdef FRAGMENT_SHADER

layout(push_constant) uniform cb10
{
	vec4 u_source_rect;
	vec4 u_target_rect;
	vec2 u_source_size;
	vec2 u_target_size;
	vec2 u_target_resolution;
	vec2 u_rcp_target_resolution; // 1 / u_target_resolution
	vec2 u_source_resolution;
	vec2 u_rcp_source_resolution; // 1 / u_source_resolution
	float u_time;
	vec3 cb0_pad0;
};

layout(location = 0) in vec2 v_tex;

layout(location = 0) out vec4 o_col0;

layout(set = 0, binding = 0) uniform sampler2D samp0;

vec4 sample_c(vec2 uv)
{
	return texture(samp0, uv);
}

void ps_copy()
{
	o_col0 = sample_c(v_tex);
}

#endif
