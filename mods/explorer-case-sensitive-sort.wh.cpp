// ==WindhawkMod==
// @id              explorer-case-sensitive-sort
// @name            Explorer Case-Sensitive Name Sort
// @description     Sorts File Explorer item names case-sensitively (uppercase before lowercase)
// @version         0.1
// @author          Alchemy
// @github          https://github.com/alchemyyy
// @include         explorer.exe
// @architecture    x86-64
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Case-Sensitive Name Sort

Makes File Explorer sort item names case-sensitively. Names are compared by
character code (ASCII order), so uppercase sorts before lowercase (`Banana`
before `apple`). Numbers inside names are still compared by value (`item2`
before `item10`), like `LC_ALL=C ls -v`.

Already-open folders re-sort on refresh (F5) or when a column header is clicked.

Not covered: the `NoStrCmpLogical` policy, and views whose order comes from the
search indexer.
*/
// ==/WindhawkModReadme==

#include <windows.h>

#include <atomic>

#define WINDOWS_STORAGE_MODULE_NAME L"windows.storage.dll"

using StrCmpLogicalW_t = int(WINAPI*)(PCWSTR, PCWSTR);
static StrCmpLogicalW_t s_strCmpLogicalWOriginal;

static std::atomic<ULONG_PTR> s_windowsStorageStart;
static std::atomic<ULONG_PTR> s_windowsStorageEnd;

// Caches the windows.storage.dll image range, returns false until the module is loaded
static bool ResolveWindowsStorageRange() {
    if (s_windowsStorageStart.load(std::memory_order_acquire)) {
        return true;
    }

    HMODULE moduleHandle = GetModuleHandleW(WINDOWS_STORAGE_MODULE_NAME);
    if (!moduleHandle) {
        return false;
    }

    const BYTE* imageBase = reinterpret_cast<const BYTE*>(moduleHandle);
    const IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    const IMAGE_NT_HEADERS* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dosHeader->e_lfanew);
    ULONG_PTR imageStart = reinterpret_cast<ULONG_PTR>(imageBase);

    s_windowsStorageEnd.store(imageStart + ntHeaders->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
    s_windowsStorageStart.store(imageStart, std::memory_order_release);
    return true;
}

static bool IsWindowsStorageAddress(const void* address) {
    if (!ResolveWindowsStorageRange()) {
        return false;
    }

    ULONG_PTR value = reinterpret_cast<ULONG_PTR>(address);
    return value >= s_windowsStorageStart.load(std::memory_order_acquire) &&
           value < s_windowsStorageEnd.load(std::memory_order_relaxed);
}

static bool IsAsciiDigit(WCHAR character) {
    return character >= L'0' && character <= L'9';
}

static int CompareOrdinal(PCWSTR left, PCWSTR right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }

    if (*left == *right) {
        return 0;
    }
    return *left < *right ? -1 : 1;
}

// Logical compare like StrCmpLogicalW, but case-sensitive
// Digit runs compare by numeric value, all other characters compare by UTF-16 code unit
static int CompareLogicalCaseSensitive(PCWSTR left, PCWSTR right) {
    PCWSTR leftCursor = left;
    PCWSTR rightCursor = right;

    while (*leftCursor && *rightCursor) {
        if (!IsAsciiDigit(*leftCursor) || !IsAsciiDigit(*rightCursor)) {
            if (*leftCursor != *rightCursor) {
                return *leftCursor < *rightCursor ? -1 : 1;
            }
            leftCursor++;
            rightCursor++;
            continue;
        }

        // Numeric run: skip leading zeros, then longer run wins, then first differing digit wins
        while (*leftCursor == L'0') {
            leftCursor++;
        }
        while (*rightCursor == L'0') {
            rightCursor++;
        }

        PCWSTR leftRunEnd = leftCursor;
        while (IsAsciiDigit(*leftRunEnd)) {
            leftRunEnd++;
        }
        PCWSTR rightRunEnd = rightCursor;
        while (IsAsciiDigit(*rightRunEnd)) {
            rightRunEnd++;
        }

        ptrdiff_t leftRunLength = leftRunEnd - leftCursor;
        ptrdiff_t rightRunLength = rightRunEnd - rightCursor;
        if (leftRunLength != rightRunLength) {
            return leftRunLength < rightRunLength ? -1 : 1;
        }

        for (ptrdiff_t digitIndex = 0; digitIndex < leftRunLength; digitIndex++) {
            if (leftCursor[digitIndex] != rightCursor[digitIndex]) {
                return leftCursor[digitIndex] < rightCursor[digitIndex] ? -1 : 1;
            }
        }

        leftCursor = leftRunEnd;
        rightCursor = rightRunEnd;
    }

    if (*leftCursor || *rightCursor) {
        return *leftCursor ? 1 : -1;
    }

    // Equal apart from leading zeros, keep the order total
    return CompareOrdinal(left, right);
}

// All four StrCmpLogicalW call sites in windows.storage.dll (build 26100) compare item names:
// CFSFolder::CompareIDs, DVCompareColumns, StrCmpLogicalRestricted, CEnumFiles::CFileInformationClassSorter::Compare
// Other callers, such as PropVariantCompareEx for non-name columns, keep the original comparison
static int WINAPI StrCmpLogicalW_Hook(PCWSTR left, PCWSTR right) {
    void* returnAddress = __builtin_return_address(0);
    if (!left || !right || !IsWindowsStorageAddress(returnAddress)) {
        return s_strCmpLogicalWOriginal(left, right);
    }

    return CompareLogicalCaseSensitive(left, right);
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    HMODULE kernelBaseModule = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelBaseModule) {
        Wh_Log(L"kernelbase.dll not loaded");
        return FALSE;
    }

    void* strCmpLogicalW = reinterpret_cast<void*>(GetProcAddress(kernelBaseModule, "StrCmpLogicalW"));
    if (!strCmpLogicalW) {
        Wh_Log(L"StrCmpLogicalW not found");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(strCmpLogicalW, reinterpret_cast<void*>(StrCmpLogicalW_Hook),
                            reinterpret_cast<void**>(&s_strCmpLogicalWOriginal))) {
        Wh_Log(L"Hooking StrCmpLogicalW failed");
        return FALSE;
    }

    Wh_Log(L"windows.storage.dll range resolved: %d", ResolveWindowsStorageRange());
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}
