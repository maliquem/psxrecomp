// Box: the whole picture at the console's resolution (BoF4 Graphics > Downsample).
// Written for psxrecomp. u_ds.x: 0 = sharp (each native pixel a square),
// 1 = smooth (bilinear between native pixels).

uniform vec4 u_ds;

void main()
{
    vec2 ds = vec2(textureSize(u_down, 0));
    vec3 c = u_ds.x > 0.5 ? texture(u_down, v_uv).rgb
                          : texelFetch(u_down, ivec2(min(v_uv * ds, ds - 1.0)), 0).rgb;
    o = vec4(c, 1.0);
}
