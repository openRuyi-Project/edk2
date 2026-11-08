/** @file
  Copyright (c) 2013-2014, ARM Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#pragma once
#include <Library/AndroidBootImgLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

#define ANDROID_FASTBOOT_VERSION  "0.4"

typedef struct {
  EFI_HANDLE    Handle;
  VOID          *LoadOptions;
} FASTBOOT_BOOT_IMAGE;

EFI_STATUS
LoadFastbootBootImage (
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer,
  OUT FASTBOOT_BOOT_IMAGE  *Image
  );

EFI_STATUS
LoadFastbootEfiImage (
  IN UINTN        BufferSize,
  IN VOID         *Buffer,
  OUT EFI_HANDLE  *Image
  );

EFI_STATUS
ParseAndroidBootImg (
  IN UINTN   BufferSize,
  IN VOID    *BootImg,
  OUT VOID   **Kernel,
  OUT UINTN  *KernelSize,
  OUT VOID   **Ramdisk,
  OUT UINTN  *RamdiskSize,
  OUT CHAR8  *KernelArgs
  );
