# EFI Memory Driver

UEFI DXE runtime driver that provides physical memory read/write from Windows
user-mode, completely invisible to kernel-level anti-cheat.

## How It Works

1. **At boot** (DXE phase): the driver builds identity-mapping page tables
   (VA == PA for 0–64 GB) using 2 MB large pages, hooks `GetVariable` and
   `SetVariable` in `EFI_RUNTIME_SERVICES`, and registers for
   `SetVirtualAddressMap` to fix up pointers when the OS remaps runtime
   services.

2. **At runtime**: the assembly stubs disable interrupts, swap CR3 to the
   identity-mapping page tables, perform the physical memory access (read or
   write), swap CR3 back, and re-enable interrupts.  Each access touches
   exactly one memory location while the alternate CR3 is active, using only
   volatile registers — no stack, no data section.

3. **Communication**: the Windows client calls `SetFirmwareEnvironmentVariableExW`
   to send a command mailbox, then `GetFirmwareEnvironmentVariableExW` to
   retrieve the result.  The hooked runtime services intercept calls with
   a private GUID.

## Commands

| Command        | Description                                    |
|----------------|------------------------------------------------|
| `CMD_PING`     | Echo test — returns `MAILBOX_MAGIC`            |
| `CMD_READ_PHYS`| Read up to 984 bytes at a physical address     |
| `CMD_WRITE_PHYS`| Write up to 984 bytes at a physical address   |
| `CMD_GET_CR3`  | Return the current CR3 (system page directory) |
| `CMD_VTOP`     | Translate VA → PA using a given CR3            |
| `CMD_READ_VIRT`| Read virtual memory using CR3 + page walk      |
| `CMD_WRITE_VIRT`| Write virtual memory using CR3 + page walk    |

## Prerequisites

### Driver build
- [EDK II](https://github.com/tianocore/edk2) cloned with submodules
- [NASM](https://nasm.us) (add to PATH)
- Python 3.x (add to PATH)
- Visual Studio 2022 Build Tools (C++ workload)

### Client build
- Visual Studio 2022 Build Tools (C++ workload)

## Building

### Driver

```
cd edk2
git submodule update --init
xcopy /E /I path\to\MemDriverPkg MemDriverPkg
build_driver.bat
```

Output: `Build\MemDriverPkg\RELEASE_VS2022\X64\MemDriver.efi`

### Client

```
cd EFI\client
build.bat
```

Output: `efi_client.exe`

## Loading the Driver at Boot (USB Key)

### 1. Disable Secure Boot

Enter BIOS setup (Del / F2 at POST) → Security → Secure Boot → **Disabled**.
The driver is unsigned, so Secure Boot will reject it.

### 2. Prepare a USB key with UEFI Shell

Format a USB drive as **FAT32** and create this layout:

```
USB:\
├── EFI\
│   └── Boot\
│       └── bootx64.efi      ← UEFI Shell binary (ShellBinPkg)
└── MemDriver.efi             ← our driver
```

Download the UEFI Shell binary from EDK II:
`edk2/ShellBinPkg/UefiShell/X64/Shell.efi` — rename to `bootx64.efi`.

Alternatively, build it from `edk2/ShellPkg`.

### 3. Boot from the USB key

Set boot priority to the USB drive (BIOS setup or boot menu F12/F8).
The system boots into the UEFI Shell.

### 4. Load the driver

```
Shell> fs0:
FS0:\> load MemDriver.efi
```

You should see the driver load successfully. Then boot Windows:

```
FS0:\> exit
```

Select your Windows boot entry. The driver persists as a runtime service —
it survives `ExitBootServices` and is accessible from Windows.

### 5. Run the client

Open an **Administrator** command prompt:

```
efi_client.exe
efi_client.exe Deceit.exe
efi_client.exe SomeOther.exe
```

## Automating Driver Load (startup.nsh)

Create `startup.nsh` on the USB root:

```
load MemDriver.efi
exit
```

The UEFI Shell executes `startup.nsh` automatically on boot, then `exit`
drops to the boot manager which loads Windows. This way the driver loads
without manual intervention each boot.

## QEMU / OVMF Testing

Test the driver in a VM before deploying on hardware:

```
qemu-system-x86_64 ^
  -drive if=pflash,format=raw,file=OVMF_CODE.fd ^
  -drive if=pflash,format=raw,file=OVMF_VARS.fd ^
  -drive file=fat:rw:path\to\usb_root,format=raw ^
  -m 4G -cpu qemu64 -net none
```

This boots OVMF (Tianocore UEFI firmware) with the USB contents as a FAT
filesystem.  You can `load MemDriver.efi` from the shell exactly as on
real hardware.

OVMF builds: download from https://retrage.github.io/edk2-nightly/ or
build from `edk2/OvmfPkg`.

## Limitations

- **64 GB physical memory limit**: the identity map covers 0–64 GB.  Increase
  `IDENT_MAP_GB` in `MemDriver.c` for systems with more RAM (each extra GB
  adds one 4 KB page allocation at boot).

- **Hypervisors**: `mov cr3` causes a VM exit.  The driver will not work
  (or will BSOD) on systems with Hyper-V, VBS, Credential Guard, or other
  hypervisors that trap CR3 writes.

- **Secure Boot**: the driver is unsigned.  Secure Boot must be disabled.

- **NMI / SMI**: interrupts are disabled during the CR3 swap (< 10 cycles),
  but an NMI or SMI cannot be masked.  An NMI during the ~3-instruction
  window would triple-fault.  This is extremely unlikely in practice.

- **Runtime service serialisation**: the EFI spec says runtime services are
  not reentrant.  The OS serialises calls, so concurrent access is not an
  issue, but throughput is limited to one command at a time.

- **Image spanning 2 MB boundary**: the `VirtualAddressChangeNotify` callback
  maps the driver image using a single pre-allocated PT page (covers up to
  2 MB).  If the OS assigns a VA that straddles a 2 MB boundary, the mapping
  will be incomplete.  This is unlikely for a small driver.
