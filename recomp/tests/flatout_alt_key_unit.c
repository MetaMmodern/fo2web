/* flatout_alt_key_unit.c -- synthetic (non-game) checks for the FlatOut
 * keyboard-picker wrap (runtime/flatout_alt_key.c).  __real_sub_0055aca0 is
 * mocked, so the wrapper's contract is checked without a lifted guest:
 *   - the original is called exactly once and its result wins;
 *   - a zero result with Alt's press counter clear stays zero;
 *   - a zero result with Alt's press counter set becomes VK_MENU (0x12);
 *   - the original's eip, esp and the word it popped off its own stack stay;
 *   - no other register, flag or guest byte is written.
 *
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime \
 *         tests/flatout_alt_key_unit.c runtime/flatout_alt_key.c \
 *         -o out/flatout_alt_key_unit
 *   out/flatout_alt_key_unit
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cpu.h's host build wants the reserved guest span; only the picker's counter
 * and the mock's stack need to be inside it. */
uint8_t *g_mem;

void __wrap_sub_0055aca0(CPU *c);

#define PRESS_BASE 0x8d7d60u
#define VK_MENU    0x12u
#define ALT_ADDR   (PRESS_BASE + VK_MENU)
#define GUEST_SIZE 0x8e0000u
#define FAKE_ESP   0x20f000u
#define FAKE_RET   0x0055eb2bu
#define FAKE_EIP   0x00401000u

static int fails;
static int real_calls;
static uint32_t fake_eax;

static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

/* What the lifted original does for this test: leave a result, pop its own
 * return address and set eip/esp accordingly. */
void __real_sub_0055aca0(CPU *c){
    real_calls++;
    c->eax = fake_eax;
    c->esp = FAKE_ESP - 4u;
    MR32(c->esp) = FAKE_RET;
    c->eip = FAKE_EIP;
}

static void set_cpu(CPU *c){
    memset(c, 0, sizeof *c);
    c->eax = 0x11111111u;
    c->ecx = 0x22222222u;
    c->edx = 0x33333333u;
    c->ebx = 0x44444444u;
    c->esp = FAKE_ESP;
    c->ebp = 0x55555555u;
    c->esi = 0x66666666u;
    c->edi = 0x77777777u;
    c->eip = 0x88888888u;
    c->fs_base = 0x7ffdf000u;
    c->gs_base = 0;
    c->cf = 1; c->pf = 0; c->af = 1; c->zf = 0; c->sf = 1; c->of = 0; c->df = 0;
    c->st[0] = 1.5;
    c->top = 3;
    c->fcw = 0x027fu;
    c->fsw = 0x0123u;
    c->mm[2] = 0x0123456789abcdefull;
    c->xmm[5].u32[1] = 0xfeedfaceu;
    c->sysfl = 0x00200000u;
}

static void run_case(const char *name, uint32_t orig_eax, uint8_t alt, uint32_t want_eax){
    static uint8_t *before_mem;
    if (!before_mem) before_mem = malloc(GUEST_SIZE);

    CPU before, got;
    set_cpu(&before);
    got = before;

    /* Mark the whole guest span; the wrapper must not write any of it. */
    memset(g_mem, 0xa5, GUEST_SIZE);
    MR8(ALT_ADDR) = alt;
    memcpy(before_mem, g_mem, GUEST_SIZE);

    fake_eax = orig_eax;
    real_calls = 0;
    __wrap_sub_0055aca0(&got);

    CPU want = before;
    want.eax = want_eax;
    want.esp = FAKE_ESP - 4u;
    want.eip = FAKE_EIP;
    /* The original popped its return address: the stack word it wrote is the
     * only guest-memory change expected. */
    before_mem[FAKE_ESP - 4u]     = (uint8_t)(FAKE_RET);
    before_mem[FAKE_ESP - 4u + 1] = (uint8_t)(FAKE_RET >> 8);
    before_mem[FAKE_ESP - 4u + 2] = (uint8_t)(FAKE_RET >> 16);
    before_mem[FAKE_ESP - 4u + 3] = (uint8_t)(FAKE_RET >> 24);

    char what[160];
    snprintf(what, sizeof what, "%s: original called once", name);
    chk(real_calls == 1, what);
    snprintf(what, sizeof what, "%s: eax %#x -> %#x", name, orig_eax, want_eax);
    chk(got.eax == want_eax, what);
    snprintf(what, sizeof what, "%s: original eip kept", name);
    chk(got.eip == FAKE_EIP, what);
    snprintf(what, sizeof what, "%s: original esp and popped return address kept", name);
    chk(got.esp == FAKE_ESP - 4u && MR32(FAKE_ESP - 4u) == FAKE_RET, what);
    snprintf(what, sizeof what, "%s: no other CPU state written", name);
    chk(memcmp(&got, &want, sizeof got) == 0, what);
    snprintf(what, sizeof what, "%s: guest memory untouched (Alt byte %#x)", name, alt);
    chk(memcmp(g_mem, before_mem, GUEST_SIZE) == 0, what);
    snprintf(what, sizeof what, "%s: Alt byte still %#x", name, alt);
    chk(MR8(ALT_ADDR) == alt, what);
}

int main(void){
    g_mem = calloc(GUEST_SIZE, 1);
    if (!g_mem){ fprintf(stderr, "out of memory\n"); return 2; }

    printf("flatout_alt_key wrapper\n");
    /* Original result wins, Alt set or not. */
    run_case("recognized key, Alt up",   0x41u, 0x00u, 0x41u);
    run_case("recognized key, Alt down", 0x30u, 0x80u, 0x30u);
    run_case("special key, Alt down",    0x25u, 0x01u, 0x25u);
    /* No key: Alt up stays "no key", Alt down becomes VK_MENU. */
    run_case("no key, Alt up",           0x00u, 0x00u, 0x00u);
    run_case("no key, Alt down",         0x00u, 0x80u, VK_MENU);
    run_case("no key, Alt held (0xff)",  0x00u, 0xffu, VK_MENU);

    if (fails) printf("FAILED (%d)\n", fails);
    else printf("all checks passed\n");
    free(g_mem);
    return fails ? 1 : 0;}
