# FlatOut 2 `.sha` effect implementation plan

This is a handoff plan for replacing the placeholder `D3DXCreateEffect` implementation. The goal is to render FlatOut's original materials through the existing D3D9/WebGPU bridge. Do not replace the game's renderer or port its gameplay.

## Observed starting point

- `runtime/win32.c` handles `D3DXCreateEffect` and passes a guest pointer and byte length to `com_d3dx_effect_create`.
- `runtime/com.c` currently ignores those bytes. Every effect gets the same hard-coded vertex and pixel shaders; all parameter lookups return one dummy handle; `Begin` reports one pass; `BeginPass` applies one texture and those shaders. This is sufficient to boot, but cannot render different materials correctly.
- The existing `runtime/vsemu.c` accepts D3D shader *tokens*, and `runtime/web/vsemu_wgsl.inc` translates supported tokens to WGSL. The WebGPU command path already registers pixel shaders; the vertex shader path translates against the active D3D vertex declaration at draw time. Keep those paths.
- The installed GOG data examined for this plan has **69 `.sha` files**. Most contain `vs.1.1`/`ps.1.1` assembly inside a D3DX effect. `post_box4.sha` instead contains HLSL and `compile ps_1_1 PostProcessPS()`. At least `emu_car_lights.sha` has multiple passes. `post_copy.sha` has `VertexShader = null`.
- The neighboring `nfsu2-web` checkout provides the shader translator and GPU bridge already reused here. Its `d3dx_hle.c` accelerates NFS's own lifted effect code; it is not a parser for FlatOut `.sha` text.

The source of truth for syntax is the user's installed `data/shader/*.sha`. Do not copy game shader source, precompiled game shaders, or generated game code into this repository. Tests may read an install path supplied locally.

## Target behavior and boundary

Implement a narrow `ID3DXEffect` compatible layer for the effect sources and COM methods that FlatOut actually uses. `D3DXCreateEffect` must parse the `source, source_len` byte span, create one independent effect instance, and return useful diagnostics on failure. Treat `source_len` as authoritative: validate guest memory bounds, copy into a temporary NUL-terminated buffer, and never use `strlen` on the guest pointer.

The effect has three distinct forms of state:

1. **Immutable program:** declared parameters, shaders, techniques, ordered passes, and pass assignments parsed from the source. Cache compiled immutable programs by source hash if repeated, but do not share mutable parameter values.
2. **Mutable values per effect:** parameter textures and scalar/vector/matrix values set by the game, active technique, and active pass. Handles must identify the owning effect and the specific parameter/technique/pass; one address must not stand for every name.
3. **Device state:** selected shaders, texture stages, vertex and pixel constants, sampler states, texture stage states, and render states. Apply it through the same shadow state that normal `IDirect3DDevice9` calls use. A guest draw must observe the selected effect state, and state blocks must be able to capture and restore it.

Do not put host pointers in guest handles. Use stable guest allocated handle records or another validated guest address scheme; check owner, kind, index and lifetime on every lookup. Maintain COM reference counts for textures retained by `SetTexture`; release them when replaced or when an effect is destroyed. Add `IF_EFFECT` cleanup in `com_destroy` so repeated effect creation cannot leak host parse structures. Shader objects/programs need their own lifetime policy; avoid making every instance permanently `CF_DEVICE` unless intentionally shared and bounded.

## Parser and compiler

Build two separate components, for example `runtime/flatout_sha.c/.h` and `runtime/flatout_sha_asm.c/.h`, then connect them in `com.c`. Include them in the actual web build's runtime list (and any native build used for tests).

**Effect parser:** tokenize case insensitively, keeping quoted strings and source locations. Recognize `//` comments, semicolon comments *inside assembly*, CRLF, whitespace, braces, angle bracket references, numeric literals with `f`, and identifiers. Use balanced brace parsing for `decl`, `asm`, `technique`, `pass`, and `sampler_state` blocks; do not use a regex to delimit nested blocks. Preserve assignment order within a pass, because later assignments may override earlier ones. Reject unresolved references and unknown active statements with a filename/source-hash plus line number. Parse at least:

- `Texture TexN`, `const string inputStreamFormat`, `vertexshader` and `pixelshader` declarations, `decl { ... }`, `asm { ... }`, `Technique Tn { Pass Pn { ... } }`.
- Pass shader references including `null`; `Texture[stage] = <parameter>`; `VertexShaderConstantF[n]` and `PixelShaderConstantF[n]` with `float4(...)` values.
- Sampler `AddressU/V`, `Min/Mag/MipFilter`; render states `SrcBlend`, `DestBlend`, `AlphaBlendEnable`, `AlphaTestEnable`, `ZWriteEnable`, `Zbias`, `DitherEnable`, `TextureFactor`; stage states `ColorOp/Arg1/Arg2`, `AlphaOp/Arg1/Arg2`, `TexCoordIndex`. Map names and enum values to D3D9's numeric types explicitly, checking the existing GPU state bridge supports each one.
- Multiple shader declarations and passes. Keep `inputStreamFormat` as metadata/diagnostic; the active D3D declaration remains authoritative for vertex fetching. Do not infer a vertex layout from the string alone.

Run a corpus audit before implementing each syntax family: count every active statement and assembly mnemonic in all 69 installed files, and record unsupported items. Comments must not count. The current inspection found one HLSL effect (`post_box4.sha`); keep an explicit exception path for it. A parser success count that silently discards unsupported statements is not a passing result.

**Assembly compiler:** emit real D3D9 shader token words consumed by `vs_compile`; never convert the assembly straight to guessed WGSL. Emit the version word, instruction and operand tokens, and `END`. Support the corpus's register names (`v`, `r`, `c`, `t`, `oPos`, `oD`, `oT`), write masks, source swizzles and modifiers (including `-r` and `1-v`), instruction modifiers (`_sat`, `_x2`, `_x4`, `_d2`), and leading `+` coissue syntax. At minimum the observed opcodes include `mov`, `add`, `sub`, `mul`, `mad`, `dp3`, `dp4`, `m4x4`, `m4x3`, `m3x3`, `rsq`, `rcp`, `max`, `sge`, `lrp`, `tex`, and `texreg2ar`. Generate and validate vertex `dcl_*` tokens with usage/index, not merely register numbers. Use named D3D token constants and a table driven opcode/operand count; check emitted words against `vs_disasm`, `vs_unknown`, and known examples. Reject unknown tokens or invalid register ranges.

`vs_compile` accepting a token stream does **not** prove the WGSL translator implements it. `ps_to_wgsl` currently skips unknown pixel opcodes, and `texreg2ar` appears in `post_colorremap_by_alpha.sha`. Add a strict translator validation mode, implement any reached missing opcodes and source modifiers, and treat a skipped instruction as a compilation error. Check coissue behavior against the original semantics before declaring shaders correct. Also check generated WGSL and WebGPU pipeline creation errors; `effect_shader_object` currently can return an object even if pixel translation fails.

For `post_box4.sha`, defer a general HLSL compiler. Implement its one observed function as a narrow equivalent (four texture samples averaged), or provide a local runtime compiler only if one is already available for browser execution. Preserve the effect's sampler bindings and constants. The exception must be named and tested; never report the HLSL effect as parsed while rendering it with the bootstrap shader.

## Effect COM integration

Implement the methods from `M_EFFECT` in `runtime/d3d9_tables.h` according to their actual slots and argument counts. The current method switch starts near `com.c`'s `iface==IF_EFFECT` branch. The minimum useful behavior is:

- `GetDesc`, `GetParameterDesc`, `GetTechniqueDesc`, `GetPassDesc`: return real counts and metadata in the correct 32 bit D3DX ABI layout. Allocate stable guest strings for returned names. Verify layouts against the D3DX9 headers before writing fields.
- `GetParameter`, `GetParameterByName`, `GetTechnique`, `GetTechniqueByName`, `GetPass`, `GetPassByName`: return distinct validated handles or null on lookup failure. Check whether FlatOut uses annotations/semantics/array elements before implementing them.
- `SetTexture`/`GetTexture` and any reached `SetValue`, `SetFloat`, `SetVector`, `SetMatrix`, or array/transposed variants: copy the actual value into the addressed parameter. An unimplemented setter that the game calls must log clearly and fail, rather than return success and leave the old value.
- `SetTechnique`/`GetCurrentTechnique`/`ValidateTechnique`/`FindNextValidTechnique`: track the selected technique and validate handle ownership.
- `Begin`: write the selected technique's real pass count to the output pointer. `BeginPass(index)`: validate the index, select the pass's shaders, evaluate texture references against **current** parameter values, apply constants and pass state. `CommitChanges`: update dirty parameters for the active pass (needed when the game changes a texture or constant between draws). `EndPass`/`End`: clear effect scope without corrupting subsequent device calls.
- `GetPixelShader`, `GetVertexShader`, `GetDevice` and COM `AddRef`/`Release`: return correct objects and ownership if observed. The current helper allocates a pixel shader with the vertex shader interface and marks both as device owned; review that before exposing shader objects through getters.

Factor reusable internal D3D state setters out of the `IF_DEV` branch or route effect assignments through a safe shared function. Directly changing only `g_cur_tex`, `g_cur_vs`, `g_cur_ps_id`, or `g_rs[]` misses related shadows, reference management, sampler state, dirty flags, and state block capture. Specifically, stage 0 and stages 1–3 must update `g_raw_tex`, `g_bind_tex`, `g_tex_stage`, `g_cur_tex` consistently; shader selection must update both raw and translated IDs; constant writes must use `g_vsk.f` and `g_psc`; render state changes must call the existing apply path. `null` vertex or pixel shaders must actively clear the previous shader. `post_copy.sha` requires `VertexShader = null` so the existing fixed function/transformed vertex path is used.

Avoid touching the existing `EndPass` fallback draw until real effect draws have been measured. It conditionally submits a synthetic quad if the game made no draw during the pass; it can hide a missing draw or duplicate geometry. Keep a counter that distinguishes guest submitted draws from fallback draws and remove the fallback once confirmed unnecessary.

## Order of work and acceptance gates

1. **Inventory and observability.** Audit the installed source corpus. Add bounded logs keyed by effect hash/filename if known: parse result, technique and pass counts, selected pass, handle lookup misses, setter calls, selected shader IDs, textures 0–3, constant ranges, and first unsupported instruction/state. Capture which effects and methods are reached during one car selection and race. The name of a `.sha` may not be passed to `D3DXCreateEffect`; identify it by source hash matched against the local corpus, not a guessed filename.
2. **Small post effect.** Implement `post_copy.sha` first: one texture, `ps.1.1`, null VS, clamp/linear sampler, proper pass lifecycle. Acceptance: its pixel token disassembles correctly, WGSL compiles, and a known image renders without inheriting a stale VS or stale texture stage.
3. **Common materials.** Implement `pro_default_static.sha` and `pro_car_body.sha` through the same parser/assembler. Confirm stage 0–3 textures, `c32` VS constant, swizzles/modifiers, and vertex declaration match a car selection draw. Compare screenshots to the native game if available. Do not attribute every remaining visual defect to the parser: texture format/upload, vertex layout, and shader translation may fail independently.
4. **Pass state and remaining assembly.** Cover all parsed assembly effects, multiple passes (`emu_car_lights.sha`), fixed function stage states, and all observed sampler/render state assignments. Run a complete corpus compile audit: for each file report parsed techniques/passes, translated shaders, and unsupported syntax/instructions. Zero silent skips is the gate.
5. **One HLSL exception and runtime regression.** Add the `post_box4.sha` equivalent; verify post processing plus menu and race rendering, no WebGPU validation errors, and no regression to startup/input/audio. Measure frame time and audio stutter separately after graphics is correct; those may have separate causes.

Useful tests are a native parser/assembler corpus audit against a user supplied game folder; token round trips or disassembly checks for a few representative shaders; and one browser smoke run through menu, car selection, and race with GPU validation errors captured. Never bundle the game folder into test fixtures or browser source control. A visibly correct car is a later integration gate, not proof that all 69 effects work.

## Decisions that require measurement

- Which effect COM setters/getters and handles the guest actually calls. Instrument first; implement only the reached ABI, but make unsupported reached calls explicit.
- Whether WebGPU vertex shader translation handles each FlatOut declaration and whether CPU vertex fallback is fast enough for shaders that fail translation. This can affect both visuals and audio responsiveness.
- Whether `CommitChanges` is called in a rendering scope and whether pass state is restored by game state blocks. Do not invent restoration behavior without a trace.
- Whether `post_box4.sha` is loaded in the tested path. It is one exception in the data corpus, not necessarily the first visual blocker.

The previously suggested order “`post_copy` and `pro_car_body`, then all `.sha` files” skipped null VS, HLSL, multi pass state, true parameter handles, and validation of translator support. Those are now explicit gates.
