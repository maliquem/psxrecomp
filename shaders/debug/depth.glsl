// Depth debug view. Written for psxrecomp.
// The framework's `depth` input is the PGXP depth buffer of the presented
// picture (d = 1 where nothing 3D wrote); SZ = 256 / (1 - d) - 256.
// u_view.x: 0 = readable view, 2 = SZ in red for measuring, 1 = flat classes:
//   grey     3D scenery with depth (white = near, dark = far)
//   red-dim  no depth and not 3D: background, sprites, HUD (shown dimmed)
//   magenta  marked 3D but no depth (a hole)
//   blue     depth, but a 2D thing (sprite, HUD) drew on top of it
// u_view.yz: SZ range of the grey ramp (log scale).

uniform vec4 u_view;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    float d = texelFetch(u_d, p, 0).r;
    float m = texelFetch(u_m, p, 0).g;
    vec3 src = texture(u_src, v_uv).rgb;
    bool has = d < 0.99999;
    float sz = has ? 256.0 / max(1.0 - d, 1e-7) - 256.0 : 0.0;
    float g = has ? 1.0 - clamp(log2(max(sz, 1.0) / u_view.y) / log2(u_view.z / u_view.y), 0.0, 1.0) : 0.0;
    bool is3d = m > 0.5;
    vec3 c;
    if (u_view.x > 2.5) {   // 3: SZ in 16 bits: red = SZ / 64, green = SZ mod 64 * 4, blue = 3D
        float s = has ? clamp(sz, 0.0, 16383.0) : 16383.0;
        c = vec3(floor(s / 64.0) / 255.0, floor(mod(s, 64.0) * 4.0) / 255.0, is3d ? 1.0 : 0.0);
    } else if (u_view.x > 1.5) {   // 2: measuring SZ: red = SZ / 16384 (1 = none), green = 3D
        c = vec3(has ? clamp(sz / 16384.0, 0.0, 0.996) : 1.0, is3d ? 1.0 : 0.0, 0.0);
    } else if (u_view.x > 0.5) {
        c = has && is3d ? vec3(0.0, 1.0, 0.0) : is3d ? vec3(1.0, 0.0, 1.0)
          : has ? vec3(0.0, 0.0, 1.0) : vec3(0.0);
    } else if (has && is3d) {
        c = vec3(0.1 + 0.9 * g);
    } else if (is3d) {
        c = vec3(1.0, 0.0, 1.0);
    } else if (has) {
        c = mix(vec3(0.15, 0.25, 0.9), src, 0.35);
    } else {
        c = src * vec3(0.45, 0.2, 0.2);
    }
    o = vec4(c, 1.0);
}
