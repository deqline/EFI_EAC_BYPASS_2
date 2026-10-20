#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

EFI_STATUS
EFIAPI
LoaderMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *DriverFile;
  EFI_HANDLE                       DriverHandle;
  VOID                             *Buffer;
  UINTN                            FileSize;
  UINTN                            Pages;
  EFI_PHYSICAL_ADDRESS             Addr;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;

  Print(L"[MemDriver Loader]\r\n");

  Status = gBS->HandleProtocol(
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR(Status)) {
    Print(L"[!] LoadedImage: %r\r\n", Status);
    goto WaitAndBoot;
  }

  Status = gBS->HandleProtocol(
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR(Status)) {
    Print(L"[!] FileSystem: %r\r\n", Status);
    goto WaitAndBoot;
  }

  Status = FileSystem->OpenVolume(FileSystem, &Root);
  if (EFI_ERROR(Status)) {
    Print(L"[!] OpenVolume: %r\r\n", Status);
    goto WaitAndBoot;
  }

  Status = Root->Open(
                   Root,
                   &DriverFile,
                   L"MemDriver.efi",
                   EFI_FILE_MODE_READ,
                   0
                   );
  if (EFI_ERROR(Status)) {
    Print(L"[!] Open MemDriver.efi: %r\r\n", Status);
    Root->Close(Root);
    goto WaitAndBoot;
  }

  InfoSize = 0;
  Status = DriverFile->GetInfo(DriverFile, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Status = gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID **)&Info);
    if (!EFI_ERROR(Status)) {
      Status = DriverFile->GetInfo(DriverFile, &gEfiFileInfoGuid, &InfoSize, Info);
      if (!EFI_ERROR(Status)) {
        FileSize = (UINTN)Info->FileSize;
      }
      gBS->FreePool(Info);
    }
  }
  if (EFI_ERROR(Status) || FileSize == 0) {
    Print(L"[!] GetInfo: %r\r\n", Status);
    DriverFile->Close(DriverFile);
    Root->Close(Root);
    goto WaitAndBoot;
  }

  Pages = (FileSize + 0xFFF) / 0x1000;
  Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, Pages, &Addr);
  if (EFI_ERROR(Status)) {
    Print(L"[!] AllocatePages: %r\r\n", Status);
    DriverFile->Close(DriverFile);
    Root->Close(Root);
    goto WaitAndBoot;
  }
  Buffer = (VOID *)(UINTN)Addr;

  Status = DriverFile->Read(DriverFile, &FileSize, Buffer);
  DriverFile->Close(DriverFile);
  Root->Close(Root);
  if (EFI_ERROR(Status)) {
    Print(L"[!] Read: %r\r\n", Status);
    gBS->FreePages(Addr, Pages);
    goto WaitAndBoot;
  }

  Print(L"[+] Read %u bytes\r\n", FileSize);

  Status = gBS->LoadImage(FALSE, ImageHandle, NULL, Buffer, FileSize, &DriverHandle);
  gBS->FreePages(Addr, Pages);
  if (EFI_ERROR(Status)) {
    Print(L"[!] LoadImage: %r\r\n", Status);
    goto WaitAndBoot;
  }

  Status = gBS->StartImage(DriverHandle, NULL, NULL);
  if (EFI_ERROR(Status)) {
    Print(L"[!] StartImage: %r\r\n", Status);
  } else {
    Print(L"[+] MemDriver loaded OK\r\n");
  }

WaitAndBoot:
  Print(L"[*] Booting Windows in 3s...\r\n");
  gBS->Stall(3000000);
  return EFI_SUCCESS;
}
