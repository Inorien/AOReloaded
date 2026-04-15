# AOReloaded — Features

Client mod for Anarchy Online. Drop `version.dll` into your client directory and go.

## Installation

1. Copy `version.dll` into the same folder as `AnarchyOnline.exe`
2. Launch the game normally
3. Check `AOReloaded.log` in the client folder to confirm it loaded

## Features

### WoW-style Camera Auto-Follow
In 3rd person, the camera smoothly returns to a position behind your character whenever you move (and you're not holding LMB). LMB-drag still orbits the camera freely; on release it stays put until you start moving again. The "behind" position is based on which way your character is facing, so walking backwards or strafing won't flip the camera around.

Additionally, any **right-click drag** realigns your character to face the direction the camera is currently looking and snaps the camera to directly behind — so after orbiting with LMB, your next RMB-drag starts from a clean "behind" view instead of preserving the orbit offset.

**Config:** Options panel (F10) → **AOReloaded** tab → **Camera** → "WoW-style camera (auto-recenter after LMB drag)". Lerp speed is controlled by the `AOR_CYawSpd` DValue (default 5; higher = snappier follow).

### Extended Ground Full Quality Radius
The stock client's **Display → Ground Full Quality Radius** slider caps at 44 and hard-crashes above ~48 because the per-patch terrain vertex buffer overflows. AOReloaded enlarges those vertex buffers at allocation time, letting you push the radius significantly higher without CTD. Without this, `DisplayGroundFullQualityRadius > 48` stomps the heap during the first render of a zone.

**Config:**
1. Options panel (F10) → **AOReloaded** tab → **Graphics** → **Ground VB size multiplier**. Default is `4x` (enough for radius ~100). Range `1..8`. Changes take effect on the next zone load (new `AnarchyGround_t` instance allocates fresh VBs).
2. Then raise **Display → Ground Full Quality Radius** (XML slider `max` already bumped to 100 in `LoginPrefs.xml`). Use values up to roughly `48 × sqrt(multiplier)` — i.e. `multiplier=4` supports radius ~96.
3. Relog or travel to a new playfield so the ground is rebuilt.

**Memory cost:** per-zone terrain VB size scales linearly with the multiplier. `4x` typically adds a few MB of VRAM per zone; `8x` is the ceiling for paranoid users.

<!-- 
Template for adding features:

### Feature Name
Brief description of what it does from a player's perspective.

**Config:** How to configure it (options panel, /command, ini file, or "none — always on").

-->

## Options Panel

AOReloaded adds an **AOReloaded** tab to the in-game options panel (F10). Mod settings will appear here as features are added.
