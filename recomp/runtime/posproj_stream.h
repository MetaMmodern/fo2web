#ifndef FO2_POSPROJ_STREAM_H
#define FO2_POSPROJ_STREAM_H

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
