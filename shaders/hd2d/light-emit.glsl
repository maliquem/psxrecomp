// Light from the game's own sources, pass 1: the emitters.
// Written for psxrecomp. The framework's `mask` red marks what may glow
// (additive 3D pieces: magic, fire, lamps). Each low-res texel averages the
// colour of the glowing pixels in its footprint and where they sit in depth
// (the depth there is the scenery right behind the effect).
// Output (rgba16f): rgb = mean emitted colour, a = SZ x luminance of the
// emitters with a depth (premultiplied, so the blur passes keep it right).

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

void main()
{
    vec2 ws = vec2(textureSize(u_m, 0));
    vec3 e = vec3(0.0);
    float zl = 0.0, l = 0.0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            vec2 uv = v_uv + (vec2(i, j) - 1.5) * 0.25 * u_rcp;
            ivec2 q = ivec2(clamp(uv * ws, vec2(0.0), ws - 1.0));
            if (texelFetch(u_m, q, 0).r < 0.5) continue;
            vec3 c = texture(u_src, uv).rgb;
            e += c;
            float d = texelFetch(u_d, q, 0).r;
            if (d < 0.99999) {
                float y = dot(c, LUMA);
                zl += (256.0 / max(1.0 - d, 1e-7) - 256.0) * y;
                l += y;
            }
        }
    e *= 1.0 / 16.0;
    float sz = l > 0.0 ? zl / l : 0.0;
    o = vec4(e, sz * dot(e, LUMA));
}
