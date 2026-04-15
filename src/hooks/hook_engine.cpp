// hook_engine.cpp — 5-byte inline hook installer for x86.
// See hook_engine.h for design notes.

#include "hooks/hook_engine.h"
#include "core/logging.h"

#include <cstring>

namespace aor {

namespace {

// Known-safe 5-byte prologues. Each is a sequence of complete, relocatable
// instructions that total exactly 5 bytes (no relative operands).
struct ProloguePattern {
    uint8_t bytes[5];
    uint8_t mask[5];   // 0xFF = exact match, 0x00 = wildcard
    const char* name;
};

constexpr ProloguePattern kPrologues[] = {
    // mov edi,edi; push ebp; mov ebp,esp  (hot-patch)
    {{0x8B, 0xFF, 0x55, 0x8B, 0xEC}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "hot-patch"},
    // push ebp; mov ebp,esp; sub esp,imm8  (standard frame + local alloc)
    {{0x55, 0x8B, 0xEC, 0x83, 0xEC}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "frame+sub"},
    // push ebp; mov ebp,esp; push esi  (frame + save reg)
    {{0x55, 0x8B, 0xEC, 0x56, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, "frame+push-r"},
    // push ebp; mov ebp,esp; push edi
    {{0x55, 0x8B, 0xEC, 0x57, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, "frame+push-r"},
    // push ebp; mov ebp,esp; push ebx
    {{0x55, 0x8B, 0xEC, 0x53, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, "frame+push-r"},
    // push ebp; mov ebp,esp; push ecx (+ any 2nd byte — e.g. push ecx; push ecx)
    {{0x55, 0x8B, 0xEC, 0x51, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, "frame+push-r"},
    // push ebp; mov ebp,esp; push eax
    {{0x55, 0x8B, 0xEC, 0x50, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, "frame+push-r"},
    // push ebp; mov ebp,esp; xor eax,eax  (frame + zeroed-return-slot)
    {{0x55, 0x8B, 0xEC, 0x33, 0xC0}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "frame+xor-eax"},
    // mov eax,[ecx+0x24]; test eax,eax  (no frame, direct this-field access)
    {{0x8B, 0x41, 0x24, 0x85, 0xC0}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, "mov-this24+test"},
};

// Check if target starts with a known-safe prologue. Returns the pattern
// name on match, nullptr on failure.
const char* MatchPrologue(const uint8_t* bytes) {
    for (const auto& pat : kPrologues) {
        bool match = true;
        for (int i = 0; i < 5; ++i) {
            if ((bytes[i] & pat.mask[i]) != (pat.bytes[i] & pat.mask[i])) {
                match = false;
                break;
            }
        }
        if (match) return pat.name;
    }
    return nullptr;
}

// Encode a 32-bit relative jump: E9 <rel32>.
void WriteJmpRel32(uint8_t* from, const void* to) {
    from[0] = 0xE9;
    auto disp = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(to) -
        (reinterpret_cast<uintptr_t>(from) + 5));
    std::memcpy(from + 1, &disp, 4);
}

}  // namespace

bool InstallHook(void* target, void* detour, void** trampoline_out) {
    if (!target || !detour || !trampoline_out) {
        Log("[hook] null argument");
        return false;
    }

    auto* code = static_cast<uint8_t*>(target);

    // Validate prologue.
    const char* prologue_name = MatchPrologue(code);
    if (!prologue_name) {
        Log("[hook] unknown prologue at %p: %02X %02X %02X %02X %02X",
            target, code[0], code[1], code[2], code[3], code[4]);
        return false;
    }

    // Allocate trampoline: 5 saved bytes + 5-byte jmp back = 10 bytes.
    // Allocate 16 for cache-line alignment.
    void* trampoline = VirtualAlloc(
        nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        Log("[hook] VirtualAlloc failed: %lu", GetLastError());
        return false;
    }

    auto* tramp = static_cast<uint8_t*>(trampoline);

    // Copy original prologue into trampoline, then jump to target+5.
    std::memcpy(tramp, code, 5);
    WriteJmpRel32(tramp + 5, code + 5);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 16);

    // Publish trampoline BEFORE patching (see ao-crashfix for rationale).
    *trampoline_out = trampoline;

    // Patch target with jmp to detour.
    DWORD old_protect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("[hook] VirtualProtect failed: %lu", GetLastError());
        *trampoline_out = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    WriteJmpRel32(code, detour);

    DWORD ignored = 0;
    VirtualProtect(target, 5, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    Log("[hook] installed at %p (%s), trampoline=%p, detour=%p",
        target, prologue_name, trampoline, detour);
    return true;
}

void* ResolveRVA(const char* module_name, uint32_t rva) {
    HMODULE mod = GetModuleHandleA(module_name);
    if (!mod) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva);
}

}  // namespace aor
