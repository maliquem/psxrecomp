// Adaptive: 3D keeps the high resolution, 2D is drawn from the console-resolution
// picture, smoothed (BoF4 Graphics > Downsample). Written for psxrecomp.
// u_m is the framework's mask3d (green = a 3D primitive drew there last). The
// mask is averaged over a small cross so the seam between the two is soft.

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    ivec2 mx = textureSize(u_m, 0) - 1;
    float m = 0.0;
    m += texelFetch(u_m, clamp(p, ivec2(0), mx), 0).g * 2.0;
    m += texelFetch(u_m, clamp(p + ivec2( 2, 0), ivec2(0), mx), 0).g;
    m += texelFetch(u_m, clamp(p + ivec2(-2, 0), ivec2(0), mx), 0).g;
    m += texelFetch(u_m, clamp(p + ivec2(0,  2), ivec2(0), mx), 0).g;
    m += texelFetch(u_m, clamp(p + ivec2(0, -2), ivec2(0), mx), 0).g;
    m /= 6.0;
    vec3 hi = texture(u_src, v_uv).rgb;
    vec3 lo = texture(u_down, v_uv).rgb;
    o = vec4(mix(lo, hi, m), 1.0);
}
