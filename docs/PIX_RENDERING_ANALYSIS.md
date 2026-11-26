# Original D3D9 PIX rendering analysis

## Scope and evidence

This is a static investigation of the CSV exports and `.PIXRun` captures in
`/Volumes/transferbuf/FO2_PIX_Results/raw`.  It does not change the port.
No screenshot or intermediate-target exports were supplied.

The disposable extractor and its JSON cache are outside the repository at:

```text
/private/var/folders/rc/xzt5y7vs6nldh4p8m6kkk_hr0000gn/T/opencode/
  pix_post_extract.py
  pix-post-analysis.json
```

All event IDs below are EIDs from the named CSV, not line numbers.

### PIXRun extraction

The binary records were parsed narrowly, using the record layout documented by
the apitrace `pixrun.py` prototype.  This matters because the real D3D calls
are largely PIX asynchronous records: CSV preserves their signatures but
omits pointed-to data.  The binary supplies the shader tokens, float payloads,
object descriptions, and `Unlock` upload bytes used below.  Extraction scripts
and JSON caches remain disposable under
`/private/var/folders/rc/xzt5y7vs6nldh4p8m6kkk_hr0000gn/T/opencode/`.

## Capture inventory

| Capture | BeginScene–EndScene ranges | Finding |
| --- | --- | --- |
| `low_test_track.csv` | 949–11136 | One frame; no fullscreen post chain. |
| `high_test_track.csv` | 950–12548 | One frame; one complete 13-draw post chain. |
| `high_high-speed.csv` | 950–8014, 8963–16869, 17818–24400 | Three frames; each has the same 13-draw chain. |
| `sunny_track_fields.csv` | 950–21279, 22228–41437 | Two frames; same 13-draw chain. |
| `night_track.csv` | 950–26028, 26977–50826 | Two frames; same 13-draw chain. |
| `menu_frame.csv` | 972–1691 | One frame; no scene post chain. |

The environment note confirms that the low capture has post-processing off,
whereas the high capture has it on.  This makes the post-chain comparison
between those two captures reliable despite their differing resolutions and
camera timing.

## Verified pipeline shape

The original does **not** perform a one-pass bloom composite.  Every high
quality scene frame has the following 13 fullscreen triangle-strip draws after
the main scene has been copied from the back buffer:

1. Three 2-primitive (four-vertex) preparatory/composition passes.
2. One 2-primitive draw to the first ping-pong target.
3. Four more 2-primitive ping-pong draws with the same pixel shader.
4. Two 2-primitive combination/copy draws.
5. Two 22-primitive (24-vertex) ping-pong draws with the same pixel shader.
6. One final 2-primitive composition draw back to the main scene target.

For a concrete, fully repeatable instance, the second Fields frame in
`sunny_track_fields.csv` uses:

| EID | Primitives | Render target | Pixel shader | Texture stages 0 / 1 |
| ---: | ---: | --- | --- | --- |
| 39788 | 2 | `0x1A9D55E8` | `0x1A9D0A70` | `0x045FEE40` / `0x1A9E0648` |
| 39872 | 2 | `0x045FEEE8` | `0x1A9D0288` | `0x1A9D5540` / `0x1A9D5720` |
| 39959 | 2 | main `0x04554EF8` | `0x1A9D0390` | `0x045FEE40` / prior stage-1 binding |
| 40044 | 2 | `0x04588C60` | `0x1A9D0128` | `0x045FEE40` / prior stage-1 binding |
| 40185 | 2 | `0x045B8C10` | `0x1A9D0860` | `0x045CDCE0` / same |
| 40243 | 2 | `0x04588C60` | `0x1A9D0860` | `0x04593B80` / same |
| 40301 | 2 | `0x045B8C10` | `0x1A9D0860` | `0x045CDCE0` / same |
| 40359 | 2 | `0x04588C60` | `0x1A9D0860` | `0x04593B80` / same |
| 40431 | 2 | `0x045B8C10` | `0x1A9D0910` | `0x045CDCE0` / `0x045FEE40` |
| 40465 | 2 | main `0x04554EF8` | `0x1A9D0390` | `0x045CDCE0` / `0x045FEE40` |
| 40515 | 22 | `0x04588C60` | `0x1A9D0860` | `0x04593B80` / same |
| 40572 | 22 | `0x045B8C10` | `0x1A9D0860` | `0x045CDCE0` / same |
| 40663 | 2 | main `0x04554EF8` | `0x1A9D09C0` | `0x04593B80` / `0x1A9D5678` |

The same topology recurs with different object addresses at
`high_test_track.csv` EIDs 11191–12111, `night_track.csv` EIDs
49224–50144, and all three frames of `high_high-speed.csv`
(23134–24054 for its final frame).

## PIXRun values recovered

The following details are now verified from the second Fields frame
(`sunny_track_fields.PIXRun`), rather than inferred from call signatures.

### Render targets and textures

| Object | Description | Use in post chain |
| --- | --- | --- |
| `0x04554EF8` | 3072×1920 `D3DFMT_X8R8G8B8` | Main scene target/final destination |
| `0x045FEE40` | 3072×1920 `D3DFMT_A8R8G8B8`, one mip | Full-size copied scene input |
| `0x1A9D55E8` / `0x045FEEE8` | 3072×1920 `D3DFMT_A8R8G8B8` | Full-size intermediate targets |
| `0x04588C60` / `0x045B8C10` | 256×256 `D3DFMT_X8R8G8B8` | Ping-pong targets |
| `0x045CDCE0` / `0x04593B80` | 256×256 `D3DFMT_X8R8G8B8`, one mip | Ping-pong textures |

Thus the blur chain is deliberately 256×256, not full resolution.  `0x15` and
`0x16` in PIX object metadata are the D3D9 values for `A8R8G8B8` and
`X8R8G8B8`, respectively.

### Pixel-shader constants and shader payloads

PIXRun contains the exact ps_1_1 token streams for each distinct shader bound
at EIDs 39788–40663.  The repeated shader at EIDs 40185, 40243, 40301,
40359, 40515, and 40572 is a four-sampler average: it samples stages 0–3,
adds them, then multiplies by the embedded `(0.25, 0.25, 0.25, 0.25)`.
This directly confirms the `post_box4` interpretation.

The per-pass dynamic values (components in register order) are:

For constant calls the count is argument word 6, the byte count is word 8,
and the float payload starts at word **11** (zero-based after the function
ID). Word 10 is reserved, not the first float. The byte count and remaining
record length were checked against `count * 16` for every update below.

| Draw EID | Pixel shader | Register values written for that pass |
| ---: | --- | --- |
| 39788 | `0x1A9D0A70` | `c5 = (0.296875, 0.59375, 0.1171875, 0)` |
| 39959 | `0x1A9D0390` | `c3 = (1, 1, 1, 1)` |
| 40044 | `0x1A9D0128` | `c3 = (0.24, 0.24, 0.24, 0)`, `c4 = (0.4276316, 0.4276316, 0.4276316, 0)`, `c5 = (0.222, 0.707, 0.071, 0)` |
| 40431 | `0x1A9D0910` | `c3 = (0.6270592, 0.6270592, 0.4129413, 0)`, `c4 = (1, 1, 1, 1)` |
| 40465 | `0x1A9D0390` | `c3 = (0.6270592, 0.6270592, 0.4129413, 0)` |
| 40663 | `0x1A9D09C0` | `c3 = (1, 1, 1, 0.09375)` |

The pass at EID 40044 copies sampled **alpha** into RGB and A, subtracts
`c3`, then multiplies by `4*c4`. It does **not** consume `c5`, despite that
register being set nearby. Its RGB result before target conversion is
`(source.a - 0.24) * 1.71052623`. The final EID 40663 shader copies texture
0 RGB and computes alpha as `texture1.a * 0.09375`, then uses the captured
SRCALPHA/INVSRCALPHA blend state.

Earlier notes in this document incorrectly described that high-pass formula
and truncated the right/bottom vertex coordinates. The corrected formula,
payload offsets, and coordinates above/below supersede those descriptions.

### Dynamic pretransformed vertex payloads

The capture records the writes into post buffer `0x04516F98`, not merely the
Lock addresses.  For EID 39788, the preceding upload (EIDs 39709–39710) is
four 24-byte `XYZRHW + UV` vertices:

```text
(-0.5,    -0.5, 0, 1, 0, 0)
(-0.5,  1919.5, 0, 1, 0, 1)
(3071.5,  -0.5, 0, 1, 1, 0)
(3071.5,1919.5, 0, 1, 1, 1)
```

The 48-byte four-UV uploads preceding the 256×256 ping-pong draws carry
explicit signed texel offsets.  For example EIDs 40075–40076 use
`±0.001953125` offsets in their four UV pairs.  The later ping-pong uploads
use the corresponding larger offsets (`±0.005859375`, `±0.009765625`, and
`±0.013671875`), rather than relying on implicit sampler offsets.  The
22-primitive passes also upload their own 24-vertex, 48-byte-per-vertex data
(EIDs 40478–40479 and 40526–40527); they are a tiled geometry pass, not a
full-screen quad repeated by the driver.

### State that must be retained

The calls immediately around the Fields sequence verify these requirements:

- Post vertex shaders are explicitly `NULL`; the passes use pretransformed
  streams.  The quads use a 24-byte stream; the 22-primitive draws use a
  48-byte stream.  Examples: EIDs 39954/39958, 40184/40185, and
  40464/40465.
- Texture stages 0–3 are deliberately all bound to the same ping-pong texture
  for the repeated blur draws, with clamp addressing, point minification,
  linear magnification, and linear mip filtering.  See EIDs 40160–40183.
- The two 22-primitive draws are not accidental large quads: the original
  supplies 24 vertices and runs the same ping-pong shader.  See EIDs
  40514–40515 and 40571–40572.
- The final pass is a two-texture pass, and surrounding code enables alpha
  blending with `SRCALPHA` / `INVSRCALPHA`; see EIDs 40586–40588 and
  40640–40663.
- Render/depth state is changed before target switches and subsequently
  restored.  Therefore implementation must model persistent state, not infer
  each draw from only the calls directly adjacent to it.

## Comparison with the port

The current implementation already contains the structural mechanisms the
trace requires:

- `recomp/runtime/posproj_stream.h` accepts both the 24-byte one-UV stream and
  the 48-byte four-UV stream.
- `recomp/runtime/com.c` dispatches pretransformed post draws through
  `draw_posproj`, including up to 24 vertices.
- `recomp/runtime/flatout_sha.c` models stages 0–3 and the `post_box4` special
  case; the four same-texture bindings in the capture are consistent with that
  case.

That is evidence that the port is no longer skipping the *shape* of the
chain.  It is **not** evidence of pixel equivalence.  PIXRun now rules out
missing shader data, effect constants, post vertex uploads, and target
dimensions/formats as unavailable evidence; it does not provide intermediate
images or prove that the port interprets every recovered value identically.

### Shader-to-asset comparison

The shipped `data/shader/post_*.sha` files are available locally and were
assembled with the port's own parser. Comparing instruction/operand tokens
to the captured programs establishes this mapping:

| Fields draw EID(s) | Shipped shader |
| --- | --- |
| 39788 | `post_luminance_to_alpha.sha` |
| 39872 | `post_colorremap_by_alpha.sha` |
| 39959, 40465 | `post_copy.sha` |
| 40044 | `post_highpass_luminance.sha` |
| 40185, 40243, 40301, 40359, 40515, 40572 | `post_box4.sha` |
| 40431 | `post_combine2.sha` |
| 40663 | `post_mask.sha` |

Apart from the opcode mismatch fixed below, the assembly programs match after
ignoring instruction-length hints and coissue markers. In these particular
coissued instructions RGB and alpha writes are independent, so sequential
execution is equivalent. The HLSL box4 special case differs in instruction
encoding: it divides the final sum by four instead of multiplying by a
shader-local `def c0 = 0.25`. Both express the same four-tap average. No
evidence here justifies replacing these shaders or hardcoding captured
track-dependent constants into the port.

### Implemented corrections and limits

- **Post POSITIONT pixel centres:** `draw_posproj` previously mapped screen XY
  directly to clip space. D3D9 uses integer pixel centres while Metal/WebGPU
  use half-integer centres. The recovered `[-0.5, size-0.5]` quad therefore
  landed half a pixel too far up/left. The post-only conversion now adds
  `0.5` before clip mapping, without changing UVs or programmable vertex
  shader positions. Tests cover full-size and 256×256 targets. This changes
  post sampling alignment; its visual impact still requires user verification.
- **D3D opcode compatibility:** the assembler and decoder both incorrectly
  numbered `texreg2ar` as 67 (`texbem`), whereas PIX emits 69 (`0x45`).
  Both now use 69. Previously their shared mistake masked itself on locally
  assembled shaders; this correction alone is **not** evidence that the
  live colour-remap path had been skipped.
- **Regression checks:** `recomp/tests/flatout_pix_post_unit.c` compares literal
  captured tokens to assembled remap/high-pass/mask programs, checks WGSL and
  MSL source emission, and runs their arithmetic against independently
  calculated expectations. These are offline CPU checks, not GPU execution
  or visual parity verification.
- **Draw diagnostics:** the existing `FO2GLOW=1` switch
  now logs these at draw time for at most 39 post draws (three 13-pass
  frames), including texture sizes, sampler state, first/last UVs, and
  `c3`–`c5`. Effect-boundary logs alone can precede later guest updates.
- **X8 sampled alpha:** the browser renderer mapped both `A8R8G8B8` and
  `X8R8G8B8` to `bgra8unorm` without forcing sampled X8 alpha to one. The
  web translator and fixed-function fallback now convert X8 samples' alpha
  before arithmetic/dependent reads, using a per-draw resolved-format mask in
  the former PS uniform padding. A8/BC alpha is preserved, uniform reuse
  includes the mask, and X8-to-A8 copies materialize alpha one. This is not
  established as the cause of the reported whitening/spread difference, and
  its implementation still needs runtime verification.

## Supplied web capture comparison

Evidence:

- `/Users/metamodern/Downloads/fo2-frame-1795700010862.wgpuc`
- `/Users/metamodern/Downloads/localhost-1795700000061.log`
- `/Users/metamodern/Downloads/localhost_8080__gpuInspector=1&env=FO2GLOW=1.png`

The `.wgpuc` header identifies WGPUCAP 1 / schema 3, frame 114. The capture
has 2,396 WebGPU commands and 16 CPU-provenance draw records, including all
13 post draws. This was inspected statically, not replayed.

| Original Fields EID | WebGPU command | Port PS |
| ---: | ---: | ---: |
| 39788 | 2230 | 43 |
| 39872 | 2239 | 44 |
| 39959 | 2248 | 37 |
| 40044 | 2266 | 39 |
| 40185 | 2275 | 40 |
| 40243 | 2284 | 40 |
| 40301 | 2293 | 40 |
| 40359 | 2302 | 40 |
| 40431 | 2311 | 41 |
| 40465 | 2320 | 37 |
| 40515 | 2329 | 40 |
| 40572 | 2338 | 40 |
| 40663 | 2347 | 42 |

The captured group-0 binding-2 uniform payloads verify **bit-for-bit** equality
with PIXRun for the registers consumed by the corresponding shaders:
`c5` for luminance-to-alpha; `c3` for copy; `c3/c4` for extraction and
combination; and `c3` for the final mask. The final mask's alpha multiplier is
`0.09375`, exactly as in PIXRun. Early log records have zero there because the
effect is fading in at startup; they must not be mistaken for the captured
steady-state draw.

The captured source-vertex bytes also match the original UVs for every
available source vertex (the provenance limit is six vertices, so this is
**not** a check of all 24 vertices in each tiled draw). The combination
pass's full-size-source UV offset is resolution-dependent: both captures use
minus half a logical source texel. Its other UVs match directly. The
ping-pong targets remain 256×256 and the logs show the authored four-UV
offsets and sampler filters. The captured WGSL contains the four-sample
average and AR-dependent LUT sample, not fallback copy shaders.

The log identifies textures 1/2 as format 22 (`X8R8G8B8`). Their captured
BGRA snapshots contain varying alpha (not 255 throughout), while the captured
pre-fix WGSL samples that raw alpha. This confirms the X8 format discrepancy
above. The final mask texture is BC2/DXT3, not X8, so its alpha must **not**
be forced opaque. In this post sequence, fixing X8 alpha alone does not
demonstrate a change to the bloom RGB; the consumed constants already match.

Limitations:

- Original full-size target: 3072×1920; port logical size: 1280×1024, backed
  by 1970×1576 textures. These are not matched-resolution/aspect pixel
  comparisons. The captured main-target pipelines use four samples; the
  original capture's settings note specifies 8× AA.
- The capture's validation-error list contains six destroyed `blit rect`
  texture errors and four destroyed canvas-texture errors. The runtime log
  does not show corresponding messages. The capture list is not correlated
  to particular recorded commands and could include inspector activity;
  it does not establish that a particular game post draw was rejected.
- Intermediate snapshots in the web capture are end-of-capture resource
  contents, not an image after every pass. There are no equivalent original
  screenshots in the supplied original capture's screenshots directory.

The disposable comparison and its machine-readable result are
`compare_fields_capture.py` and `fields-original-port-comparison.json` in the
temporary directory listed above. The format fix has offline regression
coverage in `recomp/web/test/texture-alpha.test.mjs` (synthetic records and
mock WebGPU objects), plus translator emission checks in
`recomp/tests/flatout_pix_post_unit.c`; neither executes GPU shaders.

### Final supplied browser measurement

Files:

- `/Users/metamodern/Downloads/fo2-frame-1795702249094.wgpuc`
- `/Users/metamodern/Downloads/localhost-1795702232440.log`
- `/Users/metamodern/Downloads/localhost_8080__gpuInspector=1&env=FO2GLOW=1 (1).png`

The new capture is frame 177, with 2,404 WebGPU commands. Full-size textures
8/9 are now **3072×1920**, matching the original full-size texture dimensions.
The three main-target post draws have sample count 4; offscreen post draws
have sample count 1. The bounded log's earlier 1280×1024 records should not
be used to deny the actual physical texture dimensions in this capture.

All 13 post draws are present, at commands
`2237, 2246, 2255, 2273, 2282, 2291, 2300, 2309, 2318, 2327, 2336, 2345, 2354`.
The same consumed-register comparisons against original Fields EIDs
39788–40663 pass bit-for-bit. Available source UVs match exactly except
for the resolution-dependent full-size-source offset at the combination
pass (original EID 40431 / WebGPU command 2318).

The new sampling helpers are present in captured WGSL, and captured uniform
masks verify the new format plumbing is active: four X8 blur bindings give
mask 15; the combination/copy/mask draws have only stage 0 forced opaque
(mask 1); luminance/remap and A8 scene extraction have mask 0. In particular
the final BC2 mask texture keeps its own alpha. The first full-size
pretransformed vertex is captured at clip `(-1, 1, 0, 1)`, confirming that
the half-pixel POSITIONT conversion is active.

Two unresolved findings prevent calling this pixel-exact:

1. Guest post vertices still span logical 1280×1024, which the port scales
   to the physical 3072×1920 targets. Most post UVs are normalized and match.
   However, combination-pass UV1 starts at `(-0.5/1280, -0.5/1024)`, versus
   original `(-0.5/3072, -0.5/1920)`. In physical source texels, the port's
   offset is `(-1.2, -0.9375)` rather than `(-0.5, -0.5)`: a residual
   `(-0.7, -0.4375)`-texel difference. This is a port logical/physical
   resolution-handling difference, not evidence that the user failed to
   increase resolution, and not proof of the reported whitening cause.
2. The capture lists 12 submit validation errors involving destroyed
   `blit rect 3072x1920` temporary textures. Their relationship to game-frame
   submission versus inspector readback remains unestablished. This is not
   a clean validation report and should not be described as one.

**Verdict:** post-chain topology, consumed constants, authored blur offsets,
and activation of both implemented corrections are verified. Pixel-identical
rendering and resolution-independent post sampling are not verified. No
bloom issue is removed on the strength of this capture alone.

The final machine-readable comparison is `fields-final-comparison.json` in
the temporary directory above.

## What is not established

The following are assumptions or unavailable information, not findings:

- Pass names such as “high-pass”, “box blur”, “sunbeam”, or “final bloom”
  cannot be assigned to a particular EID from these CSVs alone.
- The target contents, MSAA state, and final sampled intermediate images were
  not recovered.
- The semantic effect/pass names are not present in the PIXRun records, but
  the shipped shader comparison now identifies the programs as listed above.

## Highest-value follow-up

Do not tune extraction constants or replace the blur: the actual captured
uniforms and UVs already match the original. Verify the new format correction
on a user run after rebuilding WASM and the frontend. Any further visual
parity claim needs matched camera/resolution/quality original and port images,
or original intermediate target exports, to distinguish input-scene shading
differences from post sampling/output conversion.
