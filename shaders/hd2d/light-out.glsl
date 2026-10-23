// Light from the game's own sources, composite. Written
// for this project. Brightens and tints the 3D scenery by the gathered light:
// mostly multiplied (the texture stays readable), a little added (u_light.z,
// so a dark floor still picks up the colour of the spell). Opaque HUD and
// menus are left as they are. Scenery seen through something
// see-through (the spell itself) is lit too: leaving it out drew a dark
// outline around every effect. The light goes on screen-like (a pixel that is
// already bright takes less), so the spell itself does not burn out to white
// and the scenery seen through it is lit as much as the scenery beside it.
// Sprites standing in the scene (u_spr, the framework's `sprite` input) take
// it too, by u_lspr.x (0 = left as they are), with half the added tint (a
// dark costume still picks up the fire's colour).
// u_flicker: x = how much the light flickers (0 = steady), y = its pace: a
// flame's slow, uneven pulse (three slow waves of unrelated frequencies over
// u_time), the same for every light in the picture.
// The fire lights (the framework's `firemap`, u_fl0/u_fl1 their places in
// view space, y up, w the reach): fire_light gives how much of a light
// reaches a point, by its distance, how the surface faces it and whether
// something nearer the light (the scenery, a sprite) stands in the way.
// Each fire light takes its glow's colour (u_flc0/u_flc1 rgb, the pieces the
// game draws glowing there: a flame orange, a staff's orb white; none known:
// u_firecol.rgb) and size (w: a small glow lights less and less far,
// gl_postfx.inc).
// u_lightview.x = 1 shows the light alone; .y = 1 the fire lights alone
// (white lit, black in their shadow or out of reach).

uniform vec4 u_light;
uniform vec4 u_lightview;
uniform vec4 u_lspr;
uniform vec4 u_flicker;
uniform vec4 u_proj;
uniform vec4 u_fl0, u_fl1, u_fls0, u_fls1, u_flc0, u_flc1, u_flmap;
uniform vec4 u_firecol, u_firecfg;

float flicker(float t)
{
    float n = 0.5 * sin(t * 7.3) + 0.3 * sin(t * 13.1 + 1.7) + 0.2 * sin(t * 23.7 + 0.4);
    return 1.0 + u_flicker.x * n;
}

// the cube face of direction d (largest axis), its axes as in gl_postfx.inc
const vec3 FF[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 FR[6] = vec3[6](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(-1, 0, 0));
const vec3 FU[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
int fire_face(vec3 d)
{
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x > 0.0 ? 0 : 1;
    if (a.y >= a.z) return d.y > 0.0 ? 2 : 3;
    return d.z > 0.0 ? 4 : 5;
}
// visible from light li at P (1 lit .. 0 in its shadow): 5x5 samples over
// u_firecfg.w world units at P (a flame is no point: soft edges, wider far
// off), turned per pixel so the steps of the map's texels do not show
float fire_vis(vec3 P, vec4 fl, int li)
{
    vec3 d = P - fl.xyz;
    float dist = length(d);
    int f = fire_face(d);
    float z = dot(d, FF[f]);
    vec2 xy = vec2(dot(d, FR[f]), dot(d, FU[f])) / z;
    float t = 1.0 / u_flmap.x;
    float bias = 6.0 + 0.015 * dist;
    float spread = max(u_firecfg.w / max(z, 1.0), 1.5 * t);   // face units per step
    float a = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    float sum = 0.0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            vec2 q = clamp(xy + rot * vec2(i, j) * spread, vec2(-1.0 + t), vec2(1.0 - t)) * 0.5 + 0.5;
            vec2 uv = (vec2(float(f % 3), float(f / 3 + 2 * li)) + q) / vec2(3.0, u_flmap.z);
            sum += texture(u_fm, uv).r * fl.w + bias >= dist ? 1.0 : 0.0;
        }
    return sum / 25.0;
}
// light li at P with normal n (n 0: a sprite, facing all ways): x = how much
// reaches it, y = how much would but something stands in the way (its shadow);
// k: its size (1 = a campfire's), its reach fl.w * k (the map's depth: fl.w)
vec2 fire_light(vec3 P, vec3 n, vec4 fl, int li, float k)
{
    float R = fl.w * k;
    if (R <= 0.0) return vec2(0.0);
    vec3 d = fl.xyz - P;
    float dist = length(d);
    if (dist >= R) return vec2(0.0);
    float att = (1.0 - smoothstep(0.35 * R, R, dist)) / (1.0 + dist * dist / (0.08 * R * R));
    float face = dot(n, n) > 0.0 ? clamp(0.25 + 0.75 * dot(n, d / dist), 0.0, 1.0) : 0.7;
    float vis = fire_vis(P, fl, li);
    return vec2(att * face * vis, att * (1.0 - vis));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    float m = texelFetch(u_m3, p, 0).g;
    float zs = texelFetch(u_spr, p, 0).r;
    bool is_spr = m < 0.5 && zs > 0.0 && zs < 50000.0;
    float spr = is_spr ? u_lspr.x : 0.0;
    vec3 l = texture(u_l, v_uv).rgb * flicker(u_time * max(u_flicker.y, 0.0));

    // the fire lights at this point (scenery from its depth, a sprite on its plane)
    vec2 fire = vec2(0.0);
    vec3 firec = vec3(0.0);   // the lights' colours, each by how much of it reaches here
    if (u_flmap.y > 0.0 && (m > 0.5 || is_spr)) {
        vec2 nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
        float z = zs;
        if (!is_spr) {
            float dd = texelFetch(u_d, p, 0).r;
            z = dd < 0.99999 ? 256.0 / max(1.0 - dd, 1e-7) - 256.0 : 0.0;
        }
        if (z > 0.0) {
            vec3 P = vec3((v_uv - 0.5) * nat * z / u_proj.x, z);
            vec3 n = is_spr ? vec3(0.0) : normalize(cross(dFdx(P), dFdy(P)));
            if (!is_spr && n.z > 0.0) n = -n;
            vec2 f0 = fire_light(P, n, u_fl0, 0, u_flc0.w), f1 = vec2(0.0);
            if (u_flmap.y > 1.0) f1 = fire_light(P, n, u_fl1, 1, u_flc1.w);
            fire = f0 + f1;
            firec = u_flc0.rgb * u_flc0.w * f0.x + u_flc1.rgb * u_flc1.w * f1.x;
        }
    }
    if (u_lightview.y > 0.5) { o = vec4(src * 0.15 + vec3(min(fire.x, 1.0)), 1.0); return; }

    if (u_lightview.x > 0.5) { o = vec4(src * 0.25 + l * max(m, spr), 1.0); return; }
    vec3 add = (src * l + u_light.z * l) * m + (src * l + 0.5 * u_light.z * l) * spr;
    // the fire lights: their shadow darkens (u_firecfg.z), their light
    // brightens and tints by each light's colour (u_firecol.w the strength), flickering
    float fw = (m > 0.5 ? 1.0 : spr) * flicker(u_time * max(u_flicker.y, 0.0));
    vec3 fc = firec * (min(fire.x, 1.5) / max(fire.x, 1e-4)) * u_firecol.w * fw;
    src *= 1.0 - u_firecfg.z * min(fire.y, 1.0) * (m > 0.5 ? 1.0 : spr);
    add += src * fc + 0.15 * fc;
    o = vec4(src + add * (1.0 - src), 1.0);
}
