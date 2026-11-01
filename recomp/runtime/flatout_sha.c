#include "flatout_sha.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

enum { RTEMP=0,RIN=1,RCONST=2,RADDR=3,RRAST=4,RATTR=5,ROUT=6,RSAMP=10 };
enum { MOV=1,ADD,SUB,MAD,MUL,RCP,RSQ,DP3,DP4,MIN,MAX,SLT,SGE,LRP=18,FRC=19,M4X4=20,M4X3,M3X4,M3X3,DCL=31,ABS=35,NRM=36,TEXCOORD=64,TEXKILL,TEX,TEXREG2AR,END=0xffff };
typedef struct { char *name,*vsn,*psn,*tex[16]; FO2ShaProgram *vs,*ps; uint32_t vm[256],pm[32]; float vc[256][4],pc[32][4]; FO2ShaPassState st; } Pass;
typedef struct { char *name; Pass *p; unsigned n,cap; } Tech;
struct FO2ShaEffect { char *fmt; FO2ShaProgram *prog; unsigned np,pcap; Tech *tech; unsigned nt,tcap; };

static char *dup_n(const char *s,size_t n){char *p=malloc(n+1);if(!p)return 0;memcpy(p,s,n);p[n]=0;return p;}
static void trim(char *s){size_t n=strlen(s),i=0;while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;while(isspace((unsigned char)s[i]))i++;if(i)memmove(s,s+i,n-i+1);}
static char *find_ci(char *s,const char *k){size_t n=strlen(k);for(;*s;s++){size_t i=0;while(i<n&&s[i]&&tolower((unsigned char)s[i])==tolower((unsigned char)k[i]))i++;if(i==n){for(size_t j=0;j<n;j++)s[j]=(char)tolower((unsigned char)s[j]);return s;}}return 0;}
static void errorf(char *e,size_t n,const char *m){if(n){strncpy(e,m,n-1);e[n-1]=0;}}
static int push(FO2ShaProgram *p,uint32_t x){if(p->n==p->cap){size_t c=p->cap?p->cap*2:128;void *q=realloc(p->tok,c*4);if(!q)return 0;p->tok=q;p->cap=c;}p->tok[p->n++]=x;return 1;}
static int rtype(const char *s,int pixel){if(s[0]=='v')return RIN;if(s[0]=='r')return RTEMP;if(s[0]=='c')return RCONST;if(!pixel&&s[0]=='a')return RADDR;if(s[0]=='t')return pixel?RADDR:ROUT;if(s[0]=='s')return RSAMP;if(!strncasecmp(s,"oPos",4))return RRAST;if(!strncasecmp(s,"oD",2))return RATTR;if(!strncasecmp(s,"oT",2))return ROUT;return 0;}
static uint32_t regtok(const char *in,int dst,int pixel){
    char s[64];strncpy(s,in,sizeof(s)-1);s[sizeof(s)-1]=0;
    int smod=0;
    if (!strncasecmp(s,"1-",2)){ smod=6; memmove(s,s+2,strlen(s+2)+1); }
    else if (s[0]=='-'){ smod=1; memmove(s,s+1,strlen(s+1)+1); }
    /* vs_1_1 uses implicit a0.x for relative constants; the dot inside
     * c[a0.x + N] is not a source swizzle. */
    int relative=0, base=0, consumed=0;
    if(!dst&&!pixel&&sscanf(s,"c[a0.x + %d]%n",&base,&consumed)==1 &&
       consumed>0 && base>=0 && base<2048 && (!s[consumed] || s[consumed]=='.')){
        relative=1;
        char tail[64]; snprintf(tail,sizeof tail,"c%d%s",base,s+consumed);
        memcpy(s,tail,strlen(tail)+1);
    }
    char *d=strchr(s,'.'),*w=d?d+1:0;if(d)*d=0;
    char *m=strchr(s,'_');
    if (m){
        *m++=0;
        if (!strcasecmp(m,"bias")) smod=2;
        else if (!strcasecmp(m,"bx2")) smod=4;
        else if (!strcasecmp(m,"x2")) smod=7;
        else if (!strcasecmp(m,"comp")) smod=6;
    }
    int ty=rtype(s,pixel),num=0;
    for(char *q=s;*q;q++)if(isdigit((unsigned char)*q)){num=atoi(q);break;}
    uint32_t t=0x80000000u|((ty&7u)<<28)|((ty&0x18u)<<8)|(unsigned)num;
    if(relative)t|=0x00002000u;
    if(dst)t|=15u<<16;else t|=0xe4u<<16;
    if(w&&*w){
        size_t n=strlen(w);
        if(dst){
            /* Destination suffixes are write masks (xy -> bits 0 and 1),
             * while source suffixes below are 2-bit component selectors. */
            unsigned mask=0;
            for(size_t i=0;i<n;i++){
                switch(w[i]){
                    case 'x': case 'r': mask|=1u; break;
                    case 'y': case 'g': mask|=2u; break;
                    case 'z': case 'b': mask|=4u; break;
                    case 'w': case 'a': mask|=8u; break;
                    default: break;
                }
            }
            t=(t&~(15u<<16))|(mask<<16);
        }else{
            unsigned sw=0;
            for(int i=0;i<4;i++){
                char c=w[i<n?i:n-1];
                sw|=(unsigned)((c=='x'||c=='r')?0:(c=='y'||c=='g')?1:(c=='z'||c=='b')?2:3)<<(i*2);
            }
            t=(t&~(255u<<16))|(sw<<16);
        }
    }
    if(!dst)t|=((uint32_t)smod&15u)<<24;
    return t;
}
static unsigned op(const char *s){if(!strcasecmp(s,"mov"))return MOV;if(!strcasecmp(s,"add"))return ADD;if(!strcasecmp(s,"sub"))return SUB;if(!strcasecmp(s,"mad"))return MAD;if(!strcasecmp(s,"mul"))return MUL;if(!strcasecmp(s,"rcp"))return RCP;if(!strcasecmp(s,"rsq"))return RSQ;if(!strcasecmp(s,"dp3"))return DP3;if(!strcasecmp(s,"dp4"))return DP4;if(!strcasecmp(s,"min"))return MIN;if(!strcasecmp(s,"max"))return MAX;if(!strcasecmp(s,"slt"))return SLT;if(!strcasecmp(s,"sge"))return SGE;if(!strcasecmp(s,"lrp"))return LRP;if(!strcasecmp(s,"frc"))return FRC;if(!strcasecmp(s,"m4x4"))return M4X4;if(!strcasecmp(s,"m4x3"))return M4X3;if(!strcasecmp(s,"m3x3"))return M3X3;if(!strcasecmp(s,"abs"))return ABS;if(!strcasecmp(s,"nrm"))return NRM;if(!strcasecmp(s,"tex"))return TEX;if(!strcasecmp(s,"texcoord"))return TEXCOORD;if(!strcasecmp(s,"texkill"))return TEXKILL;if(!strcasecmp(s,"texreg2ar"))return TEXREG2AR;if(!strcasecmp(s,"dcl"))return DCL;return 0;}
static unsigned usage(const char *s){if(!strncasecmp(s,"dcl_position",12))return 0;if(!strncasecmp(s,"dcl_blendweight",15))return 1;if(!strncasecmp(s,"dcl_blendindices",15))return 2;if(!strncasecmp(s,"dcl_normal",10))return 3;if(!strncasecmp(s,"dcl_texcoord",12))return 5|((unsigned)atoi(s+12)<<16);if(!strncasecmp(s,"dcl_color",9))return 10;return 0;}
/* Parse one effect constant initializer: float4(x, y, z, w).  Each component
 * is a C-style float with optional whitespace and sign; shipped .sha files
 * also suffix components with f/F (float4(0.75f, 0.75f, 0.75f, 0.75f)), which
 * plain "%f" parsing silently truncates at the suffix.  Return 1 only for a
 * complete four-component value, optionally followed by a semicolon and/or a
 * //-comment, so partial or malformed initializers are not recorded. */
static int parse_float4(const char *s,float v[4]){
    while(isspace((unsigned char)*s))s++;
    if(strncasecmp(s,"float4",6))return 0;
    s+=6;
    while(isspace((unsigned char)*s))s++;
    if(*s++!='(')return 0;
    for(int i=0;i<4;i++){
        while(isspace((unsigned char)*s))s++;
        char *end=0;
        float f=strtof(s,&end);
        if(end==s)return 0;
        v[i]=f;
        s=end;
        if(*s=='f'||*s=='F')s++;
        if(i<3){
            while(isspace((unsigned char)*s))s++;
            if(*s++!=',')return 0;
        }
    }
    while(isspace((unsigned char)*s))s++;
    if(*s++!=')')return 0;
    while(isspace((unsigned char)*s))s++;
    if(*s==';'){s++;while(isspace((unsigned char)*s))s++;}
    if(s[0]=='/'&&s[1]=='/')return 1;
    return *s==0;
}
/* Parse one pass state value.  The value in a pass body keeps the line's
 * trailing semicolon and optional // comment, so normalise those away before
 * looking at the token.  Symbolic values are matched case-insensitively; a
 * numeric value must consume the whole token, otherwise malformed input is
 * rejected instead of being turned into an unrelated state value (the old
 * strtol("Clamp;") path silently produced 0).
 *
 * NONE is context-sensitive: D3DTEXF_NONE is 0 and is only valid for the mip
 * filter, while D3DCULL_NONE is 1.  mipfilter selects the filter meaning. */
static int state_value(const char *s,uint32_t *out,int mipfilter){
    const char *end=s+strlen(s);
    for(const char *q=s;q+1<end;q++)if(q[0]=='/'&&q[1]=='/'){end=q;break;}
    while(s<end&&isspace((unsigned char)*s))s++;
    while(end>s&&isspace((unsigned char)end[-1]))end--;
    if(end>s&&end[-1]==';'){end--;while(end>s&&isspace((unsigned char)end[-1]))end--;}
    size_t n=(size_t)(end-s);
    if(!n)return 0;
#define SV(lit,val) if(n==sizeof(lit)-1&&!strncasecmp(s,lit,n)){*out=(uint32_t)(val);return 1;}
    SV("TRUE",1) SV("FALSE",0)
    SV("ONE",2) SV("ZERO",1)
    SV("SRCALPHA",5) SV("INVSRCALPHA",6)
    SV("DESTALPHA",7) SV("INVDESTALPHA",8)
    SV("SRCOLOR",3) SV("INVSRCCOLOR",4)
    SV("DESTCOLOR",9) SV("INVDESTCOLOR",10)
    SV("ADD",1) SV("SUBTRACT",2)
    SV("REVSUBTRACT",3) SV("MODULATE",4)
    SV("SELECTARG1",2) SV("SELECTARG2",3)
    SV("CURRENT",1) SV("DIFFUSE",0)
    SV("TEXTURE",2) SV("TFactor",3)
    SV("SPECULAR",4)
    SV("DISABLE",1) SV("CLAMP",3)
    SV("WRAP",1) SV("POINT",1)
    SV("LINEAR",2) SV("NONE",mipfilter?0:1)
    SV("LESSEQUAL",4) SV("CCW",3)
    SV("CW",2)
#undef SV
    char b[64];
    if(n>=sizeof b)return 0;
    memcpy(b,s,n);b[n]=0;
    char *e=0;long v=strtol(b,&e,0);
    if(e!=b+n)return 0;
    *out=(uint32_t)v;return 1;
}
static void st_smp(FO2ShaPassState *st,unsigned stage,unsigned slot,uint32_t v){st->smp[stage][slot]=v;st->smp_mask[stage]|=1u<<slot;}
static void st_tss(FO2ShaPassState *st,unsigned stage,unsigned slot,uint32_t v){st->tss[stage][slot]=v;st->tss_mask[stage]|=1ull<<slot;}
static void st_rs(FO2ShaPassState *st,unsigned slot,uint32_t v){st->rs[slot]=v;st->rs_mask[slot>>5]|=1u<<(slot&31);}
static int pass_state(Pass *p,const char *lhs,const char *rhs){
    int stage=-1,slot=-1;uint32_t v;
    if(sscanf(lhs,"AddressU[%d]",&stage)==1) slot=1;
    else if(sscanf(lhs,"AddressV[%d]",&stage)==1) slot=2;
    else if(sscanf(lhs,"MagFilter[%d]",&stage)==1) slot=5;
    else if(sscanf(lhs,"MinFilter[%d]",&stage)==1) slot=6;
    else if(sscanf(lhs,"MipFilter[%d]",&stage)==1) slot=7;
    else if(sscanf(lhs,"TexCoordIndex[%d]",&stage)==1) slot=11;
    else if(!strcasecmp(lhs,"AddressU")){stage=0;slot=1;}
    else if(!strcasecmp(lhs,"AddressV")){stage=0;slot=2;}
    if(stage>=0 && stage<4 && slot>=0){ if(state_value(rhs,&v,slot==7)){st_smp(&p->st,(unsigned)stage,(unsigned)slot,v);if(slot==11)st_tss(&p->st,(unsigned)stage,11,v);} return 1; }
    stage=-1; slot=-1;
    if(sscanf(lhs,"ColorOp[%d]",&stage)==1) slot=1;
    else if(sscanf(lhs,"ColorArg1[%d]",&stage)==1) slot=2;
    else if(sscanf(lhs,"ColorArg2[%d]",&stage)==1) slot=3;
    else if(sscanf(lhs,"AlphaOp[%d]",&stage)==1) slot=4;
    else if(sscanf(lhs,"AlphaArg1[%d]",&stage)==1) slot=5;
    else if(sscanf(lhs,"AlphaArg2[%d]",&stage)==1) slot=6;
    if(stage>=0 && stage<4 && slot>=0){if(state_value(rhs,&v,0))st_tss(&p->st,(unsigned)stage,(unsigned)slot,v);return 1;}
    if(!strncasecmp(lhs,"TextureFactor",13)){if(state_value(rhs,&v,0))st_rs(&p->st,60,v);return 1;}
    if(!strcasecmp(lhs,"SrcBlend")){if(state_value(rhs,&v,0))st_rs(&p->st,19,v);return 1;}
    if(!strcasecmp(lhs,"DestBlend")){if(state_value(rhs,&v,0))st_rs(&p->st,20,v);return 1;}
    if(!strcasecmp(lhs,"AlphaBlendEnable")){if(state_value(rhs,&v,0))st_rs(&p->st,27,v);return 1;}
    if(!strcasecmp(lhs,"ZWriteEnable")){if(state_value(rhs,&v,0))st_rs(&p->st,14,v);return 1;}
    if(!strcasecmp(lhs,"ZEnable")){if(state_value(rhs,&v,0))st_rs(&p->st,7,v);return 1;}
    if(!strcasecmp(lhs,"CullMode")){if(state_value(rhs,&v,0))st_rs(&p->st,22,v);return 1;}
    if(!strcasecmp(lhs,"AlphaTestEnable")){if(state_value(rhs,&v,0))st_rs(&p->st,15,v);return 1;}
    return 0;
}
static int assemble(FO2ShaProgram *p,const char *src,int pixel,char *err,size_t ec){
    char *b=dup_n(src,strlen(src));
    if(!b)return 0;
    push(p,pixel?0xffff0101u:0xfffe0101u);
    for(char *ln=b;ln;){
        char *nx=strchr(ln,'\n');if(nx)*nx++=0;
        char *c=strstr(ln,"//");if(c)*c=0;c=strchr(ln,';');if(c)*c=0;
        trim(ln);if(!*ln){ln=nx;continue;}
        char *sp=ln;while(*sp&&!isspace((unsigned char)*sp))sp++;
        if(!*sp){ln=nx;continue;}
        *sp++=0;while(isspace((unsigned char)*sp))sp++;
        if(!strncasecmp(ln,"vs.",3)||!strncasecmp(ln,"ps.",3)){ln=nx;continue;}
        if(*ln=='+')ln++;

        char *a[5]={0};int na=0;
        for(char *q=sp;q&&*q&&na<5;){char *z=strchr(q,',');if(z)*z=0;trim(q);a[na++]=q;q=z?z+1:0;}

        /* D3D's dcl_* spellings carry the vertex usage in the mnemonic.
         * Do not split at '_' as we do for arithmetic modifiers below. */
        if(!strncasecmp(ln,"dcl_",4)){
            if(na<1||!push(p,DCL|(2u<<24))||!push(p,usage(ln))||!push(p,regtok(a[0],1,pixel))){free(b);return 0;}
            ln=nx;continue;
        }

        unsigned sat=0,shift=0;char *u=strchr(ln,'_');
        if(u){sat=strstr(u,"sat")!=0;shift=strstr(u,"x4")?2:strstr(u,"x2")?1:strstr(u,"d2")?15:strstr(u,"d4")?14:0;*u=0;}
        unsigned code=op(ln);
        if(!code){char m[128];snprintf(m,sizeof m,"unsupported shader opcode %s",ln);errorf(err,ec,m);free(b);return 0;}
        int n=(code==TEX||code==TEXCOORD||code==TEXKILL)?1:na;
        if(!push(p,code|((unsigned)n<<24))){free(b);return 0;}
        for(int i=0;i<na;i++){
            int isdst = i==0&&code!=TEXCOORD&&code!=TEXKILL;
            uint32_t t=regtok(a[i],isdst,pixel);
            if(isdst&&sat)t|=1u<<20;
            if(isdst&&shift)t|=shift<<24;
            if(!push(p,t)){free(b);return 0;}
        }
        ln=nx;
    }
    push(p,END);free(b);return 1;
}
static char *block(char *s,char **end){char *b=strchr(s,'{');if(!b)return 0;int d=1;char *q=b+1;while(*q&&d){if(*q=='{')d++;else if(*q=='}')d--;q++;}if(d)return 0;*end=q;return dup_n(b+1,(size_t)(q-b-2));}
static FO2ShaProgram *lookup(FO2ShaEffect *e,const char *n){for(unsigned i=0;i<e->np;i++)if(!strcasecmp(e->prog[i].name,n))return &e->prog[i];return 0;}
/* post_box4.fx is the only shipped .sha written in HLSL: its pass names a
 * compiled ps_1_1 function instead of an asm block, so the generic asm
 * collector above never sees a pixel program for it.  Recognise exactly the
 * four-sample average body and feed the D3D compiler's ps_1_1 output through
 * assemble(), so the bloom chain gets a real program instead of NULL.  Any
 * other compile form fails the parse with a message: a pass is never left
 * silently without the pixel program it declares. */
static char *ps_body(char *src,const char *fn){
    for(char *p=src;(p=find_ci(p,fn));p+=strlen(fn)){
        char *q=p+strlen(fn);while(isspace((unsigned char)*q))q++;
        if(*q!='(')continue;
        int d=0;char *r=q;
        for(;*r;r++){if(*r=='(')d++;else if(*r==')'&&!--d)break;}
        if(!*r)continue;
        for(r++;*r&&*r!='{'&&*r!=';';r++);
        if(*r!='{')continue;
        char *z=0,*b=block(r,&z);
        if(b)return b;
    }
    return 0;
}
static int ps_body_ok(char *b){
    unsigned nt=0,plus=0,mul=0;
    for(char *q=b;*q;q++){
        if(!strncasecmp(q,"tex2d",5))nt++;
        else if(*q=='+')plus++;
        else if(*q=='*'){char *e=0;mul++;if(strtof(q+1,&e)!=0.25f||e==q+1)return 0;}
    }
    if(nt!=4||plus!=3||mul!=1)return 0;
    for(int i=0;i<4;i++){char n[16];snprintf(n,sizeof n,"g_sampler%d",i);if(!find_ci(b,n))return 0;}
    return 1;
}
static int ps_samplers(Pass *x,char *src){
    for(char *p=src;(p=find_ci(p,"sampler2d"));){
        char name[64]={0};int stage=-1;
        if(sscanf(p,"%*s %63[A-Za-z0-9_]",name)==1)
            for(int i=0;i<4;i++){char n[16];snprintf(n,sizeof n,"g_sampler%d",i);if(!strcasecmp(name,n)){stage=i;break;}}
        if(stage<0){p+=9;continue;}
        char *z=0,*b=block(p,&z);
        if(!b){p+=9;continue;}
        for(char *ln=b;ln;){
            char *nx=strchr(ln,'\n');if(nx)*nx++=0;
            char *eq=strchr(ln,'=');
            if(eq){*eq++=0;trim(ln);trim(eq);
                int slot=-1;uint32_t v;
                if(!strcasecmp(ln,"Texture")){
                    char *a=strchr(eq,'<'),*end=a?strchr(a+1,'>'):0;
                    if(a&&end){char *tex=dup_n(a+1,(size_t)(end-a-1));if(!tex){free(b);return 0;}free(x->tex[stage]);x->tex[stage]=tex;}
                }
                else if(!strcasecmp(ln,"AddressU"))slot=1;
                else if(!strcasecmp(ln,"AddressV"))slot=2;
                else if(!strcasecmp(ln,"MagFilter"))slot=5;
                else if(!strcasecmp(ln,"MinFilter"))slot=6;
                else if(!strcasecmp(ln,"MipFilter"))slot=7;
                if(slot>=0&&state_value(eq,&v,slot==7))st_smp(&x->st,stage,(unsigned)slot,v);
            }
            ln=nx;
        }
        free(b);p=z;
    }
    return 1;
}
static const char ps11[]="ps.1.1\ntex t0\ntex t1\ntex t2\ntex t3\nadd r0, t0, t1\nadd r0, r0, t2\nadd_d4 r0, r0, t3\n";
static int hlsl_ps(FO2ShaEffect *e,Pass *x,char *rhs,char *src,char *err,size_t ec){
    char tgt[16]={0},fn[64]={0};
    if(sscanf(rhs,"compile %15s %63[A-Za-z0-9_]",tgt,fn)!=2){errorf(err,ec,"malformed HLSL pixel shader declaration");return 0;}
    if(strcasecmp(tgt,"ps_1_1")){char m[96];snprintf(m,sizeof m,"unsupported HLSL pixel shader target %s",tgt);errorf(err,ec,m);return 0;}
    char *b=ps_body(src,fn);
    if(!b){char m[96];snprintf(m,sizeof m,"HLSL pixel shader function %s not found",fn);errorf(err,ec,m);return 0;}
    int ok=ps_body_ok(b);free(b);
    if(!ok){errorf(err,ec,"unsupported HLSL ps_1_1 body (expected four-sample average)");return 0;}
    if(!lookup(e,fn)){
        if(e->np==e->pcap){unsigned c=e->pcap?e->pcap*2:8;void *q=realloc(e->prog,c*sizeof *e->prog);if(!q){errorf(err,ec,"out of memory");return 0;}e->prog=q;e->pcap=c;}
        FO2ShaProgram *g=&e->prog[e->np];memset(g,0,sizeof *g);g->name=dup_n(fn,strlen(fn));e->np++;
        if(!g->name||!assemble(g,ps11,1,err,ec)){free(g->name);free(g->tok);e->np--;return 0;}
    }
    free(x->psn);x->psn=dup_n(fn,strlen(fn));
    if(!ps_samplers(x,src)){errorf(err,ec,"out of memory");return 0;}
    return 1;
}
static int shaders(FO2ShaEffect *e,char *s,const char *kind,int pix,char *err,size_t ec){for(char *p=s;(p=find_ci(p,kind));){char n[64]={0};if(sscanf(p,"%*s %63s",n)!=1){p+=strlen(kind);continue;}char *a=find_ci(p,"asm"),*z=0,*body=a?block(a,&z):0;if(!body){p+=strlen(kind);continue;}if(e->np==e->pcap){unsigned c=e->pcap?e->pcap*2:8;e->prog=realloc(e->prog,c*sizeof *e->prog);e->pcap=c;}FO2ShaProgram *g=&e->prog[e->np++];memset(g,0,sizeof *g);g->name=dup_n(n,strlen(n));if(!assemble(g,body,pix,err,ec)){free(body);return 0;}free(body);p=z;}return 1;}
static int techniques(FO2ShaEffect *e,char *s,char *err,size_t ec){for(char *p=s;(p=find_ci(p,"technique"));){char n[64]={0};if(sscanf(p,"technique %63s",n)!=1){p+=9;continue;}char *te=0,*tb=block(p,&te);if(!tb)return 0;if(e->nt==e->tcap){unsigned c=e->tcap?e->tcap*2:4;e->tech=realloc(e->tech,c*sizeof *e->tech);e->tcap=c;}Tech *t=&e->tech[e->nt++];memset(t,0,sizeof *t);t->name=dup_n(n,strlen(n));for(char *q=tb;(q=find_ci(q,"pass"));){char pn[64]={0};if(sscanf(q,"pass %63s",pn)!=1){q+=4;continue;}char *pe=0,*pb=block(q,&pe);if(!pb)break;if(t->n==t->cap){unsigned c=t->cap?t->cap*2:4;t->p=realloc(t->p,c*sizeof *t->p);t->cap=c;}Pass *x=&t->p[t->n++];memset(x,0,sizeof *x);x->name=dup_n(pn,strlen(pn));for(char *ln=pb;ln;){char *nx=strchr(ln,'\n');if(nx)*nx++=0;char *eq=strchr(ln,'=');if(eq){*eq++=0;trim(ln);trim(eq);pass_state(x,ln,eq);if(!strncasecmp(ln,"Texture[",8)){int st=atoi(ln+8);char *a=strchr(eq,'<'),*b=a?strchr(a+1,'>'):0;if(a&&b&&st>=0&&st<16)x->tex[st]=dup_n(a+1,(size_t)(b-a-1));}else if(!strcasecmp(ln,"PixelShader")&&!strncasecmp(eq,"compile",7)){if(!hlsl_ps(e,x,eq,s,err,ec)){free(pb);return 0;}}else if(!strcasecmp(ln,"VertexShader")||!strcasecmp(ln,"PixelShader")){char *a=strchr(eq,'<'),*b=a?strchr(a+1,'>'):0;if(a&&b){char **dst=!strcasecmp(ln,"VertexShader")?&x->vsn:&x->psn;*dst=dup_n(a+1,(size_t)(b-a-1));}}else if(!strncasecmp(ln,"VertexShaderConstantF[",21)||!strncasecmp(ln,"PixelShaderConstantF[",20)){int pix=!strncasecmp(ln,"Pixel",5),r=-1;char *ie=strchr(ln,'[');if(ie){char *re=0;long n=strtol(ie+1,&re,10);if(re!=ie+1){while(isspace((unsigned char)*re))re++;if(*re==']'&&n>=0&&n<0x10000)r=(int)n;}}float v[4];if(parse_float4(eq,v)){if(pix&&r>=0&&r<32){x->pm[r]=1;memcpy(x->pc[r],v,16);}else if(!pix&&r>=0&&r<256){x->vm[r]=1;memcpy(x->vc[r],v,16);}}}}ln=nx;}free(pb);q=pe;}free(tb);p=te;}for(unsigned i=0;i<e->nt;i++)for(unsigned j=0;j<e->tech[i].n;j++){Pass *x=&e->tech[i].p[j];if(x->vsn&&strcasecmp(x->vsn,"null"))x->vs=lookup(e,x->vsn);if(x->psn&&strcasecmp(x->psn,"null"))x->ps=lookup(e,x->psn);if((x->vsn&&strcasecmp(x->vsn,"null")&&!x->vs)||(x->psn&&strcasecmp(x->psn,"null")&&!x->ps)){errorf(err,ec,"unresolved shader reference");return 0;}}return 1;}
FO2ShaEffect *fo2_sha_parse(const char *src,size_t len,char *err,size_t ec){char *s=dup_n(src,len);FO2ShaEffect *e=calloc(1,sizeof *e);if(!s||!e)return 0;char *f=find_ci(s,"inputStreamFormat"),*a=f?strchr(f,'"'):0,*b=a?strchr(a+1,'"'):0;if(a&&b)e->fmt=dup_n(a+1,(size_t)(b-a-1));if(!shaders(e,s,"vertexshader",0,err,ec)||!shaders(e,s,"pixelshader",1,err,ec)||!techniques(e,s,err,ec)){fo2_sha_free(e);free(s);return 0;}free(s);return e;}
void fo2_sha_free(FO2ShaEffect *e){if(!e)return;free(e->fmt);for(unsigned i=0;i<e->np;i++){free(e->prog[i].name);free(e->prog[i].tok);}free(e->prog);for(unsigned i=0;i<e->nt;i++){free(e->tech[i].name);for(unsigned j=0;j<e->tech[i].n;j++){Pass *p=&e->tech[i].p[j];free(p->name);free(p->vsn);free(p->psn);for(int k=0;k<16;k++)free(p->tex[k]);}free(e->tech[i].p);}free(e->tech);free(e);}
unsigned fo2_sha_technique_count(const FO2ShaEffect *e){return e?e->nt:0;}unsigned fo2_sha_pass_count(const FO2ShaEffect *e,unsigned t){return e&&t<e->nt?e->tech[t].n:0;}static const Pass *gp(const FO2ShaEffect *e,unsigned t,unsigned p){return e&&t<e->nt&&p<e->tech[t].n?&e->tech[t].p[p]:0;}
const FO2ShaProgram *fo2_sha_pass_program(const FO2ShaEffect *e,unsigned t,unsigned p,int pix){const Pass*x=gp(e,t,p);return x?(pix?x->ps:x->vs):0;}const char *fo2_sha_pass_texture(const FO2ShaEffect *e,unsigned t,unsigned p,unsigned s){const Pass*x=gp(e,t,p);return x&&s<16?x->tex[s]:0;}
unsigned fo2_sha_pass_vs_const_count(const FO2ShaEffect*e,unsigned t,unsigned p){const Pass*x=gp(e,t,p);unsigned n=0;if(x)for(int i=0;i<256;i++)if(x->vm[i])n=i+1;return n;}int fo2_sha_pass_vs_const(const FO2ShaEffect*e,unsigned t,unsigned p,unsigned r,float o[4]){const Pass*x=gp(e,t,p);if(!x||r>=256||!x->vm[r])return 0;memcpy(o,x->vc[r],16);return 1;}unsigned fo2_sha_pass_ps_const_count(const FO2ShaEffect*e,unsigned t,unsigned p){const Pass*x=gp(e,t,p);unsigned n=0;if(x)for(int i=0;i<32;i++)if(x->pm[i])n=i+1;return n;}int fo2_sha_pass_ps_const(const FO2ShaEffect*e,unsigned t,unsigned p,unsigned r,float o[4]){const Pass*x=gp(e,t,p);if(!x||r>=32||!x->pm[r])return 0;memcpy(o,x->pc[r],16);return 1;}
int fo2_sha_pass_state(const FO2ShaEffect*e,unsigned t,unsigned p,FO2ShaPassState*out){const Pass*x=gp(e,t,p);if(!x||!out)return 0;memcpy(out,&x->st,sizeof *out);return 1;}
const char *fo2_sha_input_format(const FO2ShaEffect*e){return e?e->fmt:0;}const uint32_t *fo2_sha_tokens(const FO2ShaProgram*p,size_t*n){if(n)*n=p?p->n:0;return p?p->tok:0;}int fo2_sha_program_is_null(const FO2ShaProgram*p){return !p;}
