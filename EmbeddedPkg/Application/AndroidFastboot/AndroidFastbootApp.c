/** @file
  Android Fastboot command and download state machine.

  Copyright (c) 2013-2014, ARM Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "AndroidFastbootApp.h"
#include "FastbootTransport.h"
#include <Protocol/AndroidFastbootPlatform.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define FASTBOOT_COMMAND_MAX_LENGTH  64
#define FASTBOOT_PACKET_BYTES        SIZE_16KB
#define DOWNLOAD_TIMEOUT_NS          5000000000ULL

typedef enum {
  ExpectCmdState,
  ExpectDataState
} ANDROID_FASTBOOT_STATE;

STATIC FASTBOOT_PLATFORM_PROTOCOL  *mPlatform;
STATIC ANDROID_FASTBOOT_STATE      mState;
STATIC UINT8                       *mDataBuffer;
STATIC UINTN                       mNumDataBytes;
STATIC UINTN                       mBytesReceivedSoFar;
STATIC BOOLEAN                     mDownloadComplete;
STATIC BOOLEAN                     mFinished;
STATIC BOOLEAN                     mReboot;
STATIC UINT64                      mLastData;

STATIC EFI_STATUS
SendResponse (
  IN CONST CHAR8  *Prefix,
  IN CONST CHAR8  *Value
  )
{
  CHAR8  Response[FASTBOOT_COMMAND_MAX_LENGTH + 1];

  AsciiSPrint (Response, sizeof (Response), "%a%a", Prefix, Value);
  return FastbootTransportSend (AsciiStrLen (Response), Response);
}

STATIC VOID
ResetDownload (
  VOID
  )
{
  if (mDataBuffer != NULL) {
    FreePool (mDataBuffer);
  }

  mDataBuffer         = NULL;
  mNumDataBytes       = 0;
  mBytesReceivedSoFar = 0;
  mDownloadComplete   = FALSE;
  mState              = ExpectCmdState;
}

STATIC BOOLEAN
HexNumber (
  IN CONST CHAR8  *Text,
  IN UINTN        Digits,
  OUT UINT64      *Value
  )
{
  UINTN  Index;
  UINTN  Digit;
  CHAR8  C;

  *Value = 0;
  if ((Digits == 0) || (Digits > 16)) {
    return FALSE;
  }

  for (Index = 0; Index < Digits; Index++) {
    C = Text[Index];
    if ((C >= '0') && (C <= '9')) {
      Digit = C - '0';
    } else if ((C >= 'a') && (C <= 'f')) {
      Digit = C - 'a' + 10;
    } else if ((C >= 'A') && (C <= 'F')) {
      Digit = C - 'A' + 10;
    } else {
      return FALSE;
    }

    *Value = (*Value << 4) | Digit;
  }

  return TRUE;
}

STATIC EFI_STATUS
GetVar (
  IN CHAR8   *Name,
  OUT CHAR8  *Value
  )
{
  if (AsciiStrCmp (Name, "version") == 0) {
    return AsciiStrCpyS (Value, 61, ANDROID_FASTBOOT_VERSION);
  }

  if (AsciiStrCmp (Name, "is-userspace") == 0) {
    return AsciiStrCpyS (Value, 61, "no");
  }

  if (AsciiStrCmp (Name, "max-download-size") == 0) {
    AsciiSPrint (Value, 61, "0x%08x", FixedPcdGet32 (PcdAndroidFastbootMaxDownloadSize));
    return EFI_SUCCESS;
  }

  Value[0] = 0;
  return mPlatform->GetVar (Name, Value);
}

STATIC EFI_STATUS EFIAPI
ReportVariable (
  IN VOID         *Context,
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Value
  )
{
  CHAR8  Info[61];

  AsciiSPrint (Info, sizeof (Info), "%a: %a", Name, Value);
  return SendResponse ("INFO", Info);
}

STATIC EFI_STATUS
HandleGetVar (
  IN CHAR8  *Name
  )
{
  STATIC CHAR8  *Variables[] = {
    "version",            "is-userspace", "max-download-size",
    "version-bootloader", "product",      "serialno", "secure"
  };
  CHAR8         Value[61];
  EFI_STATUS    Status;
  UINTN         Index;

  if (AsciiStrCmp (Name, "all") == 0) {
    for (Index = 0; Index < ARRAY_SIZE (Variables); Index++) {
      Status = GetVar (Variables[Index], Value);
      if (!EFI_ERROR (Status) && (Value[0] != 0)) {
        Status = ReportVariable (NULL, Variables[Index], Value);
        if (EFI_ERROR (Status)) {
          return Status;
        }
      }
    }

    return SendResponse ("OKAY", "");
  }

  Status = GetVar (Name, Value);
  if (EFI_ERROR (Status)) {
    return SendResponse ("FAIL", Status == EFI_NOT_FOUND ? "unknown variable" : "variable unavailable");
  }

  return SendResponse ("OKAY", Value);
}

STATIC EFI_STATUS
HandleDownload (
  IN CHAR8  *NumBytesString
  )
{
  UINT64      Bytes;
  CHAR8       Response[13];
  EFI_STATUS  Status;

  ResetDownload ();
  if ((AsciiStrLen (NumBytesString) != 8) || !HexNumber (NumBytesString, 8, &Bytes) ||
      (Bytes == 0) || (Bytes > FixedPcdGet32 (PcdAndroidFastbootMaxDownloadSize)))
  {
    return SendResponse ("FAIL", "invalid download size");
  }

  mDataBuffer = AllocatePool ((UINTN)Bytes);
  if (mDataBuffer == NULL) {
    return SendResponse ("FAIL", "not enough memory");
  }

  AsciiSPrint (Response, sizeof (Response), "DATA%08x", (UINT32)Bytes);
  Status = FastbootTransportSend (12, Response);
  if (EFI_ERROR (Status)) {
    ResetDownload ();
    return Status;
  }

  mNumDataBytes = (UINTN)Bytes;
  mLastData     = GetPerformanceCounter ();
  mState        = ExpectDataState;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
OperationResult (
  IN EFI_STATUS  Status
  )
{
  CHAR8  Value[61];

  if (!EFI_ERROR (Status)) {
    return SendResponse ("OKAY", "");
  }

  AsciiSPrint (Value, sizeof (Value), "%r", Status);
  return SendResponse ("FAIL", Value);
}

STATIC EFI_STATUS
HandleFlash (
  IN CHAR8  *PartitionName
  )
{
  EFI_STATUS  Status;

  if (!mDownloadComplete) {
    return SendResponse ("FAIL", "no complete download");
  }

  Status = mPlatform->FlashPartition (PartitionName, mNumDataBytes, mDataBuffer);
  ResetDownload ();
  return OperationResult (Status);
}

STATIC EFI_STATUS
StopTransport (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = FastbootTransportStop ();
  if (Status == EFI_NOT_STARTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    Print (L"Fastboot transport did not stop. Reset required.\r\n");
    do {
      // A debugger may release CpuDeadLoop. Recheck DMA before cleanup.
      CpuDeadLoop ();
      Status = FastbootTransportStop ();
    } while (EFI_ERROR (Status) && (Status != EFI_NOT_STARTED));

    return EFI_DEVICE_ERROR;
  }

  return Status;
}

STATIC EFI_STATUS
HandleBoot (
  VOID
  )
{
  FASTBOOT_BOOT_IMAGE  Image;
  EFI_STATUS           Status;
  EFI_STATUS           ImageStatus;

  if (!mDownloadComplete) {
    return SendResponse ("FAIL", "no complete download");
  }

  ZeroMem (&Image, sizeof (Image));
  Status = LoadFastbootBootImage (mNumDataBytes, mDataBuffer, &Image);
  if (EFI_ERROR (Status)) {
    return SendResponse ("FAIL", "no loadable EFI image");
  }

  Status = SendResponse ("OKAY", "");
  if (!EFI_ERROR (Status)) {
    Status = StopTransport ();
  }

  if (EFI_ERROR (Status)) {
    gBS->UnloadImage (Image.Handle);
  } else {
    // Command handlers run in the application loop, never a notify callback.
    ImageStatus = gBS->StartImage (Image.Handle, NULL, NULL);
    Print (L"EFI application returned: %r\r\n", ImageStatus);
    Status = FastbootTransportStart ();
  }

  if (Image.LoadOptions != NULL) {
    FreePool (Image.LoadOptions);
  }

  ResetDownload ();
  return Status;
}

STATIC EFI_STATUS
AcceptCmd (
  IN UINTN        Size,
  IN CONST CHAR8  *Data
  )
{
  CHAR8       Command[FASTBOOT_COMMAND_MAX_LENGTH + 1];
  EFI_STATUS  Status;

  if ((Size == 0) || (Size > FASTBOOT_COMMAND_MAX_LENGTH) || (ScanMem8 (Data, Size, 0) != NULL)) {
    return SendResponse ("FAIL", "invalid command");
  }

  CopyMem (Command, Data, Size);
  Command[Size] = 0;
  if (AsciiStrnCmp (Command, "getvar:", 7) == 0) {
    return HandleGetVar (Command + 7);
  }

  if (AsciiStrnCmp (Command, "download:", 9) == 0) {
    return HandleDownload (Command + 9);
  }

  if (AsciiStrnCmp (Command, "flash:", 6) == 0) {
    return HandleFlash (Command + 6);
  }

  if (AsciiStrnCmp (Command, "erase:", 6) == 0) {
    return OperationResult (mPlatform->ErasePartition (Command + 6));
  }

  if ((AsciiStrCmp (Command, "boot") == 0) || (AsciiStrCmp (Command, "oem boot-efi") == 0)) {
    return HandleBoot ();
  }

  if ((AsciiStrCmp (Command, "continue") == 0) || (AsciiStrCmp (Command, "reboot") == 0)) {
    Status = SendResponse ("OKAY", "");
    if (!EFI_ERROR (Status)) {
      mFinished = TRUE;
      mReboot   = AsciiStrCmp (Command, "reboot") == 0;
    }

    return Status;
  }

  if (AsciiStrCmp (Command, "powerdown") == 0) {
    Status = SendResponse ("OKAY", "");
    if (!EFI_ERROR (Status)) {
      StopTransport ();
      gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
      return EFI_DEVICE_ERROR;
    }

    return Status;
  }

  if (AsciiStrnCmp (Command, "oem ", 4) == 0) {
    ResetDownload ();
    return OperationResult (mPlatform->DoOemCommand (Command + 4));
  }

  return SendResponse ("FAIL", "unsupported command");
}

STATIC EFI_STATUS
AcceptData (
  IN UINTN  Size
  )
{
  if (Size > mNumDataBytes - mBytesReceivedSoFar) {
    ResetDownload ();
    return SendResponse ("FAIL", "invalid data size");
  }

  if (Size == 0) {
    return EFI_SUCCESS;
  }

  mBytesReceivedSoFar += Size;
  mLastData            = GetPerformanceCounter ();
  if (mBytesReceivedSoFar == mNumDataBytes) {
    mDownloadComplete = TRUE;
    mState            = ExpectCmdState;
    return SendResponse ("OKAY", "");
  }

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
FastbootAppEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  EFI_STATUS     StopStatus;
  EFI_INPUT_KEY  Key;
  UINT8          Command[512];
  VOID           *Buffer;
  UINTN          Size;
  BOOLEAN        Started;

  mDataBuffer = NULL;
  ResetDownload ();
  mFinished = FALSE;
  mReboot   = FALSE;
  Started   = FALSE;
  Status    = gBS->LocateProtocol (&gAndroidFastbootPlatformProtocolGuid, NULL, (VOID **)&mPlatform);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = mPlatform->Init ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FastbootTransportInit ();
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  // Stop is also required if Start fails after publishing DMA resources.
  Started = TRUE;
  Status  = FastbootTransportStart ();
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  Print (
    L"USB Fastboot ready. Waiting for host commands.\r\n"
    L"ESC or fastboot continue exits.\r\n"
    );
  while (!mFinished) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status) && (Key.ScanCode == SCAN_ESC)) {
      Status = EFI_SUCCESS;
      break;
    }

    if (mState == ExpectDataState) {
      Size   = mNumDataBytes - mBytesReceivedSoFar;
      Buffer = mDataBuffer + mBytesReceivedSoFar;
    } else {
      Size   = sizeof (Command);
      Buffer = Command;
    }

    Status = FastbootTransportReceive (&Size, Buffer, mState == ExpectDataState);
    if (Status == EFI_NOT_READY) {
      if ((mState == ExpectDataState) &&
          (GetTimeInNanoSecond (GetPerformanceCounter () - mLastData) >= DOWNLOAD_TIMEOUT_NS))
      {
        ResetDownload ();
        Status = StopTransport ();
        if (!EFI_ERROR (Status)) {
          Status = FastbootTransportStart ();
        }

        if (EFI_ERROR (Status)) {
          break;
        }

        Print (L"Download timed out. Waiting for a new connection.\r\n");
      }

      gBS->Stall (20);
      continue;
    }

    if (!EFI_ERROR (Status)) {
      Status = mState == ExpectDataState ? AcceptData (Size) : AcceptCmd (Size, Buffer);
    }

    if (EFI_ERROR (Status)) {
      ResetDownload ();
      if (!FastbootTransportCanReconnect () ||
          ((Status != EFI_DEVICE_ERROR) && (Status != EFI_TIMEOUT) && (Status != EFI_MEDIA_CHANGED)))
      {
        break;
      }

      if (Status == EFI_TIMEOUT) {
        // A local send timeout does not tell the provider to discard its queue.
        Status = StopTransport ();
        if (!EFI_ERROR (Status)) {
          Status = FastbootTransportStart ();
        }

        if (EFI_ERROR (Status)) {
          break;
        }
      }

      // GBL providers handle disconnection and reconnection themselves.
      gBS->Stall (20);
    }
  }

Out:
  if (Started) {
    StopStatus = StopTransport ();
    if (EFI_ERROR (StopStatus)) {
      Status = StopStatus;
    }
  }

  FastbootTransportUnInit ();
  ResetDownload ();
  mPlatform->UnInit ();
  if (mReboot && !EFI_ERROR (Status)) {
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}
