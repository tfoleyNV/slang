//TEST:COMPARE_HLSL: -profile vs_5_0 -target dxbc-assembly -no-checking

// Allow (but ignore) initializer on `cbuffer` member

cbuffer C : register(b0)
{
	int a = -1;
};

float4 main() : SV_Position
{
	return a;
}
