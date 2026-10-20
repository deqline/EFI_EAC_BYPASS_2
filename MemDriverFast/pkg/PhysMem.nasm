;
; PhysMem.nasm — CR3-manipulation stubs for MemDriverFast (per-process scope).
;
; Two groups of stubs:
;
;  1. Bootstrap, one-shot (AsmReadPhys8 / AsmWritePhys8). These load the
;     bootstrap identity-mapping PML4 for a single 8-byte access, then
;     restore the OS CR3. Used for:
;       - Reading / writing a target PML4 during install
;       - Walking page tables when no cached install is available (CMD_VTOP)
;       - Servicing CMD_READ_PHYS / CMD_WRITE_PHYS when the mailbox did
;         not carry a Cr3 hint
;
;  2. Scoped batch (AsmSwapCr3Enter / AsmSwapCr3Exit). These swap CR3 to
;     a target process CR3 (one that already has our PDPT installed) and
;     keep it loaded. The C side does the full batch — page walk plus
;     memcpy through the identity window — inside the Enter/Exit pair.
;     One CR3 swap per mailbox command, not per 8-byte access.
;

DEFAULT REL
SECTION .text

;======================================================================
;  Bootstrap, one-shot
;======================================================================

;----------------------------------------------------------------------
; UINT64 AsmReadPhys8(UINT64 PhysAddr, UINT64 OurCr3);
;   RCX = physical address to read (8-byte aligned)
;   RDX = physical address of the bootstrap PML4
;----------------------------------------------------------------------
global AsmReadPhys8
AsmReadPhys8:
    cli
    mov  r8, cr3
    mov  cr3, rdx
    mov  rax, [rcx]
    mov  cr3, r8
    sti
    ret

;----------------------------------------------------------------------
; void AsmWritePhys8(UINT64 PhysAddr, UINT64 Value, UINT64 OurCr3);
;   RCX = physical address (8-byte aligned)
;   RDX = value
;   R8  = physical address of the bootstrap PML4
;----------------------------------------------------------------------
global AsmWritePhys8
AsmWritePhys8:
    cli
    mov  r9, cr3
    mov  cr3, r8
    mov  [rcx], rdx
    mov  cr3, r9
    sti
    ret

;----------------------------------------------------------------------
; UINT64 AsmGetCr3(void);
;----------------------------------------------------------------------
global AsmGetCr3
AsmGetCr3:
    mov  rax, cr3
    ret

;----------------------------------------------------------------------
; void AsmReloadCr3(void);
;   mov cr3, cr3 — flushes TLB and paging-structure caches on this CPU.
;   Kept for completeness; no caller in the per-process build.
;----------------------------------------------------------------------
global AsmReloadCr3
AsmReloadCr3:
    mov  rax, cr3
    mov  cr3, rax
    ret


;======================================================================
;  Scoped batch — paired Enter / Exit, hold cli across the whole batch
;======================================================================

;----------------------------------------------------------------------
; UINT64 AsmSwapCr3Enter(UINT64 NewCr3);
;   RCX = new CR3 to load (page-aligned)
;   Returns in RAX: the previous CR3 value (to be passed to AsmSwapCr3Exit).
;   On return, interrupts are disabled and CR3 = NewCr3. The paired
;   AsmSwapCr3Exit MUST run before any other code expects interrupts.
;----------------------------------------------------------------------
global AsmSwapCr3Enter
AsmSwapCr3Enter:
    cli
    mov  rax, cr3
    mov  cr3, rcx
    ret

;----------------------------------------------------------------------
; void AsmSwapCr3Exit(UINT64 SavedCr3);
;   RCX = previous CR3 to restore (as returned by AsmSwapCr3Enter)
;   Restores CR3 and re-enables interrupts.
;----------------------------------------------------------------------
global AsmSwapCr3Exit
AsmSwapCr3Exit:
    mov  cr3, rcx
    sti
    ret
