/** @file
  Load a native EFI application from a downloaded memory buffer.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "AndroidFastbootApp.h"
#include <Protocol/LoadedImage.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS
LoadFastbootEfiImage (
  IN UINTN        BufferSize,
  IN VOID         *Buffer,
  OUT EFI_HANDLE  *Image
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Parent;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL   *FilePath;
  MEMMAP_DEVICE_PATH         Memory;
  EFI_STATUS                 Status;

  *Image = NULL;
  if ((Buffer == NULL) || (BufferSize == 0) || (BufferSize - 1 > MAX_UINTN - (UINTN)Buffer)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Parent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DevicePath = DevicePathFromHandle (Parent->DeviceHandle);
  ZeroMem (&Memory, sizeof (Memory));
  Memory.Header.Type    = HARDWARE_DEVICE_PATH;
  Memory.Header.SubType = HW_MEMMAP_DP;
  SetDevicePathNodeLength (&Memory.Header, sizeof (Memory));
  Memory.MemoryType      = EfiBootServicesData;
  Memory.StartingAddress = (UINTN)Buffer;
  Memory.EndingAddress   = (UINTN)Buffer + BufferSize - 1;
  // Retain the parent device so applications such as Shell can find their origin.
  FilePath = AppendDevicePathNode (DevicePath, &Memory.Header);
  if (FilePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage (FALSE, gImageHandle, FilePath, Buffer, BufferSize, Image);
  FreePool (FilePath);
  if ((Status == EFI_SECURITY_VIOLATION) && (*Image != NULL)) {
    gBS->UnloadImage (*Image);
    *Image = NULL;
  }

  return Status;
}
