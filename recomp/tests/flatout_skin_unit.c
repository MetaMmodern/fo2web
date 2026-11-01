/* Host test: clang -Iruntime tests/flatout_skin_unit.c runtime/flatout_sha.c runtime/vsemu.c -lm -o out/flatout_skin_unit */
#include "flatout_sha.h"
#include "vsemu.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void){
    const char *src =
        "const string inputStreamFormat = \"PosNormColorTex1\";\n"
        "vertexshader vSdr = asm {\n"
        "vs.1.1\n"
        "dcl_position v0\n"
        "mov a0.x, v1.x\n"
        "m4x4 oPos, v0, c[a0.x + 37]\n"
        "};\n"
        "technique T { pass P {\nVertexShader = <vSdr>;\n} }\n";
    char err[160]={0};
    FO2ShaEffect *effect=fo2_sha_parse(src,strlen(src),err,sizeof err);
    if(!effect){fprintf(stderr,"parse: %s\n",err);return 1;}
    size_t n=0; const FO2ShaProgram *program=fo2_sha_pass_program(effect,0,0,0);
    const uint32_t *tokens=fo2_sha_tokens(program,&n);
    VSProg *p=vs_compile(tokens,n);
    if(!p){fprintf(stderr,"vertex compile failed\n");fo2_sha_free(effect);return 1;}
    VSConst k={0}; float v[16][4]={{0}}; VSOut out;
    v[0][0]=2;v[0][1]=3;v[0][2]=4;v[0][3]=1;v[1][0]=4;
    for(int i=0;i<4;i++)k.f[41+i][i]=1;
    int ok=vs_run(p,&k,(const float (*)[4])v,&out);
    for(int i=0;i<4;i++)if(fabsf(out.pos[i]-v[0][i])>0.0001f)ok=0;
    if(!ok)fprintf(stderr,"relative skin matrix failed: %g %g %g %g\n",out.pos[0],out.pos[1],out.pos[2],out.pos[3]);
    vs_free(p);fo2_sha_free(effect);return ok?0:1;
}
