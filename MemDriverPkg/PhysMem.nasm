;
; PhysMem.nasm — Physical memory access via CR3 swap.
;
; Disables interrupts, loads an identity-mapping CR3 (VA = PA for low
; physical memory), performs the access, and restores the original CR3.
;
; All functions use only volatile registers (Microsoft x64 ABI) between
; the two CR3 writes, so no stack access occurs while our page tables
; are active.
;

DEFAULT REL
SECTION .text

;----------------------------------------------------------------------
; UINT64 AsmReadPhys8(UINT64 PhysAddr, UINT64 OurCr3);
;   RCX = physical address to read (8 bytes, must be 8-byte aligned)
;   RDX = physical address of our identity-mapping PML4
;   Returns: 8 bytes read from that physical address
;----------------------------------------------------------------------
global AsmReadPhys8
AsmReadPhys8:
    cli                       ; disable interrupts — critical section
    mov  r8, cr3              ; save current (OS) CR3
    mov  cr3, rdx             ; load our identity-mapping page tables
    mov  rax, [rcx]           ; read 8 bytes — VA == PA in our tables
    mov  cr3, r8              ; restore OS CR3
    sti                       ; re-enable interrupts
    ret

;----------------------------------------------------------------------
; void AsmWritePhys8(UINT64 PhysAddr, UINT64 Value, UINT64 OurCr3);
;   RCX = physical address to write
;   RDX = 8-byte value to write
;   R8  = physical address of our identity-mapping PML4
;----------------------------------------------------------------------
global AsmWritePhys8
AsmWritePhys8:
    cli
    mov  r9, cr3              ; save OS CR3
    mov  cr3, r8              ; load our identity map
    mov  [rcx], rdx           ; write 8 bytes at PA
    mov  cr3, r9              ; restore OS CR3
    sti
    ret

;----------------------------------------------------------------------
; UINT64 AsmGetCr3(void);
;   Returns: current CR3 value (page directory base register)
;----------------------------------------------------------------------
global AsmGetCr3
AsmGetCr3:
    mov  rax, cr3
    ret
