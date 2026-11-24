#ifndef TEXTURE_MIPS_H
#define TEXTURE_MIPS_H

/* D3D9 Levels=0 requests every level down to 1x1, not a single level.
 * Sixteen slots cover the renderer's maximum 4096x4096 texture size. */
#define TEX_MIP_SLOTS 16u
#define TEX_OBJECT_BYTES 192u

static inline unsigned tex_mip_count(unsigned w, unsigned h, unsigned requested){
    if (requested) return requested;
    unsigned n=1, size=w>h?w:h;
    while (size>1u){ size>>=1; n++; }
    return n;
}
static inline unsigned tex_mip_extent(unsigned size, unsigned level){
    return level<32u && (size>>level) ? size>>level : 1u;
}
/* Keep the legacy first eight locks and ten child slots in place. Additional
 * 2D mip locks live after the cube face locks, and children after those. */
static inline unsigned tex_mip_lock_offset(unsigned level){
    return level<8u ? 32u+4u*level : 128u+4u*(level-8u);
}
static inline unsigned tex_mip_child_offset(unsigned level){
    return level<10u ? 64u+4u*level : 160u+4u*(level-10u);
}
#endif
