/** @file
  Transport selection for Android Fastboot.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#pragma once
#include <Uefi.h>

EFI_STATUS
FastbootTransportInit (
  VOID
  );

EFI_STATUS
FastbootTransportStart (
  VOID
  );

EFI_STATUS
FastbootTransportStop (
  VOID
  );

VOID
FastbootTransportUnInit (
  VOID
  );

EFI_STATUS
FastbootTransportSend (
  IN UINTN       Size,
  IN CONST VOID  *Buffer
  );

EFI_STATUS
FastbootTransportReceive (
  IN OUT UINTN  *Size,
  OUT VOID      *Buffer,
  IN BOOLEAN    DataPhase
  );

BOOLEAN
FastbootTransportCanReconnect (
  VOID
  );
