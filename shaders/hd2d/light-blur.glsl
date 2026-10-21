// Light from the game's own sources, blur pass. Written for psxrecomp.
// A separable 9-tap Gaussian (linear-sampled, 5 fetches) along u_dir that
// keeps all four channels: rgb = emitted colour, a = depth x luminance.

uniform vec2 u_dir;

void main()
{
    vec2 d = u_dir * u_rcp;
    vec4 c = texture(u_tex, v_uv) * 0.2270270;
    c += (texture(u_tex, v_uv + d * 1.3846154) + texture(u_tex, v_uv - d * 1.3846154)) * 0.3162162;
    c += (texture(u_tex, v_uv + d * 3.2307692) + texture(u_tex, v_uv - d * 3.2307692)) * 0.0702703;
    o = c;
}
