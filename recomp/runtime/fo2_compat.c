/* FlatOut browser build: symbols that are optional NFSU2 instrumentation used
 * by the shared Win32/COM runtime. FlatOut does not install those hooks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "decls.h"
#include "fo2_default_binds.h"

uint32_t g_fe_mgr = 0;
void overrides_report(void) {}

/* FlatOut's CRT memcpy has a valid indirect-call entry three bytes before the
 * function boundary discovered by the static lifter (0x005610b4).  Those
 * bytes are exactly three MOVSD instructions, followed by the lifted tail.
 * Keep the entry explicit instead of pretending the target is generated code.
 */
static void fo2_memcpy_tail(CPU *c, unsigned n){
    int32_t step = c->df ? -4 : 4;
    for (unsigned i = 0; i < n; i++) {
        MR32(c->edi) = MR32(c->esi);
        c->esi += step;
        c->edi += step;
    }
    sub_005610b4(c);
}

#define FO2_COPY_ENTRY(N) static void fo2_memcpy_tail_##N(CPU *c){ fo2_memcpy_tail(c, N); }
FO2_COPY_ENTRY(1)  FO2_COPY_ENTRY(2)  FO2_COPY_ENTRY(3)
FO2_COPY_ENTRY(4)  FO2_COPY_ENTRY(5)  FO2_COPY_ENTRY(6)
FO2_COPY_ENTRY(7)  FO2_COPY_ENTRY(8)  FO2_COPY_ENTRY(9)
FO2_COPY_ENTRY(10) FO2_COPY_ENTRY(11) FO2_COPY_ENTRY(12)
FO2_COPY_ENTRY(13) FO2_COPY_ENTRY(14) FO2_COPY_ENTRY(15)

/* The process selects this SSE/MMX memcpy implementation through a function
 * pointer. Its interior has two computed jump tables (for 4-byte and 64-byte
 * remainders) whose byte-level entries are intentionally not functions in the
 * PE. A plain guest-memory memmove is equivalent for this calling convention
 * and avoids having to model every interior entry. */
static void fo2_memcpy(CPU *c){
    uint32_t dst = MR32(c->esp + 4u);
    uint32_t src = MR32(c->esp + 8u);
    uint32_t n = MR32(c->esp + 12u);
    if (n) {
        if (dst < src || dst - src >= n) {
            for (uint32_t i = 0; i < n; i++) MR8(dst + i) = MR8(src + i);
        } else {
            for (uint32_t i = n; i; i--) MR8(dst + i - 1u) = MR8(src + i - 1u);
        }
    }
    c->eax = dst;
    POP32(c->eip);
}

static void fo2_bind_warn(const char *msg){ fprintf(stderr, "%s\n", msg); }

/* FlatOut 2's GameSpy integration hard-codes the dead GameSpy domains
 * ("gamespy.com") in eleven places in its .rdata: the master-server list, the
 * NAT-negotiation hosts, the stats collector and the ladder URL.  ZPatch's
 * "UseOpenSpy" replaces each with "openspy.net" -- the same eleven bytes, so
 * every prefix ("master.", "%s.", "natneg") and suffix ("/games/st_ladder/
 * web/playerposition.asp") is untouched and no other pointer changes.  This
 * matches ZPatchFO2.asi's UseOpenSpy routine (100061a0 -> 100074b0), but it
 * only selects the hostnames. In the browser the Winsock emulator and server
 * gateway must still implement the game's TCP, UDP, and peer traffic.
 * FO2_OPENSPY=0 turns the patch off for comparison. */
static void fo2_openspy_patch(void){
    static const uint32_t addrs[] = {
        0x00656560u, 0x00656574u, 0x00676c89u, 0x00676d6au, 0x00676d83u,
        0x00676d9bu, 0x00676db0u, 0x00677783u, 0x00681cf2u, 0x00681d85u,
        0x00681dc5u,
    };
    static const char old[11] = "gamespy.com", new_[11] = "openspy.net";
    const char *e = getenv("FO2_OPENSPY");
    if (e && !strcmp(e, "0")) return;
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof addrs / sizeof addrs[0]; i++){
        uint32_t a = addrs[i];
        if (!mem_is_committed(a) || !mem_is_committed(a + 11u)){
            fprintf(stderr, "[fo2] openspy: guest .rdata not loaded yet at 0x%08x (called too early?)\n", a);
            continue;
        }
        int match = 1;
        for (unsigned j = 0; j < 11; j++)
            if (MR8(a + j) != (uint8_t)old[j]){ match = 0; break; }
        if (!match){
            fprintf(stderr, "[fo2] openspy: no GameSpy string at 0x%08x (another game version?)\n", a);
            continue;
        }
        for (unsigned j = 0; j < 11; j++) MR8(a + j) = (uint8_t)new_[j];
        n++;
    }
    fprintf(stderr, "[fo2] openspy: %u of 11 GameSpy host names now point at OpenSpy\n", n);
}

void fo2_compat_install(void){
    extern void dispatch_patch(uint32_t, void (*)(CPU *));
    /* 0x5610a5..0x5610b3 are a computed jump table into a row of MOVSDs.
     * The target at A5 runs 15 copies, A6 14, ..., B3 one. */
    static void (*const tail[15])(CPU *) = {
        fo2_memcpy_tail_15, fo2_memcpy_tail_14, fo2_memcpy_tail_13,
        fo2_memcpy_tail_12, fo2_memcpy_tail_11, fo2_memcpy_tail_10,
        fo2_memcpy_tail_9, fo2_memcpy_tail_8, fo2_memcpy_tail_7,
        fo2_memcpy_tail_6, fo2_memcpy_tail_5, fo2_memcpy_tail_4,
        fo2_memcpy_tail_3, fo2_memcpy_tail_2, fo2_memcpy_tail_1,
    };
    for (unsigned i = 0; i < 15; i++) dispatch_patch(0x005610a5u + i, tail[i]);
    dispatch_patch(0x00560ef0u, fo2_memcpy);

    /* Called by the web main after mem_load_pe/win32_init: the defaults table
     * is read only now, and only the table changes -- a saved mapping is
     * loaded separately by sub_00553220 and is never touched here. */
    fo2_openspy_patch();
    fo2_default_binds_apply(fo2_bind_warn);
}
