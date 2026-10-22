// Directional light (sun/moon) with cast shadows, estimate pass (BoF4 HD-2D
// look, Phase 3D prototype). Written for psxrecomp; the methods are the
// textbook ones (Lambert term from a normal rebuilt from depth; a shadow map
// of the scenery seen from the light, built by the framework from the
// frame's 3D triangles: shadows stay put in the world, whatever the camera
// sees), not anyone's code.
//
// Positions are on the PS1 camera's terms, as in ao.glsl: X = (x - cx) Z / H,
// Y up (v_uv runs bottom-up), Z away from the camera.
// u_sun:    x = azimuth (deg, around the floor's up: 0 = from the right, 90 =
//           from behind the scene, -90 = from the camera's side), y = elevation
//           (deg above the floor), zw = 0
// u_up:     the world's up in view space, measured by sun-up.glsl (fallback)
// u_world:  xyz = world east (+X) in view space, w = 1 when set: from the GTE
//           camera matrix (plugin, every frame), so the sun stays put in the
//           world when the camera turns; u_worldup: xyz = world up, w = 1
//           when set
// u_shadowdir: xyz = direction to the light in view space (the plugin, from
//           the camera), w = 1 when set; else it comes from u_sun here
// u_shadow: x = unused, y = bias (fraction of Z), z = how dark a full shadow
//           is (0..1), w = 0
// u_smx..u_sms: the framework's shadow map mapping (gl_postfx.inc)
// u_proj:   x = H, y = native height (240), z = internal scale (depth grid)
// u_spr:    the sprites standing in the scene (framework `sprite` input): view
//           depth of the sprite drawn at each pixel, >= 50000 = none; with
//           u_m (mask3d) green 0 the pixel shows that sprite, which only takes
//           shadow (a flat picture has no facing)
// Output (rgba16f): r = lit (0 = facing away or in shadow, 1 = full sun),
// g = SZ (0 = none), for the edge-aware blur passes (ao-blur.glsl).

uniform vec4 u_sun;
uniform vec4 u_world;
uniform vec4 u_worldup;
uniform vec4 u_shadow;
uniform vec4 u_shadowdir;
uniform vec4 u_smx, u_smy, u_smz, u_sms;
uniform vec4 u_proj;

vec2 g_grid;
vec2 g_nat;

vec2 snap(vec2 uv) { return (floor(uv * g_grid) + 0.5) / g_grid; }

float sz_at(vec2 uv)
{
    float d = texture(u_d, snap(uv)).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
}

vec3 pos_at(vec2 uv, float z) { return vec3((uv - 0.5) * g_nat * z / u_proj.x, z); }
vec2 uv_of(vec3 p) { return p.xy * u_proj.x / (p.z * g_nat) + 0.5; }

// Shadow at P: is something nearer the light in the shadow map? 3x3 samples
// (soft edge). 1 = lit, down to 1 - u_shadow.z.
float shadow_at(vec3 P, float bias)
{
    if (u_sms.w <= 0.0 || u_sms.z <= 0.0) return 1.0;
    vec2 suv = vec2((dot(P, u_smx.xyz) - u_smx.w) * u_sms.x, (dot(P, u_smy.xyz) - u_smy.w) * u_sms.y);
    float own = (u_smz.w - dot(P, u_smz.xyz) - bias) * u_sms.z;
    float block = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            vec2 q = suv + vec2(i, j) / u_sms.w;
            if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0)))) continue;
            block += texture(u_sm, q).r < own ? 1.0 : 0.0;
        }
    return 1.0 - (block / 9.0) * u_shadow.z;
}

void main()
{
    g_nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
    g_grid = g_nat * max(u_proj.z, 1.0);

    vec3 up = u_worldup.w > 0.5 ? normalize(u_worldup.xyz) : texelFetch(u_up, ivec2(0), 0).rgb;
    vec3 east = u_world.w > 0.5 ? u_world.xyz : vec3(1.0, 0.0, 0.0);   // no camera: screen right
    vec3 right = normalize(east - up * dot(east, up));
    vec3 fwd = cross(right, up);                     // world north (camera: away from it)
    if (u_world.w < 0.5 && fwd.z < 0.0) fwd = -fwd;
    float az = radians(u_sun.x), el = radians(u_sun.y);
    vec3 L = u_shadowdir.w > 0.5 ? normalize(u_shadowdir.xyz)
           : normalize(up * sin(el) + (right * cos(az) + fwd * sin(az)) * cos(el));
    float texel = u_sms.x > 0.0 ? 1.0 / (u_sms.x * u_sms.w) : 0.0;   // world units per map texel

    // a sprite standing here: its own place on its upright plane, shadow only;
    // the bias keeps its own cut-out from shading it
    float zs = texture(u_spr, v_uv).r;
    if (zs > 0.0 && zs < 50000.0 && texture(u_m, v_uv).g < 0.5) {
        vec3 S = pos_at(v_uv, zs);
        o = vec4(shadow_at(S, u_shadow.y * zs + 6.0 * texel + 24.0), zs, 0.0, 1.0);
        return;
    }

    vec2 uv0 = snap(v_uv);
    float z = sz_at(uv0);
    if (z <= 0.0) { o = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec3 P = pos_at(uv0, z);

    // normal from two native pixels on each side (smoother than the nearest
    // neighbour on the big flat-shaded PS1 polygons), the closer side at edges
    vec2 px = 2.0 / g_nat;
    float zr = sz_at(uv0 + vec2(px.x, 0.0)), zl = sz_at(uv0 - vec2(px.x, 0.0));
    float zu = sz_at(uv0 + vec2(0.0, px.y)), zd = sz_at(uv0 - vec2(0.0, px.y));
    vec3 dx = (zr > 0.0 && (abs(zr - z) <= abs(zl - z) || zl <= 0.0))
            ? pos_at(uv0 + vec2(px.x, 0.0), zr) - P : P - pos_at(uv0 - vec2(px.x, 0.0), zl);
    vec3 dy = (zu > 0.0 && (abs(zu - z) <= abs(zd - z) || zd <= 0.0))
            ? pos_at(uv0 + vec2(0.0, px.y), zu) - P : P - pos_at(uv0 - vec2(0.0, px.y), zd);
    vec3 n = normalize(cross(dx, dy) + vec3(0.0, 0.0, -1e-6));
    if (n.z > 0.0) n = -n;

    float lambert = clamp(dot(n, L), 0.0, 1.0);

    // the bias grows on faces turned from the light
    float shadow = lambert > 0.0
        ? shadow_at(P, (u_shadow.y * z + 2.0 * texel) * (1.0 + 2.0 * (1.0 - lambert))) : 1.0;
    o = vec4(lambert * shadow, z, 0.0, 1.0);
}
