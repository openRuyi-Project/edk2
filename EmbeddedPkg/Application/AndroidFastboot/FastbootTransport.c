/** @file
  Adapt legacy event transports and polled GBL transports to the application.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/AndroidFastbootTransport.h>
#include <Protocol/GblEfiFastbootTransport.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "FastbootTransport.h"

#define SEND_TIMEOUT_NS  2000000000ULL

STATIC FASTBOOT_TRANSPORT_PROTOCOL          *mLegacy;
STATIC GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL  *mPolled;
STATIC EFI_EVENT                            mReceiveEvent;
STATIC EFI_EVENT                            mSendErrorEvent;
STATIC BOOLEAN                              mSendFailed;

STATIC VOID EFIAPI
SendFailed (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mSendFailed = TRUE;
}

EFI_STATUS
FastbootTransportInit (
  VOID
  )
{
  EFI_STATUS  Status;

  mLegacy         = NULL;
  mPolled         = NULL;
  mReceiveEvent   = NULL;
  mSendErrorEvent = NULL;
  Status          = gBS->LocateProtocol (&gGblEfiFastbootTransportProtocolGuid, NULL, (VOID **)&mPolled);
  if (!EFI_ERROR (Status)) {
    if ((mPolled->Revision < GBL_EFI_FASTBOOT_TRANSPORT_PROTOCOL_REVISION) ||
        (mPolled->Start == NULL) || (mPolled->Stop == NULL) ||
        (mPolled->Receive == NULL) || (mPolled->Send == NULL) || (mPolled->Flush == NULL))
    {
      return EFI_UNSUPPORTED;
    }

    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gAndroidFastbootTransportProtocolGuid, NULL, (VOID **)&mLegacy);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Signal-only events keep command execution at TPL_APPLICATION.
  Status = gBS->CreateEvent (0, TPL_APPLICATION, NULL, NULL, &mReceiveEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SendFailed, NULL, &mSendErrorEvent);
  }

  if (EFI_ERROR (Status)) {
    FastbootTransportUnInit ();
  }

  return Status;
}

EFI_STATUS
FastbootTransportStart (
  VOID
  )
{
  mSendFailed = FALSE;
  return mPolled != NULL ? mPolled->Start (mPolled) : mLegacy->Start (mReceiveEvent);
}

EFI_STATUS
FastbootTransportStop (
  VOID
  )
{
  return mPolled != NULL ? mPolled->Stop (mPolled) : mLegacy->Stop ();
}

VOID
FastbootTransportUnInit (
  VOID
  )
{
  if (mReceiveEvent != NULL) {
    gBS->CloseEvent (mReceiveEvent);
    mReceiveEvent = NULL;
  }

  if (mSendErrorEvent != NULL) {
    gBS->CloseEvent (mSendErrorEvent);
    mSendErrorEvent = NULL;
  }
}

BOOLEAN
FastbootTransportCanReconnect (
  VOID
  )
{
  return mPolled != NULL;
}

EFI_STATUS
FastbootTransportSend (
  IN UINTN       Size,
  IN CONST VOID  *Buffer
  )
{
  EFI_STATUS  Status;
  UINT64      Start;
  UINTN       Sent;

  if (mSendFailed) {
    return EFI_DEVICE_ERROR;
  }

  Start = GetPerformanceCounter ();
  if (mPolled == NULL) {
    do {
      Status = mLegacy->Send (Size, Buffer, &mSendErrorEvent);
      if (mSendFailed) {
        return EFI_DEVICE_ERROR;
      }

      if (Status != EFI_NOT_READY) {
        return Status;
      }

      if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= SEND_TIMEOUT_NS) {
        return EFI_TIMEOUT;
      }

      gBS->Stall (20);
    } while (TRUE);
  }

  while (Size != 0) {
    Sent   = Size;
    Status = mPolled->Send (mPolled, &Sent, Buffer);
    if (Status == EFI_NOT_READY) {
      if (GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= SEND_TIMEOUT_NS) {
        return EFI_TIMEOUT;
      }

      gBS->Stall (20);
      continue;
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Sent == 0) || (Sent > Size)) {
      return EFI_PROTOCOL_ERROR;
    }

    Buffer = (CONST UINT8 *)Buffer + Sent;
    Size  -= Sent;
    Start  = GetPerformanceCounter ();
  }

  return mPolled->Flush (mPolled);
}

EFI_STATUS
FastbootTransportReceive (
  IN OUT UINTN  *Size,
  OUT VOID      *Buffer,
  IN BOOLEAN    DataPhase
  )
{
  EFI_STATUS  Status;
  UINTN       Received;
  VOID        *Packet;

  if (mSendFailed) {
    return EFI_DEVICE_ERROR;
  }

  if (mPolled != NULL) {
    return mPolled->Receive (
                      mPolled,
                      Size,
                      Buffer,
                      DataPhase ? GblEfiFastbootRxModeFixedLength : GblEfiFastbootRxModeSinglePacket
                      );
  }

  // Consume the notification, but drain the provider's queue independently.
  gBS->CheckEvent (mReceiveEvent);
  Packet   = NULL;
  Received = 0;
  // Legacy providers update their receive queues from TPL_CALLBACK.
  EFI_TPL  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);

  Status = mLegacy->Receive (&Received, &Packet);
  gBS->RestoreTPL (OldTpl);
  if (!EFI_ERROR (Status)) {
    if (Received > *Size) {
      Status = EFI_BUFFER_TOO_SMALL;
    } else if ((Received != 0) && (Packet == NULL)) {
      Status = EFI_PROTOCOL_ERROR;
    } else if (Received != 0) {
      CopyMem (Buffer, Packet, Received);
    }

    *Size = Received;
    if (Packet != NULL) {
      FreePool (Packet);
    }
  }

  return Status;
}
