// 3D coverage. Written for psxrecomp.
// A tiny target (`lines 16`): each texel is the share of 3D pixels (the
// framework's mask3d, green) in its patch of the screen, from a 4x4 grid of
// samples. The grade pass reads its average to tell gameplay (3D scenery)
// from 2D screens such as the logo, the title and the menus.

void main()
{
    vec2 cell = 1.0 / u_size;                    // one target texel in uv
    float acc = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            vec2 uv = v_uv + (vec2(x, y) - 1.5) * 0.25 * cell;
            acc += textureLod(u_m, uv, 0.0).g;
        }
    o = vec4(acc / 16.0);
}
