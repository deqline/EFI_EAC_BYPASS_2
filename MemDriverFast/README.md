# MemDriverFast — per-process-scoped EFI memory driver

An EFI runtime driver that performs physical and virtual memory reads/writes
through the game's **own** page tables, keeping all persistent modifications
out of the system PML4. Same mailbox protocol as the original `MemDriverPkg`,
so the existing `efi_client.exe` works unchanged.

> **Design history**
>
> - **v1** (`MemDriverPkg`): swaps CR3 to our own identity-map PML4 for
>   every 8-byte access. Hidden but slow.
> - **v2** (earlier revision of this folder): installs a PDPT entry in the
>   **OS's own** PML4 once at first command. No swap on hot path. Fast but
>   leaves a persistent fingerprint in the system PML4 — exactly the
>   structure PatchGuard-and-friends scan.
> - **v3 (this version)**: installs the PDPT entry in **each target
>   process's own PML4**, not the system PML4. One CR3 swap per mailbox
>   command (not per byte). Nothing persists in the kernel's inspection
>   scope.

## For beginners: what's going on

### Background in one minute

A 64-bit CPU does not read memory with raw addresses. The addresses in your
code are **virtual addresses (VA)**. A per-process table hierarchy (PML4 →
PDPT → PD → PT) translates each VA into a **physical address (PA)**, the
actual byte in RAM. The physical base of the current hierarchy lives in a
CPU register called **CR3**. Every running process has its own CR3; Windows
switches CR3 on every context switch.

Our driver, running from firmware, wants to read a specific game byte.
Either it does a page-table walk in software (slow), or it arranges for one
of the already-loaded VA maps to contain an **identity window** — a region
where `VA == PA + base` — so a plain `*ptr` dereference reaches the byte.

### Why not just patch Windows's PML4 once?

That was v2. It worked, but Windows's PML4 is a kernel-wide shared
structure. PatchGuard checks it. VBS/HVCI protects it with EPT. Any
custom kernel integrity scan can enumerate its 512 entries, notice our
addition, and flag or zero it.

### What v3 does instead

Each mailbox command from the Windows client already carries a target
process CR3 (used by `CMD_READ_VIRT`, `CMD_WRITE_VIRT`, `CMD_VTOP`). The
driver treats that CR3 as the install target:

1. On first contact with a given CR3, walk **that process's** PML4,
   find a free slot (zero entry) in the kernel half (slots 256..510),
   and write our PDPT physical address into it.
2. Cache `{cr3, slotIdx, identBase}` in a small table (up to 8 entries).
3. On every memory access for that CR3:
   - `AsmSwapCr3Enter(cr3)` — `cli`, save old CR3, load target CR3.
   - Software page walk + `CopyMem` through `identBase + pa`. The game
     process's own CR3 now sees an identity window at `identBase`, so
     physical-address reads via that window just work.
   - `AsmSwapCr3Exit(saved)` — restore old CR3, `sti`.

One CR3 swap per **mailbox command**, which can carry 984 bytes of data —
not one swap per 8-byte read like v1.

### Why this is quieter than v2

- The modified PML4 is the **game process's**, not the system kernel's.
- PatchGuard scans the system PML4 and parts of KPCR. It does **not**
  enumerate per-process page tables (which belong to arbitrary processes
  the kernel doesn't audit).
- An EAC-style driver scanning for memory tampering looks at the system
  PML4, the SSDT, callback registrations — not the game's own PML4.
- VBS/HVCI protects kernel structures via EPT. The game's user PML4 is
  not one of them.

### Why slots 256..510

- Slots **0..255** describe user space. Writing an entry there would
  overlap the user-mode VA space of that process — bad.
- Slot **511** is sometimes used for self-mapping.
- Slots **256..510** are kernel space. On a running Windows most are
  zero; we refuse to touch any slot that isn't, so we never clobber a
  kernel allocation.

### Why interrupts stay off during the swap

While CR3 is loaded with the game CR3, an interrupt would run with that
CR3 active. If the interrupt handler lives at a VA that depends on
kernel-half PML4 entries the game process doesn't share, the handler
would fault. We hold `cli` for the length of one 984-byte batch — on
the order of a microsecond. Short enough that NMIs are the only real
hazard, and that's the same hazard the v1 swap had.

## Costs vs. earlier versions

| Property                                   | v1 (own PML4 swap)   | v2 (system PML4)        | v3 (per-process)              |
|--------------------------------------------|----------------------|-------------------------|-------------------------------|
| CR3 writes per 8-byte access               | 2                    | 0                       | 0 (amortised over batch)      |
| CR3 writes per 984-byte batch              | ~250                 | 0                       | 2 (one Enter, one Exit)       |
| `cli` held across                          | ~3 instructions / 8B | never                   | one batch (~1 μs)             |
| Persistent change in RAM                   | none                 | 1 slot in **system** PML4 | 1 slot per touched process PML4 |
| Visible to PatchGuard                      | no                   | **possibly**            | no                            |
| Blocked by VBS/HVCI                        | partially            | **yes** (fault/revert)  | likely yes (modify user PT under HVCI is also constrained) |
| Install cost per new CR3                   | zero                 | once (first command)    | ~10 phys reads / write        |

## Mailbox protocol (unchanged)

Same `MAILBOX` struct, command numbers, GUID, and variable name as v1 and
v2. The existing `efi_client.exe` from `EFI/client/` works as-is.

| Command        | Meaning                                           |
|----------------|---------------------------------------------------|
| `CMD_PING`     | Echo test                                         |
| `CMD_READ_PHYS`| Read N bytes at a physical address — uses installed CR3 if the mailbox carried one, else a one-shot bootstrap swap |
| `CMD_WRITE_PHYS`| Write N bytes at a physical address — same     |
| `CMD_GET_CR3`  | Return the current CR3                            |
| `CMD_VTOP`     | Translate VA → PA using a given CR3               |
| `CMD_READ_VIRT`| Read N bytes at a virtual address (given CR3)     |
| `CMD_WRITE_VIRT`| Write N bytes at a virtual address (given CR3)   |

The client doesn't need to know about the install — the driver installs
on-demand for whichever CR3 appears in the mailbox.

## Building

Prerequisites (same as original package):

- EDK II at `..\MemDriverPkg2\edkII\` (reuse the existing clone)
- NASM at `C:\NASM\nasm-2.16.03\`
- Visual Studio 2022 Build Tools (C++ workload)
- A FAT32-formatted USB stick

Then from an ordinary command prompt in this folder:

```
build_and_deploy.bat          REM USB=E:
build_and_deploy.bat F        REM USB=F:
```

What the script does:

1. Loads VS2022 and NASM into the environment.
2. Copies `pkg\*` into `..\MemDriverPkg2\edkII\MemDriverFastPkg\`.
3. Runs `build -a X64 -t VS2022 -p MemDriverFastPkg/MemDriverFastPkg.dsc -b RELEASE`.
4. Compiles `..\client\client.cpp` into `efi_client.exe`.
5. Lays everything out on the USB:

```
USB:\
├── EFI\
│   └── Boot\
│       └── bootx64.efi        ← Loader (loads MemDriver.efi then boots Windows)
├── MemDriver.efi               ← the driver
└── efi_client.exe              ← Windows user-mode client
```

## Running

1. In BIOS: **disable Secure Boot** (the driver is unsigned).
2. Set USB first in the boot order (or press F12 / F8 at POST).
3. Boot from the USB. The Loader prints a few lines, calls `LoadImage` +
   `StartImage` on `MemDriver.efi`, and then returns — the firmware boot
   manager chains to Windows.
4. Log into Windows. Open an **Administrator** command prompt.
5. From the USB (or copied locally):

```
efi_client.exe
efi_client.exe <ProcessName.exe>
```

The client enables `SeSystemEnvironmentPrivilege`, pings the driver via
`SetFirmwareEnvironmentVariableExW`, walks EPROCESS to find the game CR3,
and reads a byte to prove the pipe works.

## Testing in QEMU first (recommended)

Before touching real hardware, boot the USB layout under OVMF:

```
qemu-system-x86_64 ^
  -drive if=pflash,format=raw,file=OVMF_CODE.fd ^
  -drive if=pflash,format=raw,file=OVMF_VARS.fd ^
  -drive file=fat:rw:E:\,format=raw ^
  -m 4G -cpu qemu64 -net none
```

If the driver loads cleanly in OVMF, it will almost certainly load on
real UEFI hardware too (modulo Secure Boot).

## Limitations

- **64 GB physical limit.** The PDPT covers 0–64 GB using 2 MB large
  pages. Bump `IDENT_MAP_GB` in `MemDriver.c` if you need more.
- **Hypervisors (Hyper-V, VBS, Credential Guard).** They own the real
  CR3; `mov cr3` is a VM exit, and writes to the live PML4 (ours is a
  user PML4 but still) may be inspected. Behaviour is unpredictable
  under VBS.
- **Secure Boot.** Must be off.
- **PatchGuard.** No fingerprint in the system PML4. The entries in each
  touched process's PML4 are out of PatchGuard's inspection scope.
- **Game-side detection.** The game's own code *can* read its own PML4
  physical address (via CR3) if it chooses to. Unlikely for most titles
  but not impossible — the mitigation if that ever becomes a problem is
  to install only on demand (write + do batch + zero) rather than keep
  the slot populated.
- **Cache capacity.** `MAX_INSTALLS` is 8. If the client ever targets
  more than 8 distinct CR3s in a session the oldest is evicted LRU-
  style; the stale PDPT reference in the evicted process's PML4 is
  left behind as harmless garbage (it points at valid pages but nothing
  dereferences the window if that process is dead or not re-targeted).
- **Partial image mapping during install.** Bootstrap swaps still need
  the driver image reachable while `gBootPml4` is loaded. The image
  must fit inside a single 2 MB page from Windows's VA — true for a
  driver this small.
