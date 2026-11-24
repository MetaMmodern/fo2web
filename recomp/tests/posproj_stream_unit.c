/* Host-only regression for bloom extraction's PosprojTex4/24-byte mismatch.
 * clang -Wall -Wextra -Werror -Iruntime tests/posproj_stream_unit.c -o out/posproj_stream_unit
 */
#include "posproj_stream.h"
#include <assert.h>
#include <stdio.h>

int main(void){
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
