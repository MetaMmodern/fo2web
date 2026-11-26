#ifndef FO2_POSPROJ_STREAM_H
#define FO2_POSPROJ_STREAM_H

/* D3D9's pixel centres are integers; Metal/WebGPU's are half-integers.
 * Translate screen-space POSITIONT by +0.5 before mapping to clip space.
 * In particular the original post quads span [-0.5, size-0.5], not [0,size].
 * Keep this out of programmable VS paths, whose coordinates are already clip
 * space, and do not alter the authored UVs (including radial/tiled passes). */
static inline float fo2_posproj_clip_x(float x, unsigned width, float w){
    return (2.f*(x+0.5f)/(float)width-1.f)*w;
}
static inline float fo2_posproj_clip_y(float y, unsigned height, float w){
    return (1.f-2.f*(y+0.5f)/(float)height)*w;
}

/* A null-VS post effect identifies pretransformed vertices, not necessarily
 * the number of UV pairs actually supplied. post_highpass_luminance.sha says
 * PosprojTex4 but sub_005a8700 uses FVF 0x104 / stride 24 on its luminance
 * path (only t0 is sampled). Never read four UV pairs from that stream. */
static inline unsigned fo2_posproj_stream_uvs(unsigned pass_uvs, unsigned stride,
                                             unsigned pt, unsigned nv){
    if (!pass_uvs || pt < 4u || pt > 6u || nv < 3u || nv > 24u) return 0;
    if (stride == 24u && (pass_uvs == 1u || pass_uvs == 4u)) return 1u;
    if (stride == 48u && pass_uvs == 4u) return 4u;
    return 0;
}

#endif
