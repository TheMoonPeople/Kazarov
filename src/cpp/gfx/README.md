# gfx

The port's Vulkan renderer (1.3 or later, see "Device"). `gfx.hpp` is the whole public API (namespace `gfx`); everything
else is internal. No game header is reachable from here: `platform/`, `gfx/` and `audio/` build as
`dc_host`, without `port.h` or the game's include paths. Replacement units include
`"gfx/gfx.hpp"`.

## Lifecycle

- `RendererInit(window, config)` creates the device, the swapchain, the main targets and every
  pipeline (below). `config.pipeline_cache` defaults to `save/pipeline_cache.bin`;
  `config.progress(done, total)` is called on the calling thread while pipelines compile.
- `BeginFrame()` / `EndFrame()` bracket a frame drawn as it is called. Four frames are in flight:
  a tick submits a canonical render and its display renders, so the GPU may run two ticks behind.
  `BeginFrame` returns false when there is nothing to draw to (minimised); draws are then dropped and
  `EndFrame` does nothing. Every frame starts on the main target. `EndFrame` presents. The game's
  ticks are recorded instead and rendered from their display lists (below).
- `config.present_mode`: FIFO, Mailbox (FIFO where unsupported) or Immediate (Mailbox, then FIFO,
  where unsupported). `SetPresentMode` changes it at any time; the swapchain is recreated with it
  as for a resize.
- `config.offscreen` renders without a surface or swapchain: each frame is drawn to the main target
  alone, at the window's pixel size, and `EndFrame` submits it and presents nothing. `ReadbackFrame`,
  `SnapshotFrame`, `kPreviousFrame`, depth queries and resizes behave as with a swapchain, so a
  headless run reads back the same pixels. a game's `--headless` run uses it when the Vulkan loader has
  no `VK_EXT_headless_surface` (SDL's offscreen driver cannot then make a Vulkan window), and
  `--offscreen` forces it.
- `RendererResize()` after the window's pixel size changes (`WindowPollEvents` does it); the
  swapchain and main target follow at the next `BeginFrame`. Main-target contents are lost.
- Threads: the caller and the device worker split the work; see Threads below.
- Textures, palettes, meshes and render targets can be created, updated and destroyed at any time,
  inside or outside a frame. Destruction is deferred until the GPU is done with them; a handle is
  dead as soon as it is destroyed outside a recording (handles carry a generation). When the texture
  or mesh table is full, a create outside a frame or recording first runs the pending destructions.
  The table sizes are the build maximums, which `RendererConfig::texture_slots` and `mesh_slots`
  can lower (for tests).

## Threads

- Three modes, from `RendererConfig::render_thread` or `KZ_RENDER_THREAD=off|lockstep|async` (the
  variable wins): Off (default) runs everything on the caller; Lockstep and Async run device work on
  a worker. Android is always Off.
- Device-thread rule: device work runs on the worker. Recording stays on the caller. A public call
  that touches the device is forwarded to the worker and waited for, or run inline when the caller
  is the worker or holds a `RenderLease`.
- `RenderLease` parks the worker and holds it off the device, so the holder runs device work inline.
  The game holds one per tick around its canonical render and other per-tick device work.
- Idle hook (`SetRenderIdleHook`, Async only): the worker calls it when the queue is empty and no
  lease is wanted. It runs on the worker and must only call `gfx` functions. Its return value is how
  long the worker sleeps before calling it again; zero calls again at once. `WakeRenderThread` and
  any dispatch or lease end the sleep early.
- The worker never recreates the swapchain. A display frame from the idle hook that finds the
  swapchain out of date or pending a resize fails and leaves the swapchain to the game thread's next
  recording.
- A stale handle to a slot the worker reuses for a grab twin resolves to nothing, because the
  generation no longer matches.
- `KZ_GFX_THREAD_CHECKS` (on in builds without `NDEBUG`) makes every device function fail fast when called off the device thread.
- A GPU timestamp zone must begin and end on one thread, so the frame upload zone is only recorded in Off.

## Spaces

- **Logical space** is the game's `RendererConfig::logical_width` x `logical_height` (e.g.
  640x480), y down. `Draw2D` vertices, `DrawState::scissor_rect`, `Clear` rects, `ReadDepth` rects
  and copy rects on `kMainTarget`/`kPreviousFrame` are in it. `LogicalWidth()`, `LogicalHeight()`
  and `LogicalFrame()` return it once `RendererInit` has run.
- On the **main target** (window pixel size) the logical frame is the largest rect of aspect
  `RendererConfig::display_aspect` (e.g. 4:3) that fits the window, centred with whole-pixel
  offsets. Logical space is scaled to it per axis. Coordinates outside the frame reach past it;
  nothing clips to the displayed area unless a scissor says so (or the layout is Letterbox).
  `GetLogicalMapping(target)` returns
  `pixel = logical * scale + offset`. What the window shows past the frame depends on the frame
  layout ("Aspect").
- A **render target** is `logical_width x logical_height` scaled by the render scale (pixels per
  logical texel; `config.render_scale`, 0 = `round(window height / logical_height)`, at least 1), offset 0.
  `SetRenderScale` recreates every render target, blitting its contents over.
- A render target created with **`share_main_depth`** (`CreateRenderTarget`, `NamedRenderTarget`)
  has no depth buffer of its own: drawing into it tests and writes the main target's depth and
  stencil as drawn so far this frame, which is how the GS draws into another FRAME_1 with the same
  ZBUF (shadow volumes counted into `shadow_buf` against the scene). Its pixels must line up with
  the main target's, so it is the main target's pixel size and mapped like it (the logical frame,
  letterboxed) whatever size is asked for, ignores the render scale, and is recreated with the
  main target on resize (contents lost). `Clear` of depth or `ClearStencil` while it is the target
  clears the main target's.
- **Texture coordinates** in `Draw2D` are logical texels of the texture (its `TextureDesc` size,
  or its logical size for render targets, the logical frame for `kPreviousFrame`). In `Vertex3D` they are
  normalised.
- **Meshes** draw over the whole target (not letterboxed). With `kMeshScreenUv` a vertex samples
  an image of the target where it lands itself, `Vertex3D::uv` an offset from there. `MeshConstants::mvp` (column-major)
  maps to Vulkan clip space: x right, y down, z/w in [0, 1] with **near at 1** (reverse-Z).
  Front faces are counter-clockwise as seen on the target with y down.
- **Depth** is D32 float with an 8-bit stencil (`D32_SFLOAT_S8_UINT`; `D24_UNORM_S8_UINT` where
  the device cannot attach the former), reverse-Z: cleared to 0 (far), tests are GEQUAL/GREATER,
  so the game's ZTST values map one to one. A `Vertex2D::z` is the same depth (1 near), so 3D sprites can test
  against meshes when the projection agrees. `ReadDepth` returns these units.

## Aspect

`RendererConfig::layout` (`SetFrameLayout` outside a frame) is a `FrameLayout`: the aspect mode and
`ui_scale`. The logical frame is centred in the window in both modes.

- **`AspectMode::Fill`** (`video.aspect` `auto`). The window past the frame is the game's too:
  - Meshes draw through the target's mapping, so the frame's 480 rows keep their place and a wider
    window shows more of the world at the sides at retail's vertical field of view (a narrower one
    keeps the frame's width and shows more above and below). `VisibleLogicalRect(target)` is the
    logical rect the target's pixels cover; the game's culls test against it.
  - **The full-frame rule.** Along each axis on which the target shows past its frame (both on the
    main target, the targets mapped like it and a full-height frame target, x on another frame
    target), a scissor or clear rect, and
    every axis-aligned rectangle of a `Quads` or four-vertex strip `Draw2D` whose texture is none, an
    image of the frame (`kPreviousFrame`, a frame target, a target sharing the main depth) or
    another image that shows past its frame, is carried to the target's edge on every side where it
    reaches the frame's edge from inside (its span crosses or ends on the edge, starting inside).
     The rectangle itself is drawn as given; flanking quads cover the rest, with texture coordinates
     continued (so an image of the frame shows its own sides) and colour, fog and depth held at the
     edge; flanks always sample with clamped wrap, whatever the binding's wrap. A rectangle that
     instead covers the whole frame with an ordinary texture (a cutscene still, a full-screen
     backdrop) is a cover: it is stretched to the target's edge on each axis the
     target shows past its frame, so a 4:3 still fills a wider window rather than leaving its sides
     bare. Fades, however tiled, full-frame fills and bands, the previous-frame feedback and frame
     grabs drawn back therefore reach the window's edges; HUD pieces (textured from ordinary
     textures, and never covering the whole frame) and anything wholly outside the frame do not
     grow. A `Lines` draw lying wholly within the frame (edges included) is clipped to the frame's pixels
     on each axis the target shows past: a line on the frame's edge falls on a pixel boundary and would
     otherwise rasterise into the bar beside it. A line reaching past the frame is not clipped.
     Nothing of a host list is carried:
    a display render's `options.overlay`, and the list itself with `options.host` (the FPS counter,
    whose glyph runs would otherwise streak to the window's edge wherever one lies across the
    frame's).
    A copy or blit between two images that show past their frames, whose rects cover both frames
    along an axis, copies them edge to edge along it.
  - **Frame targets** (`CreateRenderTarget(..., frame_target = true)`; the game's `frame_*` grabs)
    are the main target's pixel width with their logical width laid over its logical frame, and
    rows at the main target's scale, so a grab of the frame keeps its sides. A full-height one (as
    tall as the logical frame) is the main target's size and keeps the rows above and below as well.
    Other render targets keep their logical size at the render scale.
- **`UiAnchor`** (`SetUiAnchor`, `UiAnchorScope`; the default anchors nothing). While one is set,
  each `Draw2D` the UI mapping places on the main target is classified by the centre of its bounds
  against the anchor's four limits and moved, whole and unscaled, by the pixels between the UI
  frame's edge and the edge of what the target shows on each side it falls to: a piece 40 units
  from the frame's left edge lands 40 units from the window's. `UiAnchor::Side` sends every draw
  to one side or corner (an element made of many draws, the mini map); limits inside the frame
  split a panel whose pieces belong to different corners. The side is chosen when the draw is
  made and travels with it in a display list; the shift is taken at render time, so it follows
  the window and `ui_scale` (a HUD scaled past the window is pulled back to its edges). An
  anchored draw is never flanked by the full-frame rule and its scissor stays the frame's unless
  the anchor's `scissor` flag (`Side(x, y, true)`) moves it with the draw. A positive `max_aspect`
  moves draws to the edges of a centred area no wider than that aspect instead, so a layout made for
  16:9 stays together on an ultrawide window. With `AspectMode::Letterbox` at `ui_scale` 1 the shift
  is zero.
- **`AspectMode::Letterbox`** (`aspect = 4:3`). Nothing is carried past the frame: draws and clears on
  the main target stop at the frame's pixels, so the bars keep what they held, and frame targets are
  ordinary render targets.
- **`ui_scale`** (default 1) scales 2D that tests no depth, writes none and samples no image of the
  frame about the main target's centre (`GetUiMapping`); 2D with depth (3D sprites) and images of
  the frame stay on the logical mapping, among the meshes. It does not know which 2D the game
  placed from a 3D projection (lock-on corners, name tags): at a scale other than 1 those move with
  the HUD.

## Colour and alpha units

| Quantity | Units |
|---|---|
| Colour in textures, palettes, targets, `Clear` | bytes as the GS stores them |
| Alpha in textures, palettes, targets | **0xFF is GS 0x80** (saturating). Upload GS alpha through `ConvertPs2Alpha`. |
| Vertex colour and alpha (`Vertex2D`, `Vertex3D`) | GS bytes |
| `Clear` alpha | GS byte (0x80 opaque) |
| `MeshConstants` colours | modulation, 1.0 = GS 0x80 |
| `DrawState::alpha_ref`, `texa_ta0`, `GsBlend::fix` | GS bytes |

- Untextured, the vertex colour is written as is (0x80 is mid grey). Textured, MODULATE:
  `Cv = Ct * Cf / 0x80`, `Av = At * Af / 0x80`, so 0x80 is 1.0 and 0xFF nearly doubles.
- Mesh lighting: `colour = ambient * ambient_material + sum(max(0, n . -light_direction[i]) *
  light_color[i]) * diffuse`, alpha `diffuse.w`, times the vertex colour / 0x80 with
  `kMeshVertexColor`, saturated at 0xFF as VU1 saturates. Without `kMeshLit` it is `diffuse`
  (times the vertex colour). `kMeshShadow` is flat `diffuse`. `specular` is carried, unused.
- **TEXA**: a texture with `has_alpha = false` (24-bit TIM2, a 24-bit render target) gets
  `alpha = (texa_aem && rgb == 0) ? 0 : texa_ta0` per texel, before filtering.
- **Palettes**: an `Index8` texture is looked up in its palette before filtering, so a linear
  filter blends four palette entries (base level only). Palettes are 256x1 RGBA8 textures;
  swapping one between two draws of a frame is ordered as written.
- **Fog**: `Cv = Cs * F + FOGCOL * (1 - F)`, F per vertex (`Vertex2D::fog`; for meshes
  `clamp(fog[0] + fog[1] / w, fog[2], fog[3]) / 255` with `kMeshFog`), applied when
  `DrawState::fog`.
- **Alpha test** compares `Av` in GS units (rounded) with `alpha_ref` by ATST. AFAIL is KEEP
  (the only value the game uses).
- A **target without alpha** (`has_alpha = false`) stores alpha 0x80 whatever is drawn, so its
  destination alpha reads 0x80, as the GS reads it for PSMCT24.

## Blending

GS `((A - B) * C >> 7) + D`, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}. The fragment shader writes
the source colour and the factor C where it can compute it (As or FIX / 0x80). Vulkan then
computes `src * Fs (op) dst * Fd`. `F` below is `DST_ALPHA` for Ad; `1-F` its complement. For As
and FIX it is the shader's factor, reaching the blend one of three ways, all clamped to 1.0 and so
the same value:

- As on a target with alpha: `SRC_ALPHA`. The first output's alpha is As itself, so no second
  output is needed.
- FIX: `CONSTANT_ALPHA`, with `min(FIX / 128, 1)` set through the dynamic blend constants (deduped
  per command buffer; FIX / 128 is exact in float).
- As on a target without alpha: `SRC1_ALPHA`, the dual-source output, because the first output's
  alpha is forced to 1 there.

Pipelines whose blend reads the second source use `draw.frag` and write it; the rest use
`draw_single.frag` (the same `draw_body.glsl` without that output). The blend table is built per
target kind, and equations that coincide share a state.

| A | B | D | GS result | Fs, Fd, op | shader writes | exact |
|---|---|---|---|---|---|---|
| = | = | Cs | Cs | blend off | Cs | yes |
| = | = | Cd | Cd | 0, 1, add | | yes |
| = | = | 0 | 0 | 0, 0, add | | yes |
| Cs | Cd | Cd | Cs c + Cd (1 - c) | F, 1-F, add | Cs | yes |
| Cs | Cd | 0 | (Cs - Cd) c | F, F, sub | Cs | yes |
| Cs | Cd | Cs | Cs (1 + c) - Cd c | 1, F, sub | Cs (1 + c) | no, see below |
| Cd | Cs | Cs | Cd c + Cs (1 - c) | 1-F, F, add | Cs | yes |
| Cd | Cs | 0 | (Cd - Cs) c | F, F, rev-sub | Cs | yes |
| Cd | Cs | Cd | Cd (1 + c) - Cs c | F, 1, rev-sub | Cs | no: Cd - Cs c |
| Cs | 0 | Cd | Cd + Cs c | F, 1, add | Cs | yes |
| Cs | 0 | 0 | Cs c | F, 0, add | Cs | yes |
| Cs | 0 | Cs | Cs (1 + c) | 1, 0, add | Cs (1 + c) | As/FIX yes; Ad: Cs |
| Cd | 0 | Cs | Cs + Cd c | 1, F, add | Cs | yes |
| Cd | 0 | 0 | Cd c | 0, F, add | Cs | yes |
| Cd | 0 | Cd | Cd (1 + c) | DST_COLOR, 1, add | c | As/FIX yes; Ad: Cd |
| 0 | Cs | Cd | Cd - Cs c | F, 1, rev-sub | Cs | yes |
| 0 | Cs | Cs | Cs (1 - c) | 1-F, 0, add | Cs | yes |
| 0 | Cs | 0 | 0 (saturated) | 0, 0, add | | yes |
| 0 | Cd | Cs | Cs - Cd c | 1, F, sub | Cs | yes |
| 0 | Cd | Cd | Cd (1 - c) | 0, 1-F, add | Cs | yes |
| 0 | Cd | 0 | 0 (saturated) | 0, 0, add | | yes |

Where it is not exact, and why:

- **C above 0x80.** Blend factors and shader outputs to a UNORM attachment saturate at 1.0, so
  As, Ad and FIX above 0x80 blend as 0x80. The alpha test still sees the unsaturated value. The
  game's alpha values are 0x80 or below except in rare overbright effects.
- **Cs (1 + c) - Cd c** (A=Cs, B=Cd, D=Cs). The shader scales the source and the blend subtracts;
  the scaled source saturates before the subtraction rather than after. With C = Ad the source
  cannot be scaled and the result is `Cs - Cd Ad`.
- **Cd (1 + c) - Cs c** (A=Cd, B=Cs, D=Cd). No destination factor exceeds 1; it becomes
  `Cd - Cs c`.
- **D = A, B = 0 with C = Ad.** `Cs (1 + Ad)` and `Cd (1 + Ad)` need a factor above 1 that the
  shader cannot know; they become Cs and Cd.
- The destination alpha is the stored alpha, which is the last source alpha written (the GS does
  not blend alpha either), saturated at 0x80.

## Stencil and colour writes

| `DrawState` field | Meaning |
|---|---|
| `stencil_test` | stencil test on; off, the stencil is neither tested nor written |
| `stencil_front`, `stencil_back` | per face (by winding, as culling tells them; lines are front): `compare` (Vulkan's order), `fail`, `pass`, `depth_fail` ops, `reference`, `compare_mask`, `write_mask` |
| `color_write_mask` | `ColorWriteBits`; 0 writes no colour (a stencil or depth-only pass) |

`ClearStencil(value, rect)` clears the current target's stencil (the main one for a target that
shares it). Every frame's stencil keeps what the last frame left until cleared. The GS has no
stencil; it is here for techniques the GS did with destination alpha (DATE) or colour counting.

All stencil state is core dynamic state (Vulkan 1.3's `STENCIL_TEST_ENABLE` and `STENCIL_OP`, 1.0's
masks and reference), so it adds no pipelines. The colour write mask is dynamic through
`VK_EXT_extended_dynamic_state3` (`extendedDynamicState3ColorWriteMask`) where the device has it;
otherwise a partial mask writes every channel. `RendererConfig::dynamic_color_write_mask = false`
forces that path.

A draw that writes no colour and does not alpha test (stencil shadow volumes, depth-only passes)
binds a pipeline with no fragment stage and a fixed empty write mask, one per family, whichever
way the mask is supported; it also takes the dynamic mask out of the tracked state. With an alpha
test the shader still runs: the dynamic mask is set to 0, or without it a colourless alpha-test
pipeline per family and texture mode is precompiled.

## Ordering

Everything happens in the order it was called. An update or copy into a texture or mesh the
frame's draws have not touched yet goes into a batch that runs before the frame's draws, keeping
the render pass whole; one into something already drawn with this frame splits the pass and runs
in place. Copies and blits inside a frame always run in place. Sampling the target being drawn
to is refused: snapshot it first (`SnapshotFrame`, `CopyTexture` from `kMainTarget`).

## Pipelines

3 families (2D triangles, 2D lines, meshes) x 52 blend states x 3 texture modes (none, RGBA,
index + palette) x alpha test on/off = 936 pipelines, plus one depth-only pipeline per family
(939), or 948 without a dynamic colour write mask, which adds an alpha-testing colourless one per
family and texture mode. The 52 blend states are the Vulkan equations the table above yields for
a target with alpha (source-alpha, constant-alpha and destination-alpha forms) and for one without
(dual-source forms, plus the ones shared with FIX). All are created in `RendererInit` on up to
eight threads against the pipeline cache. With `RendererConfig::prog0_meshes` two more mesh
families (Vu_prog0 and Vu_prog0 skinned, one vertex specialisation constant apart) give
5 x 52 x 3 x 2 + 5 = 1565 pipelines (1580 without a dynamic colour write mask); without it the
families stop after the plain mesh family. Depth
test, depth write, compare op, the whole stencil state, cull mode, front face, viewport, scissor
and topology within a class are dynamic state, so they add none. The cache file is checked against the device before use and written through a temporary
file and a rename.

A CPU rasterizer (lavapipe, `VK_PHYSICAL_DEVICE_TYPE_CPU`) describes the same pipelines but compiles each one
the first time a draw selects it (`PipelineAt`): lavapipe keeps every pipeline's machine code, which for the
whole set is gigabytes per renderer, more than a CI runner has once a few test processes run at once.
`KZ_PIPELINES_LAZY` set at start-up does the same on any device.

## Depth planes

`DrawDepthPlanes2D` draws up to eight feathered planes of one quad (the heat haze's coverage image) in a
single pass per pixel. It is opt-in (`KZ_DEPTH_PLANES=1` when the renderer starts, off otherwise) and needs
the device's `depthBounds` feature, a D32_SFLOAT_S8 depth buffer and the bindless path; `DepthPlanesAvailable`
says whether it has them, and callers that want their own planes unchanged check it first.

- A pixel whose stored depth `za` passes planes by `plane_depth >= za` passes a prefix of the planes (they run
  front to back). The pass draws the quad once per prefix length k, with the depth bounds test selecting
  `(plane[k], plane[k - 1]]` (`nextafter` for the exclusive end), the depth test off and nothing written to depth,
  so each pixel is shaded at most once and the main depth keeps its usage. Intervals are drawn in ascending k.
- The colour is not computed. Blending k identical planes in 8 bits rounds at every step, so the result for a
  source value m and k planes is read from a 256 x 8 table (`depth_planes.frag`), built the first time the pass
  runs by drawing a grey ramp 1..8 times through the same blend pipeline into its own target, so the table holds the
  device's own rounding. The build happens on the device thread in the live frame (a short pass break on that
  frame only); a render scale change or a recreated target rebuilds it, and a scale below one (a table
  smaller than 256 x 8) uses the plane-by-plane path.
- Alpha is the plane's vertex alpha through the same TEXA expression `draw.frag` uses, stored outright as the
  blend would leave it.
- It applies only to the haze's state: blend `A=source B=zero C=FIX 0x80 D=destination`, greater-or-equal depth test
  without writes, no alpha test, fog or stencil, all channels written, no AEM, a target with alpha, a texture
  without alpha (or `ignore_alpha`) sampled `Nearest`, vertex colour `0x101010`, finite planes in [0, 1] that never
  increase. The destination must hold zero colour first, as the pass writes the accumulated result. Anything else
  draws the planes one by one through `Draw2D`'s own path, to the same image, as does a renderer without the option.
- Recording keeps the call whole as one `DepthPlanesEntry` (fixed-size planes, quad and state), so replays
  and display renders decide again which path to take on the device thread.

## Depth copy

`CopyMainDepthToTarget(rect)` fills the whole own depth image of the current render target from a rectangle of the
main depth, so a smaller image can be drawn with the depth test against what the frame has built (the heat haze's
low-resolution coverage). `CanCopyMainDepth(target)` says whether it applies: the device must blit its depth format
(`BLIT_SRC` and `BLIT_DST`), and the target must be an ordinary render target (not the main target, not one sharing the
main depth).

- It is a nearest-sample `vkCmdBlitImage` of the depth aspect, rect rounded to main pixels. A destination pixel takes the
  source pixel its centre falls in, as a `Nearest` sampler does for colour over the same pixel-aligned rectangle; a centre
  exactly on a pixel boundary (an exact 2x ratio) may take either neighbour, so a caller that needs agreement should not
  depend on those pixels. Stencil is not copied: the target's starts at zero.
- It runs on the draw command buffer at its place in the draw order (rendering is ended, main depth goes attachment to
  transfer-source and back), never in the upload stream, so it sees the depth of the draws before it.
- Main depth keeps its usage flags. A target's depth gains transfer-destination usage only the first time it is copied into
  (the old image is dropped and the new one cleared, stencil zero); the capability is kept through `RecreateRenderTargets`.
  A destination with transfer-destination usage may lose persistent depth compression on some drivers; the main image is not
  affected.
- Recording keeps the call whole (`DepthCopyEntry`). Replay, canonical or display, uses the render target active at that
  point (a display render's twin included) and that render's own main depth, so each render copies for itself.

## Dark Chronicle's Vu_prog0 meshes

`DrawMeshProg0` and `DrawMeshImmediateProg0` draw through `shaders/mesh_prog0.vert`, taking the
game's `Prog0Constants` beside the `MeshConstants` (which reads only `mvp` of them). Both are records in
storage-buffer arrays at set 1 (binding 0 for `MeshConstants`, binding 1 for `Prog0Constants`), one set of
arrays per transient chunk, bound once per chunk. A skinned draw also carries its `Prog0Palette`, the 32
bone matrices, as a record at binding 2 that only skinned draws use; a skinned draw without one uses
identity bones. Each draw's record indices travel in push constants (`PushConstants::mesh_index`,
`prog0_index` and `palette_index`), so Chronicle's `MeshConstants` stay 400 bytes and plain and prog0 draws
share one pipeline layout. `Prog0Constants` is the VU1 memory the game writes: `model_world`, the four
world light directions as x/y/z rows, the light colours, ambient times object alpha, object colour,
the eye in object space, the clip-or-view block, material diffuse and specular with the exponent,
four point lights relative to the object origin with their power, and the fog quadword (0x3B). It is in
the VU's units: colours on 128 = 1.0, the material on 1.0. The game copies
what its render-info packet would upload instead of converting units. A display list records the
block with its draw, and replays it as recorded; a skinned draw's palette is recorded beside it. The face
bits arrive in push constants instead (`PushConstants::face`, from `DrawState::prog0_face`); the block's
`pad0` is padding.

Dispatch follows the program. The matcap (`kProg0RefSphere`, 0x100) takes its UV from the unblended
normal. Skinning (0x040) blends the vertices through the bone slots. Then, unless the face is
`kProg0FaceNoNormal` (0x200) or the flags carry `kProg0Unlit` (0x020), which give the object colour
times the material diffuse, the ambient and four directional lights are summed; point lights
(0x010) add in place of Phong specular (0x008), with no specular. Under face 0x100 the vertex colour
multiplies the result, or under 0x080 is added. Lights reach model space as progmain's setup does.
Colour saturates at 255 and truncates per vertex as `ftoi0` does. Fog is offset + scale / w,
clamped against the near value first and the far value last, then truncated.

The flags that are the caller's are set on `DrawState`, not in the constants. Culling (0x004) is
`DrawState::cull` from `Prog0CullMode`, which also swaps the kept faces for a mirroring model_world
(progmain's QW 0x1E word). The scissor rectangle is `DrawState::scissor_rect`, enabled by
`DrawState::scissor`; fog is enabled by `DrawState::fog` and its colour is `DrawState::fog_color`.
Under 0x002 the shader clips against the six planes of `clip_or_view`, as the VU's clipper does;
without it Vulkan's own clipping stands in for the guard-band test, so a triangle crossing the near
or far plane is clipped rather than dropped whole as the GS ADC bit would.

Skinned meshes (`kProg0Skinned`) are pairs of `Vertex3D`: each vertex, then its `Prog0Skin` (four
weights and four bone slots, in the bytes of a second `Vertex3D`), so `CreateMesh`,
`DrawMeshImmediate` and the index buffer work unchanged. The skinned family reads them with a doubled
stride. Weights are not normalised (the game normalises only sums below 1). Such meshes are posed on
the GPU from the bone palette, so the vertex-rewrite interpolation of the Display lists section does
not apply to them; their palette is blended per bone there instead.

Retail quirks kept on purpose, with the spec's names: O-4 (a negative point power acts as its
magnitude), O-8 (influence 3's normal takes column 1 for its z term), O-9 (the matcap ignores
skinning), O-10 (the add step's alpha is 128 x vertex alpha squared), O-11 (the exponent is
2^(n+1), r^16 for the game's 3).

## Transient memory

Transient chunks prefer device-local host-visible memory (ReBAR or unified memory), each in its own allocation capped at a quarter of that heap, and fall back to host memory when there is none, the cap is reached or the driver refuses the allocation. `KZ_TRANSIENT_MEMORY=host` keeps them in host memory for A/B measurement.

## Readback

- `ReadDepth(id, rect)` (16 ids) reads the main depth buffer at `EndFrame` (or at the end of a
  list's canonical render), at most 64x64 pixels
  around the rect's centre, and keeps the farthest (smallest) value: the game's pick-Z keeps the
  farthest of an 8x8 block. Nothing waits for it: the answer lands when the CPU next waits on
  that frame's slot, so `DepthResult(id)` holds a frame or two late (a tick or two of the game's)
  and keeps the last answer until then.
- `ReadbackFrame` (the last presented frame: a display render, else the newest main image) and
  `ReadbackTexture` are synchronous and only allowed outside a frame or recording. `WritePng`
  writes an RGB PNG with stored (uncompressed) deflate.

## Display lists

The game runs at a fixed tick rate; the window is presented at its own. `BeginRecording()` /
`EndRecording()` bracket one tick and return its `DisplayList`; `RenderList(list, alpha, options)`
renders one.

- **Recording.** Between the two, `Draw2D`, `DrawMesh`, `DrawMeshImmediate`, `Clear`, `ClearStencil`
  and `SetRenderTarget` (draw state travels with each draw) are appended to the list, and so are the
  stateful calls: `CopyTexture`, `BlitTexture`, `SnapshotFrame`, `UpdateTexture`, `UpdatePalette`,
  `UpdateMeshVertices`, `ReadDepth`. Arguments are validated and copied at once, so each call returns
  what it would have, and `CurrentRenderTarget`, `GetTextureInfo` and `GetLogicalMapping` answer as
  inside a frame. Creating a texture, mesh or render target happens at once (the handle is needed);
  `DestroyTexture` and `DestroyMesh` keep the handle usable within the recording and by the list's
  replays, dead to every call outside it, and destroy the resource when the list is released; a
  second destroy of the handle is a no-op. `InFrame()` is true while recording. Outside
  a recording everything runs immediately, as before (`BeginFrame` / `EndFrame`, tests, the loading
  screen).
- **Canonical render** (`options.canonical`): every entry runs, in order, into the next of the two
  main colour images, which then becomes what `kPreviousFrame` samples and what a copy or snapshot
  of `kMainTarget` reads; depth queries read it (answered later, see Readback). It is the
  frame `BeginFrame` / `EndFrame` would have drawn, pixel for pixel, and nothing is presented.
  `PresentCanonical()` presents it as it is.
- **Display render**: only the drawing entries run, into a third colour image with its own depth
  buffer (the window's size). There `kMainTarget` is that image, `kPreviousFrame` the image the
  tick's canonical render sampled, and a target sharing the main depth shares that image's. Render
  targets are drawn again (the shadow volumes follow the interpolated models); textures the stateful
  entries wrote (texture animation, palette swaps, uploads) hold what the canonical
  render left. A grab of the frame is the exception: a copy or blit out of `kMainTarget`, or out
  of a texture grabbed into earlier in the list, runs again from the display image into a twin of
  its destination (the destination's contents with the grab laid over them), and the draws and
  target changes that follow use the twin, so what is drawn back from the grab (the depth of
  field's blurred bands, the water) lines up with the interpolated scene. The destination itself
  keeps the canonical grab. A display frame copies the destination into its twin before the first re-grab only when the grab does not cover the whole destination. With `options.present` it is presented and becomes what `ReadbackFrame` reads. A list
  that draws to the main target before clearing its logical frame starts from the canonical image.
  Display renders are refused once an immediate frame has followed the canonical render (the
  loading screen took the window). `options.overlay` names a second list whose drawing entries run
  after the first's, uninterpolated, on `kMainTarget`, before the present: the host's FPS counter,
  which so never reaches a canonical image.
- **Presentation composites.** A recorded `Draw2D` can supply `Presentation2D` replacement vertices and state for a
  screen-space composite of a model image that is redrawn during presentation. The
  replacement is copied at recording time and is used only by display replay; the
  canonical and immediate draw retain the original vertices and state. The
  replacement uses target-space coordinates without the original draw's UI anchor side.
  A `Clear` can supply a presentation rectangle so stale scratch-image pixels outside
  the canonical object's bounds cannot leak into the enlarged composite. Callers
  must preserve texture-to-screen mapping and reject transparent fragments when an
  enlarged composite writes depth. This is separate from keyed 2D interpolation.
  Outline callers provide full-visible-extent presentation vertices and scratch clears
  so models remain complete under interpolated rotation, skinning and display-only
  camera motion. Shifted, filtered copies can sample beyond their scissors, so scratch
  clears retain the full caller rectangle instead of a copy scissor. Each copy retains
  its texture-to-screen offset. Canonical clear/copy
  rectangles stay unchanged; transparent texels in depth-writing replacements never
  write depth. Callers that copy recorded display lists must preserve
  `presentation_vertices` and presentation clear rectangles, and leave resource
  retirement ownership with the source list. The shared GPU regressions
  `GfxGpuMeshDc2.RotatingThinOutlineKeepsIntermediateTips` and
  `InterpolatedOutlineEscapesNewestPartCrop` cover interpolated outline extent;
  `GfxGpuDisplayList.PresentationCompositeKeepsMovingMeshAndClearsReusedTarget`
  and `PresentationCompositeDoesNotWriteTransparentDepth` cover replacement
  composites, scratch clears and transparent depth rejection.
- **Interpolation.** A mesh draw may carry a `MeshTransform` (projection, view, middle, model,
  local: `mvp` is their product) and is tagged with the current `InterpKey` (`SetInterpKey`). A
  display render at `alpha` < 1 with `options.previous` matches each tagged draw with the previous
  list's draw of the same key and occurrence (below for a key drawn at several places), and draws
  the model interpolated: translation lerped,
  rotation slerped (from the Gram-Schmidt rotation of the 3x3), the remaining scale and shear lerped
  in that rotation's frame; `normal_matrix` turns with the rotation. Views are numbered by first
  appearance in a list and the n-th is interpolated with the previous list's n-th as a placed
  camera (inverse, interpolate, inverse), for every draw that carries a transform, tagged or not.
  `mvp` is rebuilt only where the model or the view changed, so a still scene replays its recorded
  constants bit for bit. Not interpolated: draws without a transform or without a match, those tagged
  `no_interpolation` or whose model moved further than the tag's teleport distance, every draw of a
  list after `CutInterpolation()`, the views after `CutCameraInterpolation()`, non-affine or
  singular matrices, and untagged 2D that tests no depth: they replay as recorded. 2D that tests depth (a
  3D sprite: a fire, its glow) is recorded as the screen corners the tick's camera gave it, so it
  is carried to the interpolated camera instead: each corner goes back through the projection and
  view of the last mesh drawn on its target to where it stood in the world, and is seen again from
  the interpolated view. Without that a torch's flame stands a tick ahead of its torch in every
  display frame while the camera moves. Own motion is opt-in through `Interp2D::world_corners`:
  matching key, emission generation and occurrence identify the corners, which are unprojected
  from their respective tick cameras, blended in world space and projected with the display camera.
  Opacity blends; UV animation, RGB and fog stay newest. Target, topology, texture binding, state
  and projection must match. New emissions, teleports, singular/behind-camera/near-plane cases,
  cuts and multi-view lists keep newest own motion. Multi-view camera association remains ordinal,
  so this path conservatively requires one camera in each list. Canonical and alpha-1 renders
  keep recorded vertices exactly.
  Vertex animation (skinning written with `UpdateMeshVertices`) is interpolated too: the rewrite a
  matched draw saw this tick (the latest of its mesh before it) is blended with the one its match saw
  in the previous tick (the game keeps a mesh per display buffer, so the two differ), positions lerped
  and normals lerped and normalised, unless a vertex moved further than the draw's teleport distance.
  A mesh shared by several models is rewritten before each one's draw; a range rewritten more than
  once in a tick is uploaded again at each rewrite, interpolated or as recorded, so every draw takes
  its own pose. The mesh holds the tick's pose again when the display render ends. Left in the
  tick's pose, a skinned body stood ahead of what hangs from it by matrices or vertices of its own
  (a weapon, the cloth) by what was left of the tick, every display frame.
  A matched immediate mesh (`DrawMeshImmediate`) tagged `blend_vertices` in both ticks, with as
  many vertices as its predecessor, has them interpolated instead: positions lerped, normals lerped
  and normalised, unless one moved further than the teleport distance. The cloth asks for it: its
  vertices are the same grid points in world space, under an identity model, every tick. The shadow
  volumes must not: they are rebuilt in each tick's eye space, so a vertex of one tick is not the
  same point as that vertex of the next.
  A matched skinned Prog0 draw (`DrawMeshProg0`, `DrawMeshImmediateProg0` with a `Prog0Palette`) has its
  32-bone model-space palette blended per bone: translation lerped, rotation slerped from the
  Gram-Schmidt rotation, scale and shear lerped in that rotation's frame. A bone that moved further
  than the draw's teleport distance keeps the newest pose. `no_interpolation` and `CutInterpolation()`
  apply as for other draws. The rest of the block follows the interpolated model and camera:
  `model_world` is the interpolated model, `clip_or_view` and the object-space eye are re-based
  through the tick's and the interpolated object-to-eye transforms, and point lights shift with the
  object's origin. This camera re-base also applies to transformed Prog0 draws with no matching
  predecessor: newly visible faces keep their newest bones, while their clip planes and lighting
  eye follow the same camera as their MVP.
- **Opt-in 2D interpolation.** `SetInterp2D({key, generation})` tags subsequent screen-space draws;
  `Interp2DScope` restores the previous tag on scope exit. Zero keys leave draws unchanged. Give each
  object a stable key and change its generation when reusing that identity for another emission.
  Display renders match key, generation and occurrence within the tick, with identical target,
  topology, binding, draw state, UVs, depth and fog. Only finite XY positions and opacity interpolate;
  RGB stays current. Canonical renders, alpha 1, cuts and unmatched/incompatible draws keep current
  values. Depth-tested sprites keep their camera-based path. This is an optional consumer API;
  recording a list does not opt its UI into interpolation.
- **Instanced keys.** A key drawn with more than one model in a tick is one object placed several
  times (a town's parts and tiles share their frames), and culling changes which places are drawn,
  so occurrences do not line up from tick to tick. Such a key, from the tick it is first seen so
  until the next `CutInterpolation()`, matches by place instead: each draw with the previous draw
  whose model is identical, and what is left with the nearest unclaimed previous model, no further
  than 25 units away. Normal and mirrored placements match only predecessors with the same
  handedness, even when their origin and surface coincide. Each face also needs the same mesh,
  index range and texture binding; an
  immediate face needs the same index topology and vertex count. Each predecessor is claimed only
  once, so culling or sorting faces does not borrow another surface's pose. All keyed draws cut
  pose matching when their texture or palette binding changes. Materials and other draw state
  remain newest. A place that comes into view starts where it is instead of sliding in from a
  neighbour's.
  A non-instanced model pairs occurrences within each surface binding and index range,
  consuming each predecessor once. Removing, inserting or reordering other surfaces does
  not prevent its later parts from interpolating. Identical repeated passes keep their order.
- **Faces that depend on the view.** A display render replays a draw under a camera the tick did
  not have, so anything the recorder decided from the tick's eye can be wrong in it. What turns
  towards or away from the eye must be left to `DrawState::cull`: the shadow volumes are recorded
  wound outwards and counted by a back-culled adding draw and a front-culled subtracting draw.

## Device

`RendererInit` takes the best device that has everything below (a discrete GPU over an integrated
one over the rest, and Vulkan 1.4 over 1.3 within a kind) and prints, for every device it passes
over, each requirement that device lacks. `requirements.cpp` holds the check as a pure function of
the device's capabilities.

Nothing the renderer calls was introduced by Vulkan 1.4, so 1.3 is the floor:

| Requirement | Introduced by |
|---|---|
| `dualSrcBlend`, `shaderClipDistance` | 1.0 features |
| `vkGetPhysicalDeviceFeatures2`, `vkGetPhysicalDeviceProperties2` | 1.1 |
| The bindless texture array (below), or `VK_KHR_push_descriptor` | 1.2 (descriptor indexing), or extension |
| `dynamicRendering` (`vkCmdBeginRendering`, `VkPipelineRenderingCreateInfo`) | 1.3 feature |
| `synchronization2` (`vkCmdPipelineBarrier2`, `vkQueueSubmit2`, the `*_2` stages and accesses) | 1.3 feature |
| Cull mode, front face, topology, depth test/write/compare, stencil test and ops as dynamic state | 1.3 core (no feature bit) |
| `D32_SFLOAT_S8_UINT` or `D24_UNORM_S8_UINT` as an attachment that copies both ways | format support |
| `maxPushConstantsSize` of 72 bytes | 1.0 limit (128 guaranteed) |
| `maxStorageBufferRange` of 8 MiB and `maxPerStageDescriptorStorageBuffers` of 3 | 1.0 limits (128 MiB and 4 guaranteed) |
| `VK_KHR_swapchain` and a graphics queue that presents to the window | not when offscreen |

The bindless texture array holds every texture slot at set 0, indexed by the push constants. It
needs `shaderSampledImageArrayDynamicIndexing`, `descriptorBindingPartiallyBound`,
`descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`, 8192
update-after-bind sampled images per stage and per set, and that many update-after-bind resources
per stage. GPUs with Metal argument buffers tier 1 (Apple A12 and older) have about a hundred, so
there `draw_bound.frag` and `draw_bound_single.frag` take set 0 as one texture, palette and sampler that each draw pushes with
`vkCmdPushDescriptorSetKHR`, skipped when the previous draw pushed the same. `MissingBindless` makes
the choice; `RendererConfig::bindless_textures = false` or `KZ_BINDLESS=0` takes the pushed path
on any device. A portability-subset device (MoltenVK) takes it too even where the array fits: there
the array is a Metal argument buffer, and drawing the quick loadout's item icons from it faults on
the GPU (Apple A18 Pro). `KZ_BINDLESS=1` keeps the array on such a device.

Optional, used when present:

- `VK_EXT_extended_dynamic_state3` with `extendedDynamicState3ColorWriteMask` (below); without it
  the colourless pipelines are added. Chosen by the device's capability, and
  `config.dynamic_color_write_mask = false` forces the fallback.
- `VK_KHR_portability_enumeration` (instance), enabled with
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` whenever the loader has it, so drivers that
  implement Vulkan on Metal (MoltenVK) are enumerated.
- `VK_KHR_portability_subset` (device), enabled whenever offered, as the spec requires. Of its
  restrictions the renderer meets two: without `triangleFans` a `Primitive::TriangleFan` is drawn as
  an indexed list in Vulkan's fan order (identical pixels; `config.triangle_fans = false` forces
  it), and without `separateStencilMaskRef` both faces take one reference and compare and write
  masks (the face culling keeps; the front's when both are drawn, with one warning if they differ;
  `config.separate_stencil_masks = false` forces it). The vertex strides (28 and 36 bytes) must be
  multiples of `minVertexInputBindingStrideAlignment`. Point polygons, events, format swizzles and
  reinterpretation, constant-alpha blend factors, mip LOD bias and comparison samplers are not used.
- `VK_EXT_debug_utils` and `VK_LAYER_KHRONOS_validation` for validation.

Shaders are compiled for `--target-env vulkan1.4`, which emits SPIR-V 1.6 with no capability beyond
what 1.3 supports (`spirv-val --target-env vulkan1.3` accepts all three).

### Android

Android asks for Vulkan 1.3 and compiles the shaders for `vulkan1.3`. Its `libvulkan` does not
export the 1.3 commands at every API level, so `vulkan_android.cpp` defines the twelve the renderer
calls, each forwarding to the device's own. The swapchain keeps the identity transform and leaves the
rotation to the compositor; the SUBOPTIMAL results that rotation reports keep the swapchain while the
surface's capabilities are unchanged. A surface lost while the app is in the background is dropped
with its swapchain and made again on a later frame. Errors also go to the system log.

## Profiling

Kazarov's renderer instrumentation is compiled in with the `KZ_PORT_TRACY` CMake option, which links `Tracy::TracyClient` into the host library. Without it the macros in `gfx/profile.hpp` compile to nothing. At run time zones record only while `gfx::profile::SetEnabled(true)` is set (the host sets it from its debug-mode gate) and a profiler is connected.

- `KZ_PROFILE_ZONE` and `KZ_PROFILE_PLOT` are CPU zones and plots.
- `KZ_PROFILE_GPU_ZONE(cmd, name)` scopes a GPU timestamp zone. `KZ_PROFILE_GPU_BEGIN` and `KZ_PROFILE_GPU_END` open and close one across functions (dynamic rendering spans).
- The GPU context is created in `RendererInit` on the graphics queue and collected when a frame slot's command buffer begins, after its fence wait.
- GPU zones may only be recorded into command buffers that are always submitted. Tracy reads queries in order, so an unsubmitted one stalls every later GPU zone.

Plots, reset each frame:

- `Kazarov mesh draws`
- `Kazarov 2D draws`
- `Kazarov descriptor binds`
- `Kazarov grab copies`
- `Kazarov grab pre-copies`
- `Kazarov transient bytes`

## Not done here

- DATE/DATM (destination alpha test; `clsmes.cpp` MakeFukidashi) is not emulated; a stencil pass
  marking the pixels whose alpha passes, then a test against it, is the way.
- GS mip LOD (TEX1 L and K) is ignored; filtering is standard trilinear over the supplied levels.
- The device requirements are listed under "Device".

Surface resynchronization requires matching material/range, handedness and part geometry
before consuming a predecessor. Buffered meshes keep their stable mesh handle. Immediate
parts require matching index topology, vertex count and immutable vertex bytes. An explicit
vertex-blending opt-in allows position/normal changes only within the same occurrence with
unchanged UV/color attributes; moving immediate parts without a stable mesh identity do not
resynchronize across occurrences. This preserves the caller's same-points/order contract.

## Shared device test classification

Shared recording, pairing, composite, Prog0, lifetime and frame-edge device tests use
suite names beginning with `GfxGpu`, including parameterized suites. Consumers can
exclude them with the single GoogleTest filter `-*GfxGpu*.*`, or CTest's
`-E "(^|/)GfxGpu"`. Register that same family with the `gpu` label and a device-suitable
timeout (30 seconds) when using label-based CI exclusion. Pure CPU display-list
math remains in `GfxDisplayList`; `GfxMeshDc2.MeshConstantsKeepChroniclesSize` is
also CPU-only. Existing renderer/texture GPU families retain their existing exclusions.
A consumer that previously excluded only `^(GfxMisc|TexDecode)\.` must add
`|(^|/)GfxGpu` to its CTest exclusion before adopting this dependency, or label
this family and use `-LE gpu`. CPU display-list coverage continues to run.

### Startup pipeline and cache work

Untextured colour pipelines keep fog and simple-colour specialization but share
TEXA/AEM variants: those flags only affect texture sampling. Original index slots
remain available internally; unused slots contain null handles and own no pipeline.
`PipelineCount()` and startup progress report the number actually created.

On Vulkan 1.3 devices, pipeline creation requests
[application-cache feedback](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreationFeedbackFlagBits.html).
Only valid, confirmed hits for every regular and optional startup pipeline allow
retaining a compatible loaded cache without serializing and rewriting it. Missing
feedback or any unconfirmed pipeline takes the persistence path; identical returned
bytes also avoid rewriting. New, incompatible and corrupt caches are still rebuilt
and written atomically. Serialized cache byte order need not be stable across runs.

`KZ_PERF_INIT_TRACE=1` reports cache input size, load/create/description/compile
spans and unconfirmed hits. The latter includes unavailable feedback and is not a
count of shader compilations. A persistence attempt reports serialization/compare
and write spans separately. Tracing is optional and its output adds observer cost.