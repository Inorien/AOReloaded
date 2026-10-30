// laa_patch.cpp — LargeAddressAware PE header patching.
//
// Strategy: the running exe is locked for writes by the OS loader. We work
// around this with rename-copy-patch:
//
//   1. Clean up any .bak left from a previous run.
//   2. Read the exe's PE header (read access is allowed on a running exe).
//   3. If IMAGE_FILE_LARGE_ADDRESS_AWARE is already set, we're done.
//   4. Rename the running exe to .bak (Windows allows renaming a running exe).
//   5. Copy .bak to the original filename (creates a new, unlocked file).
//   6. Open the new copy for writing and flip the LAA bit.
//   7. The .bak is still locked (it's the running image), so it gets cleaned
//      up on the next launch (step 1).
//
// Before patching, the untouched exe is kept as <name>_orig.exe (once): the
// launcher's file check (checksums.chk) fails on the patched exe, and AOPlugins
// hands that check the _orig instead.
//
// After patching, the flag persists permanently — no per-launch work needed.

#include "laa_patch.h"
#include "logging.h"

#include <windows.h>

namespace aor {
namespace {

// Read the COFF Characteristics field from a PE file on disk.
// Returns false if the file can't be opened or the headers are invalid.
bool ReadCharacteristics(const wchar_t* path, WORD& outCharacteristics,
                         DWORD& outCharacteristicsOffset) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    // Read DOS header to find the PE signature offset.
    IMAGE_DOS_HEADER dos = {};
    DWORD bytesRead = 0;
    if (!ReadFile(file, &dos, sizeof(dos), &bytesRead, nullptr) ||
        bytesRead != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(file);
        return false;
    }

    // Seek to the PE signature.
    if (SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) ==
        INVALID_SET_FILE_POINTER) {
        CloseHandle(file);
        return false;
    }

    // Read PE signature + COFF file header.
    DWORD peSignature = 0;
    if (!ReadFile(file, &peSignature, sizeof(peSignature), &bytesRead, nullptr) ||
        bytesRead != sizeof(peSignature) || peSignature != IMAGE_NT_SIGNATURE) {
        CloseHandle(file);
        return false;
    }

    IMAGE_FILE_HEADER fileHeader = {};
    if (!ReadFile(file, &fileHeader, sizeof(fileHeader), &bytesRead, nullptr) ||
        bytesRead != sizeof(fileHeader)) {
        CloseHandle(file);
        return false;
    }

    outCharacteristics = fileHeader.Characteristics;

    // The Characteristics field is at a fixed offset in the PE layout:
    //   e_lfanew + 4 (PE signature) + 18 (Characteristics in IMAGE_FILE_HEADER)
    // This is defined by the PE spec and will never change.
    constexpr DWORD kCharacteristicsOffsetInCoff = 18;
    outCharacteristicsOffset = static_cast<DWORD>(
        dos.e_lfanew + sizeof(DWORD) + kCharacteristicsOffsetInCoff);

    CloseHandle(file);
    return true;
}

// Write a WORD at the given file offset. The file must not be locked.
bool WriteCharacteristicsAt(const wchar_t* path, DWORD offset, WORD value) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    if (SetFilePointer(file, offset, nullptr, FILE_BEGIN) ==
        INVALID_SET_FILE_POINTER) {
        CloseHandle(file);
        return false;
    }

    DWORD bytesWritten = 0;
    bool ok = WriteFile(file, &value, sizeof(value), &bytesWritten, nullptr) &&
              bytesWritten == sizeof(value);

    CloseHandle(file);
    return ok;
}

}  // namespace

void PatchLargeAddressAware() {
    // Get the path to the running executable.
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        Log("[laa] failed to get exe path");
        return;
    }

    // Build the .bak path.
    wchar_t bakPath[MAX_PATH] = {};
    wcscpy_s(bakPath, exePath);
    wcscat_s(bakPath, L".bak");

    // Step 1: Clean up .bak from a previous run.
    // DeleteFile fails silently if the file doesn't exist or is still locked
    // (e.g. if the user relaunched very quickly). Either way, not fatal.
    DeleteFileW(bakPath);

    // Step 2: Read the current PE header.
    WORD characteristics = 0;
    DWORD characteristicsOffset = 0;
    if (!ReadCharacteristics(exePath, characteristics, characteristicsOffset)) {
        Log("[laa] failed to read PE header from exe");
        return;
    }

    // Step 3: Already patched?
    if (characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) {
        Log("[laa] LargeAddressAware already set");
        return;
    }

    Log("[laa] LargeAddressAware not set — patching exe on disk...");

    // Keep the original for the launcher's file check: AnarchyOnline_orig.exe,
    // unless one is there already (never overwrite an original with a copy).
    wchar_t origPath[MAX_PATH] = {};
    wcscpy_s(origPath, exePath);
    wchar_t* dot = wcsrchr(origPath, L'.');
    if (dot && wcsrchr(origPath, L'\\') < dot && len + 5 < MAX_PATH) {
        wchar_t ext[16] = {};
        wcscpy_s(ext, dot);
        *dot = L'\0';
        wcscat_s(origPath, L"_orig");
        wcscat_s(origPath, ext);
        if (CopyFileW(exePath, origPath, TRUE))
            Log("[laa] original kept as %ls", origPath);
        else if (GetLastError() != ERROR_FILE_EXISTS)
            Log("[laa] could not keep the original as %ls (error %lu)", origPath, GetLastError());
    }

    // Step 4: Rename the running exe to .bak.
    if (!MoveFileW(exePath, bakPath)) {
        Log("[laa] failed to rename exe to .bak (error %lu)", GetLastError());
        return;
    }

    // Step 5: Copy .bak to original path (new file, not locked).
    if (!CopyFileW(bakPath, exePath, FALSE)) {
        Log("[laa] failed to copy .bak to exe (error %lu) — restoring...",
            GetLastError());
        // Try to restore the original name so the game isn't broken.
        MoveFileW(bakPath, exePath);
        return;
    }

    // Step 6: Patch the new copy.
    WORD patched = characteristics | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    if (!WriteCharacteristicsAt(exePath, characteristicsOffset, patched)) {
        Log("[laa] failed to write patched header (error %lu)", GetLastError());
        // The copy exists but isn't patched. Not ideal, but also not harmful —
        // the exe is still functional, just without LAA. Next launch will retry.
        return;
    }

    Log("[laa] LargeAddressAware enabled — will take effect on next restart");
}

}  // namespace aor
