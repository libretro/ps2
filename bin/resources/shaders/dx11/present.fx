#ifdef SHADER_MODEL // make safe to include in resource file to enforce dependency

#ifndef PS_SCALE_FACTOR
#define PS_SCALE_FACTOR 1
#endif

struct VS_INPUT
{
	float4 p : POSITION;
	float2 t : TEXCOORD0;
	float4 c : COLOR;
};

struct VS_OUTPUT
{
	float4 p : SV_Position;
	float2 t : TEXCOORD0;
	float4 c : COLOR;
};

cbuffer cb0 : register(b0)
{
	float4 u_source_rect;
	float4 u_target_rect;
	float2 u_source_size;
	float2 u_target_size;
	float2 u_target_resolution;
	float2 u_rcp_target_resolution; // 1 / u_target_resolution
	float2 u_source_resolution;
	float2 u_rcp_source_resolution; // 1 / u_source_resolution
	float u_time;
	float3 cb0_pad0;
};

Texture2D Texture;
SamplerState TextureSampler;

float4 sample_c(float2 uv)
{
	return Texture.Sample(TextureSampler, uv);
}

struct PS_INPUT
{
	float4 p : SV_Position;
	float2 t : TEXCOORD0;
	float4 c : COLOR;
};

struct PS_OUTPUT
{
	float4 c : SV_Target0;
};

VS_OUTPUT vs_main(VS_INPUT input)
{
	VS_OUTPUT output;

	output.p = input.p;
	output.t = input.t;
	output.c = input.c;

	return output;
}

PS_OUTPUT ps_copy(PS_INPUT input)
{
	PS_OUTPUT output;

	output.c = sample_c(input.t);

	return output;
}

#endif
