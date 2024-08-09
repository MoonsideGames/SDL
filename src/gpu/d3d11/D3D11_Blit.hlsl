struct VertexToPixel
{
    float2 tex : TEXCOORD0;
};

cbuffer BlitParams
{
    float2 textureSize;
    float2 regionOrigin;
    float2 regionSize;
    uint mipLevel;
    uint layer;
};

sampler SourceSampler : register(s0);
#if ARRAY
Texture2DArray Source : register(t0);
#elif CUBE
TextureCube Source : register(t0);
#else
Texture2D Source : register(t0);
#endif

float4 main(VertexToPixel input) : SV_Target0
{
#if ARRAY || CUBE
    float3 newCoord;
    newCoord.z = layer;
#else
    float2 newCoord;
#endif
    newCoord.xy = (regionOrigin + (input.tex * regionSize)) / textureSize;
    return Source.SampleLevel(SourceSampler, newCoord, mipLevel);
}