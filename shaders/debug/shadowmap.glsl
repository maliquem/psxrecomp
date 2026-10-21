// Shadow map check. Written for psxrecomp. Each scenery
// pixel is rebuilt from `depth` and looked up in the framework's `shadow` map
// (u_smx..u_sms): green = the map holds this very surface (the mapping
// agrees), red = something nearer the light in the map (shadowed), blue = the
// map is empty there; the picture dimmed underneath.
// u_view.x = 1 shows the map's depth instead.

uniform vec4 u_view;
uniform vec4 u_proj;
uniform vec4 u_shadowdir;
uniform vec4 u_smx, u_smy, u_smz, u_sms;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb * 0.35;
    float d = texelFetch(u_d, p, 0).r;
    if (d >= 0.99999) { o = vec4(src, 1.0); return; }
    float z = 256.0 / max(1.0 - d, 1e-7) - 256.0;
    vec2 nat = vec2(u_proj.y * u_size.x / u_size.y, u_proj.y);
    vec3 P = vec3((v_uv - 0.5) * nat * z / u_proj.x, z);
    vec2 suv = vec2((dot(P, u_smx.xyz) - u_smx.w) * u_sms.x, (dot(P, u_smy.xyz) - u_smy.w) * u_sms.y);
    float own = (u_smz.w - dot(P, u_smz.xyz)) * u_sms.z;
    float m = texture(u_sm, suv).r;
    if (u_view.x > 0.5) { o = vec4(vec3(m), 1.0); return; }
    vec3 c;
    float tol = 0.004;
    if (m >= 0.99999) c = vec3(0.0, 0.0, 1.0);
    else if (m < own - tol) c = vec3(1.0, 0.0, 0.0);
    else if (abs(m - own) <= tol) c = vec3(0.0, 1.0, 0.0);
    else c = vec3(1.0, 1.0, 0.0);
    o = vec4(src + c * 0.6, 1.0);
}
