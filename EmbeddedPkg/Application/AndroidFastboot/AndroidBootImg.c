/** @file

  Copyright (c) 2013-2014, ARM Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "AndroidFastbootApp.h"

// Find the kernel and ramdisk in an Android boot.img.
// return EFI_INVALID_PARAMETER if the boot.img is invalid (i.e. doesn't have the
//  right magic value),
// return EFI_NOT_FOUND if there was no kernel in the boot.img.
// The ramdisk is optional. Return NULL and size zero when it is absent.
EFI_STATUS
ParseAndroidBootImg (
  IN  UINTN  BufferSize,
  IN  VOID   *BootImg,
  OUT VOID   **Kernel,
  OUT UINTN  *KernelSize,
  OUT VOID   **Ramdisk,
  OUT UINTN  *RamdiskSize,
  OUT CHAR8  *KernelArgs
  )
{
  ANDROID_BOOTIMG_HEADER  *Header;
  UINT8                   *BootImgBytePtr;
  UINT64                  RamdiskOffset;

  // Cast to UINT8 so we can do pointer arithmetic
  BootImgBytePtr = (UINT8 *)BootImg;

  if (BufferSize < sizeof (*Header)) {
    return EFI_INVALID_PARAMETER;
  }

  Header = (ANDROID_BOOTIMG_HEADER *)BootImg;

  if (AsciiStrnCmp (
        (CONST CHAR8 *)Header->BootMagic,
        ANDROID_BOOT_MAGIC,
        ANDROID_BOOT_MAGIC_LENGTH
        ) != 0)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Header->KernelSize == 0) {
    return EFI_NOT_FOUND;
  }

  if (!IS_VALID_ANDROID_PAGE_SIZE (Header->PageSize) ||
      (Header->PageSize > BufferSize) ||
      (Header->KernelSize > BufferSize - Header->PageSize))
  {
    return EFI_INVALID_PARAMETER;
  }

  // Check the padded kernel range before locating an optional ramdisk.
  RamdiskOffset = (UINT64)Header->PageSize +
                  ALIGN_VALUE ((UINT64)Header->KernelSize, Header->PageSize);

  if ((Header->RamdiskSize != 0) &&
      ((RamdiskOffset > BufferSize) || (Header->RamdiskSize > BufferSize - RamdiskOffset)))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Ramdisk = NULL;

  *KernelSize  = Header->KernelSize;
  *Kernel      = BootImgBytePtr + Header->PageSize;
  *RamdiskSize = Header->RamdiskSize;

  if (Header->RamdiskSize != 0) {
    *Ramdisk = BootImgBytePtr + (UINTN)RamdiskOffset;
  }

  AsciiStrnCpyS (
    KernelArgs,
    ANDROID_BOOTIMG_KERNEL_ARGS_SIZE,
    Header->KernelArgs,
    ANDROID_BOOTIMG_KERNEL_ARGS_SIZE - 1
    );

  return EFI_SUCCESS;
}
