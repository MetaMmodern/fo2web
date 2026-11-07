/* GameSpy browser diagnostics (FO2_SPY_DBG=1) and cleanup bounds check.
 * The server-browser crash entered sub_005ea310 with argument 0xffffffff.
 * sub_005e9350 stores the result of sub_005ea260 in the EXE's TLS block +4;
 * sub_005e89b0 reads it and may clear it after freeing the list. Record both
 * sides; the browser key-list guard below is the only behavioral change. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void __real_sub_005e9350(CPU *c);
void __real_sub_0050e500(CPU *c);
void __real_sub_0050f300(CPU *c);
void __real_sub_005e89b0(CPU *c);
void __real_sub_005ea310(CPU *c);
void __real_sub_005e9330(CPU *c);
void __real_sub_005e5700(CPU *c);
void __real_sub_005e84e0(CPU *c);
void __real_sub_005e6760(CPU *c);
void __real_sub_005e6c90(CPU *c);
void __real_sub_005e6d90(CPU *c);
void __real_sub_00518fe0(CPU *c);
void __real_sub_0045fc90(CPU *c);
void watch_arm(uint32_t);
void watch_dump(const char *, int);
void __real_sub_00519dd0(CPU *c);
void __real_sub_004e0bd0(CPU *c);
void __real_sub_004e3780(CPU *c);

static int enabled(void){
    static int on = -1;
    if (on < 0){
        const char *e = getenv("FO2_SPY_DBG");
        on = e && e[0] == '1' && e[1] == 0;
    }
    return on;
}

/* 005e83c0/005e83ac allocate a 0x63c-byte browser on the guest heap and
 * construct its parser at browser+0x4c (005e6650). The main stack is
 * 0x05000000..0x05FF0000 and guest thread stacks start at 0x06000000
 * (runtime/web/main_web.c, win32_ext.c stack_take), so no constructor's parser
 * can ever be there. In the observed failure the parser is 05feeb60, 0x190
 * bytes below the main thread's own esp, and its fields are stack leftovers
 * (owner 0, callback ffffffff, key count another stack address). Reject that
 * shape before the parser's poll/notify can walk the stack as an object.
 * Deliberately no callback/owner heuristics: a healthy parser must always
 * pass, or the browser would stop polling altogether. */
#define GUEST_STACK_LO 0x05000000u
#define GUEST_STACK_HI 0x07000000u
static int live_parser(uint32_t parser){
    if (!parser || (uint64_t)parser + 0x5d7u >= GUEST_END) return 0;
    if (parser >= GUEST_STACK_LO && parser < GUEST_STACK_HI) return 0;
    return mem_is_committed(parser) && mem_is_committed(parser + 0x5d7u);
}

static uint32_t readable32(uint32_t addr){
    return (uint64_t)addr + 3u < GUEST_END &&
           mem_is_committed(addr) && mem_is_committed(addr + 3u)
           ? MR32(addr) : 0xffffffffu;
}
static uint32_t readable_field(uint32_t base, uint32_t offset){
    return (uint64_t)base + offset + 3u < GUEST_END
           ? readable32(base + offset) : 0xffffffffu;
}

/* These cdecl networking routines must preserve EBX/EBP/ESI/EDI and return
 * with ESP advanced by exactly one return address. Observe, never repair:
 * a return address appearing in EDI can be an unbalanced guest frame, not
 * an invalid allocation. Log only violations, with a per-site limit. */
#define SPY_ABI_PROBE(NAME) \
    void __real_sub_##NAME(CPU *c); \
    void __wrap_sub_##NAME(CPU *c){ \
        if (!enabled()){ __real_sub_##NAME(c); return; } \
        uint32_t sp=c->esp, ret=readable32(sp); \
        uint32_t bx=c->ebx, bp=c->ebp, si=c->esi, di=c->edi; \
        __real_sub_##NAME(c); \
        if (c->esp != sp+4u || c->eip != ret || c->ebx != bx || \
            c->ebp != bp || c->esi != si || c->edi != di){ \
            static unsigned emitted; \
            if (emitted++ < 4) \
                fprintf(stderr,"[spy-abi] " #NAME " ret=%08x eip=%08x " \
                        "esp=%08x->%08x ebx=%08x->%08x ebp=%08x->%08x " \
                        "esi=%08x->%08x edi=%08x->%08x\n", \
                        ret,c->eip,sp,c->esp,bx,c->ebx,bp,c->ebp,si,c->esi,di,c->edi); \
        } \
    }
SPY_ABI_PROBE(00503cd0)
SPY_ABI_PROBE(00503830)
SPY_ABI_PROBE(005ea0d0)
SPY_ABI_PROBE(005f5b30)

static void invalid_parser(CPU *c, const char *site, uint32_t parser){
    static unsigned warned;
    if (warned++ < 4)
        fprintf(stderr, "[fo2] GameSpy parser rejected at %s: object=%08x esp=%08x ret=%08x "
                "state=%08x keys=%08x callback=%08x owner=%08x input=%08x\n",
                site, parser, c->esp, readable32(c->esp), readable32(parser),
                readable_field(parser, 0x480u), readable_field(parser, 0x488u),
                readable_field(parser, 0x494u), readable_field(parser, 0x7cu));
}

/* Every direct caller of 005e6c90 supplies a parser belonging to the
 * 005e83c0 browser. Its return is cdecl and zero means no progress. */
void __wrap_sub_005e6c90(CPU *c){
    uint32_t parser = MR32(c->esp + 4u);
    if (!live_parser(parser)){
        invalid_parser(c, "poll", parser);
        c->eax = 0;
        POP32(c->eip);
        return;
    }
    __real_sub_005e6c90(c);
}

/* 005e8790 sends a status packet through the browser: it passes browser+0x4c
 * to 005e6d90, which writes the packet into that object (005e6810) before
 * notifying. With a manager slot holding a stack address these writes land in
 * the caller's own frame, which is how the observed stack parser got its
 * fields. Reject the same impossible shape here; the caller's state machine
 * advances either way (00518fe0 sets +0x388 after this call).
 * 005e6d90 is cdecl: it restores the 0x14 frame and pops only the return. */
void __wrap_sub_005e6d90(CPU *c){
    uint32_t parser = MR32(c->esp + 4u);
    if (!live_parser(parser)){
        invalid_parser(c, "status", parser);
        c->eax = 0;
        POP32(c->eip);
        return;
    }
    __real_sub_005e6d90(c);
}

typedef struct { uint32_t index, array, block, value; } SpyTls;

static SpyTls snapshot(CPU *c){
    SpyTls s = {0};
    if (!mem_is_committed(0x6a2d28u) || !c->fs_base ||
        !mem_is_committed(c->fs_base + 0x2cu)) return s;
    s.index = MR32(0x6a2d28u);
    s.array = MR32(c->fs_base + 0x2cu);
    if (!s.array || s.index >= 128 ||
        !mem_is_committed(s.array + 4u * s.index)) return s;
    s.block = MR32(s.array + 4u * s.index);
    if (s.block && mem_is_committed(s.block + 4u))
        s.value = MR32(s.block + 4u);
    return s;
}

static void report(const char *phase, CPU *c, SpyTls s, uint32_t result){
    fprintf(stderr, "[spy-dbg] %s fs=%08x index=%u array=%08x block=%08x list=%08x eax=%08x\n",
            phase, c->fs_base, s.index, s.array, s.block, s.value, result);
}

/* Bounded per-site logs. In particular, keep the first getter call even if
 * the list was already nonzero: that distinguishes a fresh allocation from
 * a stale value inherited by the TLS block. */
void __wrap_sub_005e9350(CPU *c){
    SpyTls before = {0};
    int log = 0;
    if (enabled()){
        static unsigned calls;
        if (calls++ < 24){ before = snapshot(c); log = 1; }
    }
    __real_sub_005e9350(c);
    if (log){
        report("getter before", c, before, 0);
        report("getter after ", c, snapshot(c), c->eax);
    }
}

void __wrap_sub_005e89b0(CPU *c){
    SpyTls before = {0};
    int log = 0;
    if (enabled()){
        static unsigned calls;
        if (calls++ < 24){ before = snapshot(c); log = 1; }
    }
    if (log) report("cleanup before", c, before, c->eax);
    __real_sub_005e89b0(c);
    if (log) report("cleanup after ", c, snapshot(c), c->eax);
}

void __wrap_sub_005ea310(CPU *c){
    if (enabled()){
        static unsigned calls;
        if (calls++ < 24)
            fprintf(stderr, "[spy-dbg] list walk arg=%08x fs=%08x block=%08x list=%08x\n",
                    MR32(c->esp + 4u), c->fs_base, snapshot(c).block,
                    snapshot(c).value);
    }
    __real_sub_005ea310(c);
}

/* sub_005ea410 invokes this hash callback while disposing browser entries.
 * It reads a pointer at arg[0], then hashes the pointed-to string. The new
 * crash is inside that hash; report the actual pointer before dereferencing. */
void __wrap_sub_005e9330(CPU *c){
    if (enabled()){
        static unsigned calls, emitted;
        uint32_t key_arg = MR32(c->esp + 4u);
        uint32_t modulus = MR32(c->esp + 8u);
        int key_ok = key_arg && mem_is_committed(key_arg) && mem_is_committed(key_arg + 3u);
        uint32_t string = key_ok ? MR32(key_arg) : 0;
        int string_ok = string && mem_is_committed(string);
        if ((calls++ < 16 || !key_ok || !string_ok) && emitted++ < 40){
            fprintf(stderr, "[spy-dbg] hash key_arg=%08x key_ok=%d string=%08x str_ok=%d mod=%u fs=%08x",
                    key_arg, key_ok, string, string_ok, modulus, c->fs_base);
            if (string_ok)
                fprintf(stderr, " first=%02x %02x %02x %02x",
                        MR8(string), mem_is_committed(string + 1u) ? MR8(string + 1u) : 0,
                        mem_is_committed(string + 2u) ? MR8(string + 2u) : 0,
                        mem_is_committed(string + 3u) ? MR8(string + 3u) : 0);
            fputc('\n', stderr);
        }
    }
    __real_sub_005e9330(c);
}

/* The browser's cleanup walks its inline key array at object+0x84, count at
 * +0x480. Capture the source of the hashes before it reuses its stack slot. */
void __wrap_sub_005e5700(CPU *c){
    uint32_t obj = c->esi;
    if (enabled()){
        static unsigned calls;
        if (calls++ < 12){
            if (obj && mem_is_committed(obj + 0x484u)){
                fprintf(stderr, "[spy-dbg] browser cleanup object=%08x count=%u limit=%u keys=%08x,%08x,%08x,%08x\n",
                        obj, MR32(obj + 0x480u), MR32(obj + 0x484u),
                        MR32(obj + 0x84u), MR32(obj + 0x88u),
                        MR32(obj + 0x8cu), MR32(obj + 0x90u));
            } else fprintf(stderr, "[spy-dbg] browser cleanup object=%08x unreadable\n", obj);
        }
    }
    /* 255 inline key slots occupy +0x84 through +0x47c; +0x480 is the
     * count. The observed count was a stack address, not a key count.
     * Discard the invalid list rather than clamp and hash arbitrary memory.
     * This contains cleanup only; it does not repair the earlier corruption. */
    if (obj && mem_is_committed(obj + 0x484u)){
        uint32_t count = MR32(obj + 0x480u);
        if (count > 255u){
            static unsigned warned;
            if (warned++ < 4)
                fprintf(stderr, "[fo2] GameSpy browser cleanup: invalid key count %u at %08x; discarding corrupt list\n",
                        count, obj);
            MR32(obj + 0x480u) = 0;
        }
    }
    __real_sub_005e5700(c);
}

/* The network result handler passes its parser at arg+0x4c to 005e6c90.
 * Compare that parser before and after processing to locate the first bad
 * count, rather than treating the hash trap as the original writer. */
void __wrap_sub_005e84e0(CPU *c){
    uint32_t browser = MR32(c->esp + 4u);
    uint64_t parser_addr = (uint64_t)browser + 0x4cu;
    if (parser_addr >= GUEST_END || !live_parser((uint32_t)parser_addr)){
        invalid_parser(c, "browser", (uint32_t)parser_addr);
        c->eax = 0;
        POP32(c->eip);
        return;
    }
    int log = 0;
    uint32_t parser = 0;
    if (enabled()){
        static unsigned calls, emitted;
        static uint32_t last;
        parser = MR32(c->esp + 4u) + 0x4cu;
        uint32_t count = parser && mem_is_committed(parser + 0x480u) ? MR32(parser + 0x480u) : 0xffffffffu;
        /* Repeated healthy polls must not exhaust the budget before the
         * second (stack-backed) parser reaches its failure path. */
        if ((calls++ < 2 || parser != last || count > 255u) && emitted++ < 24) log = 1;
        last = parser;
    }
    if (log){
        if (parser && mem_is_committed(parser + 0x484u))
            fprintf(stderr, "[spy-dbg] browse parse before object=%08x state=%08x count=%u limit=%u\n",
                    parser, MR32(parser), MR32(parser + 0x480u), MR32(parser + 0x484u));
        else fprintf(stderr, "[spy-dbg] browse parse before object=%08x unreadable\n", parser);
    }
    __real_sub_005e84e0(c);
    if (log){
        if (parser && mem_is_committed(parser + 0x484u))
            fprintf(stderr, "[spy-dbg] browse parse after  object=%08x state=%08x count=%u limit=%u eax=%08x\n",
                    parser, MR32(parser), MR32(parser + 0x480u), MR32(parser + 0x484u), c->eax);
        else fprintf(stderr, "[spy-dbg] browse parse after  object=%08x unreadable eax=%08x\n", parser, c->eax);
    }
}

/* Called by the parser just before it notifies the browser and disposes its
 * pending keys. If the count changes during this call, the notification path
 * is responsible; if already bad here, parsing handed cleanup a bad object. */
void __wrap_sub_005e6760(CPU *c){
    if (!live_parser(c->esi)){
        invalid_parser(c, "notify", c->esi);
        c->eax = 0;
        POP32(c->eip);
        return;
    }
    uint32_t obj = c->esi, before = 0;
    int log = 0;
    if (enabled()){
        static unsigned emitted;
        static uint32_t last;
        before = obj && mem_is_committed(obj + 0x480u) ? MR32(obj + 0x480u) : 0xffffffffu;
        if ((obj != last || before > 255u) && emitted++ < 12) log = 1;
        last = obj;
    }
    if (log)
        fprintf(stderr, "[spy-dbg] browse notify before object=%08x count=%u state=%08x esp=%08x\n",
                obj, before, obj && mem_is_committed(obj) ? MR32(obj) : 0xffffffffu, c->esp);
    __real_sub_005e6760(c);
    if (log && obj && mem_is_committed(obj + 0x480u))
        fprintf(stderr, "[spy-dbg] browse notify after  object=%08x count=%u eax=%08x\n",
                obj, MR32(obj + 0x480u), c->eax);
}

/* After the search notification the FE update path (00521360 -> 0045f3f0 ->
 * 0045fc90) was handed 0x00519476 as its object. That is a code address --
 * the return address the browse-parse call pushes -- so 0045fc90 read
 * *(0x00519476 + 0x99c) = 0x6f6c6800 and trapped on the next load. No live
 * object is ever inside the image (.text .rdata .data 00400000..00940000) or
 * the guest stacks; reject that shape before 0045fc90's first field read.
 * cdecl: 0045fc90 pops only its own saved esi. */
#define IMAGE_LO 0x00400000u
#define IMAGE_HI 0x00940000u
static int plausible_object(uint32_t o){
    /* only shapes that can never be an object: no pointer into the image
     * (0x519476 was a return address), none into a guest stack, and none
     * outside committed memory. The object's size is not checked: rejecting
     * a real menu object by a size guess would stop the screen rendering. */
    if (!o || (uint64_t)o + 3u >= GUEST_END) return 0;
    if (o >= GUEST_STACK_LO && o < GUEST_STACK_HI) return 0;
    if (o >= IMAGE_LO && o < IMAGE_HI) return 0;
    return mem_is_committed(o) && mem_is_committed(o + 3u);
}

void __wrap_sub_0045fc90(CPU *c){
    uint32_t obj = c->eax;
    /* 0045f3f0 receives this object from the FE singleton 0x8e8410 (a watch on
     * that global shows it is never written: 087a6750 throughout). Its edi is
     * clobbered inside 0045f3f0 with a stack value (00519476, a return
     * address), so the draw must use the singleton again rather than be
     * skipped -- skipping it is what blanked the screen. */
    if (!plausible_object(obj)){
        uint32_t good = MR32(0x8e8410u);
        if (plausible_object(good)){
            static unsigned fixed;
            if (fixed++ < 4)
                fprintf(stderr, "[fo2] menu update: object %08x was clobbered; "
                        "restored the FE screen %08x\n", obj, good);
            c->eax = good;
            __real_sub_0045fc90(c);
            return;
        }
    }
    if (!plausible_object(obj)){
        static unsigned warned;
        unsigned n = warned++;
        if (n < 4)
            fprintf(stderr, "[fo2] menu update: rejected impossible object %08x "
                    "(ret=%08x esp=%08x ebx=%08x edi=%08x)\n",
                    obj, readable32(c->esp), c->esp, c->ebx, c->edi);
        /* The FE screen singleton at 0x8e8410 held that value. Only three
         * instructions in the program write it, yet none can produce a code
         * address, so catch the writer: FO2_SPY_DBG arms the watchpoint on the
         * first search update and this dump shows the last writes into it. */
        if (n == 0 && enabled())
            watch_dump("FE screen pointer that the menu update rejected", 24);
        else if (n == 64)
            fprintf(stderr, "[fo2] menu update: still rejecting impossible objects "
                    "(64 calls); the screen will not draw until this stops\n");
        c->eax = 0;
        POP32(c->eip);
        return;
    }
    __real_sub_0045fc90(c);
}

/* The window that owns the GameSpy browser stores it at +0xc. In the failure
 * that slot held a stack address, so record the window and its browser on the
 * way in: this shows the call at which the slot stops being the heap browser
 * that 005e83c0 returned (FO2_SPY_DBG only, bounded). */
void __wrap_sub_00518fe0(CPU *c){
    if (enabled()){
        static unsigned calls;
        if (calls++ < 24){
            uint32_t window = c->ecx;
            fprintf(stderr, "[spy-dbg] window=%08x state=%08x browser=%08x esp=%08x\n",
                    window, readable_field(window, 0x388u), readable_field(window, 0xcu), c->esp);
        }
    }
    __real_sub_00518fe0(c);
}

/* The next observed trap is the first table-entry load in 00519dd0:
 *   entry = *(uint32_t *)(object[0x4c] + object[4] * 8).
 * Its caller (00518fe0) takes this path after the browser notification.
 * The constructor (00518e50) starts with an empty inline array of 100 slots,
 * whose [begin,end,capacity] pointers are at +0x4c,+0x50,+0x54. The game
 * does not check that its selected index is still live before this read.
 * Reject missing/stale selections as well as out-of-span addresses, using
 * the routine's own failure state (0051a006) rather than dereferencing one. */
void __wrap_sub_00519dd0(CPU *c){
    uint32_t obj = MR32(c->esp + 4u);
    int valid_obj = obj && (uint64_t)obj + 0x57u < GUEST_END &&
                    mem_is_committed(obj) && mem_is_committed(obj + 0x57u);
    uint32_t index = valid_obj ? MR32(obj + 4u) : 0;
    uint32_t table = valid_obj ? MR32(obj + 0x4cu) : 0;
    uint32_t end = valid_obj ? MR32(obj + 0x50u) : 0;
    uint32_t cap = valid_obj ? MR32(obj + 0x54u) : 0;
    uint64_t entry = (uint64_t)table + (uint64_t)index * 8u;
    int valid_entry = valid_obj && table && end >= table && cap >= end &&
                      (end - table) % 8u == 0 && (cap - table) % 8u == 0 &&
                      (cap - table) <= 800u &&
                      (uint64_t)index < (uint64_t)(end - table) / 8u &&
                      entry + 3u < GUEST_END &&
                      mem_is_committed((uint32_t)entry) &&
                      mem_is_committed((uint32_t)entry + 3u);
    uint32_t server = valid_entry ? MR32((uint32_t)entry) : 0;
    valid_entry = valid_entry && server && (uint64_t)server + 0x15u < GUEST_END &&
                  mem_is_committed(server) && mem_is_committed(server + 0x15u);
    if (enabled()){
        static unsigned seen;
        if (seen++ < 8 || (!valid_entry && seen < 16))
            fprintf(stderr, "[spy-dbg] browser table object=%08x index=%u begin=%08x end=%08x cap=%08x entry=%llx server=%08x valid=%d\n",
                    obj, index, table, end, cap, (unsigned long long)entry, server, valid_entry);
    }
    if (!valid_entry){
        static unsigned warned;
        if (warned++ < 4)
            fprintf(stderr, "[fo2] GameSpy browser table has an invalid entry "
                    "(object=%08x index=%u begin=%08x end=%08x cap=%08x server=%08x state=%08x)\n",
                    obj, index, table, end, cap, server, readable_field(obj, 0x388u));
        if (valid_obj){
            MR8(obj + 0x48u) = 0;
            MR8(obj + 0x49u) = 0;
            MR8(obj + 0x4au) = 1;
        }
        c->eax = 0;
        POP32(c->eip);
        c->esp += 4u; /* 00519dd0 is stdcall: ret 4 */
        return;
    }
    __real_sub_00519dd0(c);
}

/* Main-network race clock catch-up probes (FO2_SPY_DBG only): 0050e500 drives
 * the race update and passes a target time to 0050f300, which advances the
 * object's last-report clock at +0x24c8 in 500 ms steps. Record the gap at
 * entry and the host time spent, without touching guest registers or flags.
 * Watchdog evidence from a fresher log: main EIP 50f9d7, whose caller chain
 * includes 50e655; those snapshots are only leads, not proof of the root. */
static uint64_t spy_mono_ns(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int spy_gap_over_5000(uint32_t target, uint32_t last, int64_t *gap){
    if (target == 0xffffffffu || last == 0xffffffffu) return 0;
    *gap = (int64_t)(int32_t)target - (int64_t)(int32_t)last;
    return *gap > 5000;
}

void __wrap_sub_0050e500(CPU *c){
    if (!enabled()){ __real_sub_0050e500(c); return; }
    uint32_t obj = readable_field(c->esp, 4u);
    uint32_t target = readable_field(c->esp, 8u);
    uint32_t last = readable_field(obj, 0x24c8u);
    uint32_t stored = readable_field(obj, 0x24ccu);
    uint32_t frame = readable_field(obj, 0x24d8u);
    uint32_t active_raw = readable_field(obj, 0x24d0u);
    int active = active_raw != 0xffffffffu && (active_raw & 0xffu) != 0;
    int64_t gap = 0;
    int large = spy_gap_over_5000(target, last, &gap);
    static unsigned active_calls, large_logged;
    static int have_prev;
    static uint32_t prev_target, prev_last;
    int changed = !have_prev || target != prev_target || last != prev_last;
    if (large && changed && large_logged < 8){
        large_logged++;
        fprintf(stderr, "[spy-race] e500 large obj=%08x target=%08x last=%08x gap=%lld stored=%08x frame=%08x active=%d\n",
                obj, target, last, (long long)gap, stored, frame, active);
    }
    if (active && active_calls < 4){
        active_calls++;
        fprintf(stderr, "[spy-race] e500 active obj=%08x target=%08x last=%08x gap=%lld stored=%08x frame=%08x\n",
                obj, target, last, (long long)gap, stored, frame);
    }
    prev_target = target; prev_last = last; have_prev = 1;
    uint64_t t0 = spy_mono_ns();
    __real_sub_0050e500(c);
    uint64_t t1 = spy_mono_ns();
    uint32_t after = readable_field(obj, 0x24c8u);
    if (t0 && t1 && t1 >= t0 && t1 - t0 >= 100000000ull){
        static unsigned slow;
        if (slow < 8){
            slow++;
            fprintf(stderr, "[spy-race] e500 slow ms=%llu obj=%08x target=%08x last=%08x after=%08x\n",
                    (unsigned long long)((t1 - t0) / 1000000ull), obj, target, last, after);
        }
    }
}

void __wrap_sub_0050f300(CPU *c){
    if (!enabled()){ __real_sub_0050f300(c); return; }
    uint32_t obj = c->esi;
    uint32_t target = readable_field(c->esp, 4u);
    uint32_t last = readable_field(obj, 0x24c8u);
    int64_t gap = 0;
    int large = spy_gap_over_5000(target, last, &gap);
    static unsigned calls, large_logged;
    static int have_prev;
    static uint32_t prev_target, prev_last;
    int changed = !have_prev || target != prev_target || last != prev_last;
    if (calls < 4){
        calls++;
        fprintf(stderr, "[spy-race] f300 obj=%08x target=%08x last=%08x gap=%lld\n",
                obj, target, last, (long long)gap);
    }
    if (large && changed && large_logged < 8){
        large_logged++;
        fprintf(stderr, "[spy-race] f300 large obj=%08x target=%08x last=%08x gap=%lld\n",
                obj, target, last, (long long)gap);
    }
    prev_target = target; prev_last = last; have_prev = 1;
    uint64_t t0 = spy_mono_ns();
    __real_sub_0050f300(c);
    uint64_t t1 = spy_mono_ns();
    uint32_t after = readable_field(obj, 0x24c8u);
    if (t0 && t1 && t1 >= t0 && t1 - t0 >= 100000000ull){
        static unsigned slow;
        if (slow < 8){
            slow++;
            fprintf(stderr, "[spy-race] f300 slow ms=%llu obj=%08x target=%08x last=%08x after=%08x\n",
                    (unsigned long long)((t1 - t0) / 1000000ull), obj, target, last, after);
        }
    }
}

/* Origin probe for the caught-up race clock (FO2_SPY_DBG only). 004e0bd0 is
 * the network clock update: cdecl gameobj at esp+4, packet at esp+8. It reads
 * the packet timestamp at packet+2, subtracts the local extrapolated time
 * (global 0x8dcc74) plus the accumulated network time (0x8dcb00), and stores
 * the result in gameobj+0x2087c. Log the first four calls only, before and
 * after the original, without touching any guest state. */
void __wrap_sub_004e0bd0(CPU *c){
    if (!enabled()){ __real_sub_004e0bd0(c); return; }
    static unsigned calls;
    int log = calls++ < 4;
    uint32_t gameobj = readable_field(c->esp, 4u);
    uint32_t packet = readable_field(c->esp, 8u);
    uint32_t timestamp = readable_field(packet, 2u);
    if (log)
        fprintf(stderr, "[spy-clock] start obj=%08x packet=%08x ts=%08x net=%08x extrap=%08x race=%08x\n",
                gameobj, packet, timestamp, readable32(0x8dcb00u),
                readable32(0x8dcc74u), readable_field(gameobj, 0x2087cu));
    __real_sub_004e0bd0(c);
    if (log)
        fprintf(stderr, "[spy-clock] start after obj=%08x packet=%08x ts=%08x net=%08x extrap=%08x race=%08x\n",
                gameobj, packet, timestamp, readable32(0x8dcb00u),
                readable32(0x8dcc74u), readable_field(gameobj, 0x2087cu));
}

/* Origin probe for the timer accumulator (FO2_SPY_DBG only). 004e3780 takes
 * the timer object in ESI; the network clock normally uses 0x8dcab8. It adds
 * the measured interval to +0x48, derives the extrapolation at +0x1bc from
 * the base at +0x1b8 and scale at +0x1c0, and keeps its embedded timer at
 * +0x20 (accumulator +0x38, last delta +0x3c, QPC low/high +0x28/+0x2c).
 * Log the first four updates and, separately, up to four updates whose
 * accumulator jumps by more than 5000 (modular 32-bit difference). */
void __wrap_sub_004e3780(CPU *c){
    if (!enabled()){ __real_sub_004e3780(c); return; }
    uint32_t obj = c->esi;
    if (obj != 0x8dcab8u){ __real_sub_004e3780(c); return; }
    static unsigned calls, large_logged;
    int first = calls++ < 4;
    uint32_t before_accum = readable_field(obj, 0x48u);
    uint32_t before_extrap = readable_field(obj, 0x1bcu);
    uint32_t before_base = readable_field(obj, 0x1b8u);
    uint32_t before_scale = readable_field(obj, 0x1c0u);
    __real_sub_004e3780(c);
    uint32_t after_accum = readable_field(obj, 0x48u);
    int32_t accum_delta = (int32_t)(after_accum - before_accum);
    int large = accum_delta > 5000 || accum_delta < -5000;
    int log_large = 0;
    if (!first && large && large_logged < 4){ large_logged++; log_large = 1; }
    if (first || log_large)
        fprintf(stderr, "[spy-clock] update obj=%08x first=%d large=%d accum=%08x->%08x delta=%d "
                "extrap=%08x->%08x base=%08x->%08x scale=%08x->%08x "
                "timer_accum=%08x lastdelta=%08x qpc=%08x:%08x freq=%08x:%08x\n",
                obj, first, large, before_accum, after_accum, accum_delta,
                before_extrap, readable_field(obj, 0x1bcu),
                before_base, readable_field(obj, 0x1b8u),
                before_scale, readable_field(obj, 0x1c0u),
                readable_field(obj, 0x38u), readable_field(obj, 0x3cu),
                readable_field(obj, 0x28u), readable_field(obj, 0x2cu),
                readable32(0x8da710u), readable32(0x8da714u));
}
