#ifndef EFFECT_SMP_SCOPE_H
#define EFFECT_SMP_SCOPE_H

#include <stdint.h>
#include "flatout_sha.h"

/* Scoped D3D9 sampler-state save/restore for ID3DXEffect::Begin/End.
 *
 * FlatOut's effect passes write their sampler states straight into the shared
 * g_smp_raw shadow (com.c).  D3DX9's effect state manager snapshots the state
 * that Begin may modify and restores it at End (unless the flags below say
 * otherwise).  Without that, a pass-local AddressU/V=Clamp leaks into the next
 * effect, which samples with repeated coordinates.
 *
 * Only the sampler slots that an effect pass actually declares are saved, and
 * only those are restored.  This helper owns no global state: the caller
 * passes the shared g_smp_raw table in, which keeps the lifecycle testable.
 */

#define FO2_SMP_STAGES 16
#define FO2_SMP_SLOTS  20
#define FO2_SMP_OBJECTS 128
#define FO2_SMP_DEPTH    4

/* ID3DXEffect::Begin flags (d3dx9effect.h). */
#define FO2_D3DXFX_DONOTSAVESTATE        0x1u
#define FO2_D3DXFX_DONOTSAVESHADERSTATE  0x2u
#define FO2_D3DXFX_DONOTSAVESAMPLERSTATE 0x4u

typedef struct {
    uint32_t mask[FO2_SMP_STAGES];
    uint32_t value[FO2_SMP_STAGES][FO2_SMP_SLOTS];
} FO2SmpSnapshot;

typedef struct {
    uint32_t object;
    unsigned depth;                    /* entries in the Begin stack */
    unsigned skipped;                  /* Begins that could not be pushed */
    FO2SmpSnapshot snap[FO2_SMP_DEPTH];
} FO2SmpScope;

typedef struct {
    FO2SmpScope obj[FO2_SMP_OBJECTS];
} FO2SmpScopeSet;

/* Forget every object's stack.  Device Reset does this. */
void fo2_smp_scope_reset(FO2SmpScopeSet *set);

/* Forget one effect object (called when it is destroyed). */
void fo2_smp_scope_forget(FO2SmpScopeSet *set, uint32_t object);

/* Union of sampler slots declared by every pass of one technique.  The .sha
 * parser currently exposes sampler state for stages 0..3, so the upper stages
 * stay clear.  Mask bits outside FO2_SMP_SLOTS are never returned. */
void fo2_smp_effect_mask(const FO2ShaEffect *effect, unsigned technique,
                         uint32_t mask[FO2_SMP_STAGES]);

/* Push one Begin.  Returns 1 when a sampler snapshot was stored, 0 when the
 * Begin only marks the stack (DO NOT SAVE flag, no mask, or depth overflow).
 * A marker is still pushed so the matching End cannot pop an outer Begin. */
int fo2_smp_scope_begin(FO2SmpScopeSet *set, uint32_t object, uint32_t flags,
                        const uint32_t raw[FO2_SMP_STAGES][FO2_SMP_SLOTS],
                        const uint32_t mask[FO2_SMP_STAGES]);

/* Pop one Begin and restore the slots saved by it.  Returns 1 when slots were
 * written back, 0 for a marker/overflow/unmatched End. */
int fo2_smp_scope_end(FO2SmpScopeSet *set, uint32_t object,
                      uint32_t raw[FO2_SMP_STAGES][FO2_SMP_SLOTS]);

#endif
