//TEST:COMPARE_HLSL: -profile cs_5_0 -target dxbc-assembly -no-checking

// Make sure we handle complex UAV write patterns

// Force import of Slang to ensure that some
// checking takes place:
#ifdef __SLANG__
__import empty;
#endif

struct Bar
{
	uint bar;
};

RWStructuredBuffer<Bar> gUAV : register(u0);

void foo(RWTexture1D<float2> uav)
{
	uint index = gUAV.IncrementCounter();
	gUAV[index].bar = 1;
	uav[index] = float2(0,0);
}

RWTexture1D<float2> gUAV2 : register(u1);

[numthreads(1,1,1)]
void main()
{
	foo(gUAV2);
}
