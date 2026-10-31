/* fo2_default_binds_unit.c -- synthetic (non-game) checks for the guarded
 * FlatOut default-binding patch (runtime/fo2_default_binds.h):
 *   - on the expected table only the vk fields of records 1 and 4 change
 *     (1: Control -> Alt, 4: PageDown -> B), every other byte stays;
 *   - calling it again is silent and changes nothing;
 *   - a different original key, a different action id, a different table
 *     version, one field already patched, and an uncommitted table span all
 *     leave every byte alone and report one diagnostic.
 *
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime \
 *         tests/fo2_default_binds_unit.c -o out/fo2_default_binds_unit
 *   out/fo2_default_binds_unit
 */
#include "fo2_default_binds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cpu.h's host build wants the reserved guest span and the committed bitmap. */
uint8_t *g_mem;
uint8_t g_cbits[1u << 15];

#define GUEST_SIZE  0x700000u           /* covers the table at 0x69b808 */
#define TABLE_BYTES (FO2_BIND_RECORD_SIZE * FO2_BIND_RECORD_COUNT)

static int fails;
static void chk(int ok, const char *what){
    if (ok) printf("  ok    %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}

static int warns;
static char last_warn[256];
static void capture(const char *msg){
    warns++;
    snprintf(last_warn, sizeof last_warn, "%s", msg);
}

static void commit(uint32_t addr, uint32_t n){
    uint32_t lo = addr >> 14, hi = (addr + n - 1u) >> 14;
    for (uint32_t p = lo; p <= hi; p++) g_cbits[p >> 3] |= (uint8_t)(1u << (p & 7));
}
static void uncommit(uint32_t addr){
    uint32_t p = addr >> 14;
    g_cbits[p >> 3] &= (uint8_t)~(1u << (p & 7));
}

static uint32_t rec(unsigned i, unsigned field){
    return FO2_BIND_TABLE_ADDR + i * FO2_BIND_RECORD_SIZE + field * 4u;
}

/* The table this PE has: record i is action i, with arbitrary non-sentinel
 * keys; records 1 and 4 keep their real Control/PageDown defaults. */
static void build_table(void){
    memset(g_mem, 0, GUEST_SIZE);
    commit(FO2_BIND_TABLE_ADDR, TABLE_BYTES);
    for (unsigned i = 0; i < FO2_BIND_RECORD_COUNT; i++){
        MR32(rec(i, 0)) = i;
        MR32(rec(i, 1)) = 0x100u + i;
    }
    MR32(rec(FO2_BIND_NITRO_ID, 1)) = FO2_BIND_NITRO_VK;
    MR32(rec(FO2_BIND_LOOKBEHIND_ID, 1)) = FO2_BIND_LOOKBEHIND_VK;
}

static uint8_t *snapshot(void){
    uint8_t *p = malloc(GUEST_SIZE);
    if (!p){ fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(p, g_mem, GUEST_SIZE);
    return p;
}

static void put32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* The whole guest span must equal the snapshot, with only the two expected
 * vk fields set to the port's defaults when changed is set. */
static void expect_table(const uint8_t *before, int changed, const char *name){
    uint8_t *want = malloc(GUEST_SIZE);
    if (!want){ fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(want, before, GUEST_SIZE);
    if (changed){
        put32(want + rec(FO2_BIND_NITRO_ID, 1), FO2_BIND_NITRO_NEW);
        put32(want + rec(FO2_BIND_LOOKBEHIND_ID, 1), FO2_BIND_LOOKBEHIND_NEW);
    }
    char what[192];
    snprintf(what, sizeof what, "%s: %s", name,
             changed ? "only record 1 and 4 vk changed" : "every byte unchanged");
    chk(memcmp(want, g_mem, GUEST_SIZE) == 0, what);
    free(want);
}

static void patch_case(const char *name, int want_ret, int want_warns, int want_changed){
    uint8_t *before = snapshot();
    warns = 0;
    last_warn[0] = 0;
    int r = fo2_default_binds_apply(capture);
    char what[192];
    snprintf(what, sizeof what, "%s: returns %d", name, want_ret);
    chk(r == want_ret, what);
    snprintf(what, sizeof what, "%s: %d diagnostic(s)", name, want_warns);
    chk(warns == want_warns, what);
    expect_table(before, want_changed, name);
    if (want_warns){
        snprintf(what, sizeof what, "%s: diagnostic bounded and mentions the table", name);
        chk(strlen(last_warn) < sizeof last_warn && strstr(last_warn, "default action bindings"),
            what);
    }
    free(before);
}

int main(void){
    g_mem = calloc(GUEST_SIZE, 1);
    if (!g_mem){ fprintf(stderr, "out of memory\n"); return 2; }

    printf("fo2 default action bindings\n");

    /* The expected table: Nitro Control -> Alt, Look Behind PageDown -> B. */
    build_table();
    patch_case("expected defaults", 1, 0, 1);

    /* A second install sees its own output and leaves it alone. */
    patch_case("second invocation", 0, 0, 0);

    /* A different original key is not overwritten. */
    build_table();
    MR32(rec(FO2_BIND_NITRO_ID, 1)) = 0x10u;
    patch_case("different Nitro key", 0, 1, 0);
    build_table();
    MR32(rec(FO2_BIND_LOOKBEHIND_ID, 1)) = 0x21u;
    patch_case("different Look Behind key", 0, 1, 0);

    /* The ids at the patched offsets must still be Nitro and Look Behind. */
    build_table();
    MR32(rec(FO2_BIND_NITRO_ID, 0)) = 2u;
    patch_case("different Nitro id", 0, 1, 0);
    build_table();
    MR32(rec(FO2_BIND_LOOKBEHIND_ID, 0)) = 5u;
    patch_case("different Look Behind id", 0, 1, 0);

    /* One field already patched is not half-applied and not migrated. */
    build_table();
    MR32(rec(FO2_BIND_NITRO_ID, 1)) = FO2_BIND_NITRO_NEW;
    patch_case("one key already patched", 0, 1, 0);

    /* A table with the port's keys but other ids is a different version. */
    build_table();
    MR32(rec(FO2_BIND_NITRO_ID, 1)) = FO2_BIND_NITRO_NEW;
    MR32(rec(FO2_BIND_LOOKBEHIND_ID, 1)) = FO2_BIND_LOOKBEHIND_NEW;
    MR32(rec(FO2_BIND_LOOKBEHIND_ID, 0)) = 5u;
    patch_case("different version", 0, 1, 0);

    /* An uncommitted table span is refused before any read/write. */
    build_table();
    uncommit(FO2_BIND_TABLE_ADDR);
    patch_case("table span not committed", 0, 1, 0);
    commit(FO2_BIND_TABLE_ADDR, TABLE_BYTES);

    if (fails) printf("fo2_default_binds_unit: %d FAILED\n", fails);
    else printf("fo2_default_binds_unit: all passed\n");
    free(g_mem);
    return fails ? 1 : 0;
}
