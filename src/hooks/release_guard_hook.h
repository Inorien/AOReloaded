#pragma once

// Defensive guard around randy31's RResource_t::ReleaseRResource.
//
// Some game code paths (observed in VisualEnvFX teardown during zone
// change with high DisplayGroundFullQualityRadius) invoke the release
// path on a stomped / use-after-freed RResource pointer, causing an
// access violation at the `MOV EAX, [ECX+0x24]` first instruction
// (reading the refcount slot).
//
// This hook validates the `this` pointer (ECX) before dereferencing.
// If it is clearly bogus (outside plausible userspace heap range), we
// skip the release entirely. This leaks a single reference worth of
// bookkeeping but avoids the CTD. The actual resource was already stale
// before our hook ran — there was nothing to release safely anyway.

namespace aor {

// Install the hook on randy31.dll. Returns true on success.
bool InitReleaseGuardHook();

}  // namespace aor
