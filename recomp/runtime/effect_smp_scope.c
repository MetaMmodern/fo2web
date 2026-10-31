#include "effect_smp_scope.h"
#include <string.h>

#define SMP_SLOT_BITS ((1u << FO2_SMP_SLOTS) - 1u)

static FO2SmpScope *scope_find(FO2SmpScopeSet *set, uint32_t object){
    if (!set || !object) return 0;
    for (unsigned i = 0; i < FO2_SMP_OBJECTS; i++)
        if (set->obj[i].object == object) return &set->obj[i];
    return 0;
}

static FO2SmpScope *scope_get(FO2SmpScopeSet *set, uint32_t object){
    FO2SmpScope *s = scope_find(set, object);
    if (s) return s;
    for (unsigned i = 0; i < FO2_SMP_OBJECTS; i++)
        if (!set->obj[i].object){
            memset(&set->obj[i], 0, sizeof set->obj[i]);
            set->obj[i].object = object;
            return &set->obj[i];
        }
    return 0;
}

void fo2_smp_scope_reset(FO2SmpScopeSet *set){
    if (set) memset(set, 0, sizeof *set);
}

void fo2_smp_scope_forget(FO2SmpScopeSet *set, uint32_t object){
    FO2SmpScope *s = scope_find(set, object);
    if (s) memset(s, 0, sizeof *s);
}

void fo2_smp_effect_mask(const FO2ShaEffect *effect, unsigned technique,
                         uint32_t mask[FO2_SMP_STAGES]){
    if (!mask) return;
    memset(mask, 0, sizeof(uint32_t) * FO2_SMP_STAGES);
    if (!effect) return;
    unsigned passes = fo2_sha_pass_count(effect, technique);
    for (unsigned p = 0; p < passes; p++){
        FO2ShaPassState st;
        if (!fo2_sha_pass_state(effect, technique, p, &st)) continue;
        for (unsigned stage = 0; stage < 4u && stage < FO2_SMP_STAGES; stage++)
            mask[stage] |= st.smp_mask[stage] & SMP_SLOT_BITS;
    }
}

int fo2_smp_scope_begin(FO2SmpScopeSet *set, uint32_t object, uint32_t flags,
                        const uint32_t raw[FO2_SMP_STAGES][FO2_SMP_SLOTS],
                        const uint32_t mask[FO2_SMP_STAGES]){
    if (!set || !raw || !mask || !object) return 0;
    FO2SmpScope *s = scope_get(set, object);
    if (!s) return 0;
    if (s->depth >= FO2_SMP_DEPTH){ s->skipped++; return 0; }

    FO2SmpSnapshot *sn = &s->snap[s->depth++];
    memset(sn, 0, sizeof *sn);

    /* DONOTSAVESTATE suppresses every category, including samplers.
     * DONOTSAVESAMPLERSTATE suppresses only this one; the shader-state flag
     * does not affect sampler state. */
    if (flags & (FO2_D3DXFX_DONOTSAVESTATE | FO2_D3DXFX_DONOTSAVESAMPLERSTATE))
        return 0;

    int saved = 0;
    for (unsigned stage = 0; stage < FO2_SMP_STAGES; stage++){
        uint32_t m = mask[stage] & SMP_SLOT_BITS;
        if (!m) continue;
        sn->mask[stage] = m;
        for (unsigned slot = 0; slot < FO2_SMP_SLOTS; slot++)
            if (m & (1u << slot)) sn->value[stage][slot] = raw[stage][slot];
        saved = 1;
    }
    return saved;
}

int fo2_smp_scope_end(FO2SmpScopeSet *set, uint32_t object,
                      uint32_t raw[FO2_SMP_STAGES][FO2_SMP_SLOTS]){
    if (!set || !raw || !object) return 0;
    FO2SmpScope *s = scope_find(set, object);
    if (!s) return 0;

    /* An overflowed Begin has no entry; its matching End only drops the
     * overflow marker.  Doing it before the stack pop keeps End nesting
     * correct when Begin calls were skipped. */
    if (s->skipped){ s->skipped--; return 0; }
    if (!s->depth) return 0;

    FO2SmpSnapshot *sn = &s->snap[--s->depth];
    int restored = 0;
    for (unsigned stage = 0; stage < FO2_SMP_STAGES; stage++){
        uint32_t m = sn->mask[stage] & SMP_SLOT_BITS;
        if (!m) continue;
        for (unsigned slot = 0; slot < FO2_SMP_SLOTS; slot++)
            if (m & (1u << slot)){ raw[stage][slot] = sn->value[stage][slot]; restored = 1; }
    }
    memset(sn, 0, sizeof *sn);
    return restored;
}
