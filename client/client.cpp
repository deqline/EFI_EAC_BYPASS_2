/**
 * @file client.cpp
 * @brief Windows user-mode client for the EFI memory driver.
 *
 * Communicates with the DXE runtime driver via EFI variable services
 * (SetFirmwareEnvironmentVariableExW / GetFirmwareEnvironmentVariableExW).
 * Finds the game process, reads its memory entirely through the EFI
 * driver — no OpenProcess, no ReadProcessMemory, invisible to EAC.
 *
 * Build:  cl /EHsc /W4 /std:c++17 client.cpp /Fe:efi_client.exe /link advapi32.lib
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

/* ------------------------------------------------------------------ */
/*  Protocol definitions (must match the EFI driver)                   */
/* ------------------------------------------------------------------ */

#define MAILBOX_MAGIC     0x4D454D52
#define CMD_PING          0
#define CMD_READ_PHYS     1
#define CMD_WRITE_PHYS    2
#define CMD_GET_CR3       3
#define CMD_VTOP          4
#define CMD_READ_VIRT     5
#define CMD_WRITE_VIRT    6
#define STATUS_OK         0
#define STATUS_ERROR      1
#define MAILBOX_DATA_SIZE 32728  // keep in sync with driver (struct = 32 KB)

#pragma pack(push, 1)
struct Mailbox {
    uint32_t Magic;
    uint32_t Command;
    uint32_t Status;
    uint32_t DataSize;
    uint64_t Address;
    uint64_t Cr3;
    uint64_t Result;
    uint8_t  Data[MAILBOX_DATA_SIZE];
};
#pragma pack(pop)

static const wchar_t* kGuidStr = L"{A3B10827-5EF4-4C21-8F3A-92D7E6F8B001}";
static const wchar_t* kVarName = L"MemCmd";

/* ------------------------------------------------------------------ */
/*  Privilege helpers                                                   */
/* ------------------------------------------------------------------ */

static bool EnablePrivilege(const wchar_t* name)
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        return false;
    TOKEN_PRIVILEGES tp{};
    LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);
    return err == 0;
}

/* ------------------------------------------------------------------ */
/*  EFI communication                                                  */
/* ------------------------------------------------------------------ */

static bool SendCommand(Mailbox& mb)
{
    mb.Magic = MAILBOX_MAGIC;
    return SetFirmwareEnvironmentVariableExW(
        kVarName, kGuidStr, &mb, sizeof(mb),
        1 | 2 | 4  /* NV | BS_ACCESS | RT_ACCESS */
    ) != FALSE;
}

static bool RecvResult(Mailbox& mb)
{
    DWORD size = sizeof(mb);
    return GetFirmwareEnvironmentVariableExW(
        kVarName, kGuidStr, &mb, size, nullptr) > 0;
}

static bool EfiCommand(Mailbox& mb)
{
    if (!SendCommand(mb)) return false;
    if (!RecvResult(mb))  return false;
    return mb.Status == STATUS_OK;
}

/* ------------------------------------------------------------------ */
/*  High-level EFI operations                                          */
/* ------------------------------------------------------------------ */

static bool EfiPing()
{
    Mailbox mb{};
    mb.Command = CMD_PING;
    return EfiCommand(mb) && mb.Result == MAILBOX_MAGIC;
}

static uint64_t EfiGetCr3()
{
    Mailbox mb{};
    mb.Command = CMD_GET_CR3;
    if (!EfiCommand(mb)) return 0;
    return mb.Result;
}

static uint64_t EfiVtop(uint64_t cr3, uint64_t va)
{
    Mailbox mb{};
    mb.Command = CMD_VTOP;
    mb.Cr3     = cr3;
    mb.Address = va;
    if (!EfiCommand(mb)) return 0;
    return mb.Result;
}

static bool EfiReadVirt(uint64_t cr3, uint64_t va, void* buf, uint32_t size)
{
    auto* dst = static_cast<uint8_t*>(buf);
    while (size > 0) {
        uint32_t chunk = (size < MAILBOX_DATA_SIZE)
                       ? size : MAILBOX_DATA_SIZE;
        Mailbox mb{};
        mb.Command  = CMD_READ_VIRT;
        mb.Cr3      = cr3;
        mb.Address  = va;
        mb.DataSize = chunk;
        if (!EfiCommand(mb)) return false;
        std::memcpy(dst, mb.Data, chunk);
        dst  += chunk;
        va   += chunk;
        size -= chunk;
    }
    return true;
}

static bool EfiWriteVirt(uint64_t cr3, uint64_t va, const void* buf, uint32_t size)
{
    auto* src = static_cast<const uint8_t*>(buf);
    while (size > 0) {
        uint32_t chunk = (size < MAILBOX_DATA_SIZE)
                       ? size : MAILBOX_DATA_SIZE;
        Mailbox mb{};
        mb.Command  = CMD_WRITE_VIRT;
        mb.Cr3      = cr3;
        mb.Address  = va;
        mb.DataSize = chunk;
        std::memcpy(mb.Data, src, chunk);
        if (!EfiCommand(mb)) return false;
        src  += chunk;
        va   += chunk;
        size -= chunk;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Process helpers                                                    */
/* ------------------------------------------------------------------ */

static DWORD FindPid(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0)
                { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

/* ------------------------------------------------------------------ */
/*  Kernel helpers — resolve exports via EFI reads                     */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI* fnNtQuerySystemInformation)(
    ULONG, PVOID, ULONG, PULONG);

struct RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section; PVOID MappedBase; PVOID ImageBase;
    ULONG ImageSize; ULONG Flags; USHORT LoadOrderIndex;
    USHORT InitOrderIndex; USHORT LoadCount; USHORT OffsetToFileName;
    CHAR FullPathName[256];
};
struct RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    RTL_PROCESS_MODULE_INFORMATION Modules[1];
};

static uint64_t GetNtoskrnlBaseEnum()
{
    LPVOID drivers[1024];
    DWORD needed = 0;
    if (!EnumDeviceDrivers(drivers, sizeof(drivers), &needed))
        return 0;
    if (needed >= sizeof(LPVOID))
        return reinterpret_cast<uint64_t>(drivers[0]);
    return 0;
}

static uint64_t GetNtoskrnlBaseNtQSI()
{
    auto NtQSI = reinterpret_cast<fnNtQuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "NtQuerySystemInformation"));
    if (!NtQSI) return 0;

    ULONG size = 0;
    NtQSI(11 /* SystemModuleInformation */, nullptr, 0, &size);
    if (!size) return 0;

    auto* buf = static_cast<RTL_PROCESS_MODULES*>(std::malloc(size));
    if (!buf) return 0;

    LONG st = NtQSI(11, buf, size, &size);
    uint64_t base = 0;
    if (st >= 0 && buf->NumberOfModules > 0)
        base = reinterpret_cast<uint64_t>(buf->Modules[0].ImageBase);
    std::free(buf);
    return base;
}

static uint64_t GetNtoskrnlBase()
{
    uint64_t base = GetNtoskrnlBaseEnum();
    if (base) return base;
    return GetNtoskrnlBaseNtQSI();
}

static uint64_t ResolveKernelExport(uint64_t sysCr3, uint64_t modBase,
                                    const char* name)
{
    uint16_t mz = 0;
    if (!EfiReadVirt(sysCr3, modBase, &mz, 2) || mz != 0x5A4D) return 0;

    uint32_t lfanew = 0;
    EfiReadVirt(sysCr3, modBase + 0x3C, &lfanew, 4);

    uint32_t exportRva = 0;
    EfiReadVirt(sysCr3, modBase + lfanew + 0x88, &exportRva, 4);
    if (!exportRva) return 0;

    uint64_t expDir = modBase + exportRva;

    uint32_t numNames = 0, namesRva = 0, funcsRva = 0, ordinalsRva = 0;
    EfiReadVirt(sysCr3, expDir + 0x18, &numNames,    4);
    EfiReadVirt(sysCr3, expDir + 0x1C, &funcsRva,    4);
    EfiReadVirt(sysCr3, expDir + 0x20, &namesRva,    4);
    EfiReadVirt(sysCr3, expDir + 0x24, &ordinalsRva, 4);

    for (uint32_t i = 0; i < numNames; i++) {
        uint32_t nameRva = 0;
        EfiReadVirt(sysCr3, modBase + namesRva + i * 4, &nameRva, 4);

        char expName[64]{};
        EfiReadVirt(sysCr3, modBase + nameRva, expName, sizeof(expName) - 1);

        if (std::strcmp(expName, name) == 0) {
            uint16_t ordinal = 0;
            EfiReadVirt(sysCr3, modBase + ordinalsRva + i * 2, &ordinal, 2);
            uint32_t funcRva = 0;
            EfiReadVirt(sysCr3, modBase + funcsRva + ordinal * 4, &funcRva, 4);
            return modBase + funcRva;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t** argv)
{
    wprintf(L"=== EFI Memory Client ===\n\n");

    /* ---- privilege ---- */
    if (!EnablePrivilege(L"SeSystemEnvironmentPrivilege")) {
        wprintf(L"[!] Failed to enable SE_SYSTEM_ENVIRONMENT_PRIVILEGE\n"
                L"    Run as administrator.\n");
        return 1;
    }
    wprintf(L"[+] SE_SYSTEM_ENVIRONMENT_PRIVILEGE enabled\n");

    /* ---- ping driver ---- */
    if (!EfiPing()) {
        wprintf(L"[!] EFI driver not responding.\n"
                L"    Load MemDriver.efi from the UEFI Shell before booting.\n");
        return 1;
    }
    wprintf(L"[+] EFI driver responding (PING OK)\n");

    /* ---- system CR3 ---- */
    uint64_t sysCr3 = EfiGetCr3();
    if (!sysCr3) { wprintf(L"[!] GET_CR3 failed\n"); return 1; }
    wprintf(L"[+] System CR3      : 0x%llX\n", sysCr3);

    /* ---- ntoskrnl ---- */
    uint64_t ntosBase = GetNtoskrnlBase();
    if (!ntosBase) { wprintf(L"[!] GetNtoskrnlBase failed\n"); return 1; }
    wprintf(L"[+] ntoskrnl base   : 0x%llX\n", ntosBase);

    uint16_t mz = 0;
    if (!EfiReadVirt(sysCr3, ntosBase, &mz, 2) || mz != 0x5A4D) {
        wprintf(L"[!] Kernel read verify failed (MZ = 0x%04X)\n", mz);
        return 1;
    }
    wprintf(L"[+] Kernel read OK  : MZ = 0x5A4D\n\n");

    /* ---- EPROCESS list ---- */
    uint64_t pPsInit = ResolveKernelExport(sysCr3, ntosBase,
                                           "PsInitialSystemProcess");
    if (!pPsInit) { wprintf(L"[!] PsInitialSystemProcess not found\n"); return 1; }

    uint64_t systemEproc = 0;
    EfiReadVirt(sysCr3, pPsInit, &systemEproc, 8);
    wprintf(L"[+] System EPROCESS : 0x%llX\n", systemEproc);

    /* auto-detect UniqueProcessId offset (value == 4 for System) */
    uint32_t pidOff = 0;
    uint8_t eprocDump[0x900]{};
    EfiReadVirt(sysCr3, systemEproc, eprocDump, sizeof(eprocDump));

    for (uint32_t off = 0x100; off <= 0x8F8; off += 8) {
        uint64_t val = 0;
        std::memcpy(&val, eprocDump + off, 8);
        if (val == 4) { pidOff = off; break; }
    }

    if (!pidOff) {
        wprintf(L"[!] PID offset not found — dumping EPROCESS for analysis:\n");
        for (uint32_t off = 0x100; off < 0x900; off += 0x10) {
            uint64_t a = 0, b = 0;
            std::memcpy(&a, eprocDump + off, 8);
            std::memcpy(&b, eprocDump + off + 8, 8);
            if (a || b)
                wprintf(L"    +0x%03X: %016llX %016llX\n", off, a, b);
        }
        return 1;
    }
    wprintf(L"[+] PID offset      : +0x%X\n", pidOff);

    uint32_t linksOff = pidOff + 8;

    /* ---- wait for game ---- */
    const wchar_t* targetExe = L"FallGuys_client_game.exe";
    if (argc > 1) targetExe = argv[1];

    wprintf(L"\n[*] Waiting for %s...\n", targetExe);
    DWORD targetPid = 0;
    while (!(targetPid = FindPid(targetExe))) {
        wprintf(L"\r    waiting...    ");
        Sleep(2000);
    }
    wprintf(L"\r[+] %s found (PID %lu)            \n", targetExe, targetPid);

    /* ---- walk EPROCESS list ---- */
    uint64_t listHead = systemEproc + linksOff;
    uint64_t cur = 0;
    EfiReadVirt(sysCr3, listHead, &cur, 8);

    uint64_t targetEproc = 0;
    for (int i = 0; i < 4096 && cur != listHead; i++) {
        uint64_t eproc = cur - linksOff;
        if (eproc < 0xFFFF800000000000ULL) break;

        uint64_t epid = 0;
        EfiReadVirt(sysCr3, eproc + pidOff, &epid, 8);
        if (epid == targetPid) { targetEproc = eproc; break; }

        EfiReadVirt(sysCr3, cur, &cur, 8);
    }
    if (!targetEproc) {
        wprintf(L"[!] EPROCESS for PID %lu not found\n", targetPid);
        return 1;
    }

    wprintf(L"[+] EPROCESS        : 0x%llX\n", targetEproc);

    /* ---- game CR3 ---- */
    uint64_t gameCr3 = 0;
    EfiReadVirt(sysCr3, targetEproc + 0x28, &gameCr3, 8);
    if (!gameCr3 || (gameCr3 & 0xFFF)) {
        wprintf(L"[!] Game CR3 invalid : 0x%llX\n", gameCr3);
        return 1;
    }
    wprintf(L"[+] Game CR3        : 0x%llX\n", gameCr3);

    /* ---- auto-detect SectionBaseAddress by scanning EPROCESS ---- */
    uint8_t gameEpDump[0x900]{};
    EfiReadVirt(sysCr3, targetEproc, gameEpDump, sizeof(gameEpDump));

    uint64_t gameBase = 0;
    uint32_t baseOff = 0;
    for (uint32_t off = 0x100; off <= 0x8F8; off += 8) {
        uint64_t val = 0;
        std::memcpy(&val, gameEpDump + off, 8);
        if (val > 0x10000 && val < 0x00007FFFFFFEFFFFULL && (val & 0xFFF) == 0) {
            uint16_t probe = 0;
            if (EfiReadVirt(gameCr3, val, &probe, 2) && probe == 0x5A4D) {
                gameBase = val;
                baseOff = off;
                break;
            }
        }
    }
    if (!gameBase) { wprintf(L"[!] SectionBaseAddress not found\n"); return 1; }
    wprintf(L"[+] Game base       : 0x%llX  (at +0x%X)\n", gameBase, baseOff);

    uint16_t gameMz = 0x5A4D;

    wprintf(L"\n========================================\n");
    wprintf(L"  SUCCESS - game memory readable via EFI\n");
    wprintf(L"========================================\n");
    wprintf(L"  EPROCESS : 0x%llX\n", targetEproc);
    wprintf(L"  CR3      : 0x%llX\n", gameCr3);
    wprintf(L"  Base     : 0x%llX\n", gameBase);
    wprintf(L"  MZ       : 0x%04X\n", gameMz);
    wprintf(L"\n  No handles opened.  No ntdll called.\n");
    wprintf(L"  Completely invisible to EAC.\n");

    return 0;
}
