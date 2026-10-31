/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "RamDiskImpl.h"
#include <Guid/Fdt.h>
#include <Guid/Acpi.h>
#include <Library/FdtLib.h>

/* DT handoff follows U-Boot fdt_fixup_pmem_region()/fdt_efi_pmem_setup().
 * Linux's of_pmem driver consumes the standard volatile pmem-region binding.
 * These pages must not also be described as ordinary EFI system RAM: Linux
 * would create conflicting struct-page mappings before of_pmem probes them.
 */
typedef struct {
  UINT64    Base;
  UINT64    Size;
} RAM_DISK_RANGE;

STATIC EFI_GET_MEMORY_MAP  mGetMemoryMap;
STATIC RAM_DISK_RANGE      *mRanges;
STATIC UINTN               mRangeCount;
STATIC BOOLEAN             mHooked;

STATIC EFI_STATUS EFIAPI
GetMemoryMap (
  UINTN                  *Size,
  EFI_MEMORY_DESCRIPTOR  *Map,
  UINTN                  *Key,
  UINTN                  *DescriptorSize,
  UINT32                 *Version
  )
{
  EFI_TPL     Old      = gBS->RaiseTPL (TPL_NOTIFY);
  UINTN       Capacity = Size ? *Size : 0;
  EFI_STATUS  Status   = mGetMemoryMap (Size, Map, Key, DescriptorSize, Version);

  if (Status == EFI_BUFFER_TOO_SMALL) {
    if (DescriptorSize && *DescriptorSize &&
        (mRangeCount <= (MAX_UINTN - *Size) / (2 * *DescriptorSize)))
    {
      *Size += 2 * mRangeCount * *DescriptorSize;
    }

    goto Done;
  }

  if (EFI_ERROR (Status) || !mRangeCount) {
    goto Done;
  }

  UINTN  Stride = *DescriptorSize;

  if ((Stride < sizeof (*Map)) || *Size % Stride ||
      (mRangeCount > (MAX_UINTN - *Size) / (2 * Stride)))
  {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  // Splitting a coalesced descriptor can need two extra descriptors per disk.
  if (Capacity < *Size + 2 * mRangeCount * Stride) {
    *Size += 2 * mRangeCount * Stride;
    Status = EFI_BUFFER_TOO_SMALL;
    goto Done;
  }

  UINTN  Count = *Size / Stride;

  for (UINTN Range = 0; Range < mRangeCount; Range++) {
    UINT64  Start = mRanges[Range].Base;
    UINT64  End   = Start + mRanges[Range].Size;
    for (UINTN Index = 0; Index < Count; Index++) {
      EFI_MEMORY_DESCRIPTOR  *Entry = (VOID *)((UINT8 *)Map + Index * Stride);
      if (Entry->Type != EfiReservedMemoryType) {
        continue;
      }

      UINT64  First = Entry->PhysicalStart;
      UINT64  Last  = First + EFI_PAGES_TO_SIZE (Entry->NumberOfPages);
      if ((Last <= Start) || (First >= End)) {
        continue;
      }

      if ((First < Start) && (Last > End)) {
        CopyMem (
          (UINT8 *)Entry + 2 * Stride,
          (UINT8 *)Entry + Stride,
          (Count - Index - 1) * Stride
          );
        EFI_MEMORY_DESCRIPTOR  *Tail = (VOID *)((UINT8 *)Entry + Stride);
        CopyMem (Tail, Entry, Stride);
        Tail->PhysicalStart = End;
        if (Tail->VirtualStart) {
          Tail->VirtualStart += End - First;
        }

        Tail->NumberOfPages  = EFI_SIZE_TO_PAGES (Last - End);
        Entry->NumberOfPages = EFI_SIZE_TO_PAGES (Start - First);
        Count++;
      } else if (First < Start) {
        Entry->NumberOfPages = EFI_SIZE_TO_PAGES (Start - First);
      } else if (Last > End) {
        Entry->PhysicalStart = End;
        if (Entry->VirtualStart) {
          Entry->VirtualStart += End - First;
        }

        Entry->NumberOfPages = EFI_SIZE_TO_PAGES (Last - End);
      } else {
        CopyMem (Entry, (UINT8 *)Entry + Stride, (Count - Index - 1) * Stride);
        Count--;
        Index--;
      }
    }
  }

  *Size = Count * Stride;
  // Keep the original key. No allocation or firmware memory ownership changes
  // occur here; this only excludes the device ranges described in the DT.
Done:
  gBS->RestoreTPL (Old);
  return Status;
}

STATIC BOOLEAN
Reserved (
  CONST RAM_DISK_RANGE  *Range,
  VOID                  *Map,
  UINTN                 Size,
  UINTN                 Stride
  )
{
  UINT64  Position = Range->Base;
  UINT64  End      = Position + Range->Size;

  while (Position < End) {
    UINT64  Next = Position;
    for (UINTN Offset = 0; Offset < Size; Offset += Stride) {
      EFI_MEMORY_DESCRIPTOR  *Entry = (VOID *)((UINT8 *)Map + Offset);
      UINT64                 Last   = Entry->PhysicalStart + EFI_PAGES_TO_SIZE (Entry->NumberOfPages);
      if ((Entry->Type == EfiReservedMemoryType) && (Position >= Entry->PhysicalStart) && (Position < Last)) {
        Next = MIN (End, Last);
        break;
      }
    }

    if (Next == Position) {
      return FALSE;
    }

    Position = Next;
  }

  return TRUE;
}

STATIC EFI_STATUS
Publish (
  VOID            *Source,
  RAM_DISK_RANGE  *Ranges,
  UINTN           Count
  )
{
  CONST UINT32  *Cells;
  INT32         Length;

  Cells = FdtGetProp (Source, 0, "#address-cells", &Length);
  if (!Cells || (Length != 4) || (Fdt32ToCpu (*Cells) != 2)) {
    return EFI_UNSUPPORTED;
  }

  Cells = FdtGetProp (Source, 0, "#size-cells", &Length);
  if (!Cells || (Length != 4) || (Fdt32ToCpu (*Cells) != 2)) {
    return EFI_UNSUPPORTED;
  }

  UINTN  Size  = FdtTotalSize (Source) + Count * 512 + SIZE_4KB;
  UINTN  Pages = EFI_SIZE_TO_PAGES (Size);
  VOID   *Fdt  = AllocateReservedPages (Pages);

  if (!Fdt) {
    return EFI_OUT_OF_RESOURCES;
  }

  INT32  Error = FdtOpenInto (Source, Fdt, EFI_PAGES_TO_SIZE (Pages));
  CHAR8  Path[96], Name[48];

  for (UINTN Index = 0; !Error && Index < mRangeCount; Index++) {
    AsciiSPrint (Path, sizeof (Path), "/pmem@%lx", mRanges[Index].Base);
    INT32  Node = FdtPathOffset (Fdt, Path);
    if (Node >= 0) {
      Error = FdtDelNode (Fdt, Node);
    }

    AsciiSPrint (Path, sizeof (Path), "/reserved-memory/ramdisk@%lx", mRanges[Index].Base);
    Node = FdtPathOffset (Fdt, Path);
    if (!Error && (Node >= 0)) {
      Error = FdtDelNode (Fdt, Node);
    }
  }

  for (UINTN Index = 0; !Error && Index < Count; Index++) {
    UINT32  Reg[4] = {
      CpuToFdt32 ((UINT32)(Ranges[Index].Base >> 32)), CpuToFdt32 ((UINT32)Ranges[Index].Base),
      CpuToFdt32 ((UINT32)(Ranges[Index].Size >> 32)), CpuToFdt32 ((UINT32)Ranges[Index].Size)
    };
    AsciiSPrint (Name, sizeof (Name), "pmem@%lx", Ranges[Index].Base);
    INT32  Node = FdtAddSubnode (Fdt, 0, Name);
    if (Node < 0) {
      Error = Node;
      break;
    }

    Error = FdtSetPropString (Fdt, Node, "compatible", "pmem-region");
    if (!Error) {
      Error = FdtSetPropEmpty (Fdt, Node, "volatile");
    }

    if (!Error) {
      Error = FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg));
    }

    INT32  Parent = FdtPathOffset (Fdt, "/reserved-memory");
    if (Parent < 0) {
      Error = Parent;
      break;
    }

    AsciiSPrint (Name, sizeof (Name), "ramdisk@%lx", Ranges[Index].Base);
    Node = FdtAddSubnode (Fdt, Parent, Name);
    if (Node < 0) {
      Error = Node;
      break;
    }

    if (!Error) {
      Error = FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg));
    }

    if (!Error) {
      Error = FdtSetPropEmpty (Fdt, Node, "no-map");
    }
  }

  EFI_STATUS  Status = Error ? EFI_DEVICE_ERROR : gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);

  if (EFI_ERROR (Status)) {
    FreePages (Fdt, Pages);
    return Status;
  }

  // Keep prior DT copies reserved: a boot loader can retain a previously
  // returned configuration-table pointer until it starts the OS.
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
UpdateFdt (
  RAM_DISK_PRIVATE_DATA  *Add,
  RAM_DISK_PRIVATE_DATA  *Remove
  )
{
  VOID            *Source, *Acpi, *Map = NULL;
  RAM_DISK_RANGE  *Ranges = NULL;
  LIST_ENTRY      *Entry;
  UINTN           Count = 0, MapSize = 0, Key, Stride = 0;
  UINT32          Version;
  EFI_STATUS      Status;

  if (Add && (!EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, &Acpi)) ||
              !EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpiTableGuid, &Acpi))))
  {
    return EFI_SUCCESS; // Retain the established ACPI/NFIT backend.
  }

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Source);
  if (EFI_ERROR (Status)) {
    // An ACPI-only platform needs no FDT publication. If a published disk
    // is being removed, keep its backing memory until it can be unpublished.
    return Remove ? Status : EFI_SUCCESS;
  }

  if (FdtCheckHeader (Source) || (FdtTotalSize (Source) > SIZE_1MB)) {
    return EFI_COMPROMISED_DATA;
  }

  if (Add && (!CompareGuid (&Add->TypeGuid, &gEfiVirtualCdGuid) &&
              !CompareGuid (&Add->TypeGuid, &gEfiVirtualDiskGuid)))
  {
    return EFI_SUCCESS;
  }

  if (Add && ((Add->StartingAddr & (SIZE_2MB - 1)) || !Add->Size ||
              (Add->Size > MAX_UINT64 - (SIZE_2MB - 1))))
  {
    return EFI_SUCCESS;
  }

  if (!mHooked) {
    mGetMemoryMap = gBS->GetMemoryMap;
  }

  Ranges = AllocateZeroPool ((mRangeCount + 1) * sizeof (*Ranges));
  if (!Ranges) {
    return EFI_OUT_OF_RESOURCES;
  }

  BASE_LIST_FOR_EACH (Entry, &RegisteredRamDisks) {
    RAM_DISK_PRIVATE_DATA  *Disk = RAM_DISK_PRIVATE_FROM_THIS (Entry);

    if ((Disk == Remove) || (!Disk->InFdt && (Disk != Add))) {
      continue;
    }

    if (Count > mRangeCount) {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }

    Ranges[Count].Base = Disk->StartingAddr;
    Ranges[Count].Size = ALIGN_VALUE (Disk->Size, SIZE_2MB);
    Count++;
  }
  Status = mGetMemoryMap (&MapSize, NULL, &Key, &Stride, &Version);
  if ((Status != EFI_BUFFER_TOO_SMALL) || !Stride) {
    goto Done;
  }

  MapSize += 8 * Stride;
  Map      = AllocatePool (MapSize);
  if (!Map) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = mGetMemoryMap (&MapSize, Map, &Key, &Stride, &Version);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (UINTN Index = 0; Index < Count; Index++) {
    if (!Reserved (&Ranges[Index], Map, MapSize, Stride)) {
      // Like NFIT publication, only memory preserved across ExitBootServices
      // can be handed to the OS. Pool-backed HII disks remain UEFI-only.
      Status = (Add && !Add->InFdt && (Ranges[Index].Base == Add->StartingAddr)) ?
               EFI_SUCCESS : EFI_COMPROMISED_DATA;
      goto Done;
    }
  }

  Status = Publish (Source, Ranges, Count);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (!mHooked) {
    gBS->GetMemoryMap = GetMemoryMap;
    gBS->Hdr.CRC32    = 0;
    Status            = gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
    ASSERT_EFI_ERROR (Status);
    mHooked = TRUE;
  }

  RAM_DISK_RANGE  *Old = mRanges;
  EFI_TPL         Tpl  = gBS->RaiseTPL (TPL_NOTIFY);

  mRanges     = Ranges;
  mRangeCount = Count;
  Ranges      = NULL;
  if (Add) {
    Add->InFdt = TRUE;
  }

  if (Remove) {
    Remove->InFdt = FALSE;
  }

  gBS->RestoreTPL (Tpl);
  if (Old) {
    FreePool (Old);
  }

  DEBUG ((DEBUG_INFO, "RAM disk FDT: %u disk(s) published\n", Count));
Done:
  if (Ranges) {
    FreePool (Ranges);
  }

  if (Map) {
    FreePool (Map);
  }

  return Status;
}

EFI_STATUS
RamDiskPublishFdt (
  RAM_DISK_PRIVATE_DATA  *Disk
  )
{
  return Disk->InFdt ? EFI_SUCCESS : UpdateFdt (Disk, NULL);
}

EFI_STATUS
RamDiskUnpublishFdt (
  RAM_DISK_PRIVATE_DATA  *Disk
  )
{
  return Disk->InFdt ? UpdateFdt (NULL, Disk) : EFI_SUCCESS;
}

EFI_STATUS
RamDiskFdtUnload (
  VOID
  )
{
  if (mRangeCount) {
    return EFI_ACCESS_DENIED;
  }

  if (!mHooked) {
    return EFI_SUCCESS;
  }

  if (gBS->GetMemoryMap == GetMemoryMap) {
    gBS->GetMemoryMap = mGetMemoryMap;
    gBS->Hdr.CRC32    = 0;
    mHooked           = FALSE;
    if (mRanges) {
      FreePool (mRanges);
      mRanges = NULL;
    }

    return gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  }

  return EFI_ACCESS_DENIED;
}
