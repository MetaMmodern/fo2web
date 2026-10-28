#ifndef FLATOUT_SHA_H
#define FLATOUT_SHA_H
#include <stdint.h>
#include <stddef.h>

typedef struct FO2ShaProgram { char *name; uint32_t *tok; size_t n, cap; } FO2ShaProgram;
typedef struct FO2ShaEffect FO2ShaEffect;
typedef struct {
    uint32_t rs_mask[8];
    uint32_t rs[256];
    uint32_t smp_mask[4];
    uint32_t smp[4][20];
    uint64_t tss_mask[4];
    uint32_t tss[4][40];
} FO2ShaPassState;

/* Parses an installed FlatOut .sha source and assembles its D3D shader blocks.
 * The returned object owns all strings and token buffers. */
FO2ShaEffect *fo2_sha_parse(const char *src, size_t len, char *err, size_t errcap);
void fo2_sha_free(FO2ShaEffect *e);
unsigned fo2_sha_technique_count(const FO2ShaEffect *e);
unsigned fo2_sha_pass_count(const FO2ShaEffect *e, unsigned technique);
const FO2ShaProgram *fo2_sha_pass_program(const FO2ShaEffect *e, unsigned technique, unsigned pass, int pixel);
const char *fo2_sha_pass_texture(const FO2ShaEffect *e, unsigned technique, unsigned pass, unsigned stage);
unsigned fo2_sha_pass_vs_const_count(const FO2ShaEffect *e, unsigned technique, unsigned pass);
int fo2_sha_pass_vs_const(const FO2ShaEffect *e, unsigned technique, unsigned pass, unsigned row, float out[4]);
unsigned fo2_sha_pass_ps_const_count(const FO2ShaEffect *e, unsigned technique, unsigned pass);
int fo2_sha_pass_ps_const(const FO2ShaEffect *e, unsigned technique, unsigned pass, unsigned row, float out[4]);
int fo2_sha_pass_state(const FO2ShaEffect *e, unsigned technique, unsigned pass, FO2ShaPassState *out);
const char *fo2_sha_input_format(const FO2ShaEffect *e);
const uint32_t *fo2_sha_tokens(const FO2ShaProgram *p, size_t *words);
int fo2_sha_program_is_null(const FO2ShaProgram *p);

#endif
