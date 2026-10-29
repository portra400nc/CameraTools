// The game's grade as the skipped tone map draw holds it, for the tonemap pass and the DNG's (dng_ps.hlsl): copied each
// frame from where its shader's code keeps it in its b0 (core/grade.h), or unbound when the shader is not the tone map the
// add-on knows or its constants could not be copied.
cbuffer GameGrade : register(b1)
{
    float4 game_color[3];   // rows: what red, green and blue each become
    float game_exposure;
    float game_bloom;       // the bloom texture's weight; stale when the shader adds no bloom, and no bloom is bound then
    float game_gamma;       // a power after the encode; 1 in every census capture
    float game_unused;
};

struct Grade
{
    float exposure;
    float bloom;        // the bloom texture's weight
    float3x3 color;     // rows: what red, green and blue each become
    float gamma;        // a power after the encode; 1 in every census capture
};

Grade game_grade()
{
    Grade g;
    g.exposure = game_exposure;
    g.bloom = game_bloom;
    g.color = float3x3(game_color[0].xyz, game_color[1].xyz, game_color[2].xyz);
    g.gamma = game_gamma;
    // Comparisons with NaN are false, so a NaN anywhere also falls back. The fallback is the census's 1920x1200 frame,
    // with its exposure of 0.985 rounded to 1.
    bool known = g.exposure > 0 && g.exposure < 100 && g.bloom >= 0 && g.bloom < 100 && g.gamma > 0.1 && g.gamma < 10
        && all(abs(g.color[0]) < 100) && all(abs(g.color[1]) < 100) && all(abs(g.color[2]) < 100);
    if (!known)
    {
        g.exposure = 1;
        g.bloom = 0.75;
        g.color = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
        g.gamma = 1;
    }
    return g;
}
