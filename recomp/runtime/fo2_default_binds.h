/* FlatOut 2's default action bindings.  The PE keeps them at 0x0069b808:
 * 20 records of 16 bytes, the action id at +0 and its virtual key at +4,
 * ending at the first vk == -1.  sub_00553430 copies id and vk out of this table when the game
 * initializes its defaults (and a reset does the same); sub_00553220 loads a
 * saved mapping from its own file into the controller object separately.
 * Changing the table here therefore changes only the defaults and a reset,
 * never a binding the user has already saved.
 *
 * The patch is a single guarded transformation so a host unit test can run it
 * on a synthetic table: it checks the committed memory, the table layout and
 * both original keys before it writes anything, and treats its own output as
 * a no-op so calling it twice changes nothing. */
#ifndef FO2_DEFAULT_BINDS_H
#define FO2_DEFAULT_BINDS_H
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"

#define FO2_BIND_TABLE_ADDR    0x0069b808u
#define FO2_BIND_RECORD_SIZE   16u
#define FO2_BIND_RECORD_COUNT  20u

/* Action ids in this PE's table, with their original keys. */
#define FO2_BIND_NITRO_ID       1u
#define FO2_BIND_NITRO_VK       0x11u    /* Control  */
#define FO2_BIND_LOOKBEHIND_ID  4u
#define FO2_BIND_LOOKBEHIND_VK  0x22u    /* PageDown */
/* The defaults this port wants: Alt and B. */
#define FO2_BIND_NITRO_NEW      0x12u
#define FO2_BIND_LOOKBEHIND_NEW 0x42u

/* Apply the default-binding patch.  warn (may be NULL) receives one bounded
 * message when the table is not the expected one; nothing is written then.
 * Returns 1 when the two keys were changed, 0 when nothing was. */
static int fo2_default_binds_apply(void (*warn)(const char *)){
    const uint32_t base = FO2_BIND_TABLE_ADDR;
    const uint32_t end  = base + FO2_BIND_RECORD_SIZE * FO2_BIND_RECORD_COUNT - 1u;
    const uint32_t nitro = base + FO2_BIND_NITRO_ID * FO2_BIND_RECORD_SIZE;
    const uint32_t look  = base + FO2_BIND_LOOKBEHIND_ID * FO2_BIND_RECORD_SIZE;
    char msg[224];

    if (!mem_is_committed(base) || !mem_is_committed(end)){
        if (warn){
            snprintf(msg, sizeof msg,
                     "fo2: default action bindings at %#010x not patched: table span %#010x..%#010x is not committed",
                     base, base, end);
            warn(msg);
        }
        return 0;
    }

    /* Layout guard: the record at Nitro's offset must still be Nitro and the
     * one at Look Behind's offset must still be Look Behind, which also
     * checks the 16-byte stride and the id == index relation. */
    const uint32_t nitro_id = MR32(nitro), nitro_vk = MR32(nitro + 4u);
    const uint32_t look_id  = MR32(look),  look_vk  = MR32(look + 4u);
    if (nitro_id != FO2_BIND_NITRO_ID || look_id != FO2_BIND_LOOKBEHIND_ID){
        if (warn){
            snprintf(msg, sizeof msg,
                     "fo2: default action bindings at %#010x not patched: unexpected table "
                     "(record1 id=%u vk=%#x, record4 id=%u vk=%#x)",
                     base, nitro_id, nitro_vk, look_id, look_vk);
            warn(msg);
        }
        return 0;
    }

    /* Already in the port's defaults: a repeated install is a no-op. */
    if (nitro_vk == FO2_BIND_NITRO_NEW && look_vk == FO2_BIND_LOOKBEHIND_NEW)
        return 0;

    if (nitro_vk != FO2_BIND_NITRO_VK || look_vk != FO2_BIND_LOOKBEHIND_VK){
        if (warn){
            snprintf(msg, sizeof msg,
                     "fo2: default action bindings at %#010x not patched: unexpected keys "
                     "(nitro id=%u vk=%#x, look-behind id=%u vk=%#x)",
                     base, nitro_id, nitro_vk, look_id, look_vk);
            warn(msg);
        }
        return 0;
    }

    /* Every check has passed: only the two vk fields change. */
    MR32(nitro + 4u) = FO2_BIND_NITRO_NEW;
    MR32(look + 4u)  = FO2_BIND_LOOKBEHIND_NEW;
    return 1;
}

#endif
