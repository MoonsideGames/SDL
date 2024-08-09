struct VertexToPixel
{
    float2 tex : TEXCOORD0;
    float4 pos : SV_POSITION;
};

VertexToPixel main(uint vI : SV_VERTEXID)
{
    float2 inTex = float2((vI << 1) & 2, vI & 2);
    VertexToPixel Out = (VertexToPixel)0;
    Out.tex = inTex;
    Out.pos = float4(inTex * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return Out;
}