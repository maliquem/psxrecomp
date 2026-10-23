// Ambient occlusion, estimate pass. Written for psxrecomp;
// the method is the textbook screen-space one (positions rebuilt from depth,
// a normal from neighbouring depths, nearby points above the surface occlude
// it by the cosine of their angle, with distance falloff), not anyone's shader
// code. Each sample counts at most 1, so foliage (many near occluders) greys
// instead of going black.
//
// Inputs: u_d = framework `depth` (window sized; SZ = 256/(1-d) - 256, d = 1:
// none). Positions use the PS1 camera: X = (x - cx) * Z / H on the native
// grid, with H the GTE projection distance (BoF4: 1024).
// u_ao:   x = radius (native px), y = intensity, z = bias (cosine, 0..1),
//         w = contrast (power)
// u_proj: x = H, y = native picture height (240), z = internal scale (the
//         depth's own grid: at low scales one depth texel covers several
//         window pixels, so reads snap to its centres and the normal steps a
//         whole texel; a staircase would read as a slope full of creases)
// Output (rgba16f): r = AO (1 = open), g = SZ (0 = none), for the blur passes.

uniform vec4 u_ao;
uniform vec4 u_proj;

vec2 g_grid;   // depth grid in texels (native grid x internal scale)

vec2 snap(vec2 uv) { return (floor(uv * g_grid) + 0.5) / g_grid; }

float sz_at(vec2 uv)
{
    float d = texture(u_d, uv).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
}

// Position on the PS1 camera's terms, from uv and SZ.
vec3 pos_at(vec2 uv, float z, vec2 nat)
{
    vec2 p = (snap(uv) - 0.5) * nat;           // native px from the centre
    return vec3(p * z / u_proj.x, z);
}

void main()
{
    vec2 nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);   // native grid, e.g. 320x240
    g_grid = nat * max(u_proj.z, 1.0);
    vec2 uv0 = snap(v_uv);
    float z = sz_at(uv0);
    if (z <= 0.0) { o = vec4(1.0, 0.0, 0.0, 1.0); return; }
    vec3 P = pos_at(uv0, z, nat);

    // normal from the closer of the two neighbours on each axis (keeps edges sharp)
    vec2 px = max(1.0 / u_size, 1.0 / g_grid);
    float zr = sz_at(uv0 + vec2(px.x, 0.0)), zl = sz_at(uv0 - vec2(px.x, 0.0));
    float zu = sz_at(uv0 + vec2(0.0, px.y)), zd = sz_at(uv0 - vec2(0.0, px.y));
    vec3 dx = (zr > 0.0 && (abs(zr - z) <= abs(zl - z) || zl <= 0.0))
            ? pos_at(uv0 + vec2(px.x, 0.0), zr, nat) - P : P - pos_at(uv0 - vec2(px.x, 0.0), zl, nat);
    vec3 dy = (zu > 0.0 && (abs(zu - z) <= abs(zd - z) || zd <= 0.0))
            ? pos_at(uv0 + vec2(0.0, px.y), zu, nat) - P : P - pos_at(uv0 - vec2(0.0, px.y), zd, nat);
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;                // face the camera (along its ray: a floor seen edge-on has n.z ~ 0)

    // 12 samples on a spiral, rotated per pixel (4x4 interleaved; the blur removes it)
    ivec2 ip = ivec2(gl_FragCoord.xy) & 3;
    float rot = float(ip.x * 4 + ip.y) * (6.2831853 / 16.0);
    float rw = u_ao.x * z / u_proj.x;           // the radius in world units at this depth
    float occ = 0.0;
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float t = (float(i) + 0.5) / float(N);
        float a = rot + t * 6.2831853 * 3.0;    // three turns
        vec2 o2 = vec2(cos(a), sin(a)) * (t * u_ao.x) / nat;
        vec2 suv = snap(uv0 + o2);
        float sz = sz_at(suv);
        if (sz <= 0.0) continue;
        vec3 v = pos_at(suv, sz, nat) - P;
        float vv = dot(v, v);
        if (vv < 1e-6) continue;
        float fall = 1.0 - smoothstep(rw * rw, 4.0 * rw * rw, vv);   // far in front: no cover
        occ += max(0.0, dot(v, n) * inversesqrt(vv) - u_ao.z) * fall;
    }
    float ao = 1.0 - u_ao.y * occ / float(N);
    ao = pow(clamp(ao, 0.0, 1.0), u_ao.w);
    o = vec4(ao, z, 0.0, 1.0);
}
