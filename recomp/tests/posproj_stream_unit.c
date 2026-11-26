/* Host-only regression for bloom extraction's PosprojTex4/24-byte mismatch.
 * clang -Wall -Wextra -Werror -Iruntime tests/posproj_stream_unit.c -o out/posproj_stream_unit
 */
#include "posproj_stream.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    /* Original Fields PIXRun EID 39710: full-size POSITIONT quad. */
    assert(fo2_posproj_clip_x(-0.5f,3072,1.f)==-1.f);
    assert(fo2_posproj_clip_x(3071.5f,3072,1.f)==1.f);
    assert(fo2_posproj_clip_y(-0.5f,1920,1.f)==1.f);
    assert(fo2_posproj_clip_y(1919.5f,1920,1.f)==-1.f);
    /* Integer D3D9 pixel centres map to half-integer modern pixel centres. */
    assert(fo2_posproj_clip_x(0.f,256,1.f)==-1.f+1.f/256.f);
    assert(fo2_posproj_clip_y(0.f,256,1.f)==1.f-1.f/256.f);
    assert(fo2_posproj_clip_x(255.5f,256,2.f)==2.f);
    assert(fo2_posproj_clip_y(255.5f,256,2.f)==-2.f);
    /* sub_005a8700: FVF 0x104, four XYZRHW+float2 vertices. */
    assert(fo2_posproj_stream_uvs(4,24,5,4)==1);
    /* Copy, four-tap blur, and radial blur paths remain pretransformed. */
    assert(fo2_posproj_stream_uvs(1,24,5,4)==1);
    assert(fo2_posproj_stream_uvs(4,48,5,4)==4);
    assert(fo2_posproj_stream_uvs(4,48,5,24)==4);
    assert(fo2_posproj_stream_uvs(4,24,4,6)==1);
    assert(fo2_posproj_stream_uvs(4,24,6,4)==1);
    /* No active null-VS post effect, wrong layout, lines, or excess vertices. */
    assert(!fo2_posproj_stream_uvs(0,24,5,4));
    assert(!fo2_posproj_stream_uvs(4,32,5,4));
    assert(!fo2_posproj_stream_uvs(1,48,5,4));
    assert(!fo2_posproj_stream_uvs(4,24,3,4));
    assert(!fo2_posproj_stream_uvs(4,24,5,25));
    puts("posproj stream tests passed");
    return 0;
}
