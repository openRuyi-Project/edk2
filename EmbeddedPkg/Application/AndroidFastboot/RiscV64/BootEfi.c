/** @file
  RISC-V Fastboot uses native EFI applications, including EFI-stub kernels.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "AndroidFastbootApp.h"

EFI_STATUS
LoadFastbootBootImage (
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer,
  OUT FASTBOOT_BOOT_IMAGE  *Image
  )
{
  Image->LoadOptions = NULL;
  return LoadFastbootEfiImage (BufferSize, Buffer, &Image->Handle);
}
