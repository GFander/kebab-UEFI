/** @file
  XNU-003b: reboot into the Qualcomm bootloader (fastboot).

  The platform exposes no reset-reason setter and no bootloader-mode service,
  so this follows the Android convention: write a bootloader control block
  with the "bootonce-bootloader" command into the misc partition and reset.

  misc is located by parsing the GPT directly from the whole-disk BlockIo,
  because gEfiPartitionInfoProtocolGuid may not be installed on this
  platform.  Everything is reported with Print(), not DEBUG() - the
  application's DebugLib does not reach ConOut here.

  Copyright (c) 2026. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>

#define BCB_CMD      "bootonce-bootloader"
#define BCB_SIZE     2048

#pragma pack (1)
typedef struct {
  CHAR8    Command[32];
  CHAR8    Status[32];
  CHAR8    Recovery[768];
  CHAR8    Stage[32];
  CHAR8    Reserved[1184];
} BOOTLOADER_MESSAGE;

typedef struct {
  UINT8     Signature[8];
  UINT32    Revision;
  UINT32    HeaderSize;
  UINT32    HeaderCrc;
  UINT32    Reserved;
  UINT64    CurrentLba;
  UINT64    BackupLba;
  UINT64    FirstUsable;
  UINT64    LastUsable;
  UINT8     DiskGuid[16];
  UINT64    EntryLba;
  UINT32    EntryCount;
  UINT32    EntrySize;
  UINT32    EntriesCrc;
} GPT_HEADER;

typedef struct {
  UINT8     TypeGuid[16];
  UINT8     UniqueGuid[16];
  UINT64    FirstLba;
  UINT64    LastLba;
  UINT64    Attributes;
  CHAR16    Name[36];
} GPT_ENTRY;
#pragma pack ()

STATIC
BOOLEAN
NameIsMisc (
  IN CONST CHAR16  *Name
  )
{
  STATIC CONST CHAR16  Target[] = L"misc";
  UINTN                i;

  for (i = 0; i < 4; i++) {
    CHAR16 c = Name[i];
    if ((c >= L'A') && (c <= L'Z')) {
      c = (CHAR16)(c - L'A' + L'a');
    }
    if (c != Target[i]) {
      return FALSE;
    }
  }
  return TRUE;
}

EFI_STATUS
EFIAPI
BootToBlEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE            *Handles = NULL;
  UINTN                  Count = 0;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL *Disk = NULL;
  EFI_BLOCK_IO_PROTOCOL *Blk;
  UINT32                 BlockSize = 0;
  UINT8                 *Buf = NULL;
  GPT_HEADER            *Hdr;
  GPT_ENTRY             *Entries;
  UINTN                  EntriesBytes;
  UINT64                 MiscLba = 0;
  UINTN                  Shown = 0;
  BOOTLOADER_MESSAGE    *Bcb;

  if (gST->ConOut != NULL) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }

  Print (L"XNU-BL: scanning BlockIo handles\n");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"XNU-BL: no BlockIo at all: %r\n", Status);
    gBS->Stall (10 * 1000 * 1000);
    return Status;
  }

  Print (L"XNU-BL: %d BlockIo handle(s)\n", (UINT32)Count);
  for (Index = 0; Index < Count; Index++) {
    Blk = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Blk)) ||
        (Blk == NULL) || (Blk->Media == NULL))
    {
      continue;
    }

    if (Shown < 8) {
      Print (L"XNU-BL:   [%d] logical=%d blockSize=%d last=%ld\n",
             (UINT32)Index, Blk->Media->LogicalPartition, (UINT32)Blk->Media->BlockSize,
             (UINT64)Blk->Media->LastBlock);
      Shown++;
    }

    if (!Blk->Media->LogicalPartition) {
      Disk      = Blk;
      BlockSize = Blk->Media->BlockSize;
    }
  }

  if ((Disk == NULL) || (BlockSize < 512) || (BlockSize > 4096)) {
    Print (L"XNU-BL: no whole-disk BlockIo\n");
    FreePool (Handles);
    gBS->Stall (10 * 1000 * 1000);
    return EFI_NOT_FOUND;
  }

  Print (L"XNU-BL: whole disk blockSize=%d\n", BlockSize);

  Buf = AllocatePool (BlockSize);
  if (Buf == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, 1, BlockSize, Buf);
  if (EFI_ERROR (Status)) {
    Print (L"XNU-BL: read GPT header failed: %r\n", Status);
    goto Out;
  }

  Hdr = (GPT_HEADER *)Buf;
  if (CompareMem (Hdr->Signature, "EFI PART", 8) != 0) {
    Print (L"XNU-BL: no GPT signature at LBA1\n");
    Status = EFI_NOT_FOUND;
    goto Out;
  }

  Print (L"XNU-BL: GPT entries LBA=%ld count=%d size=%d\n",
         (UINT64)Hdr->EntryLba, Hdr->EntryCount, Hdr->EntrySize);

  EntriesBytes = (UINTN)Hdr->EntryCount * Hdr->EntrySize;
  Entries      = AllocatePool (EntriesBytes);
  if (Entries == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Out;
  }

  {
    UINTN Sectors = (EntriesBytes + BlockSize - 1) / BlockSize;
    Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, Hdr->EntryLba, Sectors * BlockSize, Entries);
    if (EFI_ERROR (Status)) {
      Print (L"XNU-BL: read GPT entries failed: %r\n", Status);
      FreePool (Entries);
      goto Out;
    }
  }

  for (Index = 0; Index < Hdr->EntryCount; Index++) {
    GPT_ENTRY *E = (GPT_ENTRY *)((UINT8 *)Entries + Index * Hdr->EntrySize);
    BOOLEAN    Empty;

    Empty = TRUE;
    for (UINTN b = 0; b < 16; b++) {
      if (E->TypeGuid[b] != 0) {
        Empty = FALSE;
        break;
      }
    }
    if (Empty) {
      continue;
    }

    if (Shown < 40) {
      Print (L"XNU-BL:   part %d \"%s\" first=%ld\n", (UINT32)Index, E->Name, (UINT64)E->FirstLba);
      Shown++;
    }

    if (NameIsMisc (E->Name)) {
      MiscLba = E->FirstLba;
      Print (L"XNU-BL: misc found at LBA %ld\n", (UINT64)MiscLba);
      break;
    }
  }
  FreePool (Entries);

  if (MiscLba == 0) {
    Print (L"XNU-BL: no partition named misc\n");
    Status = EFI_NOT_FOUND;
    goto Out;
  }

  Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, MiscLba, BlockSize, Buf);
  if (EFI_ERROR (Status)) {
    Print (L"XNU-BL: read misc failed: %r\n", Status);
    goto Out;
  }

  Bcb = (BOOTLOADER_MESSAGE *)Buf;
  ZeroMem (Bcb, BCB_SIZE);
  CopyMem (Bcb->Command, BCB_CMD, sizeof (BCB_CMD));

  Print (L"XNU-BL: writing BCB \"%a\"\n", BCB_CMD);
  Status = Disk->WriteBlocks (Disk, Disk->Media->MediaId, MiscLba, BlockSize, Buf);
  Print (L"XNU-BL: WriteBlocks = %r\n", Status);
  if (EFI_ERROR (Status)) {
    goto Out;
  }

  ZeroMem (Buf, BlockSize);
  Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, MiscLba, BlockSize, Buf);
  Print (L"XNU-BL: readback = %r command=\"%a\"\n", Status, ((BOOTLOADER_MESSAGE *)Buf)->Command);
  if (EFI_ERROR (Status) || (CompareMem (((BOOTLOADER_MESSAGE *)Buf)->Command, BCB_CMD, sizeof (BCB_CMD)) != 0)) {
    Print (L"XNU-BL: BCB did not stick, aborting\n");
    Status = EFI_DEVICE_ERROR;
    goto Out;
  }

  Print (L"XNU-BL: resetting into fastboot\n");
  gBS->Stall (1500 * 1000);
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);

Out:
  gBS->Stall (15 * 1000 * 1000);
  if (Buf != NULL) {
    FreePool (Buf);
  }
  FreePool (Handles);
  return Status;
}
