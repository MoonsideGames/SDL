#if D3D12
#define REG(reg, space) register(reg, space)
#else
#define REG(reg, space) register(reg)
#endif

struct VertexToPixel
{
    float2 tex : TEXCOORD0;
    float4 pos : SV_POSITION;
};

cbuffer SourceRegionBuffer : REG(b0, space3)
{
    float2 UVLeftTop;
    float2 UVDimensions;
    uint MipLevel;
    uint Layer;
};

#if ARRAY
Texture2DArray SourceTexture : REG(t0, space2);
#elif THREED
Texture3D SourceTexture : REG(t0, space2);
#elif CUBE
TextureCube SourceTexture : REG(t0, space2);
#else
Texture2D SourceTexture : REG(t0, space2);
#endif
sampler SourceSampler : REG(s0, space2);

VertexToPixel FullscreenVert(uint vI : SV_VERTEXID)
{
    float2 inTex = float2((vI << 1) & 2, vI & 2);
    VertexToPixel Out = (VertexToPixel)0;
    Out.tex = inTex;
    Out.pos = float4(inTex * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return Out;
}

float4 Blit(VertexToPixel input) : SV_Target0
{
#if ARRAY || CUBE || THREED
    float3 newCoord;
#else
    float2 newCoord;
#endif

#if CUBE
    // Thanks, Wikipedia! https://en.wikipedia.org/wiki/Cube_mapping
    float2 scaledUV = UVLeftTop + UVDimensions * input.tex;
    float u = 2.0 * scaledUV.x - 1.0;
    float v = 2.0 * scaledUV.y - 1.0;
    switch (Layer) {
        case 0: newCoord = float3(1.0, -v, -u); break; // POSITIVE X
        case 1: newCoord = float3(-1.0, -v, u); break; // NEGATIVE X
        case 2: newCoord = float3(u, -1.0, -v); break; // POSITIVE Y
        case 3: newCoord = float3(u, 1.0, v); break; // NEGATIVE Y
        case 4: newCoord = float3(u, -v, 1.0); break; // POSITIVE Z
        case 5: newCoord = float3(-u, -v, -1.0); break; // NEGATIVE Z
        default: newCoord = float3(0, 0, 0); break; // silences warning
    }
#else
    newCoord.xy = UVLeftTop + UVDimensions * input.tex;
    #if ARRAY || THREED
    newCoord.z = Layer;
    #endif
#endif

    return SourceTexture.SampleLevel(SourceSampler, newCoord, MipLevel);
}