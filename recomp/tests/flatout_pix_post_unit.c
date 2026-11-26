/* Offline PIX regression: no game, browser, or GPU session.
 * From recomp/:
 * clang -O1 -g -fsanitize=address,undefined -DVSEMU_WGSL -DNFS_ENHANCED=0 \
 *   -Iruntime -Iruntime/web tests/flatout_pix_post_unit.c \
 *   runtime/flatout_sha.c runtime/vsemu.c -lm -o out/flatout_pix_post_unit
 *
 * Literal D3D9 tokens below are from sunny_track_fields.PIXRun shader
 * creation payloads, bound at EIDs 39872, 40044, and 40663. Unlike assembling
 * both sides, these fixtures catch shared assembler/decoder numbering bugs.
 * The numerical expectations are independently calculated from the D3D ops.
 */
#include "flatout_sha.h"
#include "wgsl.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t remap[] = {
    0xffff0101,0x42,0xb00f0000,0x45,0xb00f0001,0xb0e40000,
    0x02,0x800f0000,0xb0e40000,0xb2e40001,
    0x01,0x80080000,0xb0ff0000,0xffff
};
static const uint32_t highpass[] = {
    0xffff0101,0x42,0xb00f0000,
    0x01,0x80070000,0xb0ff0000,0x40000001,0x80080000,0xb0ff0000,
    0x03,0x800f0000,0x80e40000,0xa0e40003,
    0x05,0x820f0000,0x80e40000,0xa0e40004,0xffff
};
static const uint32_t mask[] = {
    0xffff0101,0x42,0xb00f0000,0x42,0xb00f0001,
    0x01,0x80070000,0xb0e40000,
    0x40000005,0x80080000,0xb0ff0001,0xa0ff0003,0xffff
};

static unsigned sample_count;
static void sample(int stage,const float *uv,int n,float out[4]){
    assert(n==2);
    sample_count++;
    if(stage==0){
        out[0]=0.8f;out[1]=0.6f;out[2]=0.4f;out[3]=0.7f;
    }else{
        assert(stage==1);
        /* Independent coordinate-sensitive LUT, checking texreg2ar's AR order. */
        out[0]=uv[0];out[1]=uv[1];out[2]=0.5f;out[3]=0.25f;
    }
}

static void check(const char *label,const uint32_t *original,size_t words,
                  const char *assembly,const PSRefPar *par,const float expected[4],
                  unsigned samples){
    char source[4096],err[256]={0};
    snprintf(source,sizeof source,
             "pixelshader p = asm {\nps.1.1\n%s\n};\n"
             "technique T {\npass P {\nVertexShader = null;\nPixelShader = <p>;\n}\n}\n",
             assembly);
    FO2ShaEffect *e=fo2_sha_parse(source,strlen(source),err,sizeof err);
    if(!e){fprintf(stderr,"%s: %s\n",label,err);abort();}
    size_t n=0;const uint32_t *assembled=fo2_sha_tokens(fo2_sha_pass_program(e,0,0,1),&n);
    assert(assembled && n==words);
    /* Arithmetic instruction length hints and coissue flags do not change
     * these shaders' semantics: coissued writes have disjoint RGB/A masks and
     * neither reads the other instruction's destination. */
    for(size_t i=0;i<n;i++){
        uint32_t a=assembled[i],b=original[i];
        if(i && !(a&0x80000000u) && !(b&0x80000000u)){
            a&=0xffffu;b&=0xffffu;
        }
        assert(a==b);
    }
    const uint32_t *tokens[2]={original,assembled};
    size_t lengths[2]={words,n};
    for(int side=0;side<2;side++){
        VSProg *p=vs_compile(tokens[side],lengths[side]);assert(p);
        char *text=malloc(65536);assert(text);
        assert(ps_to_wgsl(p,17,text,65536)>0);
        assert(strstr(text,"opaque_mask: f32"));
        assert(strstr(text,"u32(P.opaque_mask) & 1u"));
        assert(strstr(text,"u32(P.opaque_mask) & 8u"));
        assert(strstr(text,"t0 = sample0(tc0)"));
        if(original==remap)assert(strstr(text,"sample1(vec2f("));
        if(original==mask)assert(strstr(text,"t1 = sample1(tc1)"));
        /* Cube variants apply the same per-stage format conversion. */
        assert(ps_to_wgsl_ex(p,17,text,65536,2u)>0);
        assert(strstr(text,"fn sample1(q: vec3f)"));
        assert(strstr(text,"u32(P.opaque_mask) & 2u"));
        assert(ps_to_msl(p,17,text,65536)>0);
        free(text);
        PSRefIn in={0};in.fog=1;
        float out[4];int killed=0;sample_count=0;
        assert(ps_ref_run(p,par,&in,sample,0,out,&killed));
        assert(!killed && sample_count==samples);
        for(int k=0;k<4;k++)assert(fabsf(out[k]-expected[k])<1e-6f);
        vs_free(p);
    }
    fo2_sha_free(e);
    printf("%s: original tokens, assembled tokens, WGSL/MSL emission, arithmetic passed\n",label);
}

int main(void){
    PSRefPar par={0};
    const float remapped[4]={1.f,0.9f,0.4f,0.7f};
    check("Fields EID 39872 colour remap",remap,sizeof remap/4,
          "tex t0\ntexreg2ar t1, t0\nadd r0, t0, t1_bias\nmov r0.a, t0.a",
          &par,remapped,2);
    /* Correct payload starts at args[11], not args[10] (reserved word). */
    par.c[3][0]=par.c[3][1]=par.c[3][2]=0.24f;
    par.c[4][0]=par.c[4][1]=par.c[4][2]=0.42763155698776245f;
    const float extracted[4]={0.786842144f,0.786842144f,0.786842144f,0};
    check("Fields EID 40044 luminance high-pass",highpass,sizeof highpass/4,
          "tex t0\nmov r0.rgb, t0.a\n+mov r0.a, t0.a\n"
          "sub r0, r0, c3\nmul_x4 r0, r0, c4",
          &par,extracted,1);
    par.c[3][0]=par.c[3][1]=par.c[3][2]=1.f;par.c[3][3]=0.09375f;
    const float masked[4]={0.8f,0.6f,0.4f,0.0234375f};
    check("Fields EID 40663 final mask",mask,sizeof mask/4,
          "tex t0\ntex t1\nmov r0.rgb, t0\n+mul r0.a, t1.a, c3.a",
          &par,masked,2);
    return 0;
}
