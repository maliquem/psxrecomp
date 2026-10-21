// Directional light, "which way is up".
// Written for psxrecomp. The game's cameras tilt differently per scene, so
// the world's up direction is measured in the picture: the mean normal of the
// floor-like scenery (normals rebuilt from depth that point up-ish in view).
// u_upstage.x = 0: per block (an 8x8 sample grid of its footprint), output
// rgb = sum of n x w, a = sum of w; 1: all blocks of the previous pass summed,
// output rgb = up (unit), a = 1 (0 = nothing found: a default tilt is used);
// every texel holds the same answer (the framework's smallest pass is 16 lines).

uniform vec4 u_upstage;
uniform vec4 u_proj;

vec2 g_nat;

float sz_at(vec2 uv)
{
    float d = texture(u_d, uv).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
}

vec3 pos_at(vec2 uv, float z) { return vec3((uv - 0.5) * g_nat * z / u_proj.x, z); }

void main()
{
    if (u_upstage.x > 0.5) {
        ivec2 n = textureSize(u_n, 0);
        vec4 s = vec4(0.0);
        for (int y = 0; y < n.y; y++)
            for (int x = 0; x < n.x; x++) s += texelFetch(u_n, ivec2(x, y), 0);
        o = s.a > 1e-3 ? vec4(normalize(s.rgb), 1.0) : vec4(0.0, 0.766, -0.643, 0.0);
        return;
    }
    vec2 ws = vec2(textureSize(u_d, 0));
    g_nat = vec2(u_proj.y * ws.x / ws.y, u_proj.y);
    vec2 px = 3.0 / g_nat;
    vec4 s = vec4(0.0);
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            vec2 uv = v_uv + (vec2(i, j) - 3.5) / 8.0 * u_rcp;
            float z = sz_at(uv);
            if (z <= 0.0) continue;
            float zr = sz_at(uv + vec2(px.x, 0.0)), zu = sz_at(uv + vec2(0.0, px.y));
            if (zr <= 0.0 || zu <= 0.0 || abs(zr - z) > 0.05 * z || abs(zu - z) > 0.05 * z) continue;
            vec3 P = pos_at(uv, z);
            vec3 nn = normalize(cross(pos_at(uv + vec2(px.x, 0.0), zr) - P, pos_at(uv + vec2(0.0, px.y), zu) - P));
            if (nn.z > 0.0) nn = -nn;
            float w = nn.y > 0.4 ? nn.y * nn.y : 0.0;   // floors, not walls
            s += vec4(nn * w, w);
        }
    o = s;
}
