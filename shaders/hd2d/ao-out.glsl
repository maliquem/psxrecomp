// Ambient occlusion, composite. Written for psxrecomp.
// Upsamples the AO with depth-aware weights (each of the 4 nearest low-res
// texels counts only if its depth matches this pixel's) and darkens the 3D
// scenery only: sprites, HUD and menus (mask3d = 0) are left as they are, and
// so is scenery seen through something semi-transparent (mask3d blue: a
// see-through HUD panel, a light shaft).
// u_aoview.x = 1 shows the AO alone (white = open) for tuning.

uniform vec4 u_aoview;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    vec3 mk = texelFetch(u_m, p, 0).rgb;
    float m = mk.g * (1.0 - mk.b);
    float d = texelFetch(u_d, p, 0).r;
    if (d >= 0.99999 || (m < 0.5 && u_aoview.x < 0.5)) { o = vec4(src, 1.0); return; }
    float z = 256.0 / max(1.0 - d, 1e-7) - 256.0;

    vec2 ls = vec2(textureSize(u_ao, 0));
    vec2 f = v_uv * ls - 0.5;
    vec2 b = floor(f), fr = f - b;
    float acc = 0.0, wsum = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 t = texelFetch(u_ao, ivec2(clamp(b + vec2(i, j), vec2(0.0), ls - 1.0)), 0);
            float bw = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y);
            float k = (t.g > 0.0 ? exp(-abs(t.g - z) / (0.01 * z)) : 0.0) * bw;
            acc += t.r * k; wsum += k;
        }
    // No low-res texel on this surface (thin leaves, a 1-px seam): leave it open
    // rather than borrow the darkness of the surface behind it.
    float ao = mix(1.0, acc / max(wsum, 1e-6), smoothstep(0.02, 0.15, wsum));
    // Strong light washes the shade out: no dark creases inside a bright doorway
    // or a glow (they would also crawl over it as the camera moves).
    float lum = max(src.r, max(src.g, src.b));
    ao = mix(ao, 1.0, smoothstep(0.75, 0.95, lum));
    if (u_aoview.x > 0.5) { o = vec4(vec3(m > 0.5 ? ao : 0.5), 1.0); return; }
    o = vec4(src * mix(1.0, ao, m), 1.0);
}
