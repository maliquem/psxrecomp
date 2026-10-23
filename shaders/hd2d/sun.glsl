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
//           is (0..1), w = the seam at a wall's foot filled over w x the bias
//           (contact_at; 0 = off)
// u_smx..u_sms: the framework's shadow map mapping (gl_postfx.inc)
// u_soft:   x = how far the scenery's shadow edges spread (world units each
//           way; 0 = a texel); z the sprites' (0 = sharp, 3x3 texels), grown
//           by w per unit of distance like the scenery's by y. y > 0: the spread
//           grows with the distance to what casts it, y world units per unit
//           (the sun's size, as in percentage-closer soft shadows, Fernando
//           2005), from a texel up to x: a bush's leaves sharp at its foot,
//           a cliff's far edge soft; 0 = always x
// u_ssm:    the sprites' shadow map (framework `sprshadow`, mapped as u_sm);
//           u_ss.x 0 (framework) leaves it empty: no sprite shadows; u_ss.y
//           0: the sprites' shadows only, no sun on the scenery (no facing,
//           no scenery shadows; sprites stay lit)
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
uniform vec4 u_ssx, u_ssy, u_ssz, u_sss;   // the sprites' map's own mapping (w 0: none, the scenery's)
uniform vec4 u_sprsun;   // x: the sprites' sun at most this high (framework); y: their shadow's reach (world units, 0 = any); z: its softness then (world units)
uniform vec4 u_proj;
uniform vec4 u_soft;
uniform vec4 u_ss;

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

// Shadow at P: is something nearer the light in the shadow maps? The
// sprites' map (u_ssm) with 3x3 samples a texel apart (sharp pixel-art
// silhouettes), or soft by u_soft.zw; the scenery's (u_sm) with 5x5 over u_soft.x world units each
// way (soft edges; 0 = 3x3 texels; u_soft.y: over the distance to the
// blockers found there times y, see above), its bias grown with the receiver's tilt
// to the light (tilt = tan of the angle) across that spread; sprites (spr 0)
// only from the scenery's, so no sprite shades itself or another (their feet
// bent down to the floor sit off their own plane). 1 = lit, down to
// 1 - u_shadow.z.
bool sm_in(vec2 q) { return all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, vec2(1.0))); }
float shadow_at(vec3 P, float bias, float tilt, float spr, float scn)
{
    if (u_sms.w <= 0.0 || u_sms.z <= 0.0) return 1.0;
    vec2 suv = vec2((dot(P, u_smx.xyz) - u_smx.w) * u_sms.x, (dot(P, u_smy.xyz) - u_smy.w) * u_sms.y);
    float own = (u_smz.w - dot(P, u_smz.xyz) - bias) * u_sms.z;
    // the sprites' map: its own mapping when the framework gives one (a sun
    // higher than u_sprsun: the sprites cast along a lower one), else the scenery's
    bool sown_map = u_sss.w > 0.0;
    vec4 ssm = sown_map ? u_sss : u_sms;
    vec2 spv = sown_map ? vec2((dot(P, u_ssx.xyz) - u_ssx.w) * u_sss.x, (dot(P, u_ssy.xyz) - u_ssy.w) * u_sss.y) : suv;
    float sown = sown_map ? (u_ssz.w - dot(P, u_ssz.xyz) - bias) * u_sss.z : own;
    // ... and reaching no farther than u_sprsun.y world units along that light:
    // a short shadow at the feet, none on a floor far below (a sprite on a
    // deck over the sand), as under the real noon sun
    float sfar = sown_map && u_sprsun.y > 0.0 ? u_sprsun.y * u_sss.z : 1e9;
    float bs = 0.0;
    if (u_soft.z > 0.0) {
        // the sprites' soft too: the spread grows with how far above P the
        // sprite is (u_soft.w per unit), from a texel at its feet up to
        // u_soft.z; 5x5 samples turned per pixel (no steps of the map's texels)
        // a sprite laid on its floor (sown_map): no height to grow by, a fixed
        // spread of u_sprsun.z world units instead
        float smax = sown_map && u_sprsun.z > 0.0 ? u_sprsun.z : u_soft.z;
        vec2 st0 = 0.5 * smax * ssm.xy;
        float sum = 0.0, n = 0.0;
        for (int j = -2; j <= 2; j++)
            for (int i = -2; i <= 2; i++) {
                vec2 q = spv + vec2(i, j) * st0;
                float d = sm_in(q) ? texture(u_ssm, q).r : 1.0;
                if (d < sown && sown - d < sfar) { sum += d; n += 1.0; }
            }
        if (n > 0.5) {
            float texel = 1.0 / (ssm.x * ssm.w);
            float w = sown_map && u_sprsun.z > 0.0 ? smax : clamp((sown - sum / n) / ssm.z * u_soft.w, texel, u_soft.z);
            float a = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
            mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
            vec2 st = 0.5 * w * ssm.xy;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++) {
                    vec2 q = spv + (rot * vec2(i, j)) * st;
                    if (sm_in(q)) { float d = texture(u_ssm, q).r; bs += d < sown && sown - d < sfar ? 1.0 : 0.0; }
                }
            bs /= 25.0;
        }
    } else {
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                vec2 q = spv + vec2(i, j) / ssm.w;
                if (sm_in(q)) { float d = texture(u_ssm, q).r; bs += d < sown && sown - d < sfar ? 1.0 : 0.0; }
            }
        bs /= 9.0;
    }
    bs *= spr;
    float bw = 0.0;
    if (u_soft.x > 0.0) {
        float w = u_soft.x;
        float ownw = own - w * min(tilt, 3.0) * u_sms.z;
        if (u_soft.y > 0.0) {
            // the blockers over the whole spread: how far above P they are
            vec2 st0 = 0.5 * w * u_sms.xy;
            float sum = 0.0, n = 0.0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++) {
                    vec2 q = suv + vec2(i, j) * st0;
                    float d = sm_in(q) ? texture(u_sm, q).r : 1.0;
                    if (d < ownw) { sum += d; n += 1.0; }
                }
            if (n < 0.5) return 1.0 - bs * u_shadow.z;
            float texel = 1.0 / (u_sms.x * u_sms.w);   // world units per map texel
            w = clamp((own - sum / n) / u_sms.z * u_soft.y, texel, u_soft.x);
            ownw = own - w * min(tilt, 3.0) * u_sms.z;
        }
        vec2 st = 0.5 * w * u_sms.xy;   // uv per tap
        for (int j = -2; j <= 2; j++)
            for (int i = -2; i <= 2; i++) {
                vec2 q = suv + vec2(i, j) * st;
                if (sm_in(q)) bw += texture(u_sm, q).r < ownw ? 1.0 : 0.0;
            }
        bw /= 25.0;
    } else {
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                vec2 q = suv + vec2(i, j) / u_sms.w;
                if (sm_in(q)) bw += texture(u_sm, q).r < own ? 1.0 : 0.0;
            }
        bw /= 9.0;
    }
    return 1.0 - max(bs, bw * scn) * u_shadow.z;
}

// The seam where a wall meets the floor: the bias that keeps a face from
// shading itself (tens of world units on a floor under a low sun) also lifts
// the wall's shadow off its foot, a lit line along it. From P, a few steps
// toward the light over len world units: scenery standing in front of the
// ray there (nearer the camera, by less than len) shades P. It only adds
// shadow, and only that close.
float contact_at(vec3 P, vec3 L, float len)
{
    for (int i = 1; i <= 6; i++) {
        vec3 R = P + L * (len * float(i) / 6.0);
        if (R.z <= 1.0) return 1.0;
        vec2 uv = uv_of(R);
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
        float zq = sz_at(uv);
        if (zq > 0.0 && zq < R.z - 1.0 && zq > R.z - len) return 1.0 - u_shadow.z;
    }
    return 1.0;
}

// Two pixels along +d from P on its own face (see main), as a step forward.
vec3 face_step(vec2 uv0, vec3 P, float z, vec2 d)
{
    float f1 = sz_at(uv0 + d), f2 = sz_at(uv0 + 2.0 * d);
    float b1 = sz_at(uv0 - d), b2 = sz_at(uv0 - 2.0 * d);
    float ef = (f1 > 0.0 && f2 > 0.0) ? abs(1.0 / f2 - 2.0 / f1 + 1.0 / z) : 1e9;
    float eb = (b1 > 0.0 && b2 > 0.0) ? abs(1.0 / b2 - 2.0 / b1 + 1.0 / z) : 1e9;
    if (ef >= 1e9 && eb >= 1e9) return vec3(d * g_nat * z / u_proj.x * 2.0, 0.0);   // alone: facing the camera
    return ef <= eb ? pos_at(uv0 + 2.0 * d, f2) - P : P - pos_at(uv0 - 2.0 * d, b2);
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

    // a sprite standing here: its own place on its upright plane, shadow only.
    // The framework moves sprite casters toward the light by a floor's bias
    // (so their shadows start at their feet); this bias is larger than that,
    // so its own cut-out never shades it
    float zs = texture(u_spr, v_uv).r;
    if (zs > 0.0 && zs < 50000.0 && texture(u_m, v_uv).g < 0.5) {
        if (u_ss.y < 0.5) { o = vec4(1.0, zs, 0.0, 1.0); return; }   // sprites only: lit
        vec3 S = pos_at(v_uv, zs);
        float floor_bias = (u_shadow.y * zs + 2.0 * texel) * (1.0 + 2.0 * (1.0 - max(dot(up, L), 0.0)));
        o = vec4(shadow_at(S, floor_bias + 6.0 * texel + 24.0, 1.0, 0.0, 1.0), zs, 0.0, 1.0);
        return;
    }

    vec2 uv0 = snap(v_uv);
    float z = sz_at(uv0);
    if (z <= 0.0) { o = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec3 P = pos_at(uv0, z);

    // normal from two native pixels on each side (smoother than the nearest
    // neighbour on the big flat-shaded PS1 polygons), on the side that goes
    // on in P's plane: 1/Z runs straight across a plane on the picture, so
    // the side whose 1/Z bends least (P, one and two pixels on) is P's own
    // face; at a crease the other side's face would light a thin seam
    vec2 p1 = 1.0 / g_nat;
    vec3 dx = face_step(uv0, P, z, vec2(p1.x, 0.0));
    vec3 dy = face_step(uv0, P, z, vec2(0.0, p1.y));
    vec3 n = normalize(cross(dx, dy) + vec3(0.0, 0.0, -1e-6));
    // face the camera along its ray to P, not by n.z: a floor seen almost
    // edge-on (a cutscene's camera at the horizon) has n.z ~ 0, and its
    // sign flipped half the floor upside down in zigzag stripes
    if (dot(n, P) > 0.0) n = -n;

    float lambert = clamp(dot(n, L), 0.0, 1.0);

    // the bias grows on faces turned from the light
    float scn = u_ss.y > 0.5 ? 1.0 : 0.0;   // sprites only: no facing, no scenery shadows
    float bias = (u_shadow.y * z + 2.0 * texel) * (1.0 + 2.0 * (1.0 - lambert));
    float shadow = (lambert > 0.0 || scn < 0.5)
        ? shadow_at(P, bias, sqrt(max(1.0 - lambert * lambert, 0.0)) / max(lambert, 0.05), 1.0, scn) : 1.0;
    // u_shadow.w: the seam at a wall's foot filled (contact_at over w x the bias; 0 = off)
    if (u_shadow.w > 0.0 && lambert > 0.0 && scn > 0.5 && shadow > 1.0 - u_shadow.z)
        shadow = min(shadow, contact_at(P, L, u_shadow.w * bias));
    o = vec4(mix(1.0, lambert, scn) * shadow, z, 0.0, 1.0);
}
