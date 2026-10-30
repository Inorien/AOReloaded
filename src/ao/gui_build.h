#pragma once

#include <cstdint>

// GUI.dll addresses per client build.
//
// AOReloaded hooks code inside GUI.dll that isn't exported, so its addresses
// are tied to one build. The clients in use have two: Project Rubi-Ka's May
// 2019 build (the one these hooks were written against) and the classic
// 18.8.62 client's June 2023 build (AO-Andersson, AORebirth's classic client).
// The two compile the same sources - the classes are laid out alike - only
// the code has moved. The row is picked by GUI.dll's PE timestamp; on any
// other build GuiAddrs() is null and the features that need it stay off.
//
// The 2023 column was found by matching each 2019 function's code in the
// 2023 GUI.dll (relocated addresses and branch offsets left out), checked
// against the exports where the function is exported.

namespace aor {

// A function that starts with the MSVC SEH prolog: mov eax, <EH table>;
// call __EH_prolog. Both RVAs are checked before hooking.
struct GuiSeh {
    uint32_t rva;
    uint32_t ehTable;
};

struct GuiBuild {
    uint32_t    stamp;   // IMAGE_FILE_HEADER::TimeDateStamp
    const char* name;

    // Timer bars (timer_bar_drag.cpp)
    uint32_t createTimer;           // TimerSystemModule_t::CreateTimer -> TimerBar_c*
    uint32_t createGameTimer;       // TimerSystemModule_t::CreateTimer -> GameTimerBar_c*
    uint32_t tsmGetInstance;        // TimerSystemModule_t::GetInstance
    uint32_t tsmStopTimerbar;       // TimerSystemModule_t::StopTimerbarMessage
    uint32_t rwReposition;          // RenderWindow_t::Reposition
    uint32_t rwSetScale;            // RenderWindow_t::SetScale
    uint32_t rwResize;              // RenderWindow_t::Resize
    uint32_t pbAdjustPowerLevel;    // PowerBar_t::AdjustPowerLevel

    // WindowController_c
    uint32_t wcGetInstance;
    uint32_t wcGetDragObject;
    uint32_t wcSetMousePosition;
    uint32_t wcHandleTextInput;

    // Movement and mouse (input_handler.cpp)
    uint32_t slotMovementForward;   // FlowControlModule_t::SlotMovementForward
    uint32_t autorunCallerRet;      // SlotMovementAutoRun's return address from its call to it
    uint32_t onMouseDown;
    uint32_t onMouseMove;
    uint32_t onMouseUp;
    uint32_t endDrag;
    uint32_t dragThreshold;         // float, the drag threshold OnMouseUp reads

    // Numpad (numpad_fix.cpp)
    uint32_t checkInput;            // InputConfig_t::CheckInput

    // Inventory (bag_move.cpp, alt_select.cpp, inventory_pins.cpp)
    uint32_t itemActivated;         // InventoryViewBase_c item-activation slot
    GuiSeh   listMouseUp;           // MultiListView_c mouse-up
    uint32_t itemVtable;            // InventoryListViewItem_c vftable
    uint32_t itemCompare;           // InventoryListViewItem_c compare (vftable[1])
    uint32_t itemGetName;
    uint32_t splitMouseDown;
    uint32_t splitMouseDownRtti;    // InventoryListViewItem_c type descriptor

    // Skills window (skill_favorites.cpp)
    uint32_t ehProlog;              // __EH_prolog
    GuiSeh   skillWindowCtor;
    GuiSeh   initGroups;
    GuiSeh   suggestIP;
    GuiSeh   rowClicked;
    GuiSeh   rowCtor;
    GuiSeh   rowDtor;
    GuiSeh   changeDelta;
    GuiSeh   incPressed;
    GuiSeh   incReleased;
    GuiSeh   decPressed;
    GuiSeh   decReleased;
    uint32_t groupPushBack;         // vector<Group>::push_back
    uint32_t statListsResize;       // vector<vector<Stat_e>>::resize
    uint32_t statPushBack;          // vector<Stat_e>::push_back

    // Import address table slots (Interfaces.dll, AFCM.dll)
    uint32_t iatN3IMGetInstance;    // N3InterfaceModule_t::GetInstance
    uint32_t iatN3MsgGetName;
    uint32_t iatN3MsgMouseMovement;
    uint32_t iatN3MsgCameraMouseLookMovement;
    uint32_t iatN3MsgEndMouseLook;
    uint32_t iatN3MsgEndCameraMouseLook;
    uint32_t iatAFCMGetInstance;
    uint32_t iatAFCMSend;
};

// The loaded GUI.dll's row, or null (GUI.dll not loaded, or a build not in
// the table - logged once).
const GuiBuild* GuiAddrs();

}  // namespace aor
