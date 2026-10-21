// Ambient occlusion, edge-aware blur. Written for psxrecomp.
// A 9-tap Gaussian along u_dir that only mixes samples lying on the same
// surface: each sample's depth is compared with the depth this pixel's slope
// predicts there (the flatter one-sided slope, so a silhouette never sets it).
// Surfaces seen at a grazing angle still blur (no interleave stripes) and AO
// never bleeds across a silhouette (no dark halos on depth edges).
// Input/output: r = AO, g = SZ (0 = none).

uniform vec4 u_dir;

void main()
{
    const float w[5] = float[5](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);
    vec2 px = u_dir.xy / u_size;
    vec4 c = texture(u_src, v_uv);
    if (c.g <= 0.0) { o = c; return; }
    float zp = texture(u_src, v_uv + px).g, zm = texture(u_src, v_uv - px).g;
    float gp = zp > 0.0 ? zp - c.g : 1e9, gm = zm > 0.0 ? c.g - zm : 1e9;
    float slope = abs(gp) < abs(gm) ? gp : gm;
    if (abs(slope) > 0.02 * c.g) slope = 0.0;      // both sides are edges: a lone pixel
    float acc = c.r * w[0], wsum = w[0];
    for (int i = 1; i <= 4; i++) {
        for (int s = -1; s <= 1; s += 2) {
            vec4 t = texture(u_src, v_uv + px * float(i * s));
            if (t.g <= 0.0) continue;
            float want = c.g + slope * float(i * s);
            float k = w[i] * exp(-abs(t.g - want) / (0.003 * c.g));
            acc += t.r * k; wsum += k;
        }
    }
    o = vec4(acc / wsum, c.g, 0.0, 1.0);
}
