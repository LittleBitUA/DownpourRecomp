# Downpour native render — state, evidence, and the plan to finish it

Written 2026-07-25 at the end of a long bring-up session, for whoever picks this
up next. Everything below that is stated as fact was **measured**, and where a
belief turned out to be wrong that is recorded too — several of the wrong turns
in this session cost hours precisely because an argument was mistaken for a
measurement.

---

## 1. What this is trying to be

A **full native D3D12 renderer** for Silent Hill: Downpour, built the way
UnleashedRecomp / MarathonRecomp / XenonRecomp / XenosRecomp build theirs:

* the guest's D3D9/RHI calls are intercepted,
* its Xenos shaders are translated to DXIL **offline** and looked up by
  microcode hash at runtime,
* its vertex declarations become host input layouts,
* its vertex/index data is byte-swapped into host buffers,
* its textures are decoded into host textures,
* and **we** issue the draws.

The end state is that the Xenos emulation stops rasterising anything. That is
where the performance win and the end of artefacts like the HDR-resolve noise
come from. Drawing on top of the emulated frame, which is what happens today, is
strictly more work and can only ever be slower — it is a bring-up step, not a
destination.

---

## 2. What exists today

| module | role |
|---|---|
| `downpour_shader_dump.cpp` | harvests Xenos shader containers out of guest RAM |
| `downpour_native_shaders.cpp` | loads the translated DXIL, looks it up by microcode hash |
| `downpour_native_decl.cpp` | reads the guest's vertex declarations |
| `downpour_native_pipeline.cpp` | root signature, PSO cache, sampler heap, constant ring |
| `downpour_native_constants.cpp` | shadows the guest's VS/PS float constant banks |
| `downpour_native_vbuffers.cpp` | host vertex/index buffers + the dword byte swap |
| `downpour_native_tex.cpp` | Xenos texture decode into host textures |
| `downpour_native_state.cpp` | pipeline state, read out of the Xenos register shadow |
| `downpour_native_scene.cpp` | **the draw path**: capture, resolve, replay, composite |
| `downpour_native_geo.cpp` | the older hand-written geometry renderer (superseded) |

### Environment switches (all default OFF)

| switch | effect |
|---|---|
| `DPOUR_NR_SHADERS` | load the translated shader cache; changes nothing on screen |
| `DPOUR_NR_SHADERS_DRYRUN` | build a PSO per draw and throw it away (verification) |
| `DPOUR_NR_DRAW` | **the draw path** — visible |
| `DPOUR_NR_DRAW_ONLY` | composite opaquely: only our image |
| `DPOUR_NR_DRAW_SKIPGUEST` | drop the guest's submissions for draws we reproduced |
| `DPOUR_NR_DRAW_NODRAW` | do everything except the draw calls |
| `DPOUR_NR_DRAW_FLAT` | composite shows our DEPTH buffer instead of colour |
| `DPOUR_NR_DRAW_MAXDRAWS=N` | cap draws submitted per frame |
| `DPOUR_NR_DRAW_LOG=1` | one line per new shader pair, plus the first dropped draws |
| `DPOUR_NR_GEO`, `DPOUR_NR_OVERLAY`, … | the older phases |

Ready-made batch files sit next to the game: `TEST_NATIVE_DRAW.bat` (overlay),
`TEST_NATIVE_ONLY.bat` (ours only), `TEST_NATIVE_REPLACE.bat` (ours only + guest
draws dropped), `TEST_NATIVE_FLAT.bat` (depth view), `VERIFY_SHADER_PLUMBING.bat`
(dry run).

### The build switch that settles arguments

```
cmake -S . -B <build> -DDPOUR_NO_NATIVE_RENDER=ON
```

produces a binary that **cannot contain** any of this. When someone says "you
broke it", build that and compare, before reasoning. It is how the one real
performance regression in this session was found.

---

## 3. Facts established by measurement

### Shader translation
* **419/419 containers translate; 418 land in the cache.**
* Runtime lookup: **96 vs + 231 ps resolved, 1 not in cache, 0 objects unread.**
* Dry run: **26 916 pipelines created, 0 failures, 0 unusable declarations.**
  The game's own translated DXIL, our root signature and the game's vertex
  declarations all agree. This was proven before a single pixel was drawn.

### The XDK inlines `SetRenderState`
There is no `SetRenderState` function to hook. It compiles to direct writes of
the **Xenos register shadow inside the D3D device**, and `sub_82D1F0B0` passes
`&dev[10548]` to the packet builder, which is what proves the block is contiguous
(4 bytes per register from `RB_DEPTHCONTROL`).

| offset | register | fields (recovered from the recompiled `rlwimi`s) |
|---|---|---|
| `dev+10548` | RB_DEPTHCONTROL | bit1 Z_ENABLE, bit2 Z_WRITE, bits4-6 ZFUNC, 8-31 stencil |
| `dev+10552` | RB_BLENDCONTROL0 | 0-4 src, 5-7 op, 8-12 dst, 16-20 src α, 21-23 op α, 24-28 dst α |
| `dev+10556` | RB_COLORCONTROL | bits0-2 ALPHA_FUNC, bit3 ALPHA_TEST_ENABLE |
| `dev+10560` | PA_SU_SC_MODE_CNTL | bit0 CULL_FRONT, bit1 CULL_BACK, bit2 FACE |
| `dev+10500` | ALPHA_REF | float |
| `dev+10584/88/92` | RB_BLENDCONTROL1..3 | MRT |

*"Blend off" is not a flag*: the XDK writes `0x00010001` (ONE/ZERO/ADD) into all
four blend registers. `dev+10564` is **not** the cull register — it is constant
`0x00080000` across 38 809 draws.

Functions: `RHISetDepthState=sub_829C9680`, `RHISetStencilState=sub_829C9740`,
`RHISetBlendState=sub_829C9990`, `RHISetMRTBlendState=sub_829C9AA8`,
`ToggleCompareFunction=sub_829C9628`, device `SetBlendState=sub_82D1B000`,
`SetRenderState_AlphaBlendEnable=sub_82D181D0`.

### Buffers
* `RHIDrawIndexedPrimitive`'s **MinIndex/NumVertices are garbage** on most draws
  (over a million vertices for a section of a few hundred). Do not size uploads
  from them.
* The buffer's real size and address are in the **Xenos fetch constant** inside
  the D3D object (`FXeGPUResource::Resource` at +8). The offset is found by
  self-check: the fetch constant's address word must equal the `BaseAddress` the
  RHI object already carries. Result: **~180 000 resolved, 0 failures.**
  `sub_82D1A5C0` independently confirms it reads `+24`/`+28` of that object.
* A stride of **zero is a legal binding** (every vertex reads the same element —
  UE3 uses it for per-instance data), not a missing one.
* UE3 binds up to **16** streams, not 4.

### Performance
Own cost, measured in-process: **capture 0.01–0.23 ms/frame, render
0.04–0.19 ms/frame**. But see §5 — that number does not measure everything.

---

## 4. Wrong turns, so they are not repeated

1. **Emulating vertex fetch instead of using the guest's vertex declarations.**
   Reverted. 218 of the game's vertex shaders *do* carry a vertex element table;
   the ones that do not are the XDK's video-decode shaders.
2. **`ObjectReadable`/`VirtualQuery` per draw** — a system call in the hottest
   path cost 170 ms/frame. Memoise by address, negatively too.
3. **"The flag is off" is not "the code is free."** The constant-shadow hooks ran
   unconditionally; with every switch off, p90 frame time was **109 ms vs 18 ms**
   for a binary built without the layer. Median was 60 FPS in both — *look at
   p90/p99, not the median*. Fixed by `NativeActive()`: with no `DPOUR_NR*` in the
   environment every hook is a pure passthrough.
4. **Resolving one pass and publishing another.** Draws were resolved for the
   pass chosen last frame but published by "the pass with most draws this frame".
   When they disagreed, nothing was published, no composite ran, and the emulated
   frame showed through — which is indistinguishable from "the renderer does
   nothing". Now: publish what was resolved; the vote picks the *next* target.
5. **Grouping passes by (colour, depth).** Downpour renders in EDRAM tiles and
   rebinds depth between passes, so one logical target split into a dozen and
   only a fragment was published (106 of 61 147). Group by **colour surface**.
6. **Depth as composite coverage.** The depth prepass writes depth over the whole
   screen and no colour, so "depth != clear" declared every pixel covered and
   painted an empty black target over the frame. Coverage must be *colour*.
7. **A locally compiled diagnostic pixel shader.** The game's shaders are DXIL; a
   `D3DCompile`d one is DXBC. A pipeline mixing them is rejected, so **every**
   draw failed and the screen showed the emulated frame. Diagnostics must not
   change shader model — the depth view now lives in the composite.
8. **Leaving a test instance of the game running** while the user launched their
   own. Two games on one GPU looks exactly like a regression.

---

## 5. Where it stands right now

### ✅ SOLVED — why every pixel was black: the game uses INVERTED depth

Measured, not inferred. One run with the depth-direction counter produced:

```
[native-state] depth compares: 0 less, 249263 greater -> INVERTED depth
```

**249 263 draws compare depth as GREATER / GREATER_EQUAL, and not one compares as
LESS.** Downpour renders with a reversed depth buffer: near is 1, far is 0, and
the target must be cleared to **0.0**.

We were clearing to 1.0, so *no pixel could ever pass the depth test*. Geometry
was being rasterised correctly the whole time — nothing downstream of the depth
test ever ran. A scene that fails the depth test and a scene that is shaded black
are indistinguishable on screen, which is why this cost so long.

Two consequences worth keeping:

* **Do not read `GInvertZ` from a global.** Its address is disputed in our notes
  (`0x837AA000` vs `0x837AB330`, and the latter is also recorded as
  `GCurrentColorExpBias`); reading it returned 0, which is the wrong answer.
  `dpour_state::ObservedInvertedDepth()` derives the direction from the
  comparison functions the game's own draws use. That cannot be wrong, because it
  IS what the game does.
* **The direction is not knowable on frame one.** The first frame with anything
  to draw arrives long before enough draws have been seen, so the targets were
  created with clear = 1.0 and never rebuilt — the log said
  `targets ready ... (clear 1)` for the whole session. `EnsureTargets` now
  rebuilds when the answer settles and logs
  `depth direction settled: INVERTED (clear 0)`.

Built and deployed 20:43. **Not yet run** — `TEST_NATIVE_ONLY.bat` is the first
run in which our pixels can pass the depth test at all.

### ✅ RESOLVED (2026-07-25 evening): the TDR blocker — both fixes landed

The mechanism was exactly as diagnosed (a `while (true) { switch (pc) }` shader
whose exit tests a boolean constant we fed as zero), and BOTH remedies are in:

1. **A-2, the translator guard.** `shader_recompiler.cpp` now emits
   `[loop] for (uint pcGuard = 0u; pcGuard < 1024u; pcGuard++)` instead of
   `while (true)`. No legitimate UE3 shader needs more than a few hundred
   control-flow transfers (bones ≤ 75, filter taps ≤ 64). **Verified: the title
   screen draws 81 native draws/frame continuously with no device removal** —
   the exact spot that TDR'd before.
2. **A-1, the real bool/loop banks.** The setter hunt was the wrong road —
   `RHISetVertexShaderBoolParameter` does not survive as a separate function
   (inlined/stripped). The answer came from the COMMIT path instead:
   `sub_82D1E250` (flushes dirty constants before a draw) contains
   `addi r6,r31,10112 ; li r5,18688 ; lis r4,-256` — source `dev+10112`,
   register base `0x4900` (the Xenos bool bank), mask = top 40 bits = exactly
   8 bool + 32 loop dwords. So: **bool shadow @ dev+10112, loop shadow @
   dev+10144.** Cross-check: the same commit path flushes base `0x2200` from
   `dev+10548` — precisely our RB_DEPTHCONTROL map. The float shadows bracket
   it: VS floats at `dev+(reg+120)*16` (1920–6016), PS floats at
   `dev+(reg+376)*16` (6016–10112).
   `dpour_state::Read()` now reads both banks per draw and the scene copies
   them into `SharedConstants` (c32/c34). `[native-state]` logs how often the
   bool bank is nonzero plus a one-shot hex dump of the window (all zero on the
   title screen — the game simply has not set any yet; gameplay is the test).

Also fixed on the way: **the shader cache now compiles 419/419 with 0 failures.**
The one failing VS declared TEXCOORD2/3/4 twice (multi-stream duplicate
elements); the translator now deduplicates input declarations by
(usage, usageIndex) — same semantic, same data, one parameter. Failed shaders
now leave `failed_<ucode>.hlsl` next to the output for offline `dxc` runs.

### ✅ CORRECTED (later the same evening): Downpour never runs the MSAA block

The bracket hooks below were correct in mechanism but dead for this game:
`XeD3DDevice.cpp:118` sets `GUseTilingCode = FALSE` (the comment says they chose
dominant-light shadows over 2xMSAA). With tiling off, `SceneRendering.cpp:1711`
takes the else-branch: `GSceneRenderTargets.BeginRenderingSceneColor()` →
a plain `RHISetRenderTarget` with the scene colour surface — a path our hook
already sees. The scene surface's usage name in this engine version is
**"DefaultColor"** (SceneRenderTargets.cpp:1042/1058), with `DefaultColorRaw` /
`DefaultColorFixedPoint` aliasing the same EDRAM, and depth = "DefaultDepth".

Landed on top of the brackets (which stay, harmless):
* `sub_829CD600` (`RHICreateTargetableSurface`) hook reads the usage string
  (r9; the FSurfaceRHIRef return is via hidden sret in r3, shifting args) and
  the surface from the sret slot; logs every named surface (verified live:
  AuxColor ×N, DefaultDepth, DefaultColor, LightAttenuation, ...).
* Binding any `DefaultColor*` surface in `SetRenderTarget` ⇒ `SceneBlockBegin()`
  — the scene slot, deterministically, through the path the game actually uses.
* The hook also captures r7 = ResolveTargetTexture for "DefaultColor" —
  `dpour_scene::SceneResolveTexture()` — the guest texture the scene resolves
  into. This is the injection point for the double-render fix below.

### ⛔ VERDICT (25.07 night, after user testing): the HYBRID is the dead end

Two runs of the injection build settled it. The injection path (readback →
7e3+tiling encode → write into guest memory in place of the resolve) never
fired (the resolve-texture pointer chain differs from every convention tried),
SKIPGUEST without a live injection corrupted the frame exactly as predicted,
and — decisive — **the user still sees the emulated renderer's frame**. Every
piece of the hybrid (the composite overlay, the pass vote, the 7e3 encode, the
write-watch dance) is a fight against a renderer the references simply do not
run.

**How UnleashedRecomp actually does it** (verified in their code, video.cpp):

* `ProcStretchRect` (video.cpp:3265) — their resolve — **copies no pixels**:
  `texture->sourceSurface = surface;` and when that texture is sampled,
  `SetSurface(i, surface)` binds the native render target's own SRV. The
  surface↔texture pair IS one native resource. GPU-to-GPU, zero copies, no
  formats, no tiling, no guest memory.
* Their render-command queue (video.cpp:5275+) — SetBooleans,
  SetVertexShaderConstants, AddPipeline, DrawPrimitive/UP/Indexed,
  SetRenderTarget, Clear... — is THE renderer. **No emulated GPU exists.**

### ✅ Phase C step 1 LANDED (26.07 night, built+deployed, smoke-tested at title)

Implemented exactly the reference shape, all in `downpour_native_scene.cpp`:

* **Target registry**: every guest surface object → its own native RGBA16F
  target + D32 depth (lazily created at first bind; sizes captured from
  `RHICreateTargetableSurface`, fallback to the pass viewport), RTV/DSV heaps,
  and a **bindless SRV slot** so linked textures sample it.
* **Resolve = link** (`OnResolveScene`): every `RHICopyToResolveTarget`
  registers plausible destination-texture pointers ([ref+4/+8/+12] — the
  member whose value the game later passes to `RHISetSamplerState` is the one
  that matches) → source surface. No pixels move. The injection machinery is
  retired (worker/encoder still compiled, never enabled).
* **`dpour_tex::Acquire` consults the link first**: an RT-backed texture
  returns the surface target's SRV — the game's post chain and any
  render-to-texture consumer samples OUR output.
* **Ordered multi-target replay**: the WHOLE frame is published (the pass vote
  and the "other passes" drop are gone); the render thread walks the draws in
  submission order, switching native targets when the pass changes, clearing
  each target once per frame, with correct RT↔SRV transitions.
* **Composite shows the last full-size (≥1024-wide) target drawn** — once the
  game's final pass renders natively, that IS the finished frame; today it
  degrades to the scene target.
* Log: `[native-scene] registry: N surfaces known, M texture links, K SRV-backed`
  every interval; `target registered: surface .. WxH (srv S)` per creation.

Still open in step 1: depth targets are never served as SRVs (shadow maps
sample white), guest depth-only passes (colour NULL) are not replayed, colour
formats are all RGBA16F regardless of the guest format, `RHIClear` is not
routed (per-frame clear approximation instead).

### ✅ Step 2 LANDED: UP draws (the UI) — built 26.07, deployed

The recipe below was carried out after resolving the REAL shapes from the
recompiled code (downpour_recomp.84.cpp / .57.cpp) — several of the earlier
fingerprint guesses were wrong and are corrected here:

**The resolved UP surface (all verified instruction-by-instruction):**

* `sub_82D1E250` = **D3DDevice::BeginVertices(dev, d3dprim, VertexCount,
  Stride)** — returns the WRITE POINTER in r3 (0 = ring full). r6 is the
  STRIDE (it computes r5*r6 and stores stride>>2 as a byte at dev+12904).
  There is no out-parameter; the RHI stores r3 into its caller's `void*&`.
  This is also the constant-commit path (float banks dev+1920/6016, bool/loop
  dev+10112/10144) — Begin IS the guest-side draw commit.
* `sub_82D1E710` = **DrawVerticesUP(dev, d3dprim, count, pData, stride)** —
  wrapper: BeginVertices → memcpy (`sub_82FEA840`) → publish.
* `sub_82D1E758` = **BeginIndexedVertices(dev, d3dprim, BaseVertexOffset
  (MinVertexIndex NEGATED by the RHI — it is exactly D3D12's
  BaseVertexLocation), NumVertices, NumIndices, IndexFmt (1=16-bit, 6=32-bit;
  test bit 2), VertexStride, &OutIndexData, [r1+84]=&OutVertexData)** —
  returns 0 on success. The 9th argument sits at [entry r1+84] (Xenon ABI),
  verified in both binary callers.
* **EndVertices DOES NOT EXIST as a function**: every call site publishes
  inline with a bare `[dev+13844] -> dev+48` store. `sub_82D29638` (the old
  "End" fingerprint) is actually QueryBufferSpace. There are ~8 engine sites
  with the whole Begin/End pair inlined (recomp.5/.11/.41/.43/.44/.55/.56) —
  which is WHY the hooks live at the DEVICE entries: they catch every inlined
  copy; RHI-level hooks would not.
* RHI level, for the record: `sub_829CA318`=RHIBeginDrawPrimitiveUP(prim,
  NumVertices, Stride, void*&Out — NumPrimitives dropped),
  `sub_829CA780`=ValidateDrawUPCall, `sub_829CA820`=ValidateDrawIndexedUPCall
  (GetPrimitiveTypeCount inlined), `sub_829CA900`=RHIDrawPrimitiveUP (whole
  draw inlined), `sub_829CA9D8`=the occlusion-cube batch begin (8 verts ×
  12 B, 36 indices), `sub_829CAE50`=RHIDrawIndexedPrimitiveUP(prim, MinVtxIdx,
  NumVerts, NumPrims, IdxData, IdxStride, VtxData, VtxStride) — copies
  vertices FROM MinVtxIdx and negates it into the device call.
* `sub_829C9FA0` = GetD3DPrimitiveType: PT 0→4 trilist, 1→6 strip, 2→2 lines,
  3→**13 QUADLIST** — the UI uses quad lists, D3D12 has none.

**The implementation (downpour_native_scene.{h,cpp} + draws.cpp):**

* Because End is inlined everywhere, capture is split: `CaptureUPBegin` (at
  the Begin hooks) snapshots EVERYTHING except the bytes — state is complete
  at Begin, Begin itself commits it guest-side — including the PSO, the
  constant snapshot, the pass, and ring slices for the geometry;
  `CloseUPPending` (at the next captured event: any draw, the next Begin, an
  RT switch, or EndFrame) copies the now-complete bytes with byteswap32
  (vertices; all UP fields are 4-byte) / byteswap16 (16-bit indices).
  Begin/End pairs cannot interleave (GInBeginVertices), one pending slot.
* QUADLIST → indexed TRIANGLELIST with generated indices (0,1,2 / 0,2,3 per
  quad); indexed quads expand 4 guest entries → 6. Non-indexed quads generate
  their indices at Begin already (they depend on nothing the game writes).
* Geometry rides the constant upload ring (24 MB/slice, same per-frame
  lifetime); `SceneDraw` gained `base_vertex` (the negated MinVertexIndex →
  DrawIndexedInstanced's BaseVertexLocation).
* `RHISetRenderTarget(NULL, NULL)` = the back buffer (that is where the UI
  goes): the replay maps that pass to a reserved registry target
  (kBackbufferKey) sized to the output. NULL colour + a real depth surface is
  still a depth-only pass and stays unreplayed for now.
* Telemetry: `[native-scene] UP draws: N begun, M captured, K dropped`.
* ALSO fixed on the way: BeginFrame's pass-0 carry used a positional
  `PassSlot{...}` initializer that silently shifted when color_object/
  depth_object were added in step 1 — depth_object got the viewport width,
  `draws` got the height (≈720 phantom votes per frame), and the carried
  viewport was lost. Now carries the whole slot and zeroes `draws`.

**Next after this:** depth SRVs for shadow maps, RHIClear routing, depth-only
pass replay, then the step-3 flip: composite opaque when the backbuffer pass
rendered natively, and `DPOUR_NR_DRAW_SKIPGUEST` for everything = one
renderer.

### 💀→✅ The DEVICE_HUNG hunt (26.07 night): a cache use-after-free, proven by DRED

Step 2's smoke runs died at content transitions (title→attract, 39-158 s in,
4/4 runs) with DEVICE_HUNG. The isolation ladder that cracked it:

1. Emulated-only control survived 200+ s twice → the regression is ours.
2. NOUP (no UP capture) still died → not the UP draws.
3. NOBACKBUF died too (later) → not the backbuffer branch either; the SAME
   six shader pairs (`vs=0xb356a8cd...`, menu-3D streams=3 content) precede
   every death - and their DXIL has NO loops (checked with dxc: the pcGuard
   is in, the vertex shaders have no backward branches at all).
4. DRED (enable with `REX_D3D12_DEBUG=1`; the SDK's own reporter prints on
   the [gpu] thread) delivered the verdict: **every breadcrumb list completed
   (`N of N ops`) + a page fault at VA 0x3BC0C1000** = a shader read a FREED
   GPU resource. Not a hang - a use-after-free.
5. Root cause in `dpour_vbuf::RetireUploads`: retirements were stamped with
   the fence of the CURRENT submission, but a buffer retired by a grow during
   CAPTURE is referenced by draws still sitting in the staging list -
   submitted one or more fences LATER, and during a loading stall the
   published snapshot replays for many frames with no new publish. Content
   transitions grow buffers en masse - exactly where every run died. FIX:
   stamp `submitted_fence + 8` (same in dpour_tex). Validated: the user
   played through several loads into Devil's Pit gameplay, zero TDR.

Also: a `[native-scene] WATCHDOG` device-loss reporter now lives in
scene.cpp (polls GetDeviceRemovedReason, dumps DRED breadcrumbs + page fault
into OUR log before the presenter's abort can eat them); it holds a ComPtr on
the device (polling a destroyed device segfaulted at exit).

### 🛹 SKATE3RECOMP v2.0.0 = THE INFRASTRUCTURE, PORTED (26.07 night)

skate3recomp (github.com/mchughalex/skate3recomp + SDK fork
github.com/mchughalex/rexglue-skate3, branch skate3-sdk-clean, submodule at
`E:\XboxDP\_ref_skate3`) shipped a native renderer on the same ReXGlue SDK.
Their GAME layer is a semantic scene rebuild (data-driven guest-structure
walk, own shaders - NOT our replay approach), but their SDK layer is
game-agnostic and is exactly our step 3 done right. PORTED into our branch
(dpour-main-achievements), compiles + runs:

* `include/rex/graphics/native_guest_renderer.h` + `native_rhi.h` +
  `d3d12/native_rhi_d3d12.h`, `src/graphics/native_guest_renderer.cpp` +
  `d3d12/native_rhi_d3d12.cpp` - copied verbatim.
* `d3d12/command_processor.{h,cpp}`: nrhi device member + the
  TryRenderNativeGuestOutput block at the guest-output refresh (after
  SetIs8bpc), draw suppression after `memexport_used` (by pitch + depth-only
  exemption), resolve suppression in the kCopy branch, batching change in
  OnPrimaryBufferEnd, Destroy in ShutdownContext.
* `d3d12_presenter.cpp`: guest output resource gains ALLOW_RENDER_TARGET.
* `perf/counter.h`: kNative* DrawBucket additions.
* dpour-fork additions to their SDK layer: `NativeRhiGetD3D12TextureResource`
  + `NativeRhiGetDeferredCommandList` (D3D12-only escape hatches - our
  executor records raw D3D12 on its own queue; the callback only needs to
  hand the finished frame over).
* Suppression cvars (native_render_suppress_emulated_draws=true default,
  native_render_suppress_mode=2) - the mode's pitch semantics are
  SKATE3-TUNED; Downpour needs its own pitch table before the FPS gain is
  real. Occlusion-query fake-result hunk NOT ported (Downpour ships
  occlusion_query_enable=false).

**Game side (`DPOUR_NR_DRAW_GUESTOUT=1`)**: scene.cpp registers
`GuestOutputRenderCallback` (skate3 sticky-failure discipline: any yield
shows the emulated frame); Render() draws the composite OPAQUELY into an own
R10G10B10A2 texture (g_comp_pso_guestout variant), the callback D3DCopyResource-s
it over the presenter's guest output inside the deferred command list with
kGuestOutputInternalState round-trip barriers. CONFIRMED LIVE 00:49:26:
"guest output REPLACED natively for the first time (2560x1440)".

**DEPLOY RULE (cost 20 minutes tonight): the SDK layer lives in
`rexruntimerd.dll` - deploying downpour.exe without the matching DLL
fail-fasts (0xc0000409) before the first log line. ALWAYS copy BOTH.**

### 🌙 End-of-night state (26.07 ~01:40) + the next session's exact plan

* SECOND device-lost class killed: a DESCRIPTOR RACE. Once the composite
  source started switching per frame (menu flips UI-target <-> 3D-target),
  the single shared SRV slot was rewritten while in-flight frames read it -
  white screen, then DEVICE_REMOVED. Fix: kPassFrames x [colour,depth]
  descriptor pairs in g_srv_heap, each frame writes only slot
  (g_pass_index % 3) * 2; both composite paths. Run 069 survived with no
  crash after this.
* Composite source = the >=1024-wide target with the MOST draws this frame
  (+ per-window telemetry `composite source: 0xKEY with N draws`;
  0xffffffff = the scene slot). "Last bound" was an empty utility target.
* READING THE USER'S SCREEN: the WHITE frame is OUR frame (source=scene with
  6-45 draws - menu-3D geometry with unresolved materials); the black that
  follows is the source flipping to a low-content target. The whole
  capture->replay->replace mechanism works END TO END now; what remains is
  frame QUALITY.
* Scene-alias reset theory DISPROVEN: DefaultColor is NOT recreated on level
  load. OPEN: why `scene[15]:idle` and only ~30 regular draws/frame in
  gameplay/menus (title attract marks 200) - where do the world's draws go?
  FIRST thing to investigate next session.
* Suppression cvar default flipped to FALSE in our SDK fork - only turn on
  once our frame deserves to replace suppressed content.

NEXT SESSION, in order: (1) why the scene is idle in gameplay (~30
draws/frame captured - the world comes through some path we do not see);
(2) replay the game's OWN final composition pass instead of the source
heuristic; (3) menu-3D materials (white); (4) YUV movie path (black);
(5) Downpour-tuned suppression pitches = the FPS win.
Test: TEST_NATIVE_GUESTOUT.bat; smoke env REX_FULLSCREEN=false
REX_WINDOW_WIDTH=1280 REX_WINDOW_HEIGHT=720 REX_AUDIO_MUTE=true.

### 🎯 THE PLAN THAT FOLLOWS (Phase C, the rest)

Everything proven today carries over (shaders, constants+bool banks, buffers,
state shadow, PSO cache, scene identity, hooks). What changes is the shape:

1. **Native target registry keyed by SURFACE identity.** Every
   `RHICreateTargetableSurface` gets a native D3D12 texture (RTV/DSV + SRV).
   The anonymous single "scene target" dies; `RHISetRenderTarget` binds the
   registry entry. (The create hook already captures every surface + its
   resolve texture + its usage name.)
2. **Resolve = link, not copy** (exactly ProcStretchRect):
   `RHICopyToResolveTarget` marks resolve-texture → surface. Delete the whole
   readback/7e3/tiling injection machinery once this lands.
3. **Sampling RT-backed textures**: `dpour_tex::Acquire` consults the registry
   first — an RT-backed guest texture returns the native target's SRV slot, no
   decode. (This is how the game's own post chain and UI consume our scene.)
4. **All passes render natively** into their bound targets - shadows,
   reflections, post - not just the marked scene pass. The pass machinery and
   per-draw capture already handle the hard part (they reproduce ~all draws).
5. **UP draws** (`RHIBegin/End/DrawPrimitiveUP` - the whole 2D UI) captured and
   drawn natively. Hooks exist as log stubs; the capture is the missing piece.
6. **Clears routed**: `RHIClear` → native Clear on the bound registry target.
7. **Present**: the game's backbuffer pass renders into our final target →
   presented opaque. Guest draws all skipped; the emulated GPU goes idle, and
   can later be cut out entirely (that is where the real FPS lives, together
   with the guest-CPU relief).

Marker of done for the first milestone: the world visible with OUR draws only
(`DPOUR_NR_DRAW_ONLY`-style), UI included, no emulated frame underneath.

### 🏗️ BUILT earlier the same evening (now superseded by the verdict above,
### kept for the record): the injection attempt

The design below is implemented and deployed:
* `dpour_tex::EncodeColorForGuestTexture` — RGBA16F → the resolve texture's own
  layout: 7e3 (fmt 63, LUT-driven half→7e3, exact inverse of the SDK's
  `Float7e3To32`), 16F (32) or 8888 (6/50); Xenos tiling via the proven
  `TiledOffset2D`; endian swap as the fetch constant says.
* Scene render → readback buffer (one in flight, fenced) → worker thread
  encodes → `OnResolveScene` (the `sub_829CDDC0` hook) memcpy's into guest
  memory INSTEAD of the game's resolve. Skipping the original is what prevents
  the emulated GPU's own resolve from racing/stomping the injected bytes, and
  the CPU write trips the emulator's texture write-watch so it re-uploads OUR
  image for the game's post chain.
* One frame of latency by construction (scene is a frame late inside the post
  chain — consistent and invisible).
* `TEST_NATIVE_INJECT.bat` = stage 1, injection only (validates the picture;
  no FPS gain yet). `TEST_NATIVE_FPS.bat` = stage 2, + `DPOUR_NR_DRAW_SKIPGUEST`
  (guest stops rasterising the scene = the FPS win). Log: `[native-inject]`.

### 📐 The original design notes (kept for context)

Goal: guest stops rasterising the scene (the big GPU cost), our image takes its
place INSIDE the guest frame, so the game's own post-processing and UI draw on
top of it — ordering and look both correct, and FPS gets the win.

Mechanism (all inside DownpourRecomp hooks, no SDK changes):
1. Per-draw skip already exists and is correct: captured scene draws return
   early from the device draw hooks under `DPOUR_NR_DRAW_SKIPGUEST`
   (downpour_native_draws.cpp:535-561).
2. Our scene target gets a GPU→CPU readback each frame (one 1280x720 buffer,
   double-buffered, one frame of latency).
3. Hook `RHICopyToResolveTarget` (`sub_829CDDC0`, already mapped): when the
   source surface is a `DefaultColor*` alias, AFTER the original call write the
   last readback into the resolve texture's guest bytes
   (`SceneResolveTexture()` → FXeGPUResource: D3D object @+8 → data
   BaseAddress; format from creation = SceneColorBufferFormat, expect 7e3
   A2B10G10R10F 32bpp, TILED — reuse/replicate the SDK's tiling address math).
   The emulated texture cache watches guest memory, sees the write, re-uploads,
   and the game's post chain consumes OUR scene.
4. Only then does full-scene skipping become visually correct; without the
   injection, skipping leaves the UI painted over a black scene and the overlay
   composite hides it.

### ✅ LANDED (same evening): the scene is no longer guessed — it is MARKED

§5's "pass vote picks the wrong pass" had a root cause found in the game's own
source: `RHIMSAABeginRendering` binds the scene's EDRAM colour surface through
the **device's** `SetRenderTarget` (`SetMainpassRenderTargets`, inlined), never
through `RHISetRenderTarget` — so the pass tracker literally never saw the
scene's target switch, and the vote was choosing among everything else.

Fix: hook the game's own frame brackets, identified by call fingerprint:

* `sub_829CB2F8` = `RHIMSAABeginRendering` (calls BeginTiling `sub_82D28D48`,
  first-tile Clear `sub_82D27328` under tile predication)
* `sub_829CB568` = `RHIMSAAEndRendering` (four `D3DDevice::Resolve`
  `sub_82D27A70` calls — colour+depth × two tiles — then EndTiling)
* also mapped: `sub_829CB048` = `RHIMSAAInitPrepass`,
  `sub_829CB870` = `RHIRestoreColorDepth`, `sub_82D27A70` = `Resolve`

Draws between the brackets are tagged into a reserved scene slot
(`kScenePassSlot`) and published deterministically; the vote remains only for
frames with no MSAA block (menus, movies). NOT yet validated in gameplay — the
title screen has no MSAA block, so the brackets simply do not fire there.

### Still open

1. **Gameplay validation of all of today's work** — the loop guard under real
   world shaders, the bool banks going nonzero, the scene marker firing and
   publishing the full world pass. `TEST_NATIVE_DRAW.bat`, play into a level,
   read `[native-state]` / `[native-scene]` / `RHIMSAABeginRendering` lines.
2. **The full frame flow is now documented from source** (XeD3DCommands.cpp):
   prepass (depth-only, colour NULL) → MSAA block (scene, 2 EDRAM tiles) →
   Resolve into SceneColor/SceneDepth guest textures → post-processing samples
   them → UI. Next Phase B step: capture the resolve destinations in the
   `sub_829CB568` hook (r3 = depth texture, r4 = colour texture RHI refs) and
   back those guest textures with our native target, so the post chain reads
   native output.
3. **Depth prepass draws are not reproduced** (`ps == 0`). Fine while the world
   uses GREATER_EQUAL (observed so far); if gameplay shows draws with func
   EQUAL being dropped, tag the prepass block (`sub_829CB048` hook exists) and
   replay its depth-only draws first.
4. **Frame rate is low while both renderers run.** Expected: the GPU draws the
   scene twice. Must not be optimised against until the guest's draws can be
   dropped wholesale.

---

## 6. The plan to finish it properly

### Phase A — make the current pass correct (small, unblocks everything)
1. Run the depth view; settle depth direction from the new log line. Fix the
   clear value and, if inverted, make sure the prepass and base pass agree.
2. Find `SetVertexShaderConstantB` / `SetPixelShaderConstantB` and fill
   `g_Bools` / `g_Loops`. Search for the device method the RHI functions at
   `XeD3DCommands.cpp:509/527` call. Without this, materials are guesswork.
3. Verify textures independently: a mode that shows the sampled base colour only.
4. Colour-write mask and MRT: today the mask is always ALL and only one render
   target is bound. Find `RB_COLOR_MASK` in the same register block.

### Phase B — the render-target graph (this is the real work)
Today everything is replayed into **one** target and only one pass is published.
That is why post-processing, shadows, reflections and the UI are missing, and why
colour cannot match. The references do not do this — they reproduce the whole
graph. Concretely:

1. Give every guest surface its own host render target, created on
   `RHICreateTargetableSurface` (`sub_829CD600`) and bound on
   `RHISetRenderTarget` (`sub_829C9BB0`).
2. Reproduce `RHICopyToResolveTarget` (`sub_829CDDC0`, 189 calls a frame) as a
   host copy/resolve into the texture the next pass samples.
3. Once resolves exist, the game's own post-process draws — which are ordinary
   draws sampling those textures — reproduce themselves, and colour becomes
   correct by construction rather than by tuning.
4. Then `DPOUR_NR_DRAW_SKIPGUEST` can drop the guest's submissions wholesale and
   the Xenos rasteriser stops doing any scene work. **That** is the point where
   the frame rate should improve rather than halve.

### Phase C — replace the device, as the references do
UnleashedRecomp does not intercept a live guest renderer; it **replaces**
`Direct3DCreate9`/`CreateDevice` and owns every object from then on
(`GuestDevice`, `GuestSurface`, `GuestTexture`). That removes the whole class of
problems this session kept hitting — shadowing state that the guest also mutates,
guessing which pass is the scene, page-probing guest memory. Downpour's
equivalent entry points are known (`CreateDevice`-wrapper `sub_82D25DD0`,
`GDirect3DDevice = 0x83790010`). This is the largest step and should be taken
only once Phase B has proved the graph is understood.

### Phase D — performance
Only meaningful after B. Then measure with the `DPOUR_NO_NATIVE_RENDER` build as
the baseline, and look at **p90**, not the median.

---

## 7. Rules this project has earned the hard way

* Do not ship a visible change default-ON. Every switch here defaults to off.
* When told "it worked before", build the `DPOUR_NO_NATIVE_RENDER` binary and
  measure **first**. Do not argue from your own telemetry — it cannot see the
  code outside it.
* A diagnostic that changes shader model, ring size, or state is not a
  diagnostic; it is a second bug.
* Split failure counters by reason. "Buffers failed" was true of 96 % of the
  world's draws and said nothing.
* Never leave a test instance of the game running.

---

## 26.07 morning — the black frame explained: passes were tracked at the wrong layer

**The finding, from the game's own source plus live telemetry.** `XePerformSwap`
(XeD3DDevice.cpp:426) ends every frame with
`Resolve(0, NULL, GD3DFrontBuffer, ...)` followed by `Swap(GD3DFrontBuffer)`.
`Resolve` with a null source rect takes the **currently bound render target**,
and `RHIEndDrawingViewport` (XeD3DViewport.cpp:95) binds `GD3DBackBuffer`
immediately before it. So the frame that reaches the screen IS the back-buffer
surface - there is nothing to guess about which target to publish.

Our pass table said the back-buffer pass received **2 draws a frame** while a
different target received 110. The reason: we hooked `RHISetRenderTarget`
(sub_829C9BB0) only, and the engine binds targets straight through the device in
several places - `RHIEndDrawingViewport`'s own
`GDirect3DDevice->SetRenderTarget(0, GD3DBackBuffer)`, the MSAA block's
`SetMainpassRenderTargets`, the post chain. Those binds were invisible, and
their draws were charged to whatever the RHI had bound last. This is the third
time the same lesson has been paid for (MSAA brackets, UP draws, now targets):
**the complete view is at the device level, not the RHI level.**

### Landed (built, deployed, NOT yet visually validated)

1. **Device-level target hooks.** `sub_82D1A7F8` = SetRenderTarget(dev,
   index=r4, surface=r5) and `sub_82D1AB88` = SetDepthStencilSurface(dev,
   surface=r4). r5 is already the surface OBJECT, not an FSurfaceRHIRef, so it
   needs no dereference and matches what the create hook registered. New
   `dpour_scene::DeviceSetRenderTarget/DeviceSetDepthStencil`; passes are now
   keyed on the object. Keying on the ref pointer had split one physical target
   across two slots (refs 0x407c7e88 and 0x401e542c both holding object
   0x40022370). The RHI hook stands down while this path is active. Kill switch:
   `DPOUR_NR_DRAW_NODEVRT`.
2. **Back-buffer identification.** `color_object == deref(GD3DBackBuffer @
   0x83790044)`. Confirmed live: `object 0x400007c0 == BACKBUFFER`, reached
   through two different refs. `is_backbuffer` on PassSlot, the back-buffer
   target sized 1280x720 (the pass viewport is NOT its size - UI work clips to
   small rects on the same surface), and the composite publishes it in
   preference to the draw-count vote. Kill switch: `DPOUR_NR_DRAW_NOBBPRIO`.
3. **A second use-after-free mechanism, removed rather than papered over.**
   Measured: **1758 renders against 600 publishes** - the presenter calls Render
   every present, the guest publishes only on a finished frame, so the same
   snapshot was re-submitted about three times over (far more through a loading
   stall). The snapshot holds RAW vertex/index/constant GPU addresses, so a
   buffer retired while it was still being replayed got freed under a submission
   that had not run yet - the DRED signature we already knew (page fault, every
   breadcrumb list complete). The `+8 fences of slack` in the buffer cache was a
   guess at how long that replay window could be. Now: **one submission per
   published guest frame** (`items_frame == last_rendered_frame &&
   guestout_has_frame` -> return). Nothing leaves the screen - our last frame
   lives in `g_guestout_tex` and the callback keeps serving it. The overlay path
   is exempt (it paints onto the presenter's list, rebuilt each frame).
4. **DRED now names the victim.** Every resource we create is named
   (`dpour.geometry`, `dpour.upload`, `dpour.texture`, `dpour.tex.upload`,
   `dpour.rt.<key>`, `dpour.guestout`) and the watchdog prints every allocation
   node, named or not. The previous version only printed nodes carrying an ANSI
   name, which is why the whole fault dump came out as a bare address.

### Next session, in order

1. Run and read the log for `[native-scene] device pass: surface 0x... ==
   BACKBUFFER (this is the frame)` and the new pass table. The back buffer
   should now receive hundreds of draws, not two. Screenshot the client area
   (`shot_client.ps1` in the session scratchpad - client area only, exits 2 when
   the game is not running).
2. Confirm the TDR is gone (fix 3). It previously killed the process at 45-85 s
   on the attract sequence, twice in a row.
3. If the back buffer is now complete, the remaining work is frame QUALITY
   (materials, shaders) and only then the suppression pitches = the FPS win.
4. Known and not broken: the intro movie runs on the emulated fallback - the
   native path yields the frame when it has too few draws to serve
   (`kGuestOutMinDraws = 16`). That is the reference discipline working.
5. Passes with `color_object == 0` (depth-only, shadows) are still skipped at
   replay - the next hole after the back buffer.
