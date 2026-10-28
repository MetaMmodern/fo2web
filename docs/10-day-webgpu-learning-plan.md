# From a red triangle to debugging FlatOut yourself

You have read the first WebGPU Fundamentals article and understand its red triangle. That is enough to start. You do **not** need to memorize WebGPU first. The goal of these ten days is to learn how to answer, with your own eyes and tools, **where a bad frame first becomes bad**.

Spend **at least one hour studying each day**. Then practice for as long as you want. The practice may take 20 minutes or several evenings. Move to the next day when you can explain what you observed, not when the clock runs out. A screenshot, a short note, or a tiny code change counts as a project outcome. You do not have to fix a whole subsystem daily.

## The picture in plain language

FlatOut 2 was written for Windows and Direct3D 9, an older graphics API. The browser speaks WebGPU. This project sits between them:

```text
FlatOut asks to draw a car
  → our C code receives the old Direct3D call
  → our C code sends a draw command and data to the browser
  → TypeScript creates WebGPU textures, buffers, and shaders
  → WebGPU draws pixels into the canvas
```

Think of each arrow as a place where information might be lost or misread. A misshapen car could start with vertex data or indices; a correctly shaped but dark car could start with textures, shader math, or lighting values. Seeing a problem on screen does not tell you which arrow is responsible.

### Five file types you will meet

| Name | In ordinary words | Why you care |
| --- | --- | --- |
| `.ts` | TypeScript, familiar browser code | `recomp/web/src/gpu/encoder.ts` executes the WebGPU commands. |
| `.c` | C source code, compiled into the browser's game module | `recomp/runtime/com.c` imitates the old Direct3D methods. Read small functions; you need not learn all of C first. |
| `.h` | C declarations and shared definitions | It tells multiple C files what a command or structure means. |
| `.inc` | C source text included inside another C file | `recomp/runtime/web/vsemu_wgsl.inc` builds WGSL shader text. Despite the suffix, you read it like C. |
| `.sha` | FlatOut's own text file describing a material and its shaders | Your installed `data/shader/pro_car_body.sha` says how the original car body should be drawn. These are game files, so keep them out of Git. |

Other words: a **vertex** is one point of a 3D model; an **index** tells the GPU which points form a triangle; a **texture** is image data sampled by a shader; a **shader** is a little GPU program; a **render pass** is a batch of drawing into a particular image. A **cube texture** holds six images and is sampled with a 3D direction, often for reflections or lighting.

## Your milestones

1. **Days 1–3:** Build and inspect tiny WebGPU examples. You can explain a triangle without copying code blindly.
2. **Days 4–5:** Find the same WebGPU objects in FlatOut and inspect a real frame.
3. **Days 6–8:** Trace a specific bad pixel or shape back to its data and make a small experiment.
4. **Days 9–10:** Pick an issue, form competing explanations, test one, and decide the next edit without AI choosing for you.

## Day 1 — Make the red triangle yours

**Study for at least an hour:** Revisit [WebGPU Fundamentals](https://webgpufundamentals.org/webgpu/lessons/webgpu-fundamentals.html). Stop after the triangle. Write down what `device`, `canvas context`, `shader`, `pipeline`, `render pass`, and `queue.submit` each do, in your own words. If one is unclear, change that part of the example and watch what happens.

**Practice afterward:** Make a tiny triangle page in a scratch folder under `tools/` or outside the repo. Change the background, triangle color, and one vertex position without asking AI for code. Keep the working example.

**Project outcome:** You have your own minimal WebGPU playground. It will let you learn an API in isolation before looking at FlatOut's much larger renderer.

## Day 2 — Feed data to the triangle

**Study:** Read [WebGPU Vertex Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-vertex-buffers.html). Focus on how the bytes in a buffer match the shader's `@location` inputs. Look at the official [rotatingCube sample](https://webgpu.github.io/webgpu-samples/?sample=rotatingCube) once the triangle makes sense.

**Practice:** Change your triangle so its three corners have different colors coming from a vertex buffer. Intentionally give the pipeline the wrong stride, observe the result, then restore it.

**Project outcome:** A note explaining why the *same shader* can draw a mess when the buffer layout is wrong. This will later help with FlatOut's “crumpled foil” car.

## Day 3 — Learn the Inspector on your tiny example

**Study:** Read the [WebGPU debugging article](https://webgpufundamentals.org/webgpu/lessons/webgpu-debugging.html), especially **WebGPU-Inspector** and the “show a solid color” shader technique. Read the [Inspector's own README](https://github.com/brendan-duncan/webgpu_inspector) for installation and its Inspect, Capture, and Record views. The Inspector is a browser extension that can show GPU textures, buffers, draw calls, and render state. It is useful here.

**Practice:** Install the Inspector in your development browser. Open DevTools **before reloading** your triangle page. Capture one frame, find your draw call, inspect its vertex buffer and shader, and identify the final color attachment. If the panel is missing, the Inspector README says to reopen DevTools; if capture shows nothing, reload and focus the page.

**Project outcome:** One screenshot or note showing exactly where your triangle's input data and output image appear in the Inspector. This becomes your reference when FlatOut's capture looks overwhelming.

## Day 4 — Find the triangle's equivalents in FlatOut

**Study:** Read the first part of [WebGPU Debugging and Errors](https://webgpufundamentals.org/webgpu/lessons/webgpu-debugging.html) on validation messages. Then open `recomp/web/src/gpu/index.ts` and `encoder.ts`. Ignore most of the code. Search for `createRenderPipeline`, `beginRenderPass`, `setBindGroup`, and `draw`.

**Practice:** Launch FlatOut with the Inspector open. Capture **one menu frame**. Find a simple UI draw before looking for the car. Locate its pipeline, texture, and render target. Match one Inspector draw to one line in `build/flatout-smoke-live.log` if possible. Capture can be expensive; one frame is enough.

**Project outcome:** A diagram in `docs/renderer-notes.md` saying: “game draw → browser draw → canvas image,” with the function names you found. You can now navigate the renderer without reading every file.

## Day 5 — Follow one image into the GPU

**Study:** Read [WebGPU Textures](https://webgpufundamentals.org/webgpu/lessons/webgpu-textures.html) and [Cube Maps](https://webgpufundamentals.org/webgpu/lessons/webgpu-cube-maps.html). Understand that a normal image uses two coordinates, while a cube texture uses a direction and six faces.

**Practice:** In an Inspector capture of a car menu, find one texture used by the body draw. Does its image look sensible? In the log, search for `[sha-bind]` (which texture the game requested) and `[watch] tex` (what the browser received). Follow its ID from `com.c` through `gpu_web.c` to `encoder.ts`. Check all six faces if it is a cube.

**Project outcome:** A simple table: texture ID, what it should contain, what the Inspector shows, and whether the six faces arrived. This answers one concrete question about car rendering without changing shader code.

## Day 6 — Separate broken shape from broken color

**Study:** Read [Direct3D 9 DrawIndexedPrimitive](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitive) only for its parameter meanings. Then revisit the Day 2 buffer lesson. Here, Direct3D 9 is what the *game* requests; WebGPU is what our *browser* executes.

**Practice:** Find one car draw in the log (`ps=6` is the body material in current captures). Record the number of vertices, indices, triangle type, and stride (bytes per vertex). In the Inspector, inspect that draw's vertex buffer and output. If possible, temporarily force a solid body color. If the shape still looks wrong with a solid color, focus on vertex/index data or the vertex shader. If the shape looks normal, focus on textures and fragment shading.

**Project outcome:** Your own evidence-based statement: “the first visible problem is shape” or “the first visible problem is color,” with a capture supporting it. This is a major decision point.

## Day 7 — Read the car material one instruction at a time

**Study:** Read the [WGSL introduction](https://webgpufundamentals.org/webgpu/lessons/webgpu-wgsl.html). Open the original `pro_car_body.sha` from your installation. For now, read only its texture names, inputs, and the few instructions that fetch images and combine colors. Use `recomp/runtime/web/vsemu_wgsl.inc` only to find how one of those instructions is translated. Look up terms as needed; do not try to understand the whole shader in one day.

**Practice:** In a temporary local change, replace the car body's final color with one input at a time: solid red, base texture, normal as RGB, then diffuse lookup. Capture each result. Revert each diagnostic edit before the next. The debugging article uses this exact “show the value as a color” method.

**Project outcome:** A small set of images showing the **first input or operation that becomes wrong**. This can guide a real shader fix much better than guessing from the finished car.

## Day 8 — Use the same method on text or UI

**Study:** Revisit texture sampling and UV coordinates in [WebGPU Textures](https://webgpufundamentals.org/webgpu/lessons/webgpu-textures.html). Look at the official [text rendering sample](https://webgpu.github.io/webgpu-samples/?sample=textRenderingMsdf) to learn what a glyph atlas is. FlatOut may use a different font technique; the idea of picking part of an image remains useful.

**Practice:** Choose one garbled label or misplaced UI item. Capture its draw. Inspect the source texture, the coordinates used to select a glyph, and the destination rectangle. Change one clearly wrong value in a local branch, or document which one you would change and why.

**Project outcome:** One focused UI diagnosis or small fix. You have reused the same investigation method on a different bug class.

## Day 9 — Understand square output, resizing, and stutter

**Study:** Read [Resizing the Canvas](https://webgpufundamentals.org/webgpu/lessons/webgpu-resizing-the-canvas.html). A canvas has a CSS display size and a separate pixel-buffer size. Then read the first part of [Timing Performance](https://webgpufundamentals.org/webgpu/lessons/webgpu-timing.html) on JavaScript time versus GPU time.

**Practice:** Resize the game window and record four numbers: CSS canvas size, canvas pixel size, game render size, and final displayed image rectangle. Compare before/after screenshots. For stutter, compare one menu segment with a car visible and one without, using CPU time, GPU time, and draw count from the project's overlay/log. Avoid changing resolution and performance settings together.

**Project outcome:** One size diagram and one timing comparison. You can tell whether a square image comes from the game size, browser canvas, or final scaling; and whether a slowdown starts on CPU or GPU.

## Day 10 — Lead one investigation without AI choosing the answer

**Study:** Reread your notes and the one resource most relevant to the issue you choose. Spend the full study hour explaining the pipeline aloud or in writing, from the game's request to the final pixel.

**Practice:** Pick **one** problem: car, track, text, missing UI, resolution flicker, or performance. Write two possible causes. Choose one experiment that produces different results under those causes. Run it, save the evidence, and decide the next code change yourself. You may use AI afterward for repetitive editing or searching, but require it to point to the exact file, draw, texture, or measured value behind every suggestion.

**Project outcome:** A short investigation note and a commit containing either a small fix or the diagnostic tool you built. You have completed a full observe → explain → experiment → decide cycle yourself.

## A useful rule for every future bug

Start with the picture, then work backward: **final canvas → draw call → shader and render state → textures and buffers → game request**. At each step ask, “Does the data still look right here?” The first place it stops looking right is where you work. The Inspector helps you see the browser side; the game logs and C code help you see what the old game asked for.
