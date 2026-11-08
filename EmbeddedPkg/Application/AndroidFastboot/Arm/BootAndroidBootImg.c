/** @file
  Copyright (c) 2013-2015, ARM Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "AndroidFastbootApp.h"
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
LoadFastbootBootImage (
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer,
  OUT FASTBOOT_BOOT_IMAGE  *Image
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  CHAR8                      KernelArgs[ANDROID_BOOTIMG_KERNEL_ARGS_SIZE];
  VOID                       *Kernel;
  UINTN                      KernelSize;
  VOID                       *Ramdisk;
  UINTN                      RamdiskSize;
  CHAR16                     *Options;
  CHAR16                     *NewOptions;

  ZeroMem (Image, sizeof (*Image));
  if ((BufferSize < ANDROID_BOOT_MAGIC_LENGTH) ||
      (CompareMem (Buffer, ANDROID_BOOT_MAGIC, ANDROID_BOOT_MAGIC_LENGTH) != 0))
  {
    return LoadFastbootEfiImage (BufferSize, Buffer, &Image->Handle);
  }

  Status = ParseAndroidBootImg (
             BufferSize,
             Buffer,
             &Kernel,
             &KernelSize,
             &Ramdisk,
             &RamdiskSize,
             KernelArgs
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Options = CatSPrint (NULL, L"%a", KernelArgs);
  if (Options == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (RamdiskSize != 0) {
    NewOptions = CatSPrint (Options, L" initrd=0x%Lx,0x%Lx", (UINT64)(UINTN)Ramdisk, (UINT64)RamdiskSize);
    FreePool (Options);
    Options = NewOptions;
    if (Options == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  Status = LoadFastbootEfiImage (KernelSize, Kernel, &Image->Handle);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Image->Handle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
    if (!EFI_ERROR (Status)) {
      LoadedImage->LoadOptions     = Options;
      LoadedImage->LoadOptionsSize = (UINT32)StrSize (Options);
      Image->LoadOptions           = Options;
      return EFI_SUCCESS;
    }

    gBS->UnloadImage (Image->Handle);
    Image->Handle = NULL;
  }

  FreePool (Options);
  return Status;
}
