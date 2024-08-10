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
#else
    float2 newCoord;
#endif

#if CUBE
    // Thanks, Wikipedia! https://en.wikipedia.org/wiki/Cube_mapping
    float u = 2.0 * input.tex.x - 1.0;
    float v = 2.0 * input.tex.y - 1.0;
    switch (layer) {
        case 0: newCoord = float3(1.0,  -v,   -u); break; // POSITIVE X
        case 1: newCoord = float3(-1.0, -v,    u); break; // NEGATIVE X
        case 2: newCoord = float3(u,  -1.0,   -v); break; // POSITIVE Y
        case 3: newCoord = float3(u,   1.0,    v); break; // NEGATIVE Y
        case 4: newCoord = float3(u,    -v,  1.0); break; // POSITIVE Z
        case 5: newCoord = float3(-u,   -v, -1.0); break; // NEGATIVE Z
        default: newCoord = float3(0, 0, 0); break; // silences warning
    }
#else
    newCoord.xy = (regionOrigin + (input.tex * regionSize)) / textureSize;
    #if ARRAY
    newCoord.z = layer;
    #endif
#endif

    return Source.SampleLevel(SourceSampler, newCoord, mipLevel);
}