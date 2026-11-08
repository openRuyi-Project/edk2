/** @file
  Optional readback and variable enumeration for Android Fastboot.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#pragma once
#include <Uefi/UefiBaseType.h>

extern EFI_GUID  gAndroidFastbootPlatformExtensionProtocolGuid;
#define FASTBOOT_PLATFORM_EXTENSION_REVISION  1

typedef struct _FASTBOOT_PLATFORM_EXTENSION_PROTOCOL FASTBOOT_PLATFORM_EXTENSION_PROTOCOL;

/** Read exactly BufferSize bytes without sending a Fastboot DATA response.

  The application owns command parsing, bounds checks and the response stream.
  The provider must also validate the range against the selected device.
  It may send INFO messages during the read, before the application sends DATA.
**/
typedef
EFI_STATUS
(EFIAPI *FASTBOOT_READ_PARTITION)(
  IN FASTBOOT_PLATFORM_EXTENSION_PROTOCOL *This,
  IN CONST CHAR8                         *PartitionName,
  IN UINT64                              Offset,
  IN UINTN                               BufferSize,
  OUT VOID                               *Buffer
  );

/** Report one platform variable. Stop enumeration if the callback fails. **/
typedef
EFI_STATUS
(EFIAPI *FASTBOOT_VARIABLE_CALLBACK)(
  IN VOID        *Context,
  IN CONST CHAR8 *Name,
  IN CONST CHAR8 *Value
  );

typedef
EFI_STATUS
(EFIAPI *FASTBOOT_GET_VAR_ALL)(
  IN FASTBOOT_PLATFORM_EXTENSION_PROTOCOL *This,
  IN FASTBOOT_VARIABLE_CALLBACK          Callback,
  IN VOID                                *Context
  );

struct _FASTBOOT_PLATFORM_EXTENSION_PROTOCOL {
  UINT64                     Revision;
  FASTBOOT_READ_PARTITION    ReadPartition; // Optional. NULL disables fetch.
  FASTBOOT_GET_VAR_ALL       GetVarAll;     // Optional. Does not send OKAY/FAIL.
};
