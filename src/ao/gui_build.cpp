#include "ao/gui_build.h"

#include "core/logging.h"

#include <windows.h>

namespace aor {

namespace {

constexpr GuiBuild kBuilds[] = {
    {
        .stamp = 0x5CD32BEA, .name = "PRK (2019)",
        .createTimer = 0x518f0, .createGameTimer = 0x5195b,
        .tsmGetInstance = 0x51d05, .tsmStopTimerbar = 0x518b0,
        .rwReposition = 0x20f6e, .rwSetScale = 0x20b0c, .rwResize = 0x20938,
        .pbAdjustPowerLevel = 0x205de,
        .wcGetInstance = 0xb454, .wcGetDragObject = 0x156e29,
        .wcSetMousePosition = 0x156abf, .wcHandleTextInput = 0x156f92,
        .slotMovementForward = 0x27ec1, .autorunCallerRet = 0x27fcd,
        .onMouseDown = 0x2c2ee, .onMouseMove = 0x2c17b, .onMouseUp = 0x2c469,
        .endDrag = 0x2c0e5, .dragThreshold = 0x1aeaf4,
        .checkInput = 0x1a4c1,
        .itemActivated = 0xca1e7, .listMouseUp = {0x40a17, 0x17fe7c},
        .itemVtable = 0x1b0128, .itemCompare = 0x3cc19, .itemGetName = 0x3ca3d,
        .splitMouseDown = 0x40f22, .splitMouseDownRtti = 0x264934,
        .ehProlog = 0x1738a4,
        .skillWindowCtor = {0xfc18e, 0x1998a8}, .initGroups = {0xfb596, 0x199683},
        .suggestIP = {0xfac49, 0x1993e3}, .rowClicked = {0xf97a6, 0x199275},
        .rowCtor = {0xfe956, 0x199c56}, .rowDtor = {0xff3f4, 0x199c84},
        .changeDelta = {0xfde49, 0x19995d},
        .incPressed = {0xfe261, 0x18965e}, .incReleased = {0xfe361, 0x18af75},
        .decPressed = {0xfe0ca, 0x18965e}, .decReleased = {0xfe1b4, 0x18af75},
        .groupPushBack = 0xff7cc, .statListsResize = 0xff558, .statPushBack = 0x64bee,
        .iatN3IMGetInstance = 0x1a772c, .iatN3MsgGetName = 0x1a7724,
        .iatN3MsgMouseMovement = 0x1a7600, .iatN3MsgCameraMouseLookMovement = 0x1a7604,
        .iatN3MsgEndMouseLook = 0x1a7608, .iatN3MsgEndCameraMouseLook = 0x1a760c,
        .iatAFCMGetInstance = 0x1a70e0, .iatAFCMSend = 0x1a70e4,
    },
    {
        .stamp = 0x647A09DA, .name = "2023",
        .createTimer = 0x52018, .createGameTimer = 0x52083,
        .tsmGetInstance = 0x5242d, .tsmStopTimerbar = 0x51fd8,
        .rwReposition = 0x21477, .rwSetScale = 0x21015, .rwResize = 0x20e41,
        .pbAdjustPowerLevel = 0x20ae7,
        .wcGetInstance = 0xb613, .wcGetDragObject = 0x157108,
        .wcSetMousePosition = 0x156d9e, .wcHandleTextInput = 0x157271,
        .slotMovementForward = 0x289f5, .autorunCallerRet = 0x28b01,
        .onMouseDown = 0x2ce31, .onMouseMove = 0x2ccbe, .onMouseUp = 0x2cfac,
        .endDrag = 0x2cc28, .dragThreshold = 0x1aeafc,
        .checkInput = 0x1a900,
        .itemActivated = 0xcae1b, .listMouseUp = {0x4141b, 0x17fedd},
        .itemVtable = 0x1b0130, .itemCompare = 0x3d5f3, .itemGetName = 0x3d417,
        .splitMouseDown = 0x41926, .splitMouseDownRtti = 0x264934,
        .ehProlog = 0x173954,
        .skillWindowCtor = {0xfcbac, 0x199938}, .initGroups = {0xfbfb4, 0x199713},
        .suggestIP = {0xfb667, 0x199473}, .rowClicked = {0xfa1c4, 0x199305},
        .rowCtor = {0xfef4c, 0x199ce6}, .rowDtor = {0xff9ea, 0x199d14},
        .changeDelta = {0xfe550, 0x1999f7},
        .incPressed = {0xfe968, 0x189731}, .incReleased = {0xfea68, 0x18202f},
        .decPressed = {0xfe7d1, 0x189731}, .decReleased = {0xfe8bb, 0x18202f},
        .groupPushBack = 0xffd72, .statListsResize = 0xffafe, .statPushBack = 0x113414,
        .iatN3IMGetInstance = 0x1a7730, .iatN3MsgGetName = 0x1a7728,
        .iatN3MsgMouseMovement = 0x1a7604, .iatN3MsgCameraMouseLookMovement = 0x1a7608,
        .iatN3MsgEndMouseLook = 0x1a760c, .iatN3MsgEndCameraMouseLook = 0x1a7610,
        .iatAFCMGetInstance = 0x1a70e0, .iatAFCMSend = 0x1a70e4,
    },
};

}  // namespace

const GuiBuild* GuiAddrs() {
    static bool looked = false;
    static const GuiBuild* build = nullptr;
    if (looked) return build;
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA("GUI.dll"));
    if (!base) return nullptr;   // not loaded yet: look again next time
    looked = true;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    for (const GuiBuild& b : kBuilds) {
        if (b.stamp == nt->FileHeader.TimeDateStamp) {
            Log("[gui] GUI.dll build %s (0x%08X)", b.name, b.stamp);
            build = &b;
            return build;
        }
    }
    Log("[gui] GUI.dll build 0x%08X isn't known - its features stay off",
        nt->FileHeader.TimeDateStamp);
    return nullptr;
}

}  // namespace aor
