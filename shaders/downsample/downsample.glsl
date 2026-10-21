// Down-sample to the console's own grid (BoF4 Graphics > Downsample).
// Written for psxrecomp (no third-party code). Each target pixel is the
// average of the source pixels it covers: a box filter, approximated with an
// evenly spread grid of bilinear taps (each tap already averages 2x2 texels).
// The pass is `lines 240` tall, so the target is the native 320x240 picture
// (or its native-wide width); the source is the composed high-resolution image.

void main()
{
    vec2 src  = vec2(textureSize(u_src, 0));
    vec2 foot = src / u_size;                     // source px under one target px
    vec2 base = v_uv * src - 0.5 * foot;          // the footprint's corner, source px
    ivec2 n = ivec2(clamp(ceil(foot * 0.5), vec2(1.0), vec2(8.0)));
    vec2 cell = foot / vec2(n);
    vec3 acc = vec3(0.0);
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++)
            acc += texture(u_src, (base + (vec2(x, y) + 0.5) * cell) / src).rgb;
    o = vec4(acc / float(n.x * n.y), 1.0);
}
