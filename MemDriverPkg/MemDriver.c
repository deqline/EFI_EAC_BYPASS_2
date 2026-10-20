/**
 * @file MemDriver.c
 * @brief UEFI DXE runtime driver — physical memory R/W via CR3 swap.
 *
 * Hooks EFI_RUNTIME_SERVICES.GetVariable and SetVariable to provide a
 * mailbox-based command interface.  The Windows client sends a command
 * via SetVariable (SetFirmwareEnvironmentVariableExW) and reads the
 * response via GetVariable (GetFirmwareEnvironmentVariableExW).
 *
 * Physical memory access uses a set of identity-mapping page tables
 * (VA == PA for 0–64 GB) built at boot.  At runtime the assembly stubs
 * briefly swap CR3 to these tables, perform the access, and swap back.
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
/*  Protocol definitions (must match client)                           */
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
#define MAILBOX_DATA_SIZE 984

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
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define PTE_PRESENT  (1ULL << 0)
#define PTE_RW       (1ULL << 1)
#define PTE_PS       (1ULL << 7)    /* 2 MB / 1 GB large page */
#define TABLE_FLAGS  (PTE_PRESENT | PTE_RW)
#define LARGE_FLAGS  (PTE_PRESENT | PTE_RW | PTE_PS)
#define PAGE_FLAGS   (PTE_PRESENT | PTE_RW)

#define PAGE_4K      0x1000ULL
#define IDENT_MAP_GB 64             /* identity-map 0 – 64 GB */
#define MAX_PHYS     ((UINT64)IDENT_MAP_GB << 30)

static EFI_GUID gMailboxGuid = {
    0xA3B10827, 0x5EF4, 0x4C21,
    { 0x8F, 0x3A, 0x92, 0xD7, 0xE6, 0xF8, 0xB0, 0x01 }
};

/* ------------------------------------------------------------------ */
/*  Assembly stubs (PhysMem.nasm)                                      */
/* ------------------------------------------------------------------ */

extern UINT64 AsmReadPhys8  (UINT64 PhysAddr, UINT64 OurCr3);
extern void   AsmWritePhys8 (UINT64 PhysAddr, UINT64 Value, UINT64 OurCr3);
extern UINT64 AsmGetCr3     (void);

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */

/* Original runtime service pointers (converted by SetVirtualAddressMap) */
static EFI_GET_VARIABLE  gOrigGetVariable;
static EFI_SET_VARIABLE  gOrigSetVariable;

/* Identity-mapping page tables — virtual addresses (updated by ConvertPointer) */
static UINT64 *gPml4;
static UINT64 *gPdpt;
static UINT64 *gPd[IDENT_MAP_GB];

/* Physical address of PML4 (invariant — this is what we load into CR3) */
static UINT64  gPml4Phys;

/* Spare page-table pages for mapping driver code at its runtime VA */
static UINT64 *gRtPdpt;   static UINT64 gRtPdptPhys;
static UINT64 *gRtPd;     static UINT64 gRtPdPhys;
static UINT64 *gRtPt;     static UINT64 gRtPtPhys;

/* Driver image info (physical base never changes) */
static VOID   *gImageBase;
static UINT64  gImagePhysBase;
static UINTN   gImagePages;

/* Command mailbox */
static MAILBOX  gMailbox;
static BOOLEAN  gMailboxReady;

/* ------------------------------------------------------------------ */
/*  Physical memory helpers                                            */
/* ------------------------------------------------------------------ */

static UINT64 ReadPhys8(UINT64 pa)              { return AsmReadPhys8(pa, gPml4Phys); }
static void   WritePhys8(UINT64 pa, UINT64 val) { AsmWritePhys8(pa, val, gPml4Phys);  }

static void ReadPhysBuf(UINT64 pa, VOID *buf, UINTN size)
{
    UINT8 *dst = (UINT8 *)buf;
    UINTN i;
    for (i = 0; i + 8 <= size; i += 8) {
        UINT64 v = ReadPhys8(pa + i);
        CopyMem(dst + i, &v, 8);
    }
    if (i < size) {
        UINT64 v = ReadPhys8(pa + i);
        CopyMem(dst + i, &v, size - i);
    }
}

static void WritePhysBuf(UINT64 pa, VOID *buf, UINTN size)
{
    UINT8 *src = (UINT8 *)buf;
    UINTN i;
    for (i = 0; i + 8 <= size; i += 8) {
        UINT64 v;
        CopyMem(&v, src + i, 8);
        WritePhys8(pa + i, v);
    }
    if (i < size) {
        UINT64 v = ReadPhys8(pa + i);   /* read–modify–write partial */
        CopyMem(&v, src + i, size - i);
        WritePhys8(pa + i, v);
    }
}

/* ------------------------------------------------------------------ */
/*  Page-table walk (uses physical reads)                              */
/* ------------------------------------------------------------------ */

static UINT64 VirtToPhys(UINT64 cr3, UINT64 va)
{
    UINT64 base, entry;

    base  = cr3 & ~0xFFFULL;
    entry = ReadPhys8(base + ((va >> 39) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = ReadPhys8(base + ((va >> 30) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = ReadPhys8(base + ((va >> 21) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_PS) return (entry & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);

    base  = entry & 0x000FFFFFFFFFF000ULL;
    entry = ReadPhys8(base + ((va >> 12) & 0x1FF) * 8);
    if (!(entry & PTE_PRESENT)) return 0;
    return (entry & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFULL);
}

/* ------------------------------------------------------------------ */
/*  Virtual memory R/W (translates each page via VirtToPhys)           */
/* ------------------------------------------------------------------ */

static BOOLEAN ReadVirtBuf(UINT64 cr3, UINT64 va, VOID *buf, UINTN size)
{
    UINT8 *dst = (UINT8 *)buf;
    while (size > 0) {
        UINT64 pa = VirtToPhys(cr3, va);
        if (pa == 0) return FALSE;
        UINTN pageRem = (UINTN)(PAGE_4K - (va & 0xFFF));
        UINTN chunk   = (size < pageRem) ? size : pageRem;
        ReadPhysBuf(pa, dst, chunk);
        dst  += chunk;
        va   += chunk;
        size -= chunk;
    }
    return TRUE;
}

static BOOLEAN WriteVirtBuf(UINT64 cr3, UINT64 va, VOID *buf, UINTN size)
{
    UINT8 *src = (UINT8 *)buf;
    while (size > 0) {
        UINT64 pa = VirtToPhys(cr3, va);
        if (pa == 0) return FALSE;
        UINTN pageRem = (UINTN)(PAGE_4K - (va & 0xFFF));
        UINTN chunk   = (size < pageRem) ? size : pageRem;
        WritePhysBuf(pa, src, chunk);
        src  += chunk;
        va   += chunk;
        size -= chunk;
    }
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
        ReadPhysBuf(mb->Address, mb->Data, mb->DataSize);
        break;

    case CMD_WRITE_PHYS:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (mb->Address + mb->DataSize > MAX_PHYS) { mb->Status = STATUS_ERROR; break; }
        WritePhysBuf(mb->Address, mb->Data, mb->DataSize);
        break;

    case CMD_GET_CR3:
        mb->Result = AsmGetCr3();
        break;

    case CMD_VTOP:
        mb->Result = VirtToPhys(mb->Cr3, mb->Address);
        if (mb->Result == 0) mb->Status = STATUS_ERROR;
        break;

    case CMD_READ_VIRT:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (!ReadVirtBuf(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
            mb->Status = STATUS_ERROR;
        break;

    case CMD_WRITE_VIRT:
        if (mb->DataSize > MAILBOX_DATA_SIZE) mb->DataSize = MAILBOX_DATA_SIZE;
        if (!WriteVirtBuf(mb->Cr3, mb->Address, mb->Data, mb->DataSize))
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

    /* Convert all virtual pointers to their new OS-assigned addresses */
    EfiConvertPointer(0, (VOID **)&gOrigGetVariable);
    EfiConvertPointer(0, (VOID **)&gOrigSetVariable);
    EfiConvertPointer(0, (VOID **)&gPml4);
    EfiConvertPointer(0, (VOID **)&gPdpt);
    for (i = 0; i < IDENT_MAP_GB; i++)
        EfiConvertPointer(0, (VOID **)&gPd[i]);
    EfiConvertPointer(0, (VOID **)&gRtPdpt);
    EfiConvertPointer(0, (VOID **)&gRtPd);
    EfiConvertPointer(0, (VOID **)&gRtPt);
    EfiConvertPointer(0, (VOID **)&gImageBase);

    /*
     * Map our driver image in our identity-mapping page tables at its
     * new (OS-assigned) VA.  The CR3-swap asm stubs must be accessible
     * in BOTH page tables — the OS's (already mapped) and ours.
     *
     * We write PHYSICAL addresses into the page-table entries (saved at
     * boot, invariant) and access the page-table pages via their new VAs
     * (just converted above).
     */
    {
        UINT64 va = (UINT64)(UINTN)gImageBase;

        UINTN pml4Idx = (va >> 39) & 0x1FF;
        gPml4[pml4Idx] = gRtPdptPhys | TABLE_FLAGS;

        UINTN pdptIdx = (va >> 30) & 0x1FF;
        gRtPdpt[pdptIdx] = gRtPdPhys | TABLE_FLAGS;

        UINTN pdIdx = (va >> 21) & 0x1FF;
        gRtPd[pdIdx] = gRtPtPhys | TABLE_FLAGS;

        for (i = 0; i < gImagePages && i < 512; i++) {
            UINTN ptIdx = ((va >> 12) + i) & 0x1FF;
            gRtPt[ptIdx] = (gImagePhysBase + i * PAGE_4K) | PAGE_FLAGS;
        }
    }

    /* gPml4Phys is NEVER converted — it is always the physical address */
}

/* ------------------------------------------------------------------ */
/*  Page-table initialisation (called during DXE boot phase)           */
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

    /* PML4 */
    st = AllocRtPage(&gPml4, &gPml4Phys);
    if (EFI_ERROR(st)) return st;

    /* PDPT covering the low 512 GB (entry 0 of PML4) */
    st = AllocRtPage(&gPdpt, &phys);
    if (EFI_ERROR(st)) return st;
    gPml4[0] = phys | TABLE_FLAGS;

    /* One PD page per GB — each PD entry is a 2 MB large page */
    for (i = 0; i < IDENT_MAP_GB; i++) {
        st = AllocRtPage(&gPd[i], &phys);
        if (EFI_ERROR(st)) return st;
        gPdpt[i] = phys | TABLE_FLAGS;

        for (j = 0; j < 512; j++) {
            UINT64 pa = ((UINT64)i << 30) | ((UINT64)j << 21);
            gPd[i][j] = pa | LARGE_FLAGS;
        }
    }

    /* Spare pages for mapping the driver at its runtime VA */
    st = AllocRtPage(&gRtPdpt, &gRtPdptPhys);  if (EFI_ERROR(st)) return st;
    st = AllocRtPage(&gRtPd,   &gRtPdPhys);     if (EFI_ERROR(st)) return st;
    st = AllocRtPage(&gRtPt,   &gRtPtPhys);     if (EFI_ERROR(st)) return st;

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Driver entry point                                                 */
/* ------------------------------------------------------------------ */

EFI_STATUS EFIAPI
MemDriverEntry(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable)
{
    EFI_STATUS                  st;
    EFI_LOADED_IMAGE_PROTOCOL  *loadedImage;

    /* Query our own image base and size (physical == virtual during boot) */
    st = gBS->HandleProtocol(
        ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&loadedImage);
    if (EFI_ERROR(st)) return st;

    gImageBase     = loadedImage->ImageBase;
    gImagePhysBase = (UINT64)(UINTN)loadedImage->ImageBase;
    gImagePages    = (loadedImage->ImageSize + (UINTN)PAGE_4K - 1) / (UINTN)PAGE_4K;

    /* Build identity-mapping page tables (0 – 64 GB, 2 MB pages) */
    st = InitPageTables();
    if (EFI_ERROR(st)) return st;

    /* Hook GetVariable and SetVariable */
    gOrigGetVariable = SystemTable->RuntimeServices->GetVariable;
    gOrigSetVariable = SystemTable->RuntimeServices->SetVariable;
    SystemTable->RuntimeServices->GetVariable = HookedGetVariable;
    SystemTable->RuntimeServices->SetVariable = HookedSetVariable;

    /* Register for the SetVirtualAddressMap event */
    st = gBS->CreateEventEx(
        EVT_NOTIFY_SIGNAL,
        TPL_NOTIFY,
        VirtualAddressChangeNotify,
        NULL,
        &gEfiEventVirtualAddressChangeGuid,
        &gVaMapEvent);

    return st;
}
