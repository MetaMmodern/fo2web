/* flatout_sha_unit.c -- synthetic (non-game) checks for the .sha effect
 * constant initializer parser: float4 components with optional f/F suffixes,
 * whitespace, signs and exponents, plus malformed values and register range
 * checks, for both vertex and pixel constants.
 *
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime \
 *         tests/flatout_sha_unit.c runtime/flatout_sha.c -o out/flatout_sha_unit
 *   out/flatout_sha_unit
 */
#include "flatout_sha.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

static FO2ShaEffect *make(const char *vs, const char *ps){
    char src[8192], err[256] = {0};
    snprintf(src, sizeof src,
             "technique T\n{\n    pass P\n    {\n"
             "        VertexShader = null;\n"
             "        PixelShader = null;\n"
             "%s%s"
             "    }\n}\n", vs ? vs : "", ps ? ps : "");
    FO2ShaEffect *e = fo2_sha_parse(src, strlen(src), err, sizeof err);
    if (!e) printf("  parse error: %s\n", err);
    return e;
}

static int near(float a, float b){ return fabsf(a - b) <= 1e-6f; }

/* A vertex constant case at row 0. */
static void vs_case(const char *desc, const char *line, const float want[4], int want_set){
    FO2ShaEffect *e = make(line, 0);
    float got[4] = {9, 9, 9, 9};
    int set = e ? fo2_sha_pass_vs_const(e, 0, 0, 0, got) : -1;
    int ok = e && set == want_set;
    if (ok && want_set) for (int i = 0; i < 4; i++) ok = ok && near(got[i], want[i]);
    if (ok) ok = e && fo2_sha_pass_vs_const_count(e, 0, 0) == (want_set ? 1u : 0u);
    printf("  case  %s\n", desc);
    chk(ok, desc);
    fo2_sha_free(e);
}

/* A pixel constant case at row 0. */
static void ps_case(const char *desc, const char *line, const float want[4], int want_set){
    FO2ShaEffect *e = make(0, line);
    float got[4] = {9, 9, 9, 9};
    int set = e ? fo2_sha_pass_ps_const(e, 0, 0, 0, got) : -1;
    int ok = e && set == want_set;
    if (ok && want_set) for (int i = 0; i < 4; i++) ok = ok && near(got[i], want[i]);
    if (ok) ok = e && fo2_sha_pass_ps_const_count(e, 0, 0) == (want_set ? 1u : 0u);
    printf("  case  %s\n", desc);
    chk(ok, desc);
    fo2_sha_free(e);
}

static void test_values(void){
    const float plain[4] = {1, 2, 3, 4};
    const float suffix[4] = {0.75f, 0.75f, 0.75f, 0.75f};
    const float upper[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float signed_exp[4] = {-1.5f, 0.2f, -325.0f, 0.5f};
    const float spaced[4] = {1, 2, 3, 4};

    printf("values\n");
    vs_case("vertex without suffix", "VertexShaderConstantF[0] = float4(1, 2, 3, 4);\n", plain, 1);
    ps_case("pixel without suffix", "PixelShaderConstantF[0] = float4(1, 2, 3, 4);\n", plain, 1);
    vs_case("vertex f suffix (shadow color)", "VertexShaderConstantF[0] = float4( 0.75f, 0.75f, 0.75f, 0.75f );\n", suffix, 1);
    vs_case("vertex F suffix", "VertexShaderConstantF[0] = float4(0.75F, 0.75F, 0.75F, 0.75F);\n", suffix, 1);
    ps_case("pixel F suffix", "PixelShaderConstantF[0] = float4(0.1F, 0.2F, 0.3F, 0.4F);\n", upper, 1);
    vs_case("signed/exponent values", "VertexShaderConstantF[0] = float4(-1.5F, +2e-1f, -3.25E+2, .5);\n", signed_exp, 1);
    ps_case("pixel signed/exponent values", "PixelShaderConstantF[0] = float4(-1.5f, +2e-1F, -3.25E+2, .5f);\n", signed_exp, 1);
    vs_case("whitespace around components", "VertexShaderConstantF[0] = float4 ( 1.0 ,\t2 , 3 , 4 );\n", spaced, 1);
    vs_case("trailing semicolon and comment", "VertexShaderConstantF[0] = float4(1f, 2f, 3f, 4f); // color\n", plain, 1);
}

static void test_malformed(void){
    static const char *const bad[] = {
        "float4(1, 2, 3)",                       /* too few */
        "float4(0.75f, 0.75f, 0.75f)",           /* too few after f suffix */
        "float4(1, 2, 3, 4, 5)",                 /* too many */
        "float4(1, 2, 3, 4",                     /* no close paren */
        "float4(1 2 3 4)",                       /* no commas */
        "float4(0.75f 0.75f 0.75f 0.75f)",       /* no commas, f suffix */
        "float4(1, 2, 3, four)",                 /* non-numeric component */
        "float4(1, 2, 3, 4ff)",                  /* repeated suffix */
        "float4(1,,2,3)",                        /* empty component */
        "float4()",                              /* no components */
        "float4(1,2,3,4)junk",                   /* trailing junk */
        "float3(1,2,3,4)",                       /* wrong type */
        "pixel4(1,2,3,4)",                       /* wrong type */
        "float4(0.75g, 0.75, 0.75, 0.75)",       /* bad suffix */
    };
    char line[256];
    printf("malformed values are not recorded\n");
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++){
        snprintf(line, sizeof line, "VertexShaderConstantF[0] = %s;\n", bad[i]);
        vs_case(bad[i], line, 0, 0);
    }
    snprintf(line, sizeof line, "PixelShaderConstantF[0] = %s;\n", bad[0]);
    ps_case("pixel partial float4", line, 0, 0);
}

static void test_ranges(void){
    const float v[4] = {1, 2, 3, 4};
    float got[4] = {0, 0, 0, 0};
    int ok;

    printf("register ranges\n");
    FO2ShaEffect *e = make("VertexShaderConstantF[255] = float4(1,2,3,4);\n", 0);
    ok = e && fo2_sha_pass_vs_const_count(e, 0, 0) == 256;
    ok = ok && fo2_sha_pass_vs_const(e, 0, 0, 255, got);
    for (int i = 0; ok && i < 4; i++) ok = near(got[i], v[i]);
    chk(ok, "vertex highest valid row 255");
    fo2_sha_free(e);

    memset(got, 0, sizeof got);
    e = make(0, "PixelShaderConstantF[31] = float4(1,2,3,4);\n");
    ok = e && fo2_sha_pass_ps_const_count(e, 0, 0) == 32;
    ok = ok && fo2_sha_pass_ps_const(e, 0, 0, 31, got);
    for (int i = 0; ok && i < 4; i++) ok = near(got[i], v[i]);
    chk(ok, "pixel highest valid row 31");
    fo2_sha_free(e);

    memset(got, 0, sizeof got);
    e = make("VertexShaderConstantF[ 3 ] = float4(1,2,3,4);\n", 0);
    ok = e && fo2_sha_pass_vs_const_count(e, 0, 0) == 4;
    ok = ok && fo2_sha_pass_vs_const(e, 0, 0, 3, got);
    for (int i = 0; ok && i < 4; i++) ok = near(got[i], v[i]);
    chk(ok, "whitespace inside register brackets");
    fo2_sha_free(e);

    e = make(
        "VertexShaderConstantF[256] = float4(1,2,3,4);\n"
        "VertexShaderConstantF[-1] = float4(1,2,3,4);\n"
        "VertexShaderConstantF[999999999999999999999] = float4(1,2,3,4);\n",
        "PixelShaderConstantF[32] = float4(1,2,3,4);\n"
        "PixelShaderConstantF[-1] = float4(1,2,3,4);\n"
        "PixelShaderConstantF[999] = float4(1,2,3,4);\n");
    chk(e && fo2_sha_pass_vs_const_count(e, 0, 0) == 0, "out-of-range vertex rows ignored (no negative writes)");
    chk(e && fo2_sha_pass_ps_const_count(e, 0, 0) == 0, "out-of-range pixel rows ignored (no negative writes)");
    fo2_sha_free(e);
}

static void test_independent_and_ordering(void){
    const float v[4] = {5, 6, 7, 8}, p[4] = {9, 10, 11, 12};
    FO2ShaEffect *e = make(
        "VertexShaderConstantF[5] = float4(5f, 6f, 7f, 8f);\n",
        "PixelShaderConstantF[5] = float4(9F, 10F, 11F, 12F);\n");
    float got[4] = {0, 0, 0, 0};
    int ok = e && fo2_sha_pass_vs_const(e, 0, 0, 5, got);
    for (int i = 0; ok && i < 4; i++) ok = near(got[i], v[i]);
    chk(ok, "vertex row 5");
    memset(got, 0, sizeof got);
    ok = e && fo2_sha_pass_ps_const(e, 0, 0, 5, got);
    for (int i = 0; ok && i < 4; i++) ok = near(got[i], p[i]);
    chk(ok, "pixel row 5");
    chk(e && fo2_sha_pass_vs_const_count(e, 0, 0) == 6, "vertex count reflects row 5");
    chk(e && fo2_sha_pass_ps_const_count(e, 0, 0) == 6, "pixel count reflects row 5");
    fo2_sha_free(e);

    e = make(
        "VertexShaderConstantF[0] = float4(1f, 2f, 3f, 4f);\n"
        "VertexShaderConstantF[2] = float4(0.25f, 0.5f, 0.75f, 1f);\n",
        0);
    ok = e && fo2_sha_pass_vs_const_count(e, 0, 0) == 3;
    ok = ok && fo2_sha_pass_vs_const(e, 0, 0, 0, got) && near(got[0], 1) && near(got[3], 4);
    ok = ok && fo2_sha_pass_vs_const(e, 0, 0, 2, got) && near(got[0], 0.25f) && near(got[3], 1);
    chk(ok, "multiple constants in one pass");
    fo2_sha_free(e);
}

static void test_pass_state_samplers(void){
    FO2ShaEffect *e = make(
        "AddressU[0] = Clamp;\n"
        "AddressV[0] = CLAMP; // comment\n"
        "AddressU[1] = clamp ;\n"
        "AddressV[1] = Clamp;//tight comment\n"
        "AddressU[2] = Clamp; // trailing comment\n"
        "AddressV[2] = Clamp; // ; comment\n"
        "AddressU[3] = Clamp;\n"
        "AddressV[3] = Clamp;\n"
        "MagFilter[0] = Linear;\n"
        "MinFilter[0] = LINEAR ; // filter\n"
        "MagFilter[1] = 2; // numeric\n"
        "MinFilter[1] = 2;\n"
        "MipFilter[2] = Point;\n"
        "MipFilter[3] = NONE;\n",
        0);
    FO2ShaPassState st;
    int ok = e && fo2_sha_pass_state(e, 0, 0, &st);
    printf("pass state samplers\n");
    chk(ok, "pass state parsed for sampler addresses and filters");
    if (ok){
        static const struct { unsigned stage, slot; uint32_t value; const char *desc; } f[] = {
            {0, 5, 2, "stage 0 MagFilter Linear"},
            {0, 6, 2, "stage 0 MinFilter LINEAR with comment"},
            {1, 5, 2, "stage 1 MagFilter numeric 2"},
            {1, 6, 2, "stage 1 MinFilter numeric 2"},
            {2, 7, 1, "stage 2 MipFilter Point"},
        };
        char what[80];
        for (unsigned stage = 0; stage < 4; stage++){
            snprintf(what, sizeof what, "stage %u AddressU Clamp", stage);
            chk((st.smp_mask[stage] & (1u << 1)) && st.smp[stage][1] == 3, what);
            snprintf(what, sizeof what, "stage %u AddressV Clamp", stage);
            chk((st.smp_mask[stage] & (1u << 2)) && st.smp[stage][2] == 3, what);
        }
        for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
            chk((st.smp_mask[f[i].stage] & (1u << f[i].slot)) &&
                st.smp[f[i].stage][f[i].slot] == f[i].value, f[i].desc);
        chk((st.smp_mask[3] & (1u << 7)) && st.smp[3][7] == 0,
            "stage 3 MipFilter NONE == 0 with mask preserved");
    }
    fo2_sha_free(e);
}

static void test_pass_state_render(void){
    FO2ShaEffect *e = make(
        "ZEnable = TRUE;\n"
        "ZWriteEnable = FALSE; // no depth write\n"
        "CullMode = cCw;\n"
        "SrcBlend = ONE;\n"
        "DestBlend = ZERO;\n"
        "AlphaBlendEnable = true; // lower case\n"
        "AlphaTestEnable = 0; // numeric zero\n"
        "TextureFactor = 0x11223344; // argb\n",
        0);
    FO2ShaPassState st;
    int ok = e && fo2_sha_pass_state(e, 0, 0, &st);
    printf("pass state render\n");
    chk(ok, "pass state parsed for render states");
    if (ok){
        static const struct { unsigned slot; uint32_t value; const char *desc; } r[] = {
            {7, 1, "ZEnable TRUE"},
            {14, 0, "ZWriteEnable FALSE still masks a zero value"},
            {22, 3, "CullMode cCw case-insensitive"},
            {19, 2, "SrcBlend ONE"},
            {20, 1, "DestBlend ZERO"},
            {27, 1, "AlphaBlendEnable lower-case true"},
            {15, 0, "AlphaTestEnable numeric zero still masks"},
            {60, 0x11223344u, "TextureFactor numeric hex"},
        };
        for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
            chk((st.rs_mask[r[i].slot >> 5] & (1u << (r[i].slot & 31))) &&
                st.rs[r[i].slot] == r[i].value, r[i].desc);
    }
    fo2_sha_free(e);

    e = make("CullMode = NONE;\n", 0);
    ok = e && fo2_sha_pass_state(e, 0, 0, &st);
    chk(ok && (st.rs_mask[22 >> 5] & (1u << (22 & 31))) && st.rs[22] == 1,
        "CullMode NONE remains 1 (D3DCULL_NONE)");
    fo2_sha_free(e);
}

static void test_pass_state_malformed(void){
    FO2ShaEffect *e = make(
        "AddressU[0] = Clampx;\n"
        "AddressV[0] = Clamp junk;\n"
        "AddressU[1] = Bogus;\n"
        "AddressV[1] = 2xyz;\n"
        "MagFilter[2] = ;\n"
        "MinFilter[2] = 2 3;\n"
        "ZEnable = TRUEish;\n"
        "ZWriteEnable = 1x;\n"
        "TextureFactor = 0x;\n",
        0);
    FO2ShaPassState st;
    int ok = e && fo2_sha_pass_state(e, 0, 0, &st);
    printf("pass state malformed values\n");
    chk(ok, "malformed pass state still parses");
    if (ok){
        int clean = 1;
        for (unsigned stage = 0; stage < 4; stage++){
            clean = clean && st.smp_mask[stage] == 0 && st.tss_mask[stage] == 0;
            for (unsigned slot = 0; slot < 20; slot++)
                clean = clean && st.smp[stage][slot] == 0;
        }
        for (unsigned i = 0; i < 8; i++) clean = clean && st.rs_mask[i] == 0;
        for (unsigned i = 0; i < 256; i++) clean = clean && st.rs[i] == 0;
        chk(clean, "malformed symbolic/numeric values are not recorded");
    }
    fo2_sha_free(e);
}

int main(void){
    test_values();
    test_malformed();
    test_ranges();
    test_independent_and_ordering();
    test_pass_state_samplers();
    test_pass_state_render();
    test_pass_state_malformed();
    if (fails) printf("flatout_sha_unit: %d FAILED\n", fails);
    else printf("flatout_sha_unit: all passed\n");
    return fails ? 1 : 0;
}
