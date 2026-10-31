/* FlatOut keyboard picker: Alt (VK_MENU) support.
 *
 * sub_0055aca0 reads the press counters at guest 0x8d7d60 and accepts
 * 'A'..'Z', '0'..'9' and the 29 special virtual keys in its table.  Alt is
 * not among them, so the picker can never bind it; a zero result means "no
 * key".  The callers of the picker are generated code, so dispatch_patch (a
 * replacement for a guest address) cannot see it.  web/build.sh links with
 * --wrap=sub_0055aca0 instead: this function runs the lifted original once
 * and, only when the original found no key but Alt's press counter is set,
 * reports VK_MENU.  Every result the original produced, including 0 with Alt
 * up, passes through with the registers and memory it left. */
#include "cpu.h"

void __real_sub_0055aca0(CPU *c);

void __wrap_sub_0055aca0(CPU *c){
    __real_sub_0055aca0(c);
    if (c->eax == 0 && MR8(0x8d7d60u + 0x12u) != 0)
        c->eax = 0x12u;
}
