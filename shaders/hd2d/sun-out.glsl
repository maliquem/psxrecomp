// Directional light (sun/moon) with cast shadows, composite (BoF4 HD-2D look,
// Phase 3D prototype). Written for psxrecomp. Relights the 3D scenery:
// the colour is scaled between an ambient (faces turned away, shadows) and the
// sun (faces toward it, out of shadow), each with its own tint, so the scene
// gets contrast without the painted texture being lost. Sprites standing in
// the scene (u_spr, the framework's `sprite` input) keep their colour in the
// sun and take, in shadow, the same darkening the ground takes (ambient over
// sun). HUD, menus and scenery behind something see-through are left as
// they are.
// u_suncol: rgb = sun tint x level,  u_amb: rgb = ambient tint x level,
// u_sunview.x = 1 shows the lit term alone; u_ss.y 0: the sprites' shadows
// only (no sun on the scenery): the colour is kept, and darkened where a
// sprite's shadow falls as a shadow darkens it under the sun (ambient over sun).

uniform vec4 u_suncol;
uniform vec4 u_amb;
uniform vec4 u_sunview;
uniform vec4 u_ss;
uniform vec4 u_worldup, u_shadowdir;   // the framework's (pfx_shadow_uniforms): world up, the light
uniform vec4 u_proj;                   // y = native height (the view rays)
uniform vec4 u_horizon;                // the framework's: world up (view space, y up), w the camera's H (0 = none)

// depth-aware upsample of the lit term (as in ao-out.glsl)
float lit_at(float z)
{
    vec2 ls = vec2(textureSize(u_l, 0));
    vec2 f = v_uv * ls - 0.5;
    vec2 b = floor(f), fr = f - b;
    float acc = 0.0, wsum = 0.0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 t = texelFetch(u_l, ivec2(clamp(b + vec2(i, j), vec2(0.0), ls - 1.0)), 0);
            float bw = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y);
            float k = (t.g > 0.0 ? exp(-abs(t.g - z) / (0.01 * z)) : 0.0) * bw;
            acc += t.r * k; wsum += k;
        }
    return wsum > 1e-4 ? acc / wsum : 1.0;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    vec3 mk = texelFetch(u_m, p, 0).rgb;
    float m = mk.g * (1.0 - mk.b);
    float zs = texelFetch(u_spr, p, 0).r;
    if (mk.g < 0.5 && zs > 0.0 && zs < 50000.0) {   // a sprite standing in the scene
        float ls = lit_at(zs);
        if (u_sunview.x > 0.5) { o = vec4(vec3(0.25 + 0.5 * ls), 1.0); return; }
        o = vec4(src * mix(u_amb.rgb / max(u_suncol.rgb, vec3(1e-3)), vec3(1.0), ls), 1.0);
        return;
    }
    float d = texelFetch(u_d, p, 0).r;
    // the 3D mark with no depth (none, or the game's depth clear: 0.9999): a
    // painted backdrop (the framework marks what a frame draws before its 3D
    // world), lit as level ground in the sun, so the painted ground round a
    // cutscene's 3D floor matches it
    if (d >= 0.99985 && m > 0.5 && u_ss.y > 0.5 && u_shadowdir.w > 0.5 && u_horizon.w > 0.0) {
        // only its ground: below the horizon (the view ray points down, view
        // space y up; the camera's own H), fading in over the first few
        // pixels; the sky and the mountains stay
        vec3 up = normalize(u_horizon.xyz);
        vec2 nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
        vec3 ray = normalize(vec3((v_uv - 0.5) * nat / u_horizon.w, 1.0));
        float gnd = smoothstep(0.03, 0.06, -dot(ray, up));   // past the far dunes painted at the horizon
        float lg = clamp(dot(up, normalize(u_shadowdir.xyz)), 0.0, 1.0);
        o = vec4(src * mix(vec3(1.0), mix(u_amb.rgb, u_suncol.rgb, lg), gnd), 1.0); return;
    }
    if (d >= 0.99999 || (m < 0.5 && u_sunview.x < 0.5)) { o = vec4(src, 1.0); return; }
    float z = 256.0 / max(1.0 - d, 1e-7) - 256.0;
    float lit = lit_at(z);
    if (u_sunview.x > 0.5) { o = vec4(vec3(m > 0.5 ? lit : 0.5), 1.0); return; }
    vec3 shade = u_ss.y > 0.5 ? mix(u_amb.rgb, u_suncol.rgb, lit)
                              : mix(u_amb.rgb / max(u_suncol.rgb, vec3(1e-3)), vec3(1.0), lit);
    o = vec4(src * mix(vec3(1.0), shade, m), 1.0);
}
