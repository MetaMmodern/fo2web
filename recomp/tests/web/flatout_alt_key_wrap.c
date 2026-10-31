/* flatout_alt_key_wrap.c -- link-level check that wasm-ld --wrap intercepts a
 * direct call from another translation unit, then run the resulting module.
 *
 * The same source is compiled twice.  Without FAKE_REAL it holds the direct
 * caller (main) and references sub_0055aca0, which is undefined in that
 * object.  With FAKE_REAL it defines sub_0055aca0 as the "lifted original".
 * runtime/flatout_alt_key.c is the wrapper.  The linker is invoked with
 * --wrap=sub_0055aca0, so the caller's reference must go to
 * __wrap_sub_0055aca0, whose __real_sub_0055aca0 must reach the fake.
 * The Alt-present case only yields 0x12 if that interception happened.
 *
 *   WASI=deps/wasi-sdk-34.0-x86_64-macos
 *   OUT=out/web/altkey-wrap; mkdir -p "$OUT"
 *   CF="--target=wasm32-wasip1 -O2 -fno-delete-null-pointer-checks -Iruntime"
 *   $WASI/bin/clang $CF -c tests/web/flatout_alt_key_wrap.c -o "$OUT/caller.o"
 *   $WASI/bin/clang $CF -DFAKE_REAL -c tests/web/flatout_alt_key_wrap.c -o "$OUT/fake.o"
 *   $WASI/bin/clang $CF -c runtime/flatout_alt_key.c -o "$OUT/wrap.o"
 *   $WASI/bin/clang --target=wasm32-wasip1 -O2 -o "$OUT/wrap.wasm" \
 *     "$OUT/caller.o" "$OUT/fake.o" "$OUT/wrap.o" \
 *     -Wl,--wrap=sub_0055aca0 -Wl,--export-memory \
 *     -Wl,--initial-memory=16777216 -Wl,--global-base=9272704 \
 *     -Wl,--no-stack-first -Wl,-z,stack-size=65536
 *   node --no-warnings web/tools/wasi-run.mjs "$OUT/wrap.wasm"
 */
#include "cpu.h"
#include <stdio.h>

#define PRESS_BASE 0x8d7d60u
#define VK_MENU    0x12u
#define ALT_ADDR   (PRESS_BASE + VK_MENU)
#define FAKE_ESP   0x20f000u
#define FAKE_RET   0x0055eb2bu
#define FAKE_EIP   0x00401000u

#ifdef FAKE_REAL
/* The fake lifted picker: reports next_eax, pops its return address. */
int real_calls;
uint32_t next_eax;

void sub_0055aca0(CPU *c){
    real_calls++;
    c->eax = next_eax;
    c->esp = FAKE_ESP - 4u;
    MR32(c->esp) = FAKE_RET;
    c->eip = FAKE_EIP;
}
#else
void sub_0055aca0(CPU *c);
extern int real_calls;
extern uint32_t next_eax;

static int fails;
static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

static void run_case(const char *name, uint32_t orig_eax, uint8_t alt, uint32_t want){
    CPU c;
    memset(&c, 0, sizeof c);
    c.esp = FAKE_ESP;
    c.eip = 0x11111111u;
    MR8(ALT_ADDR) = alt;
    real_calls = 0;
    next_eax = orig_eax;

    sub_0055aca0(&c);                 /* direct call: --wrap must intercept */

    char what[128];
    snprintf(what, sizeof what, "%s: fake original called once", name);
    chk(real_calls == 1, what);
    snprintf(what, sizeof what, "%s: eax %#x -> %#x", name, orig_eax, want);
    chk(c.eax == want, what);
    snprintf(what, sizeof what, "%s: original eip and stack kept", name);
    chk(c.eip == FAKE_EIP && c.esp == FAKE_ESP - 4u && MR32(FAKE_ESP - 4u) == FAKE_RET, what);
    snprintf(what, sizeof what, "%s: Alt counter not written", name);
    chk(MR8(ALT_ADDR) == alt, what);
}

int main(void){
    printf("wasm --wrap sub_0055aca0\n");
    run_case("recognized key, Alt up",   0x41u, 0x00u, 0x41u);
    run_case("recognized key, Alt down", 0x30u, 0x80u, 0x30u);
    run_case("no key, Alt up",           0x00u, 0x00u, 0x00u);
    run_case("no key, Alt down",         0x00u, 0x80u, VK_MENU);
    if (fails) printf("FAILED (%d)\n", fails);
    else printf("all checks passed\n");
    return fails ? 1 : 0;
}
#endif
