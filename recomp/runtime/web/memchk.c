/* wasm debug aid: guest_memchk for NFSU2_MEMCHECK builds (runtime/cpu.h).
 * Reports the first accesses outside the guest span with the lifted function
 * and the CPU registers, then traps like the unchecked build would. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
extern CPU *g_cur_cpu;
/* Optional guest-PC breadcrumb written by a single instrumented lifted shard. */
volatile uint32_t g_fo2_diag_pc;
volatile uint32_t g_fo2_diag_regs[10];
volatile uint32_t g_fo2_diag_step;
volatile uint32_t g_fo2_parser_seq;
volatile uint32_t g_fo2_parser_trace[128];
__attribute__((export_name("fo2_diag_pc")))
uint32_t fo2_diag_pc(void){ return g_fo2_diag_pc; }
__attribute__((export_name("fo2_diag_reg")))
uint32_t fo2_diag_reg(uint32_t i){ return i < 10 ? g_fo2_diag_regs[i] : 0; }
__attribute__((export_name("fo2_diag_step")))
uint32_t fo2_diag_step(void){ return g_fo2_diag_step; }
__attribute__((export_name("fo2_parser_seq")))
uint32_t fo2_parser_seq(void){ return g_fo2_parser_seq; }
__attribute__((export_name("fo2_parser_trace")))
uint32_t fo2_parser_trace(uint32_t i){ return i < 128 ? g_fo2_parser_trace[i] : 0; }

uint8_t *guest_memchk(uint32_t a, unsigned n, const char *fn){
    if ((uint64_t)a + n <= GUEST_END && a >= 0x10000u) return (uint8_t *)(uintptr_t)a;
    static int shown = 0;
    if (shown++ < 16){
        CPU *c = g_cur_cpu;
        fprintf(stderr, "[memchk] %s: %u-byte access at 0x%08x outside the guest span", fn, n, a);
        if (c) fprintf(stderr, "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
                       c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
        fprintf(stderr, "\n");
    }
    if (a < 0x10000u && (uint64_t)a + n <= GUEST_END) return (uint8_t *)(uintptr_t)a;   /* NULL page: report, continue */
    abort();
}
