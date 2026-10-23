// Light from the game's own sources, pass 2: the gather.
// Written for psxrecomp. Each scenery point looks around it on a spiral
// for blurred emitters (pass 1 + blur) and takes their light by the distance
// between the two in 3D (positions rebuilt from depth, as the AO does), so a
// spell lights the ground under it but not a wall far behind it, and by how
// much the surface faces the light (wrapped, never fully dark).
// u_light: x = reach (native px at the light), y = strength.
// u_proj:  x = H (GTE projection distance), y = native height (240),
//          z = internal scale (reads snap to the depth's own grid).
// A sprite standing in the scene (u_spr, the framework's `sprite` input, with
// u_m3 mask3d green 0: the pixel shows it) takes the light at its own place
// on its upright plane, facing the camera (a fire beside it lights it half).
// Output (rgba16f): rgb = light reaching this point.

uniform vec4 u_light;
uniform vec4 u_proj;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);
vec2 g_grid;

vec2 snap(vec2 uv) { return (floor(uv * g_grid) + 0.5) / g_grid; }

float sz_at(vec2 uv)
{
    float d = texture(u_d, snap(uv)).r;
    return d < 0.99999 ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
}

vec3 pos_at(vec2 uv, float z, vec2 nat) { return vec3((uv - 0.5) * nat * z / u_proj.x, z); }

void main()
{
    vec2 nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
    g_grid = nat * max(u_proj.z, 1.0);
    vec2 uv0 = snap(v_uv);
    float z = sz_at(uv0);
    vec3 P, n;
    float zs = texture(u_spr, v_uv).r;
    if (zs > 0.0 && zs < 50000.0 && texture(u_m3, v_uv).g < 0.5) {   // a sprite standing here
        uv0 = v_uv; z = zs;
        P = pos_at(uv0, z, nat);
        n = vec3(0.0, 0.0, -1.0);
    } else {
        if (z <= 0.0) { o = vec4(0.0); return; }
        P = pos_at(uv0, z, nat);

        // normal from the closer neighbour on each axis (as in ao.glsl)
        vec2 px = max(1.0 / u_size, 1.0 / g_grid);
        float zr = sz_at(uv0 + vec2(px.x, 0.0)), zl = sz_at(uv0 - vec2(px.x, 0.0));
        float zu = sz_at(uv0 + vec2(0.0, px.y)), zd = sz_at(uv0 - vec2(0.0, px.y));
        vec3 dx = (zr > 0.0 && (abs(zr - z) <= abs(zl - z) || zl <= 0.0))
                ? pos_at(uv0 + vec2(px.x, 0.0), zr, nat) - P : P - pos_at(uv0 - vec2(px.x, 0.0), zl, nat);
        vec3 dy = (zu > 0.0 && (abs(zu - z) <= abs(zd - z) || zd <= 0.0))
                ? pos_at(uv0 + vec2(0.0, px.y), zu, nat) - P : P - pos_at(uv0 - vec2(0.0, px.y), zd, nat);
        n = normalize(cross(dx, dy));
        if (dot(n, P) > 0.0) n = -n;   // face the camera, along its ray
    }

    float rw = u_light.x * z / u_proj.x;        // reach in world units at this depth
    ivec2 ip = ivec2(gl_FragCoord.xy) & 3;
    float rot = float(ip.x * 4 + ip.y) * (6.2831853 / 16.0);
    vec3 sum = vec3(0.0);
    const int N = 24;
    for (int i = 0; i < N; i++) {
        float t = (float(i) + 0.5) / float(N);
        float a = rot + t * 6.2831853 * 4.0;
        vec2 suv = uv0 + vec2(cos(a), sin(a)) * (t * u_light.x) / nat;
        vec4 e = texture(u_e, suv);
        float y = dot(e.rgb, LUMA);
        if (y < 1e-4) continue;
        float zs = e.a / y;
        if (zs < 1.0) zs = z;                    // emitter over the void: take this depth
        vec3 v = pos_at(suv, zs, nat) - P;
        float dd = dot(v, v);
        float att = 1.0 / (1.0 + dd / (0.08 * rw * rw)) * (1.0 - smoothstep(0.6 * rw * rw, rw * rw, dd));
        float face = 0.5 + 0.5 * dot(n, v * inversesqrt(max(dd, 1e-6)));
        sum += e.rgb * att * face * t;           // t: the spiral samples the far ring less densely
    }
    o = vec4(sum * (2.0 / float(N)) * u_light.y, 1.0);
}
