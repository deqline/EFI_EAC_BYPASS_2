/**
 * @file MemDriver.c
 * @brief UEFI DXE runtime driver — per-process scoped PML4 install.
 *
 * Protocol (over hooked GetVariable / SetVariable) is bit-for-bit identical
 * to the original MemDriverPkg, so the same Windows client works.
 *
 * How the "per-process" fast path works
 * -------------------------------------
 * Instead of planting one identity-mapping PDPT entry in the OS's live PML4
 * at boot (version 2 of this driver), each mailbox command carries a target
 * CR3.  On first contact with a new CR3, the driver walks THAT process's
 * PML4, finds a free slot, writes our PDPT there, and caches the install.
 *
 * All subsequent memory operations for that CR3 execute inside a brief
 * CR3-swap window:
 *
 *     cli
 *     saved = cr3                       ; save the OS CR3
 *     cr3   = targetCr3                 ; load the process whose PML4 we patched
 *     memcpy via (identBase + pa)       ; identity window is live in that CR3
 *     cr3   = saved
 *     sti
 *
 * The install is persistent only in the TARGET PROCESS's own PML4 — never
 * in the system PML4.  PatchGuard scans the system PML4 and parts of KPCR,
 * not per-process page tables, so the fingerprint is out of its scope.
 *
 * Costs compared to version 2 (system PML4 install):
 *   + One CR3 swap per mailbox command (not per 8-byte access as in v1)
 *   + Install cost scales with the number of distinct CR3s touched
 *   - No persistent change to any kernel-wide structure
 *   - Fingerprint lives in the target game's own PML4 only
 */

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Protocol/LoadedImage.h>
#include <Guid/EventGroup.h>

/* ------------------------------------------------------------------ */
/*  Protocol — must stay identical to the original client               */
/* ------------------------------------------------------------------ */

#define MAILBOX_MAGIC     0x4D454D52  /* "MEMR" */
#define CMD_PING          0
#define CMD_READ_PHYS     1
#define CMD_WRITE_PHYS    2
#define CMD_GET_CR3       3
#define CMD_VTOP          4
#define CMD_READ_VIRT     5
#define CMD_WRITE_VIRT    6
#define STATUS_OK         0
#define STATUS_ERROR      1
#define MAILBOX_DATA_SIZE 32728   /* struct totals 32 KB */

#pragma pack(push, 1)
typedef struct {
    UINT32 Magic;
    UINT32 Command;
    UINT32 Status;
    UINT32 DataSize;
    UINT64 Address;
    UINT64 Cr3;
    UINT64 Result;
    UINT8  Data[MAILBOX_DATA_SIZE];
} MAILBOX;  /* sizeof == 1024 */
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  Page-table constants                                               */
/* ------------------------------------------------------------------ */

#define PTE_PRESENT   (1ULL << 0)
#define PTE_RW        (1ULL << 1)
#define PTE_US        (1ULL << 2)
#define PTE_PS        (1ULL << 7)
#define PTE_NX        (1ULL << 63)
#define TABLE_FLAGS   (PTE_PRESENT | PTE_RW)
#define LARGE_FLAGS   (PTE_PRESENT | PTE_RW | PTE_PS)

#define PAGE_4K       0x1000ULL
#define IDENT_MAP_GB  64
#define MAX_PHYS      ((UINT64)IDENT_MAP_GB << 30)

#define MAX_INSTALLS  8

static EFI_GUID gMailboxGuid = {
    0xA3B10827, 0x5EF4, 0x4C21,
    { 0x8F, 0x3A, 0x92, 0xD7, 0xE6, 0xF8, 0xB0, 0x01 }
};

/* ------------------------------------------------------------------ */
/*  Assembly stubs                                                     */
/* ------------------------------------------------------------------ */

/* Used both for the bootstrap install writes AND for one-shot physical
 * reads when the client did not supply a CR3. */
extern UINT64 AsmReadPhys8    (UINT64 PhysAddr, UINT64 OurCr3);
extern void   AsmWritePhys8   (UINT64 PhysAddr, UINT64 Value, UINT64 OurCr3);
extern UINT64 AsmGetCr3       (void);
extern void   AsmReloadCr3    (void);

/* Hot-path CR3 swap: load newCr3, return old CR3; and restore. These keep
 * interrupts disabled across the swap window, which the caller enforces
 * by pairing them in the same C function. */
extern UINT64 AsmSwapCr3Enter (UINT64 NewCr3);   /* cli; save; load; ret   */
extern void   AsmSwapCr3Exit  (UINT64 SavedCr3); /* restore; sti; ret      */

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

static EFI_GET_VARIABLE  gOrigGetVariable;
static EFI_SET_VARIABLE  gOrigSetVariable;

/* Bootstrap identity-mapping PML4 — used:
 *   - as the CR3 for the one-shot swap stubs when we need to read/write
 *     a specific physical page (walking a PML4 to install our entry,
 *     servicing CMD_READ_PHYS without a cached CR3, etc.)
 * It is NEVER left visible to the OS; we only load it briefly inside
 * AsmReadPhys8 / AsmWritePhys8. */
static UINT64 *gBootPml4;
static UINT64  gBootPml4Phys;

/* The PDPT we plant in each target process's PML4. One PDPT, reused for
 * every install — identity-maps the low 64 GB. */
static UINT64 *gPdpt;
static UINT64  gPdptPhys;

/* 64 PDs, one per GB, each full of 2 MB large pages */
static UINT64 *gPd[IDENT_MAP_GB];

/* Spare page-table pages so the driver image is reachable while
 * gBootPml4 is loaded during the bootstrap swap stubs. */
static UINT64 *gRtPdpt;   static UINT64 gRtPdptPhys;
static UINT64 *gRtPd;     static UINT64 gRtPdPhys;
static UINT64 *gRtPt;     static UINT64 gRtPtPhys;

static VOID   *gImageBase;
static UINT64  gImagePhysBase;
static UINTN   gImagePages;

/* Per-process install cache. Each entry records a CR3 we have planted
 * our PDPT in and the identity-window base VA that resulted. */
typedef struct {
    BOOLEAN valid;
    UINT64  cr3;        /* page-aligned */
    UINTN   slotIdx;    /* which PML4 slot (256..510) */
    UINT64  identBase;  /* canonical VA where PA 0 appears under this CR3 */
    UINTN   lruTick;
} PER_PROC_INSTALL;

static PER_PROC_INSTALL gInstalls[MAX_INSTALLS];
static UINTN            gLruCounter;

/* The identity-window base of whichever install is currently "active"
 * (i.e. whose CR3 is loaded in CR3). FastReadPhys8 and VirtToPhys read
 * this at every access. Only meaningful inside a swap window. */
static volatile UINT64  gActiveIdentBase;

static MAILBOX gMailbox;
static BOOLEAN gMailboxReady;

/* ------------------------------------------------------------------ */
/*  Bootstrap physical memory R/W (CR3 swap — one-shot, no persistence) */
/* ------------------------------------------------------------------ */

static UINT64 BootReadPhys8(UINT64 pa)              { return AsmReadPhys8(pa, gBootPml4Phys); }
static void   BootWritePhys8(UINT64 pa, UINT64 val) { AsmWritePhys8(pa, val, gBootPml4Phys);  }

static void BootReadPhysBuf(UINT64 pa, VOID *buf, UINTN size)
{
    UINT8 *dst = (UINT8 *)buf;
    UINTN i;
    for (i = 0; i + 8 <= size; i += 8) {
        UINT64 v = BootReadPhys8(pa + i);
        CopyMem(dst + i, &v, 8);
    }
    if (i < size) {
        UINT64 v = BootReadPhys8(pa + i);
        CopyMem(dst + i, &v, size - i);
    }
}

static void BootWritePhysBuf(UINT64 pa, VOID *buf, UINTN size)
{
    UINT8 *src = (UINT8 *)buf;
    UINTN i;
    for (i = 0; i + 8 <= size; i += 8) {
        UINT64 v;
        CopyMem(&v, src + i, 8);
        BootWritePhys8(pa + i, v);
    }
    if (i < size) {
        UINT64 v = BootReadPhys8(pa + i);
        CopyMem(&v, src + i, size - i);
        BootWritePhys8(pa + i, v);
    }
}

/* ------------------------------------------------------------------ */
/*  Canonical VA helper                                                */
/* ------------------------------------------------------------------ */

static UINT64 CanonicalVa(UINTN pml4Index)
{
    UINT64 va = (UINT64)pml4Index << 39;
    if (va & (1ULL << 47)) va |= 0xFFFF000000000000ULL;
    return va;
}

/* ------------------------------------------------------------------ */
/*  Per-process install cache                                          */
/* ------------------------------------------------------------------ */

static PER_PROC_INSTALL *FindInstall(UINT64 cr3)
{
    cr3 &= ~0xFFFULL;
    for (UINTN i = 0; i < MAX_INSTALLS; i++) {
        if (gInstalls[i].valid && gInstalls[i].cr3 == cr3)
            return &gInstalls[i];
    }
    return NULL;
}

/* Return TRUE if the slot still contains our PDPT entry (i.e. nothing
 * has cleared or overwritten it since we installed).
 *
 * We MUST NOT do a byte-for-byte compare: the CPU updates the A
 * (Accessed, bit 5) bit on every walk and may update D (Dirty, bit 6)
 * if the walker writes.  Checking all 64 bits would make the entry
 * look "stale" on literally every call after the first, and we would
 * burn through free PML4 slots at one per command.  Compare only the
 * stable parts: the physical-address field and the PRESENT bit.
 */
static BOOLEAN ValidateInstall(PER_PROC_INSTALL *inst)
{
    UINT64 live = BootReadPhys8(inst->cr3 + inst->slotIdx * 8);
    if (!(live & PTE_PRESENT)) return FALSE;
    const UINT64 paMask = 0x000FFFFFFFFFF000ULL;
    return (live & paMask) == (gPdptPhys & paMask);
}

/* Scan slots 510..256 top-down looking for a zero entry. Refuse to touch
 * any non-zero slot so we never overwrite a kernel allocation or the
 * self-reference mapping. Burnt slots (passed as -1 or inside burnMask)
 * are skipped. */
static UINTN FindFreeSlotInPml4(UINT64 pml4Phys, UINTN burnSlot)
{
    for (UINTN idx = 510; idx >= 256; idx--) {
        if (idx == burnSlot) continue;
        UINT64 entry = BootReadPhys8(pml4Phys + idx * 8);
        if (entry == 0) return idx;
    }
    return (UINTN)-1;
}

/* Install our PDPT in a free slot of the target CR3's PML4, record it in
 * gInstalls[], and return the entry.  NULL on failure. */
static PER_PROC_INSTALL *InstallInCr3(UINT64 cr3)
{
    cr3 &= ~0xFFFULL;
    if (cr3 == 0) return NULL;

    /* Fast path: already installed and still intact */
    PER_PROC_INSTALL *existing = FindInstall(cr3);
    if (existing) {
        if (ValidateInstall(existing)) {
            existing->lruTick = ++gLruCounter;
            return existing;
        }
        /* Stale — the slot was cleared under us. Try a different slot. */
        UINTN burnt = existing->slotIdx;
        UINTN newIdx = FindFreeSlotInPml4(cr3, burnt);
        if (newIdx == (UINTN)-1) {
            existing->valid = FALSE;
            return NULL;
        }
        UINT64 newEntry = (gPdptPhys & 0x000FFFFFFFFFF000ULL)
                        | PTE_PRESENT | PTE_RW;
        BootWritePhys8(cr3 + newIdx * 8, newEntry);
        existing->slotIdx   = newIdx;
        existing->identBase = CanonicalVa(newIdx);
        existing->lruTick   = ++gLruCounter;
        return existing;
    }

    /* Not installed — find a free slot in the target PML4 */
    UINTN idx = FindFreeSlotInPml4(cr3, (UINTN)-1);
    if (idx == (UINTN)-1) return NULL;

    UINT64 newEntry = (gPdptPhys & 0x000FFFFFFFFFF000ULL)
                    | PTE_PRESENT | PTE_RW;
    BootWritePhys8(cr3 + idx * 8, newEntry);

    /* Pick a cache slot: first invalid one, else oldest by LRU */
    PER_PROC_INSTALL *slot = NULL;
    for (UINTN i = 0; i < MAX_INSTALLS; i++) {
        if (!gInstalls[i].valid) { slot = &gInstalls[i]; break; }
    }
    if (!slot) {
        UINTN oldest = 0;
        for (UINTN i = 1; i < MAX_INSTALLS; i++) {
            if (gInstalls[i].lruTick < gInstalls[oldest].lruTick) oldest = i;
        }
        slot = &gInstalls[oldest];
        /* We cannot safely clean up the evicted entry: the owning process
         * may be gone, or its CR3 may have been reused. Leave it as a
         * stale PDPT reference in its PML4 — harmless, nothing walks it
         * unless that CR3 is loaded again. */
    }

    slot->valid     = TRUE;
    slot->cr3       = cr3;
    slot->slotIdx   = idx;
    slot->identBase = CanonicalVa(idx);
    slot->lruTick   = ++gLruCounter;
    return slot;
}

/* ------------------------------------------------------------------ */
/*  Page-table walk — operates inside a swap window                    */
/*  (FastReadPhys8 reads via gActiveIdentBase, which is only valid     */
/*   while that install's CR3 is loaded.)                              */
/* ------------------------------------------------------------------ */

static UINT64 FastReadPhys8(UINT64 pa)
{
    return *(volatile UINT64 *)(UINTN)(gActiveIdentBase + pa);
}

static UINT64 VirtToPhysInWindow(UINT64 cr3, UINT64 va)
{
    UINT64 base, entry;

    base  = cr3 & ~0xFFFULL;
    entry = FastReadPhys8(base + ((va >> 39) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = FastReadPhys8(base + ((va >> 30) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = FastReadPhys8(base + ((va >> 21) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = FastReadPhys8(base + ((va >> 12) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    return (entry & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFULL);
}

/* Same walk, but each page-table read uses the bootstrap swap (one
 * cr3 round-trip per 8 bytes). Used for CMD_VTOP outside any swap
 * window. Slow but doesn't require a cached install. */
static UINT64 VirtToPhysBootstrap(UINT64 cr3, UINT64 va)
{
    UINT64 base, entry;

    base  = cr3 & ~0xFFFULL;
    entry = BootReadPhys8(base + ((va >> 39) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = BootReadPhys8(base + ((va >> 30) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = BootReadPhys8(base + ((va >> 21) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = BootReadPhys8(base + ((va >> 12) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    return (entry & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFULL);
}

/* ------------------------------------------------------------------ */
/*  Scoped batch R/W — one CR3 swap, N bytes copied, then swap back    */
/* ------------------------------------------------------------------ */

static BOOLEAN ReadVirtScoped(UINT64 cr3, UINT64 va, VOID *buf, UINTN size)
{
    PER_PROC_INSTALL *inst = InstallInCr3(cr3);
    if (!inst) return FALSE;

    BOOLEAN ok = TRUE;
    UINT64 saved = AsmSwapCr3Enter(cr3 & ~0xFFFULL);
    gActiveIdentBase = inst->identBase;

    UINT8 *dst = (UINT8 *)buf;
    while (size > 0) {
        UINT64 pa = VirtToPhysInWindow(cr3, va);
        if (pa == 0) { ok = FALSE; break; }
        UINTN pageRem = (UINTN)(PAGE_4K - (va & 0xFFF));
        UINTN chunk   = (size < pageRem) ? size : pageRem;
        CopyMem(dst, (VOID *)(UINTN)(inst->identBase + pa), chunk);
        dst  += chunk;
        va   += chunk;
        size -= chunk;
    }

    gActiveIdentBase = 0;
    AsmSwapCr3Exit(saved);
    return ok;
}

static BOOLEAN WriteVirtScoped(UINT64 cr3, UINT64 va, VOID *buf, UINTN size)
{
    PER_PROC_INSTALL *inst = InstallInCr3(cr3);
    if (!inst) return FALSE;

    BOOLEAN ok = TRUE;
    UINT64 saved = AsmSwapCr3Enter(cr3 & ~0xFFFULL);
    gActiveIdentBase = inst->identBase;

    UINT8 *src = (UINT8 *)buf;
    while (size > 0) {
        UINT64 pa = VirtToPhysInWindow(cr3, va);
        if (pa == 0) { ok = FALSE; break; }
        UINTN pageRem = (UINTN)(PAGE_4K - (va & 0xFFF));
        UINTN chunk   = (size < pageRem) ? size : pageRem;
        CopyMem((VOID *)(UINTN)(inst->identBase + pa), src, chunk);
        src  += chunk;
        va   += chunk;
        size -= chunk;
    }

    gActiveIdentBase = 0;
    AsmSwapCr3Exit(saved);
    return ok;
}

/* Physical reads/writes.  If the mailbox carries a CR3 we use its
 * install (fast path); otherwise we fall back to the bootstrap swap. */
static BOOLEAN ReadPhysAny(UINT64 cr3Hint, UINT64 pa, VOID *buf, UINTN size)
{
    if (cr3Hint != 0) {
        PER_PROC_INSTALL *inst = InstallInCr3(cr3Hint);
        if (inst) {
            UINT64 saved = AsmSwapCr3Enter(cr3Hint & ~0xFFFULL);
            gActiveIdentBase = inst->identBase;
            CopyMem(buf, (VOID *)(UINTN)(inst->identBase + pa), size);
            gActiveIdentBase = 0;
            AsmSwapCr3Exit(saved);
            return TRUE;
        }
    }
    BootReadPhysBuf(pa, buf, size);
    return TRUE;
}

static BOOLEAN WritePhysAny(UINT64 cr3Hint, UINT64 pa, VOID *buf, UINTN size)
{
    if (cr3Hint != 0) {
        PER_PROC_INSTALL *inst = InstallInCr3(cr3Hint);
        if (inst) {
            UINT64 saved = AsmSwapCr3Enter(cr3Hint & ~0xFFFULL);
            gActiveIdentBase = inst->identBase;
            CopyMem((VOID *)(UINTN)(inst->identBase + pa), buf, size);
            gActiveIdentBase = 0;
            AsmSwapCr3Exit(saved);
            return TRUE;
        }
    }
    BootWritePhysBuf(pa, buf, size);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Command handler                                                    */
/* ------------------------------------------------------------------ */

static void HandleCommand(MAILBOX *mb)
{
    mb->Status = STATUS_OK;

    switch (mb->Command) {

    case CMD_PING:
        mb->Result = MAILBOX_MAGIC;
        break;

    case CMD_READ_PHYS:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (mb->Address + mb->DataSize > MAX_PHYS) { mb->Status = STATUS_ERROR; break; }
        if (!ReadPhysAny(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
            mb->Status = STATUS_ERROR;
        break;

    case CMD_WRITE_PHYS:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (mb->Address + mb->DataSize > MAX_PHYS) { mb->Status = STATUS_ERROR; break; }
        if (!WritePhysAny(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
            mb->Status = STATUS_ERROR;
        break;

    case CMD_GET_CR3:
        mb->Result = AsmGetCr3();
        break;

    case CMD_VTOP:
        /* No swap window needed — use the bootstrap walk. */
        mb->Result = VirtToPhysBootstrap(mb->Cr3, mb->Address);
        if (mb->Result == 0) mb->Status = STATUS_ERROR;
        break;

    case CMD_READ_VIRT:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (!ReadVirtScoped(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
            mb->Status = STATUS_ERROR;
        break;

    case CMD_WRITE_VIRT:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (!WriteVirtScoped(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
            mb->Status = STATUS_ERROR;
        break;

    default:
        mb->Status = STATUS_ERROR;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Hooked runtime services                                            */
/* ------------------------------------------------------------------ */

static EFI_STATUS EFIAPI
HookedSetVariable(
    IN CHAR16    *VariableName,
    IN EFI_GUID  *VendorGuid,
    IN UINT32     Attributes,
    IN UINTN      DataSize,
    IN VOID      *Data)
{
    if (VendorGuid != NULL &&
        CompareGuid(VendorGuid, &gMailboxGuid) &&
        Data != NULL &&
        DataSize >= sizeof(MAILBOX))
    {
        MAILBOX *mb = (MAILBOX *)Data;
        if (mb->Magic == MAILBOX_MAGIC) {
            HandleCommand(mb);
            CopyMem(&gMailbox, mb, sizeof(MAILBOX));
            gMailboxReady = TRUE;
            return EFI_SUCCESS;
        }
    }
    return gOrigSetVariable(VariableName, VendorGuid, Attributes, DataSize, Data);
}

static EFI_STATUS EFIAPI
HookedGetVariable(
    IN     CHAR16    *VariableName,
    IN     EFI_GUID  *VendorGuid,
    OUT    UINT32    *Attributes   OPTIONAL,
    IN OUT UINTN     *DataSize,
    OUT    VOID      *Data         OPTIONAL)
{
    if (VendorGuid != NULL &&
        CompareGuid(VendorGuid, &gMailboxGuid) &&
        gMailboxReady &&
        Data != NULL &&
        *DataSize >= sizeof(MAILBOX))
    {
        CopyMem(Data, &gMailbox, sizeof(MAILBOX));
        *DataSize = sizeof(MAILBOX);
        if (Attributes != NULL) *Attributes = 0;
        gMailboxReady = FALSE;
        return EFI_SUCCESS;
    }
    return gOrigGetVariable(VariableName, VendorGuid, Attributes, DataSize, Data);
}

/* ------------------------------------------------------------------ */
/*  SetVirtualAddressMap callback                                      */
/* ------------------------------------------------------------------ */

static EFI_EVENT gVaMapEvent;

static VOID EFIAPI
VirtualAddressChangeNotify(IN EFI_EVENT Event, IN VOID *Context)
{
    UINTN i;

    EfiConvertPointer(0, (VOID **)&gOrigGetVariable);
    EfiConvertPointer(0, (VOID **)&gOrigSetVariable);
    EfiConvertPointer(0, (VOID **)&gBootPml4);
    EfiConvertPointer(0, (VOID **)&gPdpt);
    for (i = 0; i < IDENT_MAP_GB; i++)
        EfiConvertPointer(0, (VOID **)&gPd[i]);
    EfiConvertPointer(0, (VOID **)&gRtPdpt);
    EfiConvertPointer(0, (VOID **)&gRtPd);
    EfiConvertPointer(0, (VOID **)&gRtPt);
    EfiConvertPointer(0, (VOID **)&gImageBase);

    /* Map our image into the bootstrap PML4 at its new OS VA so the
     * bootstrap swap stubs can execute while gBootPml4 is loaded. */
    {
        UINT64 va = (UINT64)(UINTN)gImageBase;

        UINTN pml4Idx = (va >> 39) & 0x1FF;
        gBootPml4[pml4Idx] = gRtPdptPhys | TABLE_FLAGS;

        UINTN pdptIdx = (va >> 30) & 0x1FF;
        gRtPdpt[pdptIdx] = gRtPdPhys | TABLE_FLAGS;

        UINTN pdIdx = (va >> 21) & 0x1FF;
        gRtPd[pdIdx] = gRtPtPhys | TABLE_FLAGS;

        for (i = 0; i < gImagePages && i < 512; i++) {
            UINTN ptIdx = ((va >> 12) + i) & 0x1FF;
            gRtPt[ptIdx] = (gImagePhysBase + i * PAGE_4K)
                         | PTE_PRESENT | PTE_RW;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Page-table init (DXE boot phase)                                   */
/* ------------------------------------------------------------------ */

static EFI_STATUS
AllocRtPage(OUT UINT64 **VirtAddr, OUT UINT64 *PhysAddr)
{
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS st = gBS->AllocatePages(
        AllocateAnyPages, EfiRuntimeServicesData, 1, &addr);
    if (EFI_ERROR(st)) return st;
    *VirtAddr = (UINT64 *)(UINTN)addr;
    *PhysAddr = addr;
    SetMem(*VirtAddr, (UINTN)PAGE_4K, 0);
    return EFI_SUCCESS;
}

static EFI_STATUS InitPageTables(VOID)
{
    EFI_STATUS st;
    UINT64     phys;
    UINTN      i, j;

    /* Bootstrap PML4 — used only for one-shot swap stubs. */
    st = AllocRtPage(&gBootPml4, &gBootPml4Phys);
    if (EFI_ERROR(st)) return st;

    /* The PDPT we plant in every target process's PML4 */
    st = AllocRtPage(&gPdpt, &gPdptPhys);
    if (EFI_ERROR(st)) return st;
    gBootPml4[0] = gPdptPhys | TABLE_FLAGS;

    /* One PD per GB, each entry a 2 MB large page */
    for (i = 0; i < IDENT_MAP_GB; i++) {
        st = AllocRtPage(&gPd[i], &phys);
        if (EFI_ERROR(st)) return st;
        gPdpt[i] = phys | TABLE_FLAGS;

        for (j = 0; j < 512; j++) {
            UINT64 pa = ((UINT64)i << 30) | ((UINT64)j << 21);
            gPd[i][j] = pa | LARGE_FLAGS;
        }
    }

    /* Spare pages so the driver image is reachable while gBootPml4
     * is loaded during a bootstrap swap. */
    st = AllocRtPage(&gRtPdpt, &gRtPdptPhys); if (EFI_ERROR(st)) return st;
    st = AllocRtPage(&gRtPd,   &gRtPdPhys);    if (EFI_ERROR(st)) return st;
    st = AllocRtPage(&gRtPt,   &gRtPtPhys);    if (EFI_ERROR(st)) return st;

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Driver entry                                                       */
/* ------------------------------------------------------------------ */

EFI_STATUS EFIAPI
MemDriverEntry(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable)
{
    EFI_STATUS                  st;
    EFI_LOADED_IMAGE_PROTOCOL  *loadedImage;

    st = gBS->HandleProtocol(
        ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&loadedImage);
    if (EFI_ERROR(st)) return st;

    gImageBase     = loadedImage->ImageBase;
    gImagePhysBase = (UINT64)(UINTN)loadedImage->ImageBase;
    gImagePages    = (loadedImage->ImageSize + (UINTN)PAGE_4K - 1) / (UINTN)PAGE_4K;

    st = InitPageTables();
    if (EFI_ERROR(st)) return st;

    gOrigGetVariable = SystemTable->RuntimeServices->GetVariable;
    gOrigSetVariable = SystemTable->RuntimeServices->SetVariable;
    SystemTable->RuntimeServices->GetVariable = HookedGetVariable;
    SystemTable->RuntimeServices->SetVariable = HookedSetVariable;

    st = gBS->CreateEventEx(
        EVT_NOTIFY_SIGNAL,
        TPL_NOTIFY,
        VirtualAddressChangeNotify,
        NULL,
        &gEfiEventVirtualAddressChangeGuid,
        &gVaMapEvent);

    return st;
}
