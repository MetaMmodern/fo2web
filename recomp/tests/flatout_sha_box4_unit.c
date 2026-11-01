/* flatout_sha_box4_unit.c -- host-only checks for the HLSL ps_1_1 path used
 * by FlatOut 2's post_box4.sha bloom blur.  The shipped file is the only .sha
 * written in HLSL: the pass declares
 *     PixelShader = compile ps_1_1 PostProcessPS();
 * and the four-sampler average body must be turned into a real ps_1_1 token
 * stream (plus Clamp/Point/Linear sampler state for stages 0..3), while a
 * different HLSL body must fail the parse with an error instead of silently
 * leaving the pass without a pixel program.  No GPU, no WGSL translation.
 *
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime \
 *         tests/flatout_sha_box4_unit.c runtime/flatout_sha.c -o out/flatout_sha_box4_unit
 *   out/flatout_sha_box4_unit
 */
#include "flatout_sha.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REAL_SHA "/Volumes/BOOTCAMP/Program Files (x86)/FlatOut 2/data/shader/post_box4.sha"

static int fails;
static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

/* Trimmed copy of the shipped post_box4.sha (comments removed, shipping
 * whitespace in the HLSL body kept). */
static const char box4[] =
    "const string inputStreamFormat = \"PosprojTex4\";\n"
    "\n"
    "Texture Tex0;\n"
    "Texture Tex1;\n"
    "Texture Tex2;\n"
    "Texture Tex3;\n"
    "\n"
    "sampler2D g_sampler0 =\n"
    "sampler_state\n"
    "{\n"
    "    Texture = <Tex0>;\n"
    "    AddressU = Clamp;\n"
    "    AddressV = Clamp;\n"
    "    MinFilter = Point;\n"
    "    MagFilter = Linear;\n"
    "    MipFilter = Linear;\n"
    "};\n"
    "\n"
    "sampler2D g_sampler1 =\n"
    "sampler_state\n"
    "{\n"
    "    Texture = <Tex1>;\n"
    "    AddressU = Clamp;\n"
    "    AddressV = Clamp;\n"
    "    MinFilter = Point;\n"
    "    MagFilter = Linear;\n"
    "    MipFilter = Linear;\n"
    "};\n"
    "\n"
    "sampler2D g_sampler2 =\n"
    "sampler_state\n"
    "{\n"
    "    Texture = <Tex2>;\n"
    "    AddressU = Clamp;\n"
    "    AddressV = Clamp;\n"
    "    MinFilter = Point;\n"
    "    MagFilter = Linear;\n"
    "    MipFilter = Linear;\n"
    "};\n"
    "\n"
    "sampler2D g_sampler3 =\n"
    "sampler_state\n"
    "{\n"
    "    Texture = <Tex3>;\n"
    "    AddressU = Clamp;\n"
    "    AddressV = Clamp;\n"
    "    MinFilter = Point;\n"
    "    MagFilter = Linear;\n"
    "    MipFilter = Linear;\n"
    "};\n"
    "\n"
    "float4 PostProcessPS( float2 Tex0 : TEXCOORD0, float2 Tex1 : TEXCOORD1, "
    "float2 Tex2 : TEXCOORD2, float2 Tex3 : TEXCOORD3 ) : COLOR\n"
    "{\n"
    "\tfloat4 color;\n"
    "\tcolor.rgba = (tex2D( g_sampler0, Tex0 ) + tex2D( g_sampler1, Tex1 ) + "
    "tex2D( g_sampler2, Tex2 ) + tex2D( g_sampler3, Tex3 )) * 0.25f;\n"
    "\treturn color;\n"
    "}\n"
    "\n"
    "technique PostProcess\n"
    "{\n"
    "    pass p0\n"
    "    {\n"
    "        VertexShader = null;\n"
    "        PixelShader = compile ps_1_1 PostProcessPS();\n"
    "    }\n"
    "}\n";

/* Shared success checks: exactly one technique/pass, a non-empty pixel
 * program, no vertex program, and Clamp/Point-min/Linear-mag state on every
 * one of the four sampled stages. */
static void check_box4(const char *tag, const char *src, size_t len){
    char err[256] = {0};
    printf("%s\n", tag);
    FO2ShaEffect *e = fo2_sha_parse(src, len, err, sizeof err);
    chk(e != 0, "parse succeeds");
    if (!e){ printf("  parse error: %s\n", err[0] ? err : "(none)"); return; }

    chk(fo2_sha_technique_count(e) == 1, "technique count 1");
    chk(fo2_sha_pass_count(e, 0) == 1, "pass count 1");

    const FO2ShaProgram *ps = fo2_sha_pass_program(e, 0, 0, 1);
    size_t n = 0;
    const uint32_t *tok = fo2_sha_tokens(ps, &n);
    chk(ps != 0, "pixel program non-NULL");
    chk(tok != 0 && n > 0, "pixel program tokens non-NULL and non-empty");
    printf("  info  pixel program words: %zu\n", n);
    /* Parameter tokens have bit 31 set; find the last ADD instruction,
     * then inspect the shift nibble of its following destination token. */
    size_t last_add = n;
    if (tok){
        for (size_t i = 1; i + 1 < n; i++)
            if (!(tok[i] & 0x80000000u) && (tok[i] & 0xffffu) == 2u)
                last_add = i;
    }
    chk(last_add < n && (tok[last_add + 1] & 0x80000000u) &&
        ((tok[last_add + 1] >> 24) & 0xfu) == 14u,
        "last ADD destination shift is divide by 4 (14)");
    chk(fo2_sha_program_is_null(ps) == 0, "pixel program is not null");
    chk(fo2_sha_pass_program(e, 0, 0, 0) == 0, "vertex program NULL by declaration");

    for (unsigned stage = 0; stage < 4; stage++){
        char expected[16], what[80];
        snprintf(expected, sizeof expected, "Tex%u", stage);
        snprintf(what, sizeof what, "stage %u texture is %s", stage, expected);
        const char *texture = fo2_sha_pass_texture(e, 0, 0, stage);
        chk(texture && strcmp(texture, expected) == 0, what);
    }

    FO2ShaPassState st;
    int ok = fo2_sha_pass_state(e, 0, 0, &st);
    chk(ok, "pass state available");
    if (ok){
        char what[80];
        for (unsigned stage = 0; stage < 4; stage++){
            snprintf(what, sizeof what, "stage %u AddressU Clamp", stage);
            chk((st.smp_mask[stage] & (1u << 1)) && st.smp[stage][1] == 3, what);
            snprintf(what, sizeof what, "stage %u AddressV Clamp", stage);
            chk((st.smp_mask[stage] & (1u << 2)) && st.smp[stage][2] == 3, what);
            snprintf(what, sizeof what, "stage %u MinFilter Point", stage);
            chk((st.smp_mask[stage] & (1u << 6)) && st.smp[stage][6] == 1, what);
            snprintf(what, sizeof what, "stage %u MagFilter Linear", stage);
            chk((st.smp_mask[stage] & (1u << 5)) && st.smp[stage][5] == 2, what);
            snprintf(what, sizeof what, "stage %u MipFilter Linear", stage);
            chk((st.smp_mask[stage] & (1u << 7)) && st.smp[stage][7] == 2, what);
        }
    }
    fo2_sha_free(e);
}

static void test_embedded(void){
    check_box4("embedded post_box4.sha", box4, strlen(box4));
}

static void test_real_file(void){
    FILE *f = fopen(REAL_SHA, "rb");
    if (!f){ printf("real post_box4.sha not present, skipping\n"); return; }
    if (fseek(f, 0, SEEK_END) != 0){ fclose(f); printf("real post_box4.sha unreadable, skipping\n"); return; }
    long sz = ftell(f);
    if (sz <= 0){ fclose(f); printf("real post_box4.sha empty, skipping\n"); return; }
    rewind(f);
    char *buf = malloc((size_t)sz);
    size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!buf || got != (size_t)sz){ free(buf); printf("real post_box4.sha unreadable, skipping\n"); return; }
    check_box4("real post_box4.sha", buf, got);
    free(buf);
}

static void test_negative_body(void){
    printf("negative: HLSL body is not the four-sample average\n");
    size_t len = strlen(box4);
    char *bad = malloc(len + 1);
    if (!bad){ chk(0, "test allocation"); return; }
    memcpy(bad, box4, len + 1);
    /* Multiply the samples instead of averaging them: (*) instead of (+). */
    int replaced = 0;
    for (char *p = bad; (p = strstr(p, " + ")) != 0; p += 3){ memcpy(p, " * ", 3); replaced++; }
    chk(replaced == 3, "negative copy uses multiplication");
    char err[256] = {0};
    FO2ShaEffect *e = fo2_sha_parse(bad, len, err, sizeof err);
    chk(e == 0, "parse fails for non-average body");
    chk(err[0] != 0, "parse failure carries a non-empty error");
    printf("  info  error: %s\n", err[0] ? err : "(none)");
    fo2_sha_free(e);
    free(bad);
}

int main(void){
    test_embedded();
    test_real_file();
    test_negative_body();
    if (fails) printf("flatout_sha_box4_unit: %d FAILED\n", fails);
    else printf("flatout_sha_box4_unit: all passed\n");
    return fails ? 1 : 0;
}
