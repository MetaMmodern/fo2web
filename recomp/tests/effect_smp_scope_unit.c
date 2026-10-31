/* effect_smp_scope_unit.c -- synthetic (non-game) checks for the scoped D3D9
 * sampler save/restore used by ID3DXEffect::Begin/End (runtime/com.c,
 * runtime/effect_smp_scope.c).
 *
 * The helper receives the g_smp_raw shadow table, so these tests exercise the
 * real lifecycle without a device, shader or game asset:
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime \
 *         tests/effect_smp_scope_unit.c runtime/effect_smp_scope.c \
 *         runtime/flatout_sha.c -o out/effect_smp_scope_unit
 *   out/effect_smp_scope_unit
 */
#include "effect_smp_scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t Raw[FO2_SMP_STAGES][FO2_SMP_SLOTS];
typedef uint32_t Mask[FO2_SMP_STAGES];

static int fails;
static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

static void fill_raw(Raw raw, uint32_t base){
    for (unsigned s = 0; s < FO2_SMP_STAGES; s++)
        for (unsigned t = 0; t < FO2_SMP_SLOTS; t++)
            raw[s][t] = base + s * 100u + t;
}

static int same_raw(const Raw a, const Raw b){
    return memcmp(a, b, sizeof(Raw)) == 0;
}

static void set_bit(Mask mask, unsigned stage, unsigned slot){
    mask[stage] |= 1u << slot;
}

static FO2ShaEffect *make_effect(const char *body){
    char src[4096], err[256] = {0};
    snprintf(src, sizeof src,
             "technique T\n{\n%s}\n", body);
    FO2ShaEffect *e = fo2_sha_parse(src, strlen(src), err, sizeof err);
    if (!e) printf("  parse error: %s\n", err);
    return e;
}

/* Same application the effect BeginPass does in com.c: only the slots the
 * pass declares are written. */
static void apply_pass_state(const FO2ShaPassState *st, Raw raw){
    for (unsigned stage = 0; stage < 4u; stage++)
        for (unsigned slot = 0; slot < FO2_SMP_SLOTS; slot++)
            if (st->smp_mask[stage] & (1u << slot))
                raw[stage][slot] = st->smp[stage][slot];
}

static const char *const multi_pass =
    "    pass P0\n    {\n"
    "        AddressU[0] = Clamp;\n"
    "        AddressV[1] = Wrap;\n"
    "    }\n"
    "    pass P1\n    {\n"
    "        MagFilter[0] = Linear;\n"
    "        MinFilter[2] = Point;\n"
    "    }\n"
    "    pass P2\n    {\n"
    "        AddressV[0] = Clamp;\n"
    "        MipFilter[3] = None;\n"
    "    }\n";

static void test_effect_mask(void){
    printf("effect sampler mask (union of all passes)\n");
    FO2ShaEffect *e = make_effect(multi_pass);
    chk(e && fo2_sha_pass_count(e, 0) == 3, "synthetic effect has 3 passes");

    Mask got, want = {0};
    fo2_smp_effect_mask(e, 0, got);
    set_bit(want, 0, 1); set_bit(want, 0, 2); set_bit(want, 0, 5);
    set_bit(want, 1, 2);
    set_bit(want, 2, 6);
    set_bit(want, 3, 7);
    chk(memcmp(got, want, sizeof(Mask)) == 0,
        "mask is the union of AddressU/V and filter slots from every pass");
    chk((got[0] & (1u << 1)) && (got[0] & (1u << 5)) && (got[0] & (1u << 6)) == 0,
        "stage 0 keeps AddressU and MagFilter but no undeclared MinFilter");

    Mask empty = {0};
    fo2_smp_effect_mask(e, 1, empty);
    int zero = 1;
    for (unsigned s = 0; s < FO2_SMP_STAGES; s++) zero = zero && empty[s] == 0;
    chk(zero, "an out-of-range technique produces an empty mask");

    memset(got, 0xA5, sizeof got);
    fo2_smp_effect_mask(0, 0, got);
    zero = 1;
    for (unsigned s = 0; s < FO2_SMP_STAGES; s++) zero = zero && got[s] == 0;
    chk(zero, "null effect produces an empty mask");
    fo2_sha_free(e);
}

static void test_flags(void){
    static const struct { uint32_t flags; int saves; const char *desc; } f[] = {
        {0, 1, "flags 0 saves samplers"},
        {FO2_D3DXFX_DONOTSAVESTATE, 0, "DONOTSAVESTATE (1) suppresses samplers"},
        {FO2_D3DXFX_DONOTSAVESHADERSTATE, 1, "DONOTSAVESHADERSTATE (2) leaves samplers saved"},
        {FO2_D3DXFX_DONOTSAVESAMPLERSTATE, 0, "DONOTSAVESAMPLERSTATE (4) suppresses samplers"},
        {FO2_D3DXFX_DONOTSAVESTATE | FO2_D3DXFX_DONOTSAVESAMPLERSTATE, 0,
         "combined flags 1|4 still suppress samplers"},
        {FO2_D3DXFX_DONOTSAVESHADERSTATE | FO2_D3DXFX_DONOTSAVESAMPLERSTATE, 0,
         "combined flags 2|4 suppress samplers"},
    };
    printf("Begin flags\n");
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++){
        FO2SmpScopeSet set;
        Raw raw, first;
        Mask mask = {0};
        memset(&set, 0, sizeof set);
        fill_raw(raw, 1000u);
        memcpy(first, raw, sizeof raw);
        set_bit(mask, 0, 1);
        set_bit(mask, 1, 2);

        int saved = fo2_smp_scope_begin(&set, 0x100u, f[i].flags, raw, mask);
        raw[0][1] = 777u;
        raw[1][2] = 888u;
        raw[0][0] = 999u;                 /* not in the mask */
        int restored = fo2_smp_scope_end(&set, 0x100u, raw);

        chk(saved == f[i].saves, f[i].desc);
        if (f[i].saves){
            chk(restored == 1 && raw[0][1] == first[0][1] && raw[1][2] == first[1][2],
                "saved slots come back");
            chk(raw[0][0] == 999u, "unrelated sampler slot is left alone");
        } else {
            chk(restored == 0 && raw[0][1] == 777u && raw[1][2] == 888u,
                "disabled save leaves the pass values in place");
            chk(raw[0][0] == 999u, "disabled save also leaves unrelated state alone");
        }
    }
}

static void test_pass_lifecycle(void){
    printf("Begin -> pass -> End\n");
    FO2ShaEffect *e = make_effect(multi_pass);
    Mask mask;
    fo2_smp_effect_mask(e, 0, mask);

    for (unsigned pass = 0; pass < 3u; pass++){
        FO2SmpScopeSet set;
        Raw raw, first;
        FO2ShaPassState st;
        memset(&set, 0, sizeof set);
        fill_raw(raw, 2000u);
        memcpy(first, raw, sizeof raw);

        int saved = fo2_smp_scope_begin(&set, 0x200u + pass, 0, raw, mask);
        int have = e && fo2_sha_pass_state(e, 0, pass, &st);
        if (have) apply_pass_state(&st, raw);
        int restored = fo2_smp_scope_end(&set, 0x200u + pass, raw);
        char what[96];
        snprintf(what, sizeof what, "pass %u sampler values are restored at End", pass);
        chk(saved == 1 && have && restored == 1 && same_raw(raw, first), what);
    }

    {
        FO2SmpScopeSet set;
        Raw raw, first;
        memset(&set, 0, sizeof set);
        fill_raw(raw, 3000u);
        memcpy(first, raw, sizeof raw);
        int saved = fo2_smp_scope_begin(&set, 0x201u, 0, raw, mask);
        for (unsigned pass = 0; pass < 3u; pass++){
            FO2ShaPassState st;
            if (e && fo2_sha_pass_state(e, 0, pass, &st)) apply_pass_state(&st, raw);
        }
        int restored = fo2_smp_scope_end(&set, 0x201u, raw);
        chk(saved == 1 && restored == 1 && same_raw(raw, first),
            "all passes applied between one Begin/End are undone");
    }
    fo2_sha_free(e);
}

static void test_only_masked_slots(void){
    printf("restore is scoped to the snapshot\n");
    FO2SmpScopeSet set;
    Raw raw, first;
    Mask mask = {0};
    memset(&set, 0, sizeof set);
    fill_raw(raw, 4000u);
    memcpy(first, raw, sizeof raw);
    set_bit(mask, 0, 1);

    fo2_smp_scope_begin(&set, 0x300u, 0, raw, mask);
    raw[0][1] = 111u;                     /* saved slot: must come back */
    raw[0][0] = 222u;                     /* same stage, undeclared slot */
    raw[7][19] = 333u;                    /* another stage */
    int restored = fo2_smp_scope_end(&set, 0x300u, raw);
    chk(restored == 1 && raw[0][1] == first[0][1], "declared slot is restored");
    chk(raw[0][0] == 222u && raw[7][19] == 333u,
        "undeclared slots and unrelated stages keep newer values");
}

static void test_nested_and_repeated(void){
    printf("nested and repeated Begin/End\n");
    FO2SmpScopeSet set;
    Raw raw;
    Mask mask = {0};
    memset(&set, 0, sizeof set);
    fill_raw(raw, 5000u);
    set_bit(mask, 0, 1);

    raw[0][1] = 10u;
    chk(fo2_smp_scope_begin(&set, 0x400u, 0, raw, mask) == 1, "outer Begin saves");
    raw[0][1] = 20u;
    chk(fo2_smp_scope_begin(&set, 0x400u, 0, raw, mask) == 1, "inner Begin saves");
    raw[0][1] = 30u;
    chk(fo2_smp_scope_end(&set, 0x400u, raw) == 1 && raw[0][1] == 20u,
        "first End restores the inner snapshot");
    chk(fo2_smp_scope_end(&set, 0x400u, raw) == 1 && raw[0][1] == 10u,
        "second End restores the outer snapshot");

    raw[0][1] = 40u;
    chk(fo2_smp_scope_begin(&set, 0x400u, 0, raw, mask) == 1, "another Begin works");
    raw[0][1] = 50u;
    chk(fo2_smp_scope_end(&set, 0x400u, raw) == 1 && raw[0][1] == 40u,
        "another matching End works");

    raw[0][1] = 60u;
    chk(fo2_smp_scope_end(&set, 0x400u, raw) == 0 && raw[0][1] == 60u,
        "an unmatched End is a no-op");
}

static void test_guarded_begin(void){
    printf("guard markers for disabled Begins\n");
    FO2SmpScopeSet set;
    Raw raw;
    Mask mask = {0};
    memset(&set, 0, sizeof set);
    fill_raw(raw, 6000u);
    set_bit(mask, 0, 1);

    raw[0][1] = 10u;
    chk(fo2_smp_scope_begin(&set, 0x500u, 0, raw, mask) == 1, "outer saving Begin");
    raw[0][1] = 20u;
    chk(fo2_smp_scope_begin(&set, 0x500u, FO2_D3DXFX_DONOTSAVESTATE, raw, mask) == 0,
        "disabled Begin pushes only a guard");
    raw[0][1] = 30u;
    chk(fo2_smp_scope_end(&set, 0x500u, raw) == 0 && raw[0][1] == 30u,
        "guard End must not restore the outer snapshot");
    chk(fo2_smp_scope_end(&set, 0x500u, raw) == 1 && raw[0][1] == 10u,
        "outer End still restores after the guard");

    raw[0][1] = 70u;
    chk(fo2_smp_scope_begin(&set, 0x501u, FO2_D3DXFX_DONOTSAVESAMPLERSTATE, raw, mask) == 0,
        "sampler-disabled Begin on a fresh object");
    raw[0][1] = 80u;
    chk(fo2_smp_scope_end(&set, 0x501u, raw) == 0 && raw[0][1] == 80u,
        "sampler-disabled Begin/End leaves state alone");
    chk(fo2_smp_scope_end(&set, 0x501u, raw) == 0 && raw[0][1] == 80u,
        "extra End after the disabled pair is harmless");
}

static void test_overflow(void){
    printf("depth overflow stays balanced\n");
    FO2SmpScopeSet set;
    Raw raw;
    Mask mask = {0};
    memset(&set, 0, sizeof set);
    set_bit(mask, 0, 1);
    raw[0][1] = 7u;

    for (unsigned i = 0; i < FO2_SMP_DEPTH; i++){
        chk(fo2_smp_scope_begin(&set, 0x600u, 0, raw, mask) == 1, "Begin fits the stack");
        raw[0][1] = 100u + i;
    }
    chk(fo2_smp_scope_begin(&set, 0x600u, 0, raw, mask) == 0,
        "one Begin beyond the depth is skipped");
    raw[0][1] = 999u;
    chk(fo2_smp_scope_end(&set, 0x600u, raw) == 0 && raw[0][1] == 999u,
        "skipped Begin's End does not pop a real snapshot");
    for (unsigned i = 0; i < FO2_SMP_DEPTH; i++)
        chk(fo2_smp_scope_end(&set, 0x600u, raw) == 1, "remaining Ends restore");
    chk(raw[0][1] == 7u, "overflow did not disturb the outermost saved value");
}

static void test_separate_objects(void){
    printf("separate effect objects\n");
    FO2SmpScopeSet set;
    Raw raw;
    Mask ma = {0}, mb = {0};
    memset(&set, 0, sizeof set);
    fill_raw(raw, 7000u);
    set_bit(ma, 0, 1);
    set_bit(mb, 0, 2);

    raw[0][1] = 1u; raw[0][2] = 2u;
    chk(fo2_smp_scope_begin(&set, 0xA00u, 0, raw, ma) == 1, "effect A Begin");
    raw[0][1] = 11u; raw[0][2] = 12u;
    chk(fo2_smp_scope_begin(&set, 0xB00u, 0, raw, mb) == 1, "effect B Begin");
    raw[0][1] = 21u; raw[0][2] = 22u;

    chk(fo2_smp_scope_end(&set, 0xA00u, raw) == 1 && raw[0][1] == 1u && raw[0][2] == 22u,
        "A restores only A's slot while B stays active");
    chk(fo2_smp_scope_end(&set, 0xB00u, raw) == 1 && raw[0][2] == 12u,
        "B restores B's slot independently (value from its own Begin)");

    raw[0][1] = 3u;
    chk(fo2_smp_scope_begin(&set, 0xA00u, 0, raw, ma) == 1, "A Begin again");
    raw[0][1] = 4u;
    fo2_smp_scope_forget(&set, 0xA00u);
    chk(fo2_smp_scope_end(&set, 0xA00u, raw) == 0 && raw[0][1] == 4u,
        "forget drops a live scope without restoring");
    chk(fo2_smp_scope_begin(&set, 0xA00u, 0, raw, ma) == 1, "A can Begin after forget");
    raw[0][1] = 5u;
    chk(fo2_smp_scope_end(&set, 0xA00u, raw) == 1 && raw[0][1] == 4u,
        "the fresh A snapshot is independent");

    raw[0][1] = 6u;
    raw[0][2] = 7u;
    fo2_smp_scope_begin(&set, 0xC00u, 0, raw, ma);
    raw[0][1] = 8u;
    fo2_smp_scope_reset(&set);
    chk(fo2_smp_scope_end(&set, 0xC00u, raw) == 0 && raw[0][1] == 8u,
        "reset drops every live scope");
}

static void test_bounds(void){
    printf("mask and slot bounds\n");
    FO2SmpScopeSet set;
    Raw raw, first;
    Mask mask = {0};
    memset(&set, 0, sizeof set);
    fill_raw(raw, 8000u);
    memcpy(first, raw, sizeof raw);
    mask[0] = (1u << 1) | (1u << 20) | (1u << 31);
    set_bit(mask, 15, 19);

    fo2_smp_scope_begin(&set, 0xD00u, 0, raw, mask);
    raw[0][1] = 111u;                     /* bit 1: saved */
    raw[0][19] = 222u;                    /* bit 19 is outside the supplied bits */
    raw[15][19] = 333u;                   /* highest supported stage/slot */
    int restored = fo2_smp_scope_end(&set, 0xD00u, raw);
    chk(restored == 1 && raw[0][1] == first[0][1], "valid low mask bit restores");
    chk(raw[0][19] == 222u, "mask bits beyond 20 slots are ignored");
    chk(raw[15][19] == first[15][19], "stage 15/slot 19 stays in bounds and restores");

    Mask none = {0};
    Raw r2;
    fill_raw(r2, 9000u);
    chk(fo2_smp_scope_begin(&set, 0xD01u, 0, r2, none) == 0 && r2[0][1] == 9001u,
        "an empty mask is a guard-only Begin");
    r2[0][1] = 444u;
    chk(fo2_smp_scope_end(&set, 0xD01u, r2) == 0 && r2[0][1] == 444u,
        "an empty-mask End restores nothing");

    chk(fo2_smp_scope_begin(0, 0xD02u, 0, r2, none) == 0, "null scope set is rejected");
    chk(fo2_smp_scope_begin(&set, 0, 0, r2, none) == 0, "object 0 is rejected");
    chk(fo2_smp_scope_begin(&set, 0xD02u, 0, 0, none) == 0, "null raw table is rejected");
    chk(fo2_smp_scope_begin(&set, 0xD02u, 0, r2, 0) == 0, "null mask is rejected");
    chk(fo2_smp_scope_end(&set, 0xD02u, 0) == 0, "null raw table is rejected at End");
    chk(fo2_smp_scope_end(0, 0xD02u, r2) == 0, "null scope set is rejected at End");
}

int main(void){
    test_effect_mask();
    test_flags();
    test_pass_lifecycle();
    test_only_masked_slots();
    test_nested_and_repeated();
    test_guarded_begin();
    test_overflow();
    test_separate_objects();
    test_bounds();
    if (fails) printf("effect_smp_scope_unit: %d FAILED\n", fails);
    else printf("effect_smp_scope_unit: all passed\n");
    return fails ? 1 : 0;
}
