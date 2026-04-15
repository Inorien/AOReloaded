# Extended `DisplayGroundFullQualityRadius` — Investigation & State

Branch snapshot. Unfinished work — pick up from the "Where we left off" section.

## Goal

Raise the `DisplayGroundFullQualityRadius` cap in `LoginPrefs.xml`. Stock client caps at **44** (XML max) and hard-crashes above **48** (empirical, user confirmed).

## What the Setting Controls

`DisplayGroundFullQualityRadius` is read by `n3GroundRenderer_t` (N3.dll) via `SlotGroundFQRadiusChanged`, which forwards to `AnarchyGround_t::SetHQRadius(int)` in DisplaySystem.dll (RVA `0x3255b`). `SetHQRadius` stores the value at `AnarchyGround_t+0x1a8` and regenerates a 256-entry LOD threshold table via `FUN_1003216f` using the formula:

```
table[i] = int( sqrt( i * quality / C + radius² ) )   for i = 0..255
```

The table drives per-patch LOD level selection during render. Higher radius → more patches stay at full detail → more vertices/indices written into per-patch vertex buffers. **The crash at radius > 48 is a per-patch vertex-buffer overflow.**

## Ghidra Findings (DisplaySystem.dll)

Relevant addresses (RVAs, Ghidra base 0x10000000):

| RVA | Symbol | Notes |
|-----|--------|-------|
| `0x3255b` | `AnarchyGround_t::SetHQRadius` | thiscall, `void(this, int)` |
| `0x32546` | `AnarchyGround_t::SetQuality` | same shape |
| `0x1003216f` | `FUN_1003216f` | builds 256-entry LOD table from radius + quality |
| `0x35348` | `AnarchyGround_t::Tesselate(int, int)` | stores grid dims; no allocation |
| `0x33b97` | `AnarchyGround_t::_New` (ctor) | allocates cells, per-cell buffers |
| `0x34658` | `FUN_10034658` | per-frame render pass 1 (count → allocate VBs → pass 2 writes) |
| `0x38606` | `FUN_10038606` | VertexBuffer_c wrapper ctor. `void*(this, int size)`. Prologue `55 8B EC 33 C0`. **Our hook target.** Minimum size internally clamped to `0xcbc`. |
| `0x10031c0a` | `FUN_10031c0a` | per-cell init buffer filler; writes bytes up to `2^(subdivExp-1)` — sized correctly, NOT the overflow. |

Ghidra's xref search for `FUN_10038606` reported only 2 call sites (both inside `FUN_10034658`). **This is incomplete.** A third caller exists: `AnarchyGroundDataDB_t::Instantiate` also calls it (confirmed at runtime via `_ReturnAddress()` instrumentation — return address points into `FUN_10034658`, but the xref analysis had missed the DB-level path). Treat Ghidra xrefs as a hint, not a guarantee.

## Ghidra Findings (randy31.dll)

| RVA | Symbol | Notes |
|-----|--------|-------|
| `0x46224` | `RResource_t::ReleaseRResource` | decrements refcount; calls vtable[0] if 0. Prologue `8B 41 24 85 C0`. |
| `0x48f39` | `RTriMesh_t::~RTriMesh_t` | dtor. Calls `ReleaseRResource(this+0x184)` then `~RVisual_t`. |
| `0x4d7d3` | `~RVisual_t` | calls `ReleaseRResource(this+0xB8)` — **this is where the zone-teardown crash occurs**. |
| `0x114b1` | `VertexBuffer_c::VertexBuffer_c` | allocates inner 0x1c-byte object, calls setup helper. |
| `0x11797` | VB setup helper | stores params; computes stride; `dwNumVertices = total_bytes / stride`. |
| `0x11745` | VB final init | builds `_D3DVERTEXBUFFERDESC` and calls `render_t::CreateVertexBuffer`. |
| `0x24a77` | `render_t::CreateVertexBuffer` | calls D3D7 vtable. **Throws `fun::DXError` if D3D returns an error** — static `_ThrowInfo` at `0xa45d4`. |

## The Two Bugs

### Bug A — Per-patch VB overflow at high radius (fixable; our primary target)

Per-patch vertex buffer is sized via `FUN_10038606(size)`. At `radius > 48`, pass-1 counting produces a `size` that the pass-2 writer overflows. Stomps adjacent heap objects; classic signature was `ECX = 0x00410040` ("`@`" "`A`" in UTF-16) on the next traversal — a stomped tree-node pointer.

**Our fix:** hook `FUN_10038606` via AOReloaded and multiply `size` by a user-tunable scale factor (`AOR_GndVBScale`, DValue int, default 4, range 1..8). Enlarging the VB prevents overflow. See [../../src/hooks/ground_hq_hook.cpp](../../src/hooks/ground_hq_hook.cpp).

**Gotcha (learned from instrumented run):** scaling blindly produces `dwNumVertices > 65535` for larger input sizes, which D3D7 refuses (16-bit index limit, hardware cap). Instrumentation output:

```
[gndvb] call #1: size=15    scaled=60    ret=5CD547F9
[gndvb] call #2: size=6867  scaled=27468 ret=5CD547F9
[gndvb] call #3: size=19710 scaled=78840 ret=5CD547F9  → CRASH
```

Call #3's 78840 exceeded D3D7's 65535 cap. Current detour caps `scaled` at 65535 (never reduces below original) — effective scale for call #3 is 65535/19710 ≈ 3.33×, still above the stock ceiling.

Stock error on violation is `fun::DXError` throw from `render_t::CreateVertexBuffer`. Exception code `e06d7363` (MSVC C++ EH). Static throwinfo at randy31 `0xa45d4`. Error message string: `"render_t::CreateVertexBuffer: D3D-Call failed"`.

### Bug B — VisualEnvFX RTriMesh stale pointer at zone teardown (latent, partially fixed)

After Bug A is worked around, zone teardown hits a second crash:

```
randy31!RResource_t::ReleaseRResource   (ECX = 0x0012000c)
randy31!RTriMesh_t::~RTriMesh_t+0x3f     (actually inside ~RVisual_t)
randy31!~RVisual_t                       ← reads this+0xB8
DisplaySystem!VisualEnvFX_t::SetDataPath+0x120  (actually FUN_10061233)
DisplaySystem!VisualEnvFX_t::DeleteInstance
DisplaySystem!VisualEnvFX_t::ReadyNextFX
N3!n3Playfield_t::~n3Playfield_t
```

`~RVisual_t` at randy31 `0x4d7d3` reads `this+0xB8` and calls `ReleaseRResource` on it. The value was `0x0012000c` at crash — a stomped/freed pointer below normal heap range. Memory at that address is unmapped (`dd` returns `????????`).

Model: some VisualEnvFX RTriMesh object is being destroyed with a stale `RVisual_t+0xB8` slot. This is likely a **pre-existing use-after-free** in the client. At stock radius the corrupted memory happens to contain a non-crashing value; at high radius the heap layout shifts and the corruption lands on an unmapped address. **Evidence this isn't caused by our hook:** the crash vanished when we reverted radius to 44 *with our scale hook still active* — our hook doesn't participate in this code path.

Allocator ownership investigation of the VisualEnvFX container in `FUN_1005e716` (DisplaySystem) shows the container's `+0x10` slot (the "RTriMesh head") is **not AddRef'd**: `FUN_10061505` populates arr1/arr2 with `AddRefRResource` on each element but sets `container+0x10 = vtable[3](DbObject+0x20)` without a ref increment. Later `FUN_10061233` destroys the container by calling `vtable[0](1)` on `container+0x10` as if it owned the pointer — classic borrowed-pointer-then-release pattern. Whether this specific path produces the stomp at `RVisual_t+0xB8` is **unconfirmed** — we ran out of time tracing it.

**Our partial fix:** SEH-based guard around `ReleaseRResource`. See [../../src/hooks/release_guard_hook.cpp](../../src/hooks/release_guard_hook.cpp). The detour wraps `g_origRelease(this_)` in `__try/__except(EXCEPTION_ACCESS_VIOLATION)`. If the release AVs (as it did at `ECX=0x0012000c`), we log and return, leaking the stale reference but preventing CTD.

**An earlier version of this guard used a pointer-range check** (`this < 0x00200000` rejected). That was wrong — 32-bit processes have valid mappings below that threshold (`AnarchyOnline.exe` base is `0x00010000`), and the range check broke legitimate releases of statically-linked objects in the .exe, causing cascade failures later (including the D3D throw we eventually diagnosed in Bug A). Do not reintroduce the range check. SEH is the correct mechanism here.

## Files Touched

- [../../src/hooks/hook_engine.cpp](../../src/hooks/hook_engine.cpp) — added two new prologue patterns: `frame+xor-eax` (`55 8B EC 33 C0`) and `mov-this24+test` (`8B 41 24 85 C0`).
- [../../src/hooks/ground_hq_hook.{h,cpp}](../../src/hooks/ground_hq_hook.cpp) — scale hook on DisplaySystem `FUN_10038606`, with 65535 cap. DValue `AOR_GndVBScale` (int, 1..8, default 4). Poller thread reads DValue every 500ms.
- [../../src/hooks/release_guard_hook.{h,cpp}](../../src/hooks/release_guard_hook.cpp) — SEH guard on randy31 `ReleaseRResource`.
- [../../src/dllmain.cpp](../../src/dllmain.cpp) — registers `AOR_GndVBScale` DValue; installs both hooks after `camera_mode` detected.
- [../../CMakeLists.txt](../../CMakeLists.txt) — added the two new sources.
- [../../../client/cd_image/gui/Default/LoginPrefs.xml](../../../client/cd_image/gui/Default/LoginPrefs.xml) — raised `max` on `DisplayGroundFullQualityRadius` to `100`.
- [../../../client/cd_image/gui/Default/OptionPanel/Root.xml](../../../client/cd_image/gui/Default/OptionPanel/Root.xml) — added "Ground VB size multiplier" slider in AOReloaded → Graphics tab (binds to `AOR_GndVBScale`).
- [../../CLAUDE.md](../../CLAUDE.md), [../../FEATURES.md](../../FEATURES.md), [../../../IDEAS.md](../../../IDEAS.md) — updated to reflect new hooks and feature.

## Timeline (for future-Claude context)

1. **Rendering overflow at radius > 48.** First CTD, reported at WinDbg AV `FUN_10032f1b+0x2a` reading stomped tree-node pointer `0x00410040`. Heap stomp from per-patch VB overflow.
2. **Scale hook installed (4×).** Rendering works at radius 100. Zone change → new CTD at `~RVisual_t` reading stomped `RVisual_t+0xB8 = 0x0012000c`.
3. **Test at radius 44 with scale hook still active → no crash.** Proved Bug B is radius-sensitive heap layout, not caused by our hook.
4. **Added release-guard hook (range-check version).** New CTD at startup in `render_t::CreateVertexBuffer` throw, inside `AnarchyGround_t::_New` → our detour. Range check was rejecting valid releases.
5. **Rewrote guard as SEH.** Still crashed at startup — same throw. Guard was never the cause of step-4 crash; the real cause was unchecked scaling.
6. **Instrumented scale hook.** Confirmed call #3 (`size=19710, scaled=78840`) exceeds 65535 D3D7 cap.
7. **Added 65535 cap.** Not yet verified — user branched here.

## Where we left off

The latest `version.dll` was **built but not tested** with the 65535 cap. The user needs to:

1. Close any open WinDbg session (holds `client/version.dll` open; blocks the build copy).
2. `cmake --build build/debug` (in `AOReloaded/`) — should copy to `client/version.dll`.
3. Launch with WinDbg:
   ```
   sxe e06d7363
   sxe av
   g
   ```
4. Verify: no startup crash. Expected log entries in `client/AOReloaded.log`:
   ```
   [gndvb] ground-VB scale hook installed ...
   [relguard] release-guard hook installed (SEH-based)
   ```
5. Enter world at radius=100, verify rendering is clean.
6. Zone-change. Expected: either clean zone change (SEH guard catches stale release silently — look for `[relguard] caught AV in ReleaseRResource(this=...)` in the log), or new CTD somewhere else entirely.

## Open questions / next directions

- **Verify the 65535 cap is the real D3D ceiling on this user's driver.** If a zone legitimately needs a VB > 65535 vertices, our cap would silently clip it. We don't currently log when the cap fires. Next session should add a one-shot log when `product > kMaxVBCount`.
- **Root-cause Bug B** instead of papering over with SEH. Load DisplaySystem.dll into Ghidra; trace who calls `FUN_10061505` and confirm whether `container+0x10` is ever AddRef'd elsewhere. If not, the fix is either (a) AddRef on container creation, or (b) remove the vtable-dtor call inside `FUN_10061233`. Either is a targeted byte patch or small hook.
- **Third caller of `FUN_10038606` that Ghidra missed** — run the scale hook with return-address logging enabled (it was there during step 6, then removed) to confirm whether `AnarchyGroundDataDB_t::Instantiate` is actually reached, or whether all calls come through `FUN_10034658`. Current data shows all 3 calls from `FUN_10034658`. The crash stack's `Instantiate` label may be a WinDbg nearest-export artifact.
- **Slider widget is broken.** The `AOR_GndVBScale` DValue registered via `AddVariable` lacks `has_min`/`has_max` flags in the registry node (offsets `+0x61`/`+0x62` in `AODistributedValue`). The game's `OptionSlider` reads those from the DValue node, not from the XML. Result: slider shows 0 with no knob. Fix: find the Utils.dll setter for min/max (probably `DistributedValue_c::SetRange` or similar), call it after `AddVariable`. Unrelated to CTD work; independent followup.
- **Measure per-zone VRAM cost of scale=4.** We're 4× the per-patch VB memory. Budget impact unknown. Add logging or a runtime counter.

## Reproduction helpers

- WinDbg one-liners to resume from scratch (exception codes):
  ```
  sxe e06d7363      # MSVC C++ throw
  sxe av            # access violation
  .sympath+ "D:\Games\Project Rubi-Ka\client"
  .reload /f
  g
  ```
- On break: `.exr -1`, `k 30`, `r`, `~*k`.
- Key address-translation formula: WinDbg address − (module base from `lm`) = Ghidra RVA (Ghidra base `0x10000000`).
- All affected DLLs can be loaded individually into Ghidra via MCP (see root `CLAUDE.md`). randy31 base `0x10000000 + .text 0x10001000-0x100893ff`. DisplaySystem same base, `.text 0x10001000-0x10088fff`.
