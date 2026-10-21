// Colour grade and vignette. Written for psxrecomp.
// u_grade: x = saturation (1 = unchanged), y = contrast (1 = unchanged),
//          z = warmth (-1 cool .. 0 .. +1 warm), w = vignette strength (0 = off).
// u_shape: x = vignette inner radius, y = outer radius (as a fraction of the
//          half diagonal), z = lift of the blacks (0 = none).
// The look applies to gameplay only: u_cov (the coverage pass) tells how much
// of the screen is 3D scenery; 2D screens (logo, title, menus) have none and
// are left exactly as they are.

uniform vec4 u_grade;
uniform vec4 u_shape;

void main()
{
    vec3 src = texture(u_src, v_uv).rgb;
    float cov = 0.25 * (texture(u_cov, vec2(0.25, 0.25)).r + texture(u_cov, vec2(0.75, 0.25)).r
                      + texture(u_cov, vec2(0.25, 0.75)).r + texture(u_cov, vec2(0.75, 0.75)).r);
    float k = smoothstep(0.05, 0.20, cov);      // 0 on 2D screens, 1 in 3D scenes
    if (k <= 0.0) { o = vec4(src, 1.0); return; }
    vec3 c = src;
    // contrast around mid grey, then saturation around the luma
    c = (c - 0.5) * u_grade.y + 0.5;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, u_grade.x);
    // warmth: a gentle shift of red/blue, stronger in the highlights
    c += vec3(0.06, 0.015, -0.06) * u_grade.z * (0.35 + 0.65 * l);
    c = u_shape.z + c * (1.0 - u_shape.z);
    // vignette at the picture's aspect, so it stays round in 16:9
    vec2 p = (v_uv - 0.5) * vec2(u_size.x / u_size.y, 1.0);
    float r = length(p) / length(vec2(0.5 * u_size.x / u_size.y, 0.5));
    float v = smoothstep(u_shape.x, u_shape.y, r);
    c *= 1.0 - v * u_grade.w;
    o = vec4(mix(src, clamp(c, 0.0, 1.0), k), 1.0);
}
