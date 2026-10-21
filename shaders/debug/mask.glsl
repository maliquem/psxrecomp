// Mask debug view. Written for psxrecomp.
// The picture dimmed, with the framework masks on top:
//   red    `mask` red: may glow (the bloom's sources: additive 3D pieces)
//   green  `mask3d` green: 3D scenery is the last opaque thing here
//   blue   `mask3d` blue: something see-through was drawn over it

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    float g = texelFetch(u_m, p, 0).r;
    vec3 m3 = texelFetch(u_m3, p, 0).rgb;
    o = vec4(src * 0.35 + vec3(g, m3.g * 0.35, m3.b * 0.6), 1.0);
}
