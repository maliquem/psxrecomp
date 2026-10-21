// Light from the game's own sources, composite. Written
// for this project. Brightens and tints the 3D scenery by the gathered light:
// mostly multiplied (the texture stays readable), a little added (u_light.z,
// so a dark floor still picks up the colour of the spell). Sprites, opaque
// HUD and menus are left as they are. Scenery seen through something
// see-through (the spell itself) is lit too: leaving it out drew a dark
// outline around every effect. The light goes on screen-like (a pixel that is
// already bright takes less), so the spell itself does not burn out to white
// and the scenery seen through it is lit as much as the scenery beside it.
// u_lightview.x = 1 shows the light alone.

uniform vec4 u_light;
uniform vec4 u_lightview;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - u_org;
    vec3 src = texture(u_src, v_uv).rgb;
    float m = texelFetch(u_m3, p, 0).g;
    vec3 l = texture(u_l, v_uv).rgb;
    if (u_lightview.x > 0.5) { o = vec4(src * 0.25 + l * m, 1.0); return; }
    vec3 add = (src * l + u_light.z * l) * m;
    o = vec4(src + add * (1.0 - src), 1.0);
}
