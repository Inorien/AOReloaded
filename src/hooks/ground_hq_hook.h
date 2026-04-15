#pragma once

// Ground-terrain vertex-buffer scaler.
//
// Hooks AnarchyGround_t's per-patch VertexBuffer_c wrapper constructor in
// DisplaySystem.dll and multiplies the requested capacity by a configurable
// scale factor before chaining to the original. This unlocks higher values
// of the LoginPrefs setting `DisplayGroundFullQualityRadius` without heap
// corruption.
//
// Background: at radius > ~48 the game's per-patch vertex buffers overflow.
// The overflow writes trash neighboring heap objects, and the next traversal
// of the per-cell quadtree reads a stomped pointer and AVs. See the
// investigation notes in docs/graphics/ground_hq_radius.md.
//
// Controlled by DValue `AOR_GndVBScale` (int, clamped 1..8). Read by a
// polling thread every 500ms; changes take effect on next playfield load
// (that's when AnarchyGround_t instances are rebuilt and new VBs allocated).

namespace aor {

// Install the hook. Requires DisplaySystem.dll to be loaded. Returns true
// on success; false leaves the game running unmodified.
bool InitGroundHQHook();

}  // namespace aor
