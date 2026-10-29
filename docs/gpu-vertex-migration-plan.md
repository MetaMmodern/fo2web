# Execution plan: move FlatOut 3D vertex processing to WebGPU

## Objective and baseline

Baseline commit: `95a972d` (working car textures, windows and tyres). Preserve that visual correctness while replacing interpreted CPU vertex shaders with the existing shader-bytecode-to-WGSL path. Pixel shading/rasterization already run on the GPU; this task moves vertex transformation and lighting, not the whole renderer.

The supplied Chrome trace `/Users/metamodern/Downloads/perfrecord` covers menus/car selection, approximately 17 seconds. Sampled self time concentrates in `vs_run`, `dst_write`, `src_read`, register lookup and vertex fetching. Several workers wait in `gil_acquire`. This demonstrates an expensive software vertex path; it does not prove the race's entire cost or the precise audio starvation mechanism. Do not optimize the shared lock speculatively.

Do not rewrite shaders by hand, parse game materials again, replace the renderer, or change UVs/textures to make GPU output look plausible. Translate the original shader bytecode and preserve its inputs and constants.

## Read these implementation points first

- `recomp/runtime/com.c`: `draw_up`, declaration cache, `implicit_layout`, `gpuvs_id`, `dip_indexed`, `DrawIndexedPrimitive` fallback, `fill_caps`.
- `recomp/runtime/vsemu.c` and `recomp/runtime/web/vsemu_wgsl.inc`: CPU semantics, `vs_to_wgsl`, `decl_read_wgsl`.
- `recomp/runtime/web/gpu_web.c`: `metal_register_vs`, `metal_vs_ready`, `metal_draw_raw`, `metal_draw_ix`, pool mirroring and arena capacities.
- `recomp/web/src/gpu/encoder.ts`, `proto.ts`, and shader registration code: raw vertices, indexed fetch, constants, texture-coordinate selection, shader validation and pipeline creation.
- `recomp/web/src/app/game.ts`: process environment. Detailed diagnostics were removed; retain periodic summaries.

## Confirmed obstacles in current code

1. `draw_up` gates GPU translation with `!implicit_layout`. This excludes both the special startup quad (`implicit_layout == 1`) and valid inferred mesh layouts (`== 2`). The latter have resolved shader-register mappings and should become eligible.
2. `gpuvs_id` caches by `(shader ID, declaration handle)` only. Inferred declarations all use handle zero. The same shader can see 32-byte and 36-byte layouts, with different UV/color offsets. Simply opening the GPU gate creates incorrect shader-cache reuse.
3. GPUVS must actually be enabled in the process environment. Verify effective settings rather than assuming the existing switch is on.
4. `dip_indexed` supports triangle lists only. Strips/fans expand to lists in guest memory before `draw_up`. They can initially use raw GPU vertex processing after expansion; indexed strips are a later optimization.
5. Registration/readiness and rejection handling must be understood before enabling translation broadly. WGSL/pipeline compilation can be asynchronous. A failed or pending shader must leave the CPU fallback usable.

## Stage 1: establish safe layout identity

Implement this before changing GPU eligibility.

The GPU shader cache must distinguish every baked vertex-fetch layout. Use the shader identity plus element count and each element's `(register, offset, type)` as the authoritative signature. Include stride conservatively. Preserve declaration identity if useful, but it cannot substitute for the signature. Hash lookup is fine only with an equality check to prevent collisions.

Inspect the existing `g_gvs` struct and all lookup/write sites, including debug dumps and non-WebAssembly builds. Keep positive and negative cache entries scoped to the full key. An unsupported layout must not disable a different supported layout using the same shader.

Use the already-resolved `el[]` from `draw_up`; do not construct a second competing layout table. Current inferred layouts are:

| Layout | Position | Normal | Color | UV0 |
|---|---:|---:|---:|---:|
| 32 bytes | 0, FLOAT3 | 12, FLOAT3 | absent | 24, FLOAT2 |
| 36 bytes | 0, FLOAT3 | 12, FLOAT3 | 24, D3DCOLOR | 28, FLOAT2 |

Register numbers come from `vs_input_reg` semantics, not hardcoded assumptions such as UV always being v2. The tyre source shader uses v3 for UV. Unwritten input components/default registers must match the CPU interpreter.

Verification: exercise one shader with two different layouts and confirm different translations/IDs; repeat a layout and confirm reuse. A small focused cache test is useful here. Do not use texture IDs or car names in cache keys.

## Stage 2: enable GPU vertex processing with CPU fallback

Change eligibility to allow valid inferred meshes (`implicit_layout == 2`) as well as explicit declarations. Keep startup quads (`== 1`), widened lines, unsupported layouts and rejected shader instructions on their existing CPU paths. Preserve existing bounds and stride restrictions.

Set `GPUVS=1` deliberately for the FlatOut process after the cache fix. Retain an easy `GPUVS=0` A/B override. Do not overwrite a caller's explicit choice accidentally through environment merge order.

Preserve all draw state passed through the working GPU branch: corrected constants from `fixmat_apply`/`aspect_apply`, pixel shader and constants, texture stages, sampler state, projected coordinates, fog, coordinate-set selection, culling, blending and depth. Audit CPU/GPU output conventions, including clip-space depth and any UI corrections; do not add another axis/depth conversion blindly.

Audit translation lifecycle: pending versus ready versus unsupported versus permanent compile failure. Ensure no GPU draw references an invalid pipeline, no permanently pending entry is incorrectly cached as unsupported, and no failed raw submission drops geometry. If submission returns failure, retain CPU fallback. If an indexed attempt cannot proceed, return to the existing index-expansion path.

Initially leave triangle-strip/fan expansion unchanged. Its expanded raw vertices should run through the GPU shader instead of `vs_run`. This isolates vertex execution from topology changes.

## Stage 3: verify numerical and visual parity

Use two modes of the same build, CPU and GPU, with identical assets/settings. Validate home UI/startup, close-up rotating car, glass/reflections/tyres, another car, and a race containing track/world geometry. Do not assume tracks use car layouts: report explicit/inferred layouts observed and fallback reasons.

For strong numerical evidence, reuse any existing shader-check/capture support first. If unavailable, add a bounded diagnostic compute dispatch that evaluates the generated vertex logic for a small recorded batch and reads outputs asynchronously. Compare against `vs_run` for exactly the same raw vertices and constant snapshot. Capture/readback must be diagnostic-only, never in the normal frame loop. Shader-stage reuse must be explicit; copying a second approximate implementation does not validate the actual GPU shader.

Compare clip position, both colors, all texture outputs and fog. Record absolute/relative errors with tolerances appropriate to float arithmetic. Start around 1e-4 absolute/relative for ordinary finite values; inspect reciprocal/rsqrt and near-zero cases rather than hiding discrepancies by widening thresholds. Match register initialization, masks, swizzles, saturation and source modifiers. Do not require bit equality for GPU floating-point arithmetic.

Inspect NaN/infinity behavior and position w explicitly. Validate 32/36-byte UV/color fetching and D3DCOLOR channel order. A passing red triangle or plausible car silhouette is insufficient.

Gate: no new WebGPU validation errors, no missing geometry, correct tyres/body/windows/UI, and actual nonzero GPU-vertex draws. If a particular shader is unsupported, log its stable reason once and fall back; do not block progress on translating every shader immediately.

## Stage 4: measure before adding further optimizations

Retain lightweight periodic counters for CPU/GPU vertex draws and vertices, CPU fallback reasons, interpreter time, raw upload bytes, pool builds/updates, draw/pass count, arena overflow, CPU waits, GPU completion time, and audio underruns/buffer depth. Extend existing counters rather than emitting per-draw logs. Check timing flags: an inactive instrumentation flag can make `vs_run` report zero time despite substantial work.

Repeat the same menu -> car selection -> race sequence. Compare unprofiled FPS and audio behavior as well as Chrome samples; profiler overhead and shader warm-up affect measurements. Expect `vs_run` to shrink on eligible geometry. Do not promise a specific FPS increase or complete audio fix before measurement.

If substantial track geometry remains on CPU, rank fallback vertex counts by layout/shader/reason. Fix the dominant supported-layout exclusion first. Add genuinely missing vertex formats using the real declaration and CPU semantics. Keep unknown formats on CPU.

## Stage 5: reduce repeated vertex upload/strip expansion

Only after GPU parity is established, optimize topology/persistent buffers if counters justify it. First exploit the existing indexed triangle-list pool with generation-based invalidation; verify dynamic writes/locks invalidate mirrors.

For strips/fans, the safest incremental path is cached expansion of source indices into triangle-list indices, keeping vertices in a persistent raw pool. Key by IB identity/generation, index width, start, primitive count and topology. Respect signed BaseVertexIndex and stream offsets, buffer bounds and index width. Preserve strip parity across degenerates; never treat 0xffff as a restart without establishing D3D semantics. MinVertexIndex is a range declaration, not another base offset.

Alternatively add native strip pipelines only if the existing protocol cleanly supports them. This requires topology and stripIndexFormat in pipeline keys plus correct index handling; it is not a one-line topology switch. Triangle fans require expansion because WebGPU lacks triangle-fan topology.

Do not mix this stage with the first eligibility/cache change. Commit each verified milestone independently.

## Stage 6: reassess audio and input stalls

GPU migration should shorten CPU rendering sections, but verify whether audio improves. Correlate remaining underruns with lock holds/waits and thread scheduling. Find the audio producer's actual dependency on guest state before changing locks or yielding. Do not release the shared game lock around arbitrary code touching mutable guest memory. Do not increase audio buffering as the primary rendering fix; it can mask starvation at the cost of latency.

## Build and handoff

From repository root, build WASM:

```sh
cd recomp
WASI=/Users/metamodern/Documents/Github/Personal/fo2web/deps/wasi-sdk-34.0-x86_64-macos SKIP_GENCHECK=1 J=4 OBJCACHE=/Users/metamodern/Documents/Github/Personal/fo2web/.fo2-objcache ./web/build.sh
```

Then build frontend and verify WASM magic:

```sh
cd web
node build.mjs
xxd -l 8 ../out/web/nfsu2.wasm
```

Expected header: `00 61 73 6d 01 00 00 00`. Never hand off a build while linking is still running. Report build results, which paths migrated, fallback counts, parity checks, performance evidence and remaining issues. A user reload starts the new process; ordinary asset rebuilds do not require backend restart.

Start with stages 1–3, then provide a testable build. Do not disable the confirmed rectangular-texture capability fix, restore old rendering code, or reintroduce verbose tracing. Preserve unrelated changes. Generated WASM is ignored by Git; commit source milestones and retain a usable CPU A/B path.
