// Verdant FX — nitidez adaptativa com foco em vegetação (v0.1.0)
//
// Porta fiel de tests/prototype_sharpen.py. ESTE ARQUIVO NÃO FOI COMPILADO
// NEM EXECUTADO: o ambiente de desenvolvimento não tem compilador HLSL nem
// Windows. A lógica foi validada apenas no protótipo Python.
//
// Limitações conhecidas (documentadas em docs/ARQUITETURA.md):
//  - Roda no Present, sobre a imagem final. Não há máscara real de vegetação:
//    "vegetação" aqui é uma heurística de cor (dominância do canal verde).
//  - HUD, painel e interior do caminhão também passam pelo filtro.
//  - Não recupera detalhe que o jogo já perdeu (LOD, textura, TAA).

cbuffer Params : register(b0)
{
    float4 P0; // x = intensidade, y = supressão de halo, z = preservação de bordas, w = controle de ruído
    float4 P1; // x = foco em vegetação, y = view sRGB (0/1), z = taps (5 ou 9), w = reservado
};

Texture2D<float4> Src : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 ToWork(float3 c)
{
    return (P1.y > 0.5) ? pow(max(c, 0.0), 1.0 / 2.2) : c;
}

float3 FromWork(float3 c)
{
    return (P1.y > 0.5) ? pow(max(c, 0.0), 2.2) : c;
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 Fetch(int2 p, int2 mx)
{
    return ToWork(Src.Load(int3(clamp(p, int2(0, 0), mx), 0)).rgb);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    int2 p = int2(pos.xy);
    uint w, h;
    Src.GetDimensions(w, h);
    int2 mx = int2(int(w) - 1, int(h) - 1);

    float4 raw = Src.Load(int3(clamp(p, int2(0, 0), mx), 0));
    float3 c = ToWork(raw.rgb);

    float lc = Luma(c);
    float ln = Luma(Fetch(p + int2(0, -1), mx));
    float ls = Luma(Fetch(p + int2(0, 1), mx));
    float lw = Luma(Fetch(p + int2(-1, 0), mx));
    float le = Luma(Fetch(p + int2(1, 0), mx));

    float blur, mn, mxl;
    if (P1.z > 6.5)
    {
        float lnw = Luma(Fetch(p + int2(-1, -1), mx));
        float lne = Luma(Fetch(p + int2(1, -1), mx));
        float lsw = Luma(Fetch(p + int2(-1, 1), mx));
        float lse = Luma(Fetch(p + int2(1, 1), mx));
        blur = (4.0 * lc + 2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 16.0;
        mn = min(min(min(lc, ln), min(ls, lw)), min(min(le, lnw), min(lne, min(lsw, lse))));
        mxl = max(max(max(lc, ln), max(ls, lw)), max(max(le, lnw), max(lne, max(lsw, lse))));
    }
    else
    {
        blur = (2.0 * lc + ln + ls + lw + le) / 6.0;
        mn = min(min(min(lc, ln), min(ls, lw)), le);
        mxl = max(max(max(lc, ln), max(ls, lw)), le);
    }

    float rng = mxl - mn;
    float detail = lc - blur;

    // controle de ruído (coring suave)
    float nf = P0.w * 0.006;
    detail = sign(detail) * max(abs(detail) - nf, 0.0);

    // preservação de bordas fortes
    float edgeAtt = 1.0 - P0.z * smoothstep(0.10, 0.35, rng);

    // proteção de sombras e realces
    float tone = smoothstep(0.02, 0.10, lc) * (1.0 - smoothstep(0.90, 0.99, lc));

    // heurística de vegetação: dominância do verde
    float gdom = c.g - max(c.r, c.b);
    float veg = smoothstep(0.01, 0.08, gdom);
    float weight = (1.0 - P1.x) + P1.x * veg;

    float amount = P0.x * 1.5;
    float delta = detail * amount * weight * edgeAtt * tone;

    // supressão de halo: limita o overshoot ao intervalo local
    float slack = (1.0 - P0.y) * rng * 0.5;
    float target = clamp(lc + delta, mn - slack, mxl + slack);
    delta = target - lc;

    float3 outc = saturate(c + delta.xxx);
    return float4(FromWork(outc), raw.a);
}
