#include "texture_mips.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    assert(tex_mip_count(2048,2048,0)==12);
    assert(tex_mip_count(512,256,0)==10);
    assert(tex_mip_count(4096,1,0)==13);
    assert(tex_mip_count(1,1,0)==1);
    assert(tex_mip_count(7,3,0)==3);
    assert(tex_mip_count(2048,2048,1)==1);
    assert(tex_mip_count(2048,2048,5)==5);
    assert(tex_mip_extent(512,8)==2);
    assert(tex_mip_extent(256,8)==1);
    assert(tex_mip_extent(256,15)==1);
    assert(tex_mip_extent(256,32)==1);
    /* Every lock/child has its own slot within the texture object. None
     * overwrites the core fields or the reserved cube face locks. */
    unsigned char used[TEX_OBJECT_BYTES]={0};
    for (unsigned i=0;i<TEX_MIP_SLOTS;i++){
        unsigned offsets[]={tex_mip_lock_offset(i),tex_mip_child_offset(i)};
        for (unsigned j=0;j<2;j++){
            unsigned o=offsets[j];
            assert(o>=32 && o+4<=TEX_OBJECT_BYTES);
            assert(o<104 || o>=128);
            for (unsigned k=0;k<4;k++){ assert(!used[o+k]); used[o+k]=1; }
        }
    }
    puts("texture mip tests passed");
    return 0;
}
