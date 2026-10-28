/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/AcpiTableUpdateLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SecureBootHandlerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/UfsDeviceConfig.h>
#include <IndustryStandard/Ufs.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/UfsHostController.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/FirmwareVolume2.h>
#include <Guid/GlobalVariable.h>

//
// XNU-002c: 1 = UniXNU is the default boot entry (CATEGORY_BOOT, first
// in BootOrder).  0 = previous behaviour (CATEGORY_APP, appended last,
// manual selection only).  Flip and rebuild to switch.
//
#define XNU_UNIXNU_AUTOBOOT  1


#include <Configuration/BootDevices.h>

#include "BootManager.h"

//
// Global Variables
//
STATIC UINTN XPos = 0;
STATIC UINTN YPos = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

VOID
EFIAPI
PlatformBootManagerBeforeConsole ()
{
  // Signal End Of DXE
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  // Dispatch Deferred Images
  EfiBootManagerDispatchDeferredImages ();

  // Register Boot Options
  MsBootOptionsLibRegisterDefaultBootOptions ();

  // Register Console Variables
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,  NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Execute Secondary Before Console
  DeviceBootManagerBeforeConsole (NULL, NULL);
}

VOID
PostGopSetup (OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **GopProtocol)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *LocalGopProtocol;

  // Verify Parameters
  if (GopProtocol == NULL) {
    return;
  }

  // Locate GOP Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID *)&LocalGopProtocol);
  if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol from Handle! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Check Status
  if (Status == EFI_UNSUPPORTED) {
    // Locate GOP Protocol
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID *)&LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol! Status = %r\n", __FUNCTION__, Status));
      return;
    }

    // Install GOP Protocol
    Status = gBS->InstallProtocolInterface (&gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE, LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Install GOP Protocol onto \"ConsoleOutHandle\"! Status = %r\n", __FUNCTION__, Status));
    }
  }

  // Pass GOP Protocol
  *GopProtocol = LocalGopProtocol;
}


//
// XNU-002a-4: register the UniXNU application as an ADDITIONAL boot option.
//
// Runs from PlatformBootManagerAfterConsole(), i.e. after
// PlatformBootManagerBeforeConsole() has registered Internal Storage /
// USB Storage / FFU Mode, and before BdsEntry fetches the boot option
// array.  That is the only point where the platform list is final but the
// menu has not been built yet -- neither DXE dispatch (too early) nor
// ReadyToBoot (fires inside EfiBootManagerBoot, far too late).
//
// Self-gating: if no FV contains the UniXNU application the function
// returns immediately, so other Mu-Silicium devices are unaffected.
//
#define XNU_APP_FILE_GUID \
  { 0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }}

STATIC EFI_GUID mXnuAppGuid = XNU_APP_FILE_GUID;
STATIC CHAR16   mXnuDesc[]  = L"UniXNU";
//
// UniXNU compares LoadOptions against L"unixnu:firmware" with
// sizeof (CHAR16[16]) = 32 bytes, so the marker MUST be UTF-16.
// An ASCII copy (16 bytes) fails both the size and the memcmp and
// leaves UniXNU in Application host mode, where "Return to
// firmware" does not return EFI_ABORTED.
//
STATIC CHAR16   mXnuOptData[] = L"unixnu:firmware";

STATIC
EFI_STATUS
XnuFindAppFv (
  OUT EFI_HANDLE *FvHandle
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                    *Handles = NULL;
  UINTN                          Count = 0;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  EFI_FV_FILETYPE                Type;
  EFI_FV_FILE_ATTRIBUTES         Attr;
  UINT32                         Auth;
  UINTN                          Size;

  *FvHandle = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return Status;
  }
  for (Index = 0; Index < Count; Index++) {
    Fv = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv)) || (Fv == NULL)) {
      continue;
    }
    Size = 0;
    if (!EFI_ERROR (Fv->ReadFile (Fv, &mXnuAppGuid, NULL, &Size, &Type, &Attr, &Auth))) {
      *FvHandle = Handles[Index];
      break;
    }
  }
  FreePool (Handles);
  return (*FvHandle != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
EFI_DEVICE_PATH_PROTOCOL *
XnuBuildFilePath (
  IN EFI_HANDLE FvHandle
  )
{
  EFI_DEVICE_PATH_PROTOCOL          *FvPath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *Node;
  EFI_DEVICE_PATH_PROTOCOL          *Out;

  FvPath = DevicePathFromHandle (FvHandle);
  if (FvPath == NULL) {
    return NULL;
  }
  Node = AllocateZeroPool (sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH));
  if (Node == NULL) {
    return NULL;
  }
  Node->Header.Type    = MEDIA_DEVICE_PATH;
  Node->Header.SubType = 0x06;                 // PIWG firmware file
  SetDevicePathNodeLength (&Node->Header, sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH));
  CopyGuid (&Node->FvFileName, &mXnuAppGuid);
  Out = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)Node);
  FreePool (Node);
  return Out;
}

//
// Identity check: the option must carry our description AND a firmware-file
// node naming the UniXNU application.  A same-named option that points at a
// different file is therefore never overwritten.
//
STATIC
BOOLEAN
XnuOptionIsOurs (
  IN CONST UINT8 *Option,
  IN UINTN        Size
  )
{
  UINT16                          PathLen;
  CONST CHAR16                   *Desc;
  CONST EFI_DEVICE_PATH_PROTOCOL *Node;
  UINTN                           DescBytes = 0;
  UINTN                           PathStart;
  UINTN                           PathEnd;
  UINTN                           NodeLen;
  UINTN                           NodeOff;

  if ((Option == NULL) || (Size < 8)) {
    return FALSE;
  }
  PathLen = *(CONST UINT16 *)(Option + 4);
  Desc    = (CONST CHAR16 *)(Option + 6);

  //
  // Find a NUL inside the variable BEFORE any string comparison, so a
  // malformed Boot#### cannot make us read past the buffer.
  //
  while (DescBytes + sizeof (CHAR16) <= (Size - 6)) {
    if (Desc[DescBytes / sizeof (CHAR16)] == L'\0') {
      break;
    }
    DescBytes += sizeof (CHAR16);
  }
  if (DescBytes + sizeof (CHAR16) > (Size - 6)) {
    return FALSE;                       // no terminator inside the variable
  }
  DescBytes += sizeof (CHAR16);

  if (StrCmp ((CHAR16 *)Desc, mXnuDesc) != 0) {
    return FALSE;
  }

  //
  // The device path must fit inside the variable AND inside
  // FilePathListLength; walk it only within those bounds.
  //
  PathStart = 6 + DescBytes;
  if ((PathStart > Size) || (PathLen == 0) || (PathLen > (Size - PathStart))) {
    return FALSE;
  }
  PathEnd = PathStart + PathLen;
  Node    = (CONST EFI_DEVICE_PATH_PROTOCOL *)(Option + PathStart);
  while (((UINTN)Node - (UINTN)Option + sizeof (EFI_DEVICE_PATH_PROTOCOL)) <= PathEnd) {
    if (IsDevicePathEnd (Node)) {
      break;
    }
    NodeLen = DevicePathNodeLength (Node);
    NodeOff = (UINTN)Node - (UINTN)Option;
    if (NodeLen < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
      return FALSE;                     // malformed node length
    }
    if (NodeLen > (PathEnd - NodeOff)) {
      return FALSE;                     // node claims to extend past the list
    }
    if ((Node->Type == MEDIA_DEVICE_PATH) && (Node->SubType == 0x06) &&
        (NodeLen >= sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH))) {
      CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *F = (CONST MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Node;
      return (BOOLEAN)CompareGuid (&F->FvFileName, &mXnuAppGuid);
    }
    Node = NextDevicePathNode (Node);
  }
  return FALSE;
}

STATIC
VOID
XnuMakeLast (
  IN UINT16 Number
  )
{
  EFI_STATUS Status;
  UINT16    *Order = NULL;
  UINT16    *New   = NULL;
  UINTN      Size  = 0;
  UINTN      Count = 0;
  UINTN      Out   = 0;
  BOOLEAN    Changed = FALSE;

  Status = gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Order = AllocatePool (Size);
    if (Order == NULL) {
      return;
    }
    Status = gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, Order);
    if (EFI_ERROR (Status)) {
      FreePool (Order);
      return;
    }
    Count = Size / sizeof (UINT16);
  } else if (Status != EFI_NOT_FOUND) {
    return;
  }

  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderBefore = %d entries, first = %04x\n",
          (UINT32)Count, (Count > 0) ? Order[0] : 0xFFFF));

  New = AllocatePool ((Count + 1) * sizeof (UINT16));
  if (New == NULL) {
    if (Order != NULL) FreePool (Order);
    return;
  }
#if XNU_UNIXNU_AUTOBOOT
  //
  // Default-boot mode: UniXNU first, every other entry keeps its order.
  //
  New[Out++] = Number;
#endif
  for (UINTN i = 0; i < Count; i++) {
    if (Order[i] == Number) {
      continue;                        // drop from its current position
    }
    New[Out++] = Order[i];
  }
#if !XNU_UNIXNU_AUTOBOOT
  New[Out++] = Number;
#endif

  //
  // Write whenever the number is not already the last element.  The old
  // condition only fired when the number was ALREADY present, so a brand new
  // option was built in memory but never stored - while the debug line still
  // printed Count+1, which is what made the earlier 4->5 report misleading.
  //
#if XNU_UNIXNU_AUTOBOOT
  Changed = (BOOLEAN) !((Count > 0) && (Order[0] == Number) && (Out == Count));
#else
  Changed = (BOOLEAN) !((Count > 0) && (Order[Count - 1] == Number) && (Out == Count));
#endif

  if (Changed) {
    Status = gRT->SetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid,
                               EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                               Out * sizeof (UINT16), New);
  } else {
    Status = EFI_SUCCESS;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderWrite  = %r (%a)\n", Status, Changed ? "moved to end" : "already last"));
  if (!EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderAfter  = %d entries, first = %04x\n", (UINT32)Out, New[0]));
    DEBUG ((EFI_D_ERROR, "[XNU-BOOT] DefaultPreserved= %d\n", (Count == 0) ? 1 : (New[0] == Order[0])));
  }
  FreePool (New);
  if (Order != NULL) FreePool (Order);
}



STATIC
VOID
XnuTraceOrderNums (
  IN CONST CHAR8 *Where
  )
{
  EFI_STATUS  Status;
  UINT16     *Order = NULL;
  UINTN       Size  = 0;

  Status = gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Order = AllocatePool (Size);
    if (Order != NULL) {
      if (EFI_ERROR (gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, Order))) {
        FreePool (Order);
        Order = NULL;
      }
    }
  }
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] BootOrder (%a) = %d entries:", Where,
          (Order != NULL) ? (UINT32)(Size / sizeof (UINT16)) : 0));
  if (Order != NULL) {
    for (UINTN i = 0; i < Size / sizeof (UINT16); i++) {
      DEBUG ((EFI_D_ERROR, " %04x", Order[i]));
    }
    FreePool (Order);
  }
  DEBUG ((EFI_D_ERROR, "\n"));
}

STATIC
VOID
XnuDumpBootEntry (
  IN UINT16 Number
  )
{
  EFI_STATUS  Status;
  CHAR16      Name[10];
  UINT8      *Var = NULL;
  UINTN       Size = 0;
  UINT32      Attr;
  UINT16      PathLen;
  CHAR16     *Desc;
  UINT8      *Path;
  UINTN       Off;
  UINTN       OptOff;

  UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);
  Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] %S GetVariable(size probe) = %r size = %d\n", Name, Status, (UINT32)Size));
  if ((Status != EFI_BUFFER_TOO_SMALL) || (Size < 8)) {
    return;
  }
  Var = AllocatePool (Size);
  if (Var == NULL) {
    return;
  }
  Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Size, Var);
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] %S read = %r varSize = %d\n", Name, Status, (UINT32)Size));
  if (EFI_ERROR (Status)) {
    FreePool (Var);
    return;
  }

  Attr    = *(UINT32 *)Var;
  PathLen = *(UINT16 *)(Var + 4);
  Desc    = (CHAR16 *)(Var + 6);
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] Attr = 0x%08x (ACTIVE=%d CATEGORY=0x%x)\n",
          Attr, (Attr & LOAD_OPTION_ACTIVE) ? 1 : 0, Attr & LOAD_OPTION_CATEGORY));
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] PathLen = %d  Desc = \"%S\"\n", (UINT32)PathLen, Desc));

  OptOff = 6 + StrSize (Desc);
  Path   = Var + OptOff;
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] OptDataOff = %d OptDataSize = %d\n",
          (UINT32)OptOff + PathLen, (UINT32)(Size - OptOff - PathLen)));

  Off = 0;
  while (Off + 4 <= PathLen) {
    EFI_DEVICE_PATH_PROTOCOL *N = (EFI_DEVICE_PATH_PROTOCOL *)(Path + Off);
    UINT16 L = *(UINT16 *)(Path + Off + 2);
    DEBUG ((EFI_D_ERROR, "[XNU-DUMP]   node@%d type=%d sub=%d len=%d\n",
            (UINT32)Off, N->Type, N->SubType, L));
    if ((N->Type == MEDIA_DEVICE_PATH) && (N->SubType == 0x06) && (L >= 20)) {
      EFI_GUID *G = (EFI_GUID *)(Path + Off + 4);
      DEBUG ((EFI_D_ERROR, "[XNU-DUMP]   FVFILE %g\n", G));
    }
    if (N->Type == END_DEVICE_PATH_TYPE) {
      break;
    }
    if (L < 4) {
      DEBUG ((EFI_D_ERROR, "[XNU-DUMP]   BAD node length, stop\n"));
      break;
    }
    Off += L;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-DUMP] walked %d of PathLen %d (IsDevicePathValid=%d)\n",
          (UINT32)Off, (UINT32)PathLen, IsDevicePathValid ((EFI_DEVICE_PATH_PROTOCOL *)Path, PathLen) ? 1 : 0));
  FreePool (Var);
}


STATIC
VOID
XnuProbeParse (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_BOOT_MANAGER_LOAD_OPTION   Opt;
  EFI_BOOT_MANAGER_LOAD_OPTION  *List = NULL;
  UINTN                          Count = 0;
  BOOLEAN                        Has = FALSE;

  ZeroMem (&Opt, sizeof (Opt));
  Status = EfiBootManagerVariableToLoadOption (L"Boot9800", &Opt);
  DEBUG ((EFI_D_ERROR, "[XNU-PROBE] VariableToLoadOption(Boot9800) = %r\n", Status));
  if (!EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[XNU-PROBE]   num=%04x attr=0x%08x desc=\"%S\"\n",
            (UINT32)Opt.OptionNumber, Opt.Attributes, Opt.Description));
    DEBUG ((EFI_D_ERROR, "[XNU-PROBE]   dpSize=%d optData=%d\n",
            (UINT32)GetDevicePathSize (Opt.FilePath), (UINT32)Opt.OptionalDataSize));
    EfiBootManagerFreeLoadOption (&Opt);
  }

  //
  // Controlled reproduction of what the menu does: this call deletes any
  // option that fails to parse.  Dump the order before and after.
  //
  XnuTraceOrderNums ("before GetLoadOptions probe");
  List = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  DEBUG ((EFI_D_ERROR, "[XNU-PROBE] GetLoadOptions -> %d entries (status %r)\n", (UINT32)Count, (List == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS));
  for (UINTN i = 0; i < Count; i++) {
    DEBUG ((EFI_D_ERROR, "[XNU-PROBE]   [%d] num=%04x attr=0x%08x desc=\"%S\"\n",
            (UINT32)i, (UINT32)List[i].OptionNumber, List[i].Attributes, List[i].Description));
    if (List[i].OptionNumber == 0x9800) {
      Has = TRUE;
    }
  }
  DEBUG ((EFI_D_ERROR, "[XNU-PROBE] 0x9800 survived GetLoadOptions = %d\n", Has ? 1 : 0));
  if (List != NULL) {
    EfiBootManagerFreeLoadOptions (List, Count);
  }
  XnuTraceOrderNums ("after GetLoadOptions probe");
}


//
// XNU-004 stage 1: UFS + vendor IOMMU observation.
//
// Runs from PlatformBootManagerAfterConsole(), i.e. with a live console.  An
// earlier attempt put this in a DXE driver dispatched from APRIORI and it
// asserted in Print() because ConOut does not exist yet.
//
STATIC EFI_GUID  mSmemGuid = { 0xF4E5C7D0, 0xD239, 0x47CB, { 0xAA, 0xCD, 0x7F, 0x66, 0xEF, 0x76, 0x32, 0x38 } };

STATIC
UINT32
XnuRd32 (
  IN UINTN Addr
  )
{
  return *(volatile UINT32 *)Addr;
}

STATIC
VOID
XnuDumpUfs (
  VOID
  )
{
  UINTN B = 0x01D84000;

  DEBUG ((EFI_D_ERROR, "[XNU-UFS2] CAP=0x%08x VER=0x%08x AHIT=0x%08x\n",
          XnuRd32 (B + 0x00), XnuRd32 (B + 0x08), XnuRd32 (B + 0x18)));
  DEBUG ((EFI_D_ERROR, "[XNU-UFS2] IS=0x%08x IE=0x%08x HCS=0x%08x HCE=0x%08x\n",
          XnuRd32 (B + 0x20), XnuRd32 (B + 0x24), XnuRd32 (B + 0x30), XnuRd32 (B + 0x34)));
  DEBUG ((EFI_D_ERROR, "[XNU-UFS2] errPA=0x%08x errDL=0x%08x errN=0x%08x errT=0x%08x errDME=0x%08x\n",
          XnuRd32 (B + 0x38), XnuRd32 (B + 0x3C), XnuRd32 (B + 0x40), XnuRd32 (B + 0x44), XnuRd32 (B + 0x48)));
  DEBUG ((EFI_D_ERROR, "[XNU-UFS2] UTRLBA=0x%08x%08x UTRLDBR=0x%08x UTRLRSR=0x%08x UICCMD=0x%08x\n",
          XnuRd32 (B + 0x54), XnuRd32 (B + 0x50), XnuRd32 (B + 0x58), XnuRd32 (B + 0x60), XnuRd32 (B + 0x90)));
}


STATIC
VOID
XnuWalkVa (
  IN UINT64 Va,
  IN UINT64 Ttbr0,
  IN UINT32 Tcr
  )
{
  UINT32 T0Sz  = Tcr & 0x3F;
  UINT32 Tg0   = (Tcr >> 14) & 0x3;
  UINTN  Bits  = 64 - T0Sz;
  UINTN  Levels;
  UINTN  Level;
  UINT64 Table;

  if (Tg0 != 0) {                       // only 4K granule (TG0 == 0)
    DEBUG ((EFI_D_ERROR, "[XNU-WALK] VA=0x%lx TG0=%d not 4K, skip\n", Va, Tg0));
    return;
  }
  if ((Bits < 21) || (Bits > 48)) {
    DEBUG ((EFI_D_ERROR, "[XNU-WALK] VA=0x%lx T0SZ=%d unsupported\n", Va, T0Sz));
    return;
  }
  Levels = (Bits - 12 + 8) / 9;
  Level  = 4 - Levels;
  Table  = Ttbr0 & 0x0000FFFFFFFFF000ULL;

  while (Level <= 3) {
    UINTN  Shift = 12 + 9 * (3 - Level);
    UINTN  Index = (UINTN)((Va >> Shift) & 0x1FF);
    UINT64 *Entry = (UINT64 *)(UINTN)(Table + Index * 8);
    UINT64  Desc  = *Entry;
    UINT64  BlockSize;

    if ((Desc & 1) == 0) {
      DEBUG ((EFI_D_ERROR, "[XNU-WALK] VA=0x%lx L%d idx=%d invalid -> unmapped\n",
              Va, (UINT32)Level, (UINT32)Index));
      return;
    }
    if ((Desc & 3) == 3) {
      Table = Desc & 0x0000FFFFFFFFF000ULL;
      Level++;
      if (Level > 3) {
        break;
      }
      continue;
    }
    BlockSize = 1ULL << (12 + 9 * (3 - Level));
    {
      UINT64 Pa = (Desc & 0x0000FFFFFFF00000ULL & ~(BlockSize - 1)) | (Va & (BlockSize - 1));
      DEBUG ((EFI_D_ERROR, "[XNU-WALK] VA=0x%lx -> PA=0x%lx at L%d (%s)\n",
              Va, Pa, (UINT32)Level, (Pa == Va) ? "IDENTITY" : "mapped elsewhere"));
    }
    return;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-WALK] VA=0x%lx walk ended without leaf\n", Va));
}

STATIC
VOID
XnuDumpSmmu (
  VOID
  )
{
  UINTN  Base  = 0x15000000;
  UINT32 Id0   = XnuRd32 (Base + 0x20);
  UINT32 Id1   = XnuRd32 (Base + 0x24);
  UINTN  Shift = (Id1 & (1u << 31)) ? 16 : 12;
  UINTN  Gr1   = Base + ((UINTN)1 << Shift);
  UINT32 Groups = Id0 & 0xFF;
  UINT32 Ctx   = Id1 & 0xFF;
  UINT32 Valid = 0, Trans = 0, Byp = 0, Flt = 0;
  UINT32 Shown = 0;

  DEBUG ((EFI_D_ERROR, "[XNU-SMMU2] ID0=0x%08x ID1=0x%08x shift=%d GR1=0x%08x groups=%d ctxBanks=%d\n",
          Id0, Id1, (UINT32)Shift, (UINT32)Gr1, Groups, Ctx));
  DEBUG ((EFI_D_ERROR, "[XNU-SMMU2] sCR0=0x%08x GFSR=0x%08x\n", XnuRd32 (Base + 0x00), XnuRd32 (Base + 0x48)));

  for (UINTN i = 0; i < 64; i++) {
    UINT32 Smr  = XnuRd32 (Base + 0x800 + i * 4);
    UINT32 S2cr = XnuRd32 (Base + 0xC00 + i * 4);
    UINT32 Type;
    if ((Smr & (1u << 31)) == 0) {
      continue;
    }
    Valid++;
    Type = (S2cr >> 16) & 0x3;
    if (Type == 0) {
      Trans++;
    } else if (Type == 1) {
      Byp++;
    } else {
      Flt++;
    }
    if (Shown < 12) {
      DEBUG ((EFI_D_ERROR, "[XNU-SMMU2] slot %2d SID=0x%04x mask=0x%04x S2CR=0x%08x type=%d cb=%d\n",
              (UINT32)i, Smr & 0xFFFF, (Smr >> 16) & 0xFFFF, S2cr, Type, S2cr & 0xFF));
      Shown++;
    }
  }
  DEBUG ((EFI_D_ERROR, "[XNU-SMMU2] valid=%d translate=%d bypass=%d fault=%d\n",
          Valid, Trans, Byp, Flt));

  //
  // Walk context bank 0: does UFS DMA go through a real translation, or is it
  // effectively identity?  Read-only, and image A proved reading the SMMU at
  // this point is safe.
  //
  {
    UINT32 NumPage = 1u << (((Id1 >> 28) & 0x7) + 1);
    UINTN  Cb      = Base + (((UINTN)NumPage + 0) << Shift);
    UINT32 Sctlr   = XnuRd32 (Cb + 0x00);
    UINT32 Tcr     = XnuRd32 (Cb + 0x30);
    UINT32 Tcr2    = XnuRd32 (Cb + 0x10);
    UINT32 Mair0   = XnuRd32 (Cb + 0x38);
    UINT32 Mair1   = XnuRd32 (Cb + 0x3C);
    UINT64 Ttbr0   = (UINT64)XnuRd32 (Cb + 0x20) | ((UINT64)XnuRd32 (Cb + 0x24) << 32);
    UINT32 Cbar0   = XnuRd32 (Gr1 + 0x00);
    UINT32 Cba2r0  = XnuRd32 (Gr1 + 0x800);
    UINT32 Fsr     = XnuRd32 (Cb + 0x58);
    UINT32 Far     = XnuRd32 (Cb + 0x60);
    UINT32 Fsynr0  = XnuRd32 (Cb + 0x68);

    DEBUG ((EFI_D_ERROR, "[XNU-CB0] base=0x%08x SCTLR=0x%08x(M=%d,A=%d) TCR=0x%08x TCR2=0x%08x\n",
            (UINT32)Cb, Sctlr, Sctlr & 1, (Sctlr >> 1) & 1, Tcr, Tcr2));
    DEBUG ((EFI_D_ERROR, "[XNU-CB0] TTBR0=0x%016lx MAIR0=0x%08x MAIR1=0x%08x\n",
            Ttbr0, Mair0, Mair1));
    DEBUG ((EFI_D_ERROR, "[XNU-CB0] CBAR0=0x%08x type=%d (0=S2_TRANS 1=S1_TRANS_S2_BYPASS) CBA2R=0x%08x\n",
            Cbar0, (Cbar0 >> 16) & 0x3, Cba2r0));
    DEBUG ((EFI_D_ERROR, "[XNU-CB0] FSR=0x%08x FAR=0x%08x FSYNR0=0x%08x\n", Fsr, Far, Fsynr0));

    //
    // If stage 1 is off (M=0) the stream is identity regardless of the tables;
    // if stage 2 is bypassed as well there is no translation at all.
    //
    if ((Sctlr & 1) == 0) {
      DEBUG ((EFI_D_ERROR, "[XNU-CB0] stage-1 disabled -> UFS DMA uses physical addresses\n"));
    } else if (Ttbr0 != 0) {
      DEBUG ((EFI_D_ERROR, "[XNU-CB0] stage-1 enabled, T0SZ=%d TG0=%d -> real page table\n",
              Tcr & 0x3F, (Tcr >> 14) & 0x3));
      XnuWalkVa (0x000000009A830000ULL, Ttbr0, Tcr);   // UTRLBA from the controller
      XnuWalkVa (0x0000000080000000ULL, Ttbr0, Tcr);   // sample DRAM
      XnuWalkVa (0x00000000B0000000ULL, Ttbr0, Tcr);   // sample high DRAM
    } else {
      DEBUG ((EFI_D_ERROR, "[XNU-CB0] stage-1 enabled but TTBR0==0\n"));
    }
  }
}

STATIC
VOID
XnuUfsDiag (
  VOID
  )
{
  EFI_STATUS             Status;
  VOID                  *Smem = NULL;
  EFI_HANDLE            *Handles = NULL;
  UINTN                  Count = 0;
  EFI_BLOCK_IO_PROTOCOL *Blk;

  {
    UINT32 V[2];
    UINTN  Sz = sizeof (V);
    EFI_GUID Vg = { 0x7B1C2A44, 0x5E3D, 0x4F6A, { 0x9C, 0x11, 0x8E, 0x2D, 0x4F, 0x6A, 0x1B, 0x23 } };
    if (!EFI_ERROR (gRT->GetVariable (L"XnuSmmuEarlyRead", &Vg, NULL, &Sz, V))) {
      DEBUG ((EFI_D_ERROR, "[XNU-EARLY] APRIORI read ID0=0x%08x ID1=0x%08x\n", V[0], V[1]));
    } else {
      DEBUG ((EFI_D_ERROR, "[XNU-EARLY] no early-read record (driver did not run)\n"));
    }
  }

  Status = gBS->LocateProtocol (&mSmemGuid, NULL, &Smem);
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] SMEM protocol = %r\n", Status));

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] BlockIo handles = %d (%r)\n", (UINT32)Count, Status));
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    for (UINTN i = 0; i < Count; i++) {
      Blk = NULL;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&Blk)) &&
          (Blk != NULL) && (Blk->Media != NULL))
      {
        DEBUG ((EFI_D_ERROR, "[XNU-UFS]   [%d] logical=%d blockSize=%d last=%ld\n",
                (UINT32)i, Blk->Media->LogicalPartition, (UINT32)Blk->Media->BlockSize,
                (UINT64)Blk->Media->LastBlock));
      }
    }
    FreePool (Handles);
  }

  {
    UINT32 V = 0;
    UINTN  Sz = sizeof (V);
    EFI_GUID Tg = { 0x6A1F3C58, 0x2E47, 0x4D90, { 0xB3, 0x8C, 0x51, 0x7A, 0xE2, 0x94, 0x0F, 0x6D } };
    if (!EFI_ERROR (gRT->GetVariable (L"XnuUfsTrace", &Tg, NULL, &Sz, &V))) {
      DEBUG ((EFI_D_ERROR, "[XNU-UFSTRACE] last step = %d (1=entry 2=utrl-stop 3=utmr-stop 4=polled 5=doorbells 6=pre-install 7=installed 0x100=install-failed)\n", V));
    } else {
      DEBUG ((EFI_D_ERROR, "[XNU-UFSTRACE] no marker found\n"));
    }
  }

  XnuDumpUfs ();
  XnuDumpSmmu ();
  // vendor IOMMU probe removed: it mutates domain state

}

STATIC
VOID
XnuRegisterUniXNU (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                FvHandle = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Dp;
  UINTN                     DpSize;
  UINT8                    *Option;
  UINTN                     OptionSize;
  UINT16                    Number = 0xFFFF;
  CHAR16                    Name[10];
#if XNU_UNIXNU_AUTOBOOT
  UINT32                    Attr = LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_BOOT;
#else
  UINT32                    Attr = LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP;
#endif

  Status = XnuFindAppFv (&FvHandle);
  if (EFI_ERROR (Status)) {
    return;                                  // no UniXNU on this device
  }
  Dp = XnuBuildFilePath (FvHandle);
  if (Dp == NULL) {
    return;
  }
  DpSize = GetDevicePathSize (Dp);

  OptionSize = sizeof (UINT32) + sizeof (UINT16) + sizeof (mXnuDesc) + DpSize + sizeof (mXnuOptData);
  Option     = AllocateZeroPool (OptionSize);
  if (Option == NULL) {
    FreePool (Dp);
    return;
  }
  CopyMem (Option, &Attr, sizeof (UINT32));
  *(UINT16 *)(Option + sizeof (UINT32)) = (UINT16)DpSize;
  CopyMem (Option + 6, mXnuDesc, sizeof (mXnuDesc));
  CopyMem (Option + 6 + sizeof (mXnuDesc), Dp, DpSize);
  CopyMem (Option + 6 + sizeof (mXnuDesc) + DpSize, mXnuOptData, sizeof (mXnuOptData));

  for (UINT16 N = 0x9800; N < 0x9900; N++) {
    UINTN  VarSize = 0;
    UINT8 *Var;

    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", N);
    Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &VarSize, NULL);
    if (Status == EFI_NOT_FOUND) {
      if (Number == 0xFFFF) {
        Number = N;
      }
      continue;
    }
    if ((Status != EFI_BUFFER_TOO_SMALL) || (VarSize < 8)) {
      continue;
    }
    Var = AllocatePool (VarSize);
    if (Var == NULL) {
      continue;
    }
    if (!EFI_ERROR (gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &VarSize, Var))) {
      if (XnuOptionIsOurs (Var, VarSize)) {
        Number = N;                          // ours: reuse, never duplicate
        FreePool (Var);
        break;
      }
    }
    FreePool (Var);
  }
  if (Number == 0xFFFF) {
    FreePool (Option);
    FreePool (Dp);
    return;
  }
  UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);

  Status = gRT->SetVariable (Name, &gEfiGlobalVariableGuid,
                             EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                             OptionSize, Option);
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] Create %S      = %r\n", Name, Status));
  if (!EFI_ERROR (Status)) {
    XnuMakeLast (Number);
  }
  FreePool (Option);
  FreePool (Dp);
}


//
// UFS bring-up at BDS.  Doing this here instead of in a DXE driver means the
// console exists, so every step is visible, and EDK2's UfsPassThru messages
// reach the screen too.
//
#define XNU_UFS_HC_BASE  0x01D84000ULL
#define XNU_UFS_VERBOSE  0

STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL  mXnuUfsHc;
STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL      mXnuUfsPlat;
STATIC EFI_HANDLE                          gXnuUfsPassThruImage = NULL;

//
// All DMA buffers come from this one uncached pool.  Marking arbitrary pages
// uncached is not safe: the attribute survives FreePages, so once EDK2 reuses
// the page for ordinary data an unaligned access faults (Device memory forbids
// it).  Own the memory instead and never hand it back.
//
#define XNU_UFS_POOL_PAGES  64
STATIC EFI_PHYSICAL_ADDRESS  mXnuUfsPoolBase = 0;
STATIC UINT64                mXnuUfsPoolMap = 0;   // bit set = page in use

//
// ArmCacheMaintenanceLib implements WriteBackDataCache() as ASSERT(FALSE), so a
// whole-cache flush is not available.  Remember every CommonBuffer mapping and
// write those ranges back explicitly at the doorbell.
//

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} XNU_UFS_DP;
#pragma pack ()

STATIC XNU_UFS_DP  mXnuUfsDp = {
  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x3C1E7A54, 0x9D28, 0x4B67, { 0x8E, 0x15, 0xA2, 0x6F, 0x4D, 0x39, 0xC7, 0x10 } } },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_STATUS EFIAPI XnuUfsGetBar (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, UINTN *Bar) { *Bar = (UINTN)XNU_UFS_HC_BASE; return EFI_SUCCESS; }
STATIC
VOID *
XnuUfsPoolAlloc (
  IN UINTN  Pages
  )
{
  EFI_STATUS  St;
  VOID        *P;

  if (Pages == 0) {
    return NULL;
  }

  if (mXnuUfsPoolBase == 0) {
    St = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, XNU_UFS_POOL_PAGES, &mXnuUfsPoolBase);
    if (EFI_ERROR (St)) {
      return NULL;
    }
    //
    // Normal non-cacheable (EFI_MEMORY_WC), not EFI_MEMORY_UC: UC maps to
    // Device memory, which additionally forbids unaligned accesses.  If the
    // attribute cannot be set the pool is useless, so give it back.
    //
    St = gDS->SetMemorySpaceAttributes (mXnuUfsPoolBase, XNU_UFS_POOL_PAGES * EFI_PAGE_SIZE, EFI_MEMORY_UC);
    DEBUG ((EFI_D_ERROR, "[XNU-UFSHC] pool %d pages at 0x%lx, set UC = %r\n",
            XNU_UFS_POOL_PAGES, (UINT64)mXnuUfsPoolBase, St));
    if (EFI_ERROR (St)) {
      gBS->FreePages (mXnuUfsPoolBase, XNU_UFS_POOL_PAGES);
      mXnuUfsPoolBase = 0;
      return NULL;
    }
  }

  if (Pages > XNU_UFS_POOL_PAGES) {
    return NULL;
  }

  for (UINTN start = 0; start + Pages <= XNU_UFS_POOL_PAGES; start++) {
    UINT64 Need = (((UINT64)1 << Pages) - 1) << start;
    if ((mXnuUfsPoolMap & Need) == 0) {
      mXnuUfsPoolMap |= Need;
      P = (VOID *)(UINTN)(mXnuUfsPoolBase + start * EFI_PAGE_SIZE);
      return P;
    }
  }
  return NULL;
}

STATIC
VOID
XnuUfsPoolFree (
  IN VOID   *Addr,
  IN UINTN   Pages
  )
{
  UINTN  Start;

  if ((mXnuUfsPoolBase == 0) || (Addr == NULL) || (Pages == 0) ||
      (Pages > XNU_UFS_POOL_PAGES) || ((UINTN)Addr < (UINTN)mXnuUfsPoolBase)) {
    return;
  }
  Start = ((UINTN)Addr - (UINTN)mXnuUfsPoolBase) / EFI_PAGE_SIZE;
  if (Start + Pages > XNU_UFS_POOL_PAGES) {
    return;
  }
  mXnuUfsPoolMap &= ~((((UINT64)1 << Pages) - 1) << Start);
}

STATIC EFI_STATUS EFIAPI XnuUfsAlloc (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, EFI_ALLOCATE_TYPE Ty, EFI_MEMORY_TYPE Mt, UINTN Pg, VOID **Ha, UINT64 At)
{
  VOID  *P;

  if (Ha == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  P = XnuUfsPoolAlloc (Pg);
  if (P == NULL) {
    DEBUG ((EFI_D_ERROR, "[XNU-UFSHC] Alloc %d page(s) FAILED, map=0x%lx\n", (UINT32)Pg, mXnuUfsPoolMap));
    return EFI_OUT_OF_RESOURCES;
  }
  *Ha = P;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI XnuUfsFree (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, UINTN Pg, VOID *Ha)
{
  XnuUfsPoolFree (Ha, Pg);
  return EFI_SUCCESS;
}

//
// Most SCSI buffers do NOT come from our AllocateBuffer: UfsPassThru hands us
// the caller's buffer (a stack buffer for MODE SENSE, a pool allocation for
// READ CAPACITY).  Those are cached, and the SMMU being bypassed says nothing
// about cache coherency.  So bounce them through the non-cacheable pool; that
// is correct by construction and needs no per-transfer cache maintenance.
//
#define XNU_UFS_MAX_MAPS  16
STATIC struct {
  VOID   *Host;
  VOID   *Bounce;     // NULL => mapped in place
  UINTN   Len;
  UINT32  Op;
  UINTN   Pages;
} mMaps[XNU_UFS_MAX_MAPS];
STATIC UINTN  mMapsN = 0;

STATIC EFI_STATUS EFIAPI XnuUfsMap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, EDKII_UFS_HOST_CONTROLLER_OPERATION Op, VOID *Ha, UINTN *N, EFI_PHYSICAL_ADDRESS *Da, VOID **Mp)
{
  if ((Ha == NULL) || (N == NULL) || (Da == NULL) || (Mp == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // CommonBuffer already comes from the non-cacheable pool: the caller keeps
  // writing into the host pointer and expects the device address to alias it.
  // Bouncing it would hand the controller a different (empty) buffer.
  //
  if (Op == EdkiiUfsHcOperationBusMasterCommonBuffer) {
    if (mMapsN < XNU_UFS_MAX_MAPS) {
      mMaps[mMapsN].Host   = Ha;
      mMaps[mMapsN].Bounce = NULL;
      mMaps[mMapsN].Len    = *N;
      mMaps[mMapsN].Op     = (UINT32)Op;
      mMaps[mMapsN].Pages  = 0;
      mMapsN++;
    }
    *Da = (EFI_PHYSICAL_ADDRESS)(UINTN)Ha;
    *Mp = Ha;
    return EFI_SUCCESS;
  }

  //
  // CommonBuffer always comes from the pool (Device memory): identity, no
  // maintenance.  Everything else belongs to the caller and is cacheable, so
  // clean before the device reads it and invalidate around the device writes.
  //
  if (Op == EdkiiUfsHcOperationBusMasterRead) {
    WriteBackDataCacheRange (Ha, *N);
  } else if (Op == EdkiiUfsHcOperationBusMasterWrite) {
    InvalidateDataCacheRange (Ha, *N);
  }

  if (mMapsN < XNU_UFS_MAX_MAPS) {
    mMaps[mMapsN].Host   = Ha;
    mMaps[mMapsN].Bounce = NULL;
    mMaps[mMapsN].Len    = *N;
    mMaps[mMapsN].Op     = (UINT32)Op;
    mMaps[mMapsN].Pages  = 0;
    mMapsN++;
  }
  *Da = (EFI_PHYSICAL_ADDRESS)(UINTN)Ha;
  *Mp = Ha;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI XnuUfsUnmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, VOID *Mp)
{
  UINTN  i;

  for (i = 0; i < mMapsN; i++) {
    if (mMaps[i].Host != Mp) {
      continue;
    }
    if (mMaps[i].Op == (UINT32)EdkiiUfsHcOperationBusMasterWrite) {
      InvalidateDataCacheRange (mMaps[i].Host, mMaps[i].Len);
    }
    mMaps[i] = mMaps[mMapsN - 1];
    mMapsN--;
    return EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI XnuUfsFlush (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T)
{
  MemoryFence ();
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI XnuUfsRead (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Off, UINTN Cnt, VOID *Buf)
{
  UINTN A = (UINTN)XNU_UFS_HC_BASE + (UINTN)Off;
  if ((Buf == NULL) || (Cnt == 0) || (Off >= 0x1C000)) return EFI_INVALID_PARAMETER;
  for (UINTN i = 0; i < Cnt; i++) {
    switch (W) {
      case EfiUfsHcWidthUint8:  ((UINT8 *)Buf)[i]  = MmioRead8  (A + i);     break;
      case EfiUfsHcWidthUint16: ((UINT16 *)Buf)[i] = MmioRead16 (A + i * 2); break;
      case EfiUfsHcWidthUint32: ((UINT32 *)Buf)[i] = MmioRead32 (A + i * 4); break;
      case EfiUfsHcWidthUint64: *(UINT64 *)((UINT8 *)Buf + i * 8) = MmioRead64 (A + i * 8); break;
      default: return EFI_INVALID_PARAMETER;
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI XnuUfsWrite (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *T, EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Off, UINTN Cnt, VOID *Buf)
{
  UINTN A = (UINTN)XNU_UFS_HC_BASE + (UINTN)Off;
  if ((Buf == NULL) || (Cnt == 0) || (Off >= 0x1C000)) return EFI_INVALID_PARAMETER;
  for (UINTN i = 0; i < Cnt; i++) {
    switch (W) {
      case EfiUfsHcWidthUint8:  MmioWrite8  (A + i,     ((UINT8 *)Buf)[i]);  break;
      case EfiUfsHcWidthUint16: MmioWrite16 (A + i * 2, ((UINT16 *)Buf)[i]); break;
      case EfiUfsHcWidthUint32:
        if (((Off == 0x50) || (Off == 0x54) || (Off == 0x58) || (Off == 0x60) ||
             (Off == 0x70) || (Off == 0x74) || (Off == 0x78) || (Off == 0x80) || (Off == 0x90))) {
        }
        //
        // The doorbell is the "DMA may start now" signal.  Everything the CPU
        // wrote into the rings and descriptors must be visible in memory
        // before we poke it, otherwise the controller reads stale data.
        //
        if ((Off == 0x58) || (Off == 0x78)) {
          MemoryFence ();
        }
        MmioWrite32 (A + i * 4, ((UINT32 *)Buf)[i]);
        break;
      case EfiUfsHcWidthUint64: MmioWrite64 (A + i * 8, *(UINT64 *)((UINT8 *)Buf + i * 8)); break;
      default: return EFI_INVALID_PARAMETER;
    }
  }
  return EFI_SUCCESS;
}
//
// UIC command over the host controller registers (UFS 2.0 5.3).  Returns the
// UIC result code (0 = success) and, for DME_GET, the value in *OutArg3.
//
STATIC UINT32
XnuUicCmd (
  UINT32  Opcode,
  UINT32  Arg1,
  UINT32  Arg2,
  UINT32  Arg3,
  UINT32  *OutArg2,
  UINT32  *OutArg3
  )
{
  UINTN   Base = (UINTN)XNU_UFS_HC_BASE;
  UINT32  Is, V;
  UINTN   i;

  Is = MmioRead32 (Base + 0x20);
  if ((Is & BIT10) != 0) {
    MmioWrite32 (Base + 0x20, BIT10);
  }
  for (i = 0; i < 10000; i++) {
    if ((MmioRead32 (Base + 0x30) & BIT3) != 0) {
      break;
    }
    gBS->Stall (1);
  }
  MmioWrite32 (Base + 0x94, Arg1);
  MmioWrite32 (Base + 0x98, Arg2);
  MmioWrite32 (Base + 0x9C, Arg3);
  MmioWrite32 (Base + 0x90, Opcode);
  for (i = 0; i < 10000; i++) {
    if ((MmioRead32 (Base + 0x20) & BIT10) != 0) {
      break;
    }
    gBS->Stall (1);
  }
  V = MmioRead32 (Base + 0x98);
  if (OutArg2 != NULL) {
    *OutArg2 = V;
  }
  if (OutArg3 != NULL) {
    *OutArg3 = MmioRead32 (Base + 0x9C);
  }
  MmioWrite32 (Base + 0x20, BIT10);
  return V & 0xFF;
}

/**
  Move the UFS link from PWM to HS.

  EDK2 never negotiates a gear: it runs link startup and leaves whatever the
  bootloader set, which on kebab is PWM, ~0.5 MB/s.  The device rejects most
  combinations outright; the one it accepts is HS-G3, one lane, UniPro series
  B, with PA_TxHSAdaptType = 3 (no adaptation).  Two lanes never complete the
  transition, series A produces Check Condition on every read, and adapt
  types 1 are rejected with 0x2.

  Measured result: 0.5 MB/s -> 421 MB/s with 256 KiB reads.
**/
STATIC
VOID
XnuUfsGearNegotiate (
  VOID
  )
{
  UINT32  R2, R3, Is, Hcs;
  UINTN   Base = (UINTN)XNU_UFS_HC_BASE;

  // The device reference clock output must be on before HS (REG_UFS_CFG1.26).
  MmioWrite32 (Base + 0xDC, MmioRead32 (Base + 0xDC) | BIT26);
  (VOID)MmioRead32 (Base + 0xDC);
  gBS->Stall (1);

  MmioWrite32 (Base + 0x20, 0x410);                 // clear UCCS | UPMS

  // PA_RxTermination, PA_TxTermination, PA_HSSeries = B
  XnuUicCmd (2, 0x15840000U, 0, 1, &R2, &R3);
  XnuUicCmd (2, 0x15690000U, 0, 1, &R2, &R3);
  XnuUicCmd (2, 0x156A0000U, 0, 2, &R2, &R3);
  // PA_RxGear, PA_TxGear = 3
  XnuUicCmd (2, 0x15830000U, 0, 3, &R2, &R3);
  XnuUicCmd (2, 0x15680000U, 0, 3, &R2, &R3);
  // PA_ActiveRxDataLanes, PA_ActiveTxDataLanes = 1
  XnuUicCmd (2, 0x15800000U, 0, 1, &R2, &R3);
  XnuUicCmd (2, 0x15600000U, 0, 1, &R2, &R3);
  // PA_TxHSAdaptType = 3 (PA_NO_ADAPT)
  XnuUicCmd (2, 0x15D40000U, 0, 3, &R2, &R3);

  // PA_PwrMode = 0x11: RX FAST | TX FAST
  XnuUicCmd (2, 0x15710000U, 0, 0x11, &R2, &R3);

  Is  = MmioRead32 (Base + 0x20);
  Hcs = MmioRead32 (Base + 0x30);
  MmioWrite32 (Base + 0x20, 0x410);

  DEBUG ((EFI_D_ERROR, "[XNU-GEAR] HS transition rc=0x%x UPMS=%d UPMCRS=%d\n",
          (UINT32)(R2 & 0xFF), (UINT32)((Is >> 4) & 1), (UINT32)((Hcs >> 8) & 7)));
}

STATIC EFI_STATUS EFIAPI XnuUfsOvr (EFI_HANDLE H, EDKII_UFS_HC_INFO *I) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI XnuUfsCb (EFI_HANDLE H, EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE P, VOID *D)
{
  switch (P) {
    case EdkiiUfsHcPreHce:         DEBUG ((EFI_D_ERROR, "[XNU-UFSCB] PreHce\n"));         break;
    case EdkiiUfsHcPostHce:        DEBUG ((EFI_D_ERROR, "[XNU-UFSCB] PostHce\n"));        break;
    case EdkiiUfsHcPreLinkStartup: DEBUG ((EFI_D_ERROR, "[XNU-UFSCB] PreLinkStartup\n")); break;
    case EdkiiUfsHcPostLinkStartup:
      DEBUG ((EFI_D_ERROR, "[XNU-UFSCB] PostLinkStartup\n"));
      // negotiate after init: switching gear here breaks the unit descriptor
      // reads that EDK2 performs immediately afterwards.
      break;
    default: break;
  }
  return EFI_SUCCESS;
}


//
// UfsPassThruDxe and the SCSI drivers have a DEPEX on protocols that only
// appear when we install them at BDS, i.e. after the DXE dispatcher has
// finished.  They are therefore still sitting in the FV, unloaded.  Pull them
// out and start them by hand so they run with a live console and we can see
// their diagnostics.
//


//
// The three EDK2 drivers live in our own FAT volume (the FV search cannot see
// them: they sit in the inner decompressed FV which is not exposed as an FV
// protocol handle).  Read them from the volume and start them here, with the
// console alive.
//

//
// LoadImage rejects a NULL ParentImageHandle, so hand it a real loaded image.
//
STATIC
EFI_HANDLE
XnuParentImage (
  VOID
  )
{
  EFI_STATUS  St;
  EFI_HANDLE *H = NULL;
  UINTN       N = 0;
  EFI_HANDLE  Pick = NULL;

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &N, &H);
  if (!EFI_ERROR (St) && (H != NULL) && (N > 0)) {
    Pick = H[0];
    FreePool (H);
  }
  return Pick;
}

STATIC
EFI_STATUS
XnuLoadVolumeDriver (
  IN CONST CHAR16  *Path
  )
{
  EFI_STATUS                       St;
  EFI_HANDLE                      *Handles = NULL;
  UINTN                            Count = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  EFI_FILE_INFO                   *Info;
  UINTN                            InfoSize;
  VOID                            *Buf = NULL;
  UINTN                            Size = 0;
  EFI_HANDLE                       Img = NULL;
  UINTN                            ExitSize = 0;
  CHAR16                          *ExitData = NULL;
  EFI_HANDLE                       Parent;

  Parent = XnuParentImage ();
  

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[XNU-UFSBU]   no SimpleFileSystem (%r)\n", St));
    return St;
  }

  for (UINTN i = 0; i < Count; i++) {
    Sfs = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs)) || (Sfs == NULL)) {
      continue;
    }
    Root = NULL;
    if (EFI_ERROR (Sfs->OpenVolume (Sfs, &Root)) || (Root == NULL)) {
      continue;
    }
    File = NULL;
    St   = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (St) || (File == NULL)) {
      Root->Close (Root);
      continue;
    }

    InfoSize = 0;
    St = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (St != EFI_BUFFER_TOO_SMALL) {
      File->Close (File); Root->Close (Root); continue;
    }
    Info = AllocatePool (InfoSize);
    St = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (EFI_ERROR (St)) {
      FreePool (Info); File->Close (File); Root->Close (Root); continue;
    }
    Size = (UINTN)Info->FileSize;
    FreePool (Info);

    Buf = AllocatePool (Size);
    if (Buf != NULL) {
      St = File->Read (File, &Size, Buf);
      if (!EFI_ERROR (St) && (Size > 0)) {
        DEBUG ((EFI_D_ERROR, "[XNU-UFSBU]   %s: read %d bytes from volume %d\n", Path, (UINT32)Size, (UINT32)i));
        St = gBS->LoadImage (FALSE, Parent, NULL, Buf, Size, &Img);
        DEBUG ((EFI_D_ERROR, "[XNU-UFSBU]   LoadImage  = %r img=%p\n", St, Img));
        if (!EFI_ERROR (St) && (StrCmp ((CHAR16 *)Path, L"\\UFSPT.EFI") == 0)) {
          gXnuUfsPassThruImage = Img;
        }
        if (!EFI_ERROR (St)) {
          St = gBS->StartImage (Img, &ExitSize, &ExitData);
          DEBUG ((EFI_D_ERROR, "[XNU-UFSBU]   StartImage = %r\n", St));
        }
      }
      FreePool (Buf);
    }
    File->Close (File);
    Root->Close (Root);
    FreePool (Handles);
    return St;
  }

  FreePool (Handles);
  DEBUG ((EFI_D_ERROR, "[XNU-UFSBU]   %s: not found on any volume\n", Path));
  return EFI_NOT_FOUND;
}


//
// The vendor HALIOMMU must give the UFS streams a domain before the controller
// starts DMA.  Leaving S2CR at TRANSLATE with a context bank whose stage-1 is
// disabled makes the first transfer fault (SError).
//
STATIC EFI_GUID  mXnuHalIommuGuid = { 0x54B6D3B4, 0x5D33, 0x4F91, { 0x86, 0x00, 0x6C, 0x41, 0xD5, 0xDE, 0xB1, 0x9A } };

typedef EFI_STATUS (EFIAPI *XNU_HAL_GET_OPS)(VOID ***Ops);
typedef EFI_STATUS (EFIAPI *XNU_HAL_CREATE)(VOID **Dom);
typedef EFI_STATUS (EFIAPI *XNU_HAL_CONFIG)(VOID *Dom, UINT32 Mode);
typedef EFI_STATUS (EFIAPI *XNU_HAL_ATTACH)(VOID *Dom, CHAR8 *Name, UINT32 Arid, UINT32 Arg);

STATIC
VOID
XnuUfsIommuSetup (
  VOID
  )
{
  EFI_STATUS  St;
  VOID        *Iface = NULL;
  VOID        **Ops = NULL;
  VOID        *Dom = NULL;

  St = gBS->LocateProtocol (&mXnuHalIommuGuid, NULL, &Iface);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] locate=%r iface=%p\n", St, Iface));
  if (EFI_ERROR (St) || (Iface == NULL)) {
    return;
  }

  St = ((XNU_HAL_GET_OPS)((VOID **)Iface)[1])(&Ops);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] getops=%r ops=%p\n", St, Ops));
  if (EFI_ERROR (St) || (Ops == NULL)) {
    return;
  }

  St = ((XNU_HAL_CREATE)Ops[0])(&Dom);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] create=%r dom=%p\n", St, Dom));
  if (EFI_ERROR (St) || (Dom == NULL)) {
    return;
  }

  St = ((XNU_HAL_CONFIG)Ops[8])(Dom, 0);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] configure(mode0) raw=0x%x\n", (UINT32)St));

  St = ((XNU_HAL_ATTACH)Ops[2])(Dom, "\\_SB_.UFS0", 0x00, 0);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] attach ARID 0x00 raw=0x%x\n", (UINT32)St));

  St = ((XNU_HAL_ATTACH)Ops[2])(Dom, "\\_SB_.UFS0", 0x10, 0);
  DEBUG ((EFI_D_ERROR, "[XNU-IOMMU] attach ARID 0x10 raw=0x%x\n", (UINT32)St));
}


//
// Read the GPT straight off the UFS disk.  This is the point of the whole
// exercise: proving we can see the real partitions.
//

//
// Read the first block of a named partition and show its magic.  The GPT only
// proves we can parse metadata; this proves we can read payload bytes.
//
//
// Print every GPT entry whose name contains one of the interesting substrings.
// Exact-name lookups already failed, so this rules out naming differences.
//
//
// Gate-LU-Inventory: unit descriptors + every BlockIo handle, classified.
// Raw LU handles have LogicalPartition = FALSE; GPT partitions have TRUE and
// carry an EFI_PARTITION_INFO_PROTOCOL with the partition name.
//
//
// Read the first block of a named GPT partition and show its magic.
//

//
// One-line-per-item summary.  Records gXnuUfsBootBBlocks so the
// loader can use boot_b later without re-walking the GPT.
//
STATIC UINT64  gXnuUfsBootBBlocks = 0;

//
// Payload container inside boot_b.
//
// boot_b layout (measured with avbtool on our own image):
//   0x000000 .. 0x71C000   boot image + kernel + ramdisk  (AVB hashes exactly this)
//   0x71C000 .. 0x71C540   embedded vbmeta
//   0x71C540 .. 0x5FFFFC0  zero, NOT hashed  <-- payload lives here
//   0x5FFFFC0 .. end       AVB footer, must never be touched
//
#define XNU_PAYLOAD_BASE     0x00800000ULL          // 8 MiB, 1 MiB aligned
#define XNU_PAYLOAD_MAX      0x05600000ULL          // ~86 MiB, stops before the footer
#define XNU_PAYLOAD_MAGIC    "XNUPAY01"

#pragma pack(1)
typedef struct {
  CHAR8   Magic[8];
  UINT32  Version;
  UINT32  HeaderSize;
  UINT64  Generation;
  UINT64  KernelOffset;
  UINT64  KernelLength;
  UINT64  RamdiskOffset;
  UINT64  RamdiskLength;
  UINT32  KernelCrc32;
  UINT32  RamdiskCrc32;
  UINT32  HeaderCrc32;       // over the header with this field zeroed
  UINT8   Reserved[512 - 68];
} XNU_PAYLOAD_HEADER;
#pragma pack()

STATIC EFI_BLOCK_IO  *mXnuUfsBootB = NULL;

STATIC
VOID
XnuUfsDumpLus (
  VOID
  )
{
  EFI_STATUS                      St;
  EFI_UFS_DEVICE_CONFIG_PROTOCOL  *Cfg = NULL;

  St = gBS->LocateProtocol (&gEfiUfsDeviceConfigProtocolGuid, NULL, (VOID **)&Cfg);
  if (EFI_ERROR (St) || (Cfg == NULL)) {
    return;
  }
  for (UINT8 i = 0; i < 8; i++) {
    UFS_UNIT_DESC  D;
    UINT32         Sz = sizeof (D);

    ZeroMem (&D, sizeof (D));
    if (EFI_ERROR (Cfg->RwUfsDescriptor (Cfg, TRUE, UfsUnitDesc, i, 0, (UINT8 *)&D, &Sz))) {
      continue;
    }
    if (D.LunEn != 1) {
      continue;
    }
    DEBUG ((EFI_D_ERROR, "[XNU-UFS] LU%d boot=%d wp=%d mem=0x%x blocks=0x%lx x %dB\n",
            i, D.BootLunId, D.LunWriteProt, D.MemType,
            SwapBytes64 (D.LogicBlkCount),
            (D.LogicBlkSize < 63) ? (UINT32)(1U << D.LogicBlkSize) : 0));
  }
}

STATIC
BOOLEAN
XnuUfsPayloadCheck (
  VOID
  )
{
  XNU_PAYLOAD_HEADER  Hdr;
  UINT8               *Raw;
  UINTN               Bs;
  UINT64              Lba;
  EFI_STATUS          St;
  XNU_PAYLOAD_HEADER  Copy;
  UINT32              Crc;

  if (mXnuUfsBootB == NULL) {
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] boot_b not found, skipping\n"));
    return FALSE;
  }
  Bs  = mXnuUfsBootB->Media->BlockSize;
  Lba = XNU_PAYLOAD_BASE / Bs;

  Raw = AllocatePool (Bs);
  if (Raw == NULL) {
    return FALSE;
  }
  St = mXnuUfsBootB->ReadBlocks (mXnuUfsBootB, mXnuUfsBootB->Media->MediaId, Lba, Bs, Raw);
  if (EFI_ERROR (St)) {
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] read header = %r\n", St));
    FreePool (Raw);
    return FALSE;
  }
  CopyMem (&Hdr, Raw, sizeof (Hdr));
  FreePool (Raw);

  if (CompareMem (Hdr.Magic, XNU_PAYLOAD_MAGIC, 8) != 0) {
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] no payload (magic=\"%a\")\n", Hdr.Magic));
    return FALSE;
  }
  if ((Hdr.Version != 1) || (Hdr.HeaderSize != sizeof (Hdr))) {
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] bad header ver=%d size=%d\n", Hdr.Version, Hdr.HeaderSize));
    return FALSE;
  }

  CopyMem (&Copy, &Hdr, sizeof (Copy));
  Copy.HeaderCrc32 = 0;
  Crc = CalculateCrc32 (&Copy, sizeof (Copy));
  if (Crc != Hdr.HeaderCrc32) {
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] header CRC mismatch (0x%x != 0x%x)\n", Crc, Hdr.HeaderCrc32));
    return FALSE;
  }

  DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] container OK gen=%ld\n", Hdr.Generation));
  DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] kernel  off=0x%lx len=0x%lx crc=0x%x\n",
          Hdr.KernelOffset, Hdr.KernelLength, Hdr.KernelCrc32));
  DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] ramdisk off=0x%lx len=0x%lx crc=0x%x\n",
          Hdr.RamdiskOffset, Hdr.RamdiskLength, Hdr.RamdiskCrc32));

  //
  // Verify both blobs by reading them back through the same path.
  //
  for (UINTN which = 0; which < 2; which++) {
    UINT64  Off = (which == 0) ? Hdr.KernelOffset  : Hdr.RamdiskOffset;
    UINT64  Len = (which == 0) ? Hdr.KernelLength  : Hdr.RamdiskLength;
    UINT32  Want = (which == 0) ? Hdr.KernelCrc32  : Hdr.RamdiskCrc32;
    UINT8   *Buf;
    UINTN   Pages;

    if ((Len == 0) || (Off + Len > XNU_PAYLOAD_MAX)) {
      DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] %a out of range\n", (which == 0) ? "kernel" : "ramdisk"));
      return FALSE;
    }
    Pages = EFI_SIZE_TO_PAGES ((UINTN)Len);
    Buf   = AllocatePages (Pages);
    if (Buf == NULL) {
      return FALSE;
    }
    St = mXnuUfsBootB->ReadBlocks (
           mXnuUfsBootB,
           mXnuUfsBootB->Media->MediaId,
           (XNU_PAYLOAD_BASE + Off) / Bs,
           EFI_PAGES_TO_SIZE (Pages),
           Buf
           );
    if (EFI_ERROR (St)) {
      DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] %a read = %r\n", (which == 0) ? "kernel" : "ramdisk", St));
      FreePages (Buf, Pages);
      return FALSE;
    }
    Crc = CalculateCrc32 (Buf, (UINTN)Len);
    DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] %a CRC 0x%x %a\n",
            (which == 0) ? "kernel" : "ramdisk", Crc, (Crc == Want) ? "OK" : "MISMATCH"));
    FreePages (Buf, Pages);
    if (Crc != Want) {
      return FALSE;
    }
  }

  DEBUG ((EFI_D_ERROR, "[XNU-PAYLOAD] result = PASS\n"));
  return TRUE;
}


STATIC
VOID
XnuPlatformFacts (
  VOID
  )
{
  EFI_STATUS  St;
  UINT8       *Map = NULL;
  UINTN       Size = 0;
  UINTN       Key = 0, DescSize = 0;
  UINT32      DescVer = 0;
  UINT64      Top = 0, Sum = 0;
  UINT64      Frq, Mpidr;
  UINT32      GicdPidr2, GicrTyper0;

  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (Frq));
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (Mpidr));
  GicdPidr2  = *(volatile UINT32 *)(UINTN)(0x17A00000 + 0xFFE8);
  GicrTyper0 = *(volatile UINT32 *)(UINTN)(0x17A60000 + 0x08);
  DEBUG ((EFI_D_ERROR, "[XNU-HW] cntfrq=%ld mpidr=0x%lx gicd_pidr2=0x%x gicr_typer=0x%x\n",
          (UINT64)Frq, (UINT64)Mpidr, GicdPidr2, GicrTyper0));

  St = gBS->GetMemoryMap (&Size, NULL, &Key, &DescSize, &DescVer);
  if (St != EFI_BUFFER_TOO_SMALL) {
    return;
  }
  Size += 16 * DescSize;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    return;
  }
  St = gBS->GetMemoryMap (&Size, (EFI_MEMORY_DESCRIPTOR *)Map, &Key, &DescSize, &DescVer);
  if (!EFI_ERROR (St)) {
    UINTN Count = Size / DescSize;
    for (UINTN i = 0; i < Count; i++) {
      EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)(Map + i * DescSize);
      UINT64                End;
      if (D->Type > EfiConventionalMemory) {
        continue;
      }
      End = D->PhysicalStart + (D->NumberOfPages << 12);
      if (End > Top) {
        Top = End;
      }
      Sum += D->NumberOfPages << 12;
    }
    DEBUG ((EFI_D_ERROR, "[XNU-HW] memmap regions=%d top=0x%lx ram_total=0x%lx dram_base=0x80000000 dram_size=0x%lx\n",
            (UINT32)Count, (UINT64)Top, (UINT64)Sum,
            (UINT64)((Top > 0x80000000ULL) ? (Top - 0x80000000ULL) : 0)));
  }
  FreePool (Map);
}


//
// ARM generic-timer counter: 19.2 MHz on this SoC, far better resolution than
// EFI_TIME, whose one-second granularity made every previous measurement read
// as "1 s".
//
STATIC UINT64
XnuCntpct (
  VOID
  )
{
  UINT64  V;
  __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r" (V));
  return V;
}

STATIC UINT64
XnuCntfrq (
  VOID
  )
{
  UINT64  V;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (V));
  return V;
}

STATIC
VOID
__attribute__((unused))
XnuUfsBench (
  VOID
  )
{
  EFI_STATUS  St;
  UINT64      T0c, T1c;
  UINTN       Bs, Chunk, Blocks, Done;
  UINT8       *Buf;
  INTN        Elapsed;

  if (mXnuUfsBootB == NULL) {
    return;
  }
  Bs    = mXnuUfsBootB->Media->BlockSize;
  Chunk = Bs * 64;                                  // 256 KiB: the largest pass below
  Buf   = AllocatePool (Chunk);
  if (Buf == NULL) {
    return;
  }
  Blocks = (UINTN)(mXnuUfsBootB->Media->LastBlock + 1);
  if (Blocks > 512) {
    Blocks = 512;                                   // ~2 MiB of reads
  }
  T0c = XnuCntpct ();
  Done = 0;
  for (UINTN i = 0; i + 8 <= Blocks; i += 8) {
    St = mXnuUfsBootB->ReadBlocks (mXnuUfsBootB, mXnuUfsBootB->Media->MediaId, i, Chunk, Buf);
    if (EFI_ERROR (St)) {
      break;
    }
    Done += 8;
  }
  T1c = XnuCntpct ();
  {
    UINT64 Us = (T1c - T0c) * 1000000ULL / XnuCntfrq ();
    if (Us == 0) Us = 1;
    DEBUG ((EFI_D_ERROR, "[XNU-BENCH] chunk=%d n=%d %d us = %d KB/s\n",
            (UINT32)(Bs * 8), (UINT32)Done, (UINT32)Us,
            (UINT32)(((UINT64)Done * Bs / 1024) * 1000000ULL / Us)));
  }

  // Second pass: one block (4 KiB) per call, same total bytes, to separate a
  // fixed per-command cost from a throughput limit.
  T0c = XnuCntpct ();
  Done = 0;
  for (UINTN i = 0; i < Blocks; i++) {
    St = mXnuUfsBootB->ReadBlocks (mXnuUfsBootB, mXnuUfsBootB->Media->MediaId, i, Bs, Buf);
    if (EFI_ERROR (St)) {
      break;
    }
    Done++;
  }
  T1c = XnuCntpct ();
  Elapsed = (INTN)((T1c - T0c) * 1000000ULL / XnuCntfrq ());
  if (Elapsed <= 0) {
    Elapsed = 1;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-BENCH2] chunk=%d n=%d %d us = %d KB/s\n",
          (UINT32)Bs, (UINT32)Done, (UINT32)Elapsed,
          (UINT32)(((UINT64)Done * Bs / 1024) * 1000000ULL / (UINT64)Elapsed)));

  // Third pass: 256 KiB per call, 8 calls for the same 2 MiB.  If this is much
  // faster than the 32 KiB pass the cost is per command, not per byte.
  T0c = XnuCntpct ();
  Done = 0;
  for (UINTN i = 0; i + 64 <= Blocks; i += 64) {
    St = mXnuUfsBootB->ReadBlocks (mXnuUfsBootB, mXnuUfsBootB->Media->MediaId, i, Bs * 64, Buf);
    if (EFI_ERROR (St)) {
      break;
    }
    Done += 64;
  }
  T1c = XnuCntpct ();
  Elapsed = (INTN)((T1c - T0c) * 1000000ULL / XnuCntfrq ());
  if (Elapsed <= 0) {
    Elapsed = 1;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-BENCH3] chunk=%d n=%d %d us = %d KB/s\n",
          (UINT32)(Bs * 64), (UINT32)Done, (UINT32)Elapsed,
          (UINT32)(((UINT64)Done * Bs / 1024) * 1000000ULL / (UINT64)Elapsed)));
  FreePool (Buf);
}


STATIC
VOID
XnuMemMapDetail (
  VOID
  )
{
  EFI_STATUS  St;
  UINT8       *Map = NULL;
  UINTN       Size = 0, Key = 0, DescSize = 0;
  UINT32      DescVer = 0;
  UINT64      TypeTotal[16] = { 0 };
  UINT64      BestBase = 0, BestSize = 0, CurBase = 0, CurSize = 0;

  St = gBS->GetMemoryMap (&Size, NULL, &Key, &DescSize, &DescVer);
  if (St != EFI_BUFFER_TOO_SMALL) {
    return;
  }
  Size += 32 * DescSize;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    return;
  }
  St = gBS->GetMemoryMap (&Size, (EFI_MEMORY_DESCRIPTOR *)Map, &Key, &DescSize, &DescVer);
  if (!EFI_ERROR (St)) {
    UINTN Count = Size / DescSize;
    for (UINTN i = 0; i < Count; i++) {
      EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)(Map + i * DescSize);
      UINT64                Bytes = D->NumberOfPages << 12;
      if (D->Type < 16) {
        TypeTotal[D->Type] += Bytes;
      }
      if (D->Type == EfiConventionalMemory) {
        if ((CurSize != 0) && (D->PhysicalStart == CurBase + CurSize)) {
          CurSize += Bytes;
        } else {
          CurBase = D->PhysicalStart;
          CurSize = Bytes;
        }
        if (CurSize > BestSize) {
          BestSize = CurSize;
          BestBase = CurBase;
        }
      } else {
        CurSize = 0;
      }
    }
    DEBUG ((EFI_D_ERROR, "[XNU-MMAP] conventional=0x%lx loader=0x%lx/0x%lx bootsvc=0x%lx/0x%lx rt=0x%lx/0x%lx acpi=0x%lx nvs=0x%lx reserved=0x%lx mmio=0x%lx\n",
            (UINT64)TypeTotal[EfiConventionalMemory],
            (UINT64)TypeTotal[EfiLoaderCode], (UINT64)TypeTotal[EfiLoaderData],
            (UINT64)TypeTotal[EfiBootServicesCode], (UINT64)TypeTotal[EfiBootServicesData],
            (UINT64)TypeTotal[EfiRuntimeServicesCode], (UINT64)TypeTotal[EfiRuntimeServicesData],
            (UINT64)TypeTotal[EfiACPIReclaimMemory], (UINT64)TypeTotal[EfiACPIMemoryNVS],
            (UINT64)TypeTotal[EfiReservedMemoryType], (UINT64)TypeTotal[EfiMemoryMappedIO]));
    DEBUG ((EFI_D_ERROR, "[XNU-MMAP] largest contiguous conventional: base=0x%lx size=0x%lx (%ld MiB)\n",
            (UINT64)BestBase, (UINT64)BestSize, (UINT64)(BestSize >> 20)));
    if (BestBase + BestSize > 0x380000000ULL) {
      DEBUG ((EFI_D_WARN, "[XNU-MMAP] WARNING: conventional memory extends past 0x380000000 (12 GiB DRAM end)\n"));
    }
    {
      UINT64  Bs[6] = { 0 }, Bb[6] = { 0 };
      UINT64  Cb = 0, Cs = 0;
      UINTN   N = 0;
      for (UINTN i = 0; i < Count; i++) {
        EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)(Map + i * DescSize);
        UINT64                B2 = D->NumberOfPages << 12;
        if (D->Type != EfiConventionalMemory) {
          Cs = 0;
          continue;
        }
        if ((Cs != 0) && (D->PhysicalStart == Cb + Cs)) {
          Cs += B2;
        } else {
          Cb = D->PhysicalStart;
          Cs = B2;
        }
        for (UINTN k = 0; k < 6; k++) {
          if (Cs > Bs[k]) {
            for (UINTN j = 5; j > k; j--) {
              Bs[j] = Bs[j-1];
              Bb[j] = Bb[j-1];
            }
            Bs[k] = Cs;
            Bb[k] = Cb;
            break;
          }
        }
      }
      for (N = 0; N < 6; N++) {
        if (Bs[N] == 0) {
          break;
        }
        DEBUG ((EFI_D_ERROR, "[XNU-MMAP-DUMP] block %d: base=0x%lx end=0x%lx size=0x%lx (%ld MiB)\n",
                (UINT32)(N+1), (UINT64)Bb[N], (UINT64)(Bb[N]+Bs[N]), (UINT64)Bs[N], (UINT64)(Bs[N]>>20)));
      }
    }
  }
  FreePool (Map);
}


STATIC
VOID
XnuCheckLoader (
  VOID
  )
{
  EFI_GUID    LoaderGuid = { 0x0f0a492e, 0x5a6a, 0x49f6,
                             { 0xa1, 0xbf, 0x74, 0x3c, 0x87, 0xab, 0x1c, 0xfe } };
  VOID        *Loader = NULL;
  EFI_STATUS  St;

  St = gBS->LocateProtocol (&LoaderGuid, NULL, &Loader);
  DEBUG ((EFI_D_ERROR, "[XNU-LDR] DarwinBoot loader protocol: %r  instance=%p\n", St, Loader));
  if (EFI_ERROR (St)) {
    DEBUG ((EFI_D_WARN, "[XNU-LDR] driver not dispatched or protocol not installed\n"));
  }
}


STATIC
VOID
XnuUfsSummary (
  VOID
  )
{
  EFI_STATUS  St;
  EFI_HANDLE  *Hs = NULL;
  UINTN       N = 0;
  UINTN       Raw = 0;
  UINTN       Part = 0;

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &N, &Hs);
  if (EFI_ERROR (St) || (Hs == NULL)) {
    DEBUG ((EFI_D_ERROR, "[XNU-UFS] no BlockIo (%r)\n", St));
    return;
  }

  for (UINTN i = 0; i < N; i++) {
    EFI_BLOCK_IO                *B = NULL;
    EFI_PARTITION_INFO_PROTOCOL *Pi = NULL;
    CHAR16                      Name[37];

    if (EFI_ERROR (gBS->HandleProtocol (Hs[i], &gEfiBlockIoProtocolGuid, (VOID **)&B)) || (B == NULL)) {
      continue;
    }
    if (!B->Media->LogicalPartition) {
      Raw++;
      continue;
    }
    Part++;

    if (EFI_ERROR (gBS->HandleProtocol (Hs[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Pi)) ||
        (Pi == NULL) || (Pi->Type != PARTITION_TYPE_GPT)) {
      continue;
    }
    for (UINTN k = 0; k < 36; k++) {
      Name[k] = Pi->Info.Gpt.PartitionName[k];
    }
    Name[36] = 0;

    if (StrCmp (Name, L"boot_b") == 0) {
      UINT8  *Buf;
      mXnuUfsBootB = B;
      UINTN   Bs = B->Media->BlockSize;

      gXnuUfsBootBBlocks = B->Media->LastBlock + 1;
      Buf = AllocatePool (Bs);
      if (Buf != NULL) {
        St = B->ReadBlocks (B, B->Media->MediaId, 0, Bs, Buf);
        DEBUG ((EFI_D_ERROR, "[XNU-UFS] boot_b: %ld blocks x %d, read=%r, magic=\"%a\"\n",
                gXnuUfsBootBBlocks, (UINT32)Bs, St, (CHAR8 *)Buf));
        FreePool (Buf);
      }
    }
  }
  FreePool (Hs);

  DEBUG ((EFI_D_ERROR, "[XNU-UFS] volumes: raw=%d partitions=%d total=%d\n",
          (UINT32)Raw, (UINT32)Part, (UINT32)N));
  XnuUfsDumpLus ();
  XnuUfsPayloadCheck ();
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] ready\n"));
}


STATIC
VOID
XnuUfsBringUp (
  VOID
  )
{
  EFI_STATUS  St;
  EFI_HANDLE  Handle = NULL;

  XnuUfsIommuSetup ();

  MmioWrite32 ((UINTN)XNU_UFS_HC_BASE + 0x0060, 0);
  MmioWrite32 ((UINTN)XNU_UFS_HC_BASE + 0x0080, 0);
  for (UINTN i = 0; i < 100; i++) {
    if ((MmioRead32 ((UINTN)XNU_UFS_HC_BASE + 0x0060) & 1) == 0) break;
    gBS->Stall (1000);
  }
  MmioWrite32 ((UINTN)XNU_UFS_HC_BASE + 0x005C, 0xFFFFFFFF);   // UTRLCLR
  MmioWrite32 ((UINTN)XNU_UFS_HC_BASE + 0x007C, 0xFFFFFFFF);   // UTMRLCLR

  mXnuUfsHc.GetUfsHcMmioBar = XnuUfsGetBar;
  mXnuUfsHc.AllocateBuffer  = XnuUfsAlloc;
  mXnuUfsHc.FreeBuffer      = XnuUfsFree;
  mXnuUfsHc.Map             = XnuUfsMap;
  mXnuUfsHc.Unmap           = XnuUfsUnmap;
  mXnuUfsHc.Flush           = XnuUfsFlush;
  mXnuUfsHc.Read            = XnuUfsRead;
  mXnuUfsHc.Write           = XnuUfsWrite;

  mXnuUfsPlat.Version         = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  mXnuUfsPlat.OverrideHcInfo  = XnuUfsOvr;
  mXnuUfsPlat.Callback        = XnuUfsCb;
  mXnuUfsPlat.RefClkFreq      = EdkiiUfsCardRefClkFreq19p2Mhz;
  mXnuUfsPlat.SkipHceReenable = FALSE;
  mXnuUfsPlat.SkipLinkStartup = FALSE;

  St = gBS->InstallMultipleProtocolInterfaces (
              &Handle,
              &gEfiDevicePathProtocolGuid,        &mXnuUfsDp,
              &gEdkiiUfsHostControllerProtocolGuid, &mXnuUfsHc,
              &gEdkiiUfsHcPlatformProtocolGuid,     &mXnuUfsPlat,
              NULL);
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] install protocols = %r\n", St));
  if (EFI_ERROR (St)) {
    return;
  }

  XnuLoadVolumeDriver (L"\\UFSPT.EFI");
  XnuLoadVolumeDriver (L"\\SCSIBUS.EFI");
  XnuLoadVolumeDriver (L"\\SCSIDISK.EFI");


  {
    EFI_HANDLE  Drivers[2];
    Drivers[0] = gXnuUfsPassThruImage;
    Drivers[1] = NULL;
    St = gBS->ConnectController (Handle, Drivers, NULL, FALSE);
  }
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] connect UfsPassThru = %r\n", St));


  St = gBS->ConnectController (Handle, NULL, NULL, TRUE);
  DEBUG ((EFI_D_ERROR, "[XNU-UFS] connect SCSI layer = %r\n", St));

  // The payload volume driver needs the UFS/SCSI partition tree that was just
  // created, so it is started here instead of at DXE dispatch.
  St = XnuLoadVolumeDriver (L"\\XNUPAY.EFI");
  DEBUG ((EFI_D_ERROR, "[XNU-PAY] start driver = %r\n", St));

  XnuUfsSummary ();
  XnuPlatformFacts ();
  XnuMemMapDetail ();
  XnuUfsGearNegotiate ();
  XnuCheckLoader ();

  {
    EFI_HANDLE *Hs = NULL;
    UINTN       N  = 0;
    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &N, &Hs))) {

      {
        UINTN  Shown  = 0;
        UINTN  Dumped = 0;
        for (UINTN i = 0; (i < N) && (Shown < 12); i++) {
          EFI_BLOCK_IO  *B = NULL;
          if (EFI_ERROR (gBS->HandleProtocol (Hs[i], &gEfiBlockIoProtocolGuid, (VOID **)&B)) || (B == NULL)) {
            continue;
          }
          DEBUG ((EFI_D_ERROR, "[XNU-GPT] disk %d: blocks=0x%lx blkSize=%d rw=%d\n",
                  (UINT32)i, (UINT64)(B->Media->LastBlock + 1),
                  B->Media->BlockSize, !B->Media->ReadOnly));
          Shown++;
          //
          // Only the big device carries a partition table; dumping GPT for all
          // 128 enumerated LUs would take minutes.
          //
          if ((Dumped == 0) && (B->Media->LastBlock > 0x100000) && (B->Media->BlockSize >= 512)) {
            Dumped++;
          }
        }
      }
      FreePool (Hs);
    }
    Hs = NULL; N = 0;

  }

}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;

  XnuUfsDiag ();
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  // Post Setup GOP
  PostGopSetup (&GopProtocol);

  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  if (!EFI_ERROR (Status) && GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    // Set Combo Message Position
    XPos = (ScreenWidth - AsciiStrLen (ComboMessage) * EFI_GLYPH_WIDTH) / 2;
    YPos = ScreenHeight * 48 / 50;
  }

  // Launch the UFS stack with the console available
  XnuUfsBringUp ();

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();

  //
  // XNU-002b-2 trace: capture the state BEFORE we (re)write anything, so a
  // surviving variable from the previous boot can be told apart from one
  // that never persisted.
  //
  XnuTraceOrderNums ("before register");
  XnuDumpBootEntry (0x9800);

  // XNU-002a-4: append the UniXNU boot option (no-op if absent)
  XnuRegisterUniXNU ();

  XnuTraceOrderNums ("after register");
  XnuDumpBootEntry (0x9800);
  XnuProbeParse ();
  // Probe the exact predicate BDS will apply, without changing state.
  {
    EFI_STATUS St2; UINTN Sz2 = 0; UINT8 *V2 = NULL; UINT16 PL2;
    St2 = gRT->GetVariable (L"Boot9800", &gEfiGlobalVariableGuid, NULL, &Sz2, NULL);
    if (St2 == EFI_BUFFER_TOO_SMALL) {
      V2 = AllocatePool (Sz2);
      if (V2 != NULL && !EFI_ERROR (gRT->GetVariable (L"Boot9800", &gEfiGlobalVariableGuid, NULL, &Sz2, V2))) {
        PL2 = *(UINT16 *)(V2 + 4);
        DEBUG ((EFI_D_ERROR, "[XNU-DUMP] BDS predicate: %d + %d + %d + %d <= %d -> %d\n",
                4, 2, (UINT32)StrSize ((CHAR16 *)(V2 + 6)), (UINT32)PL2, (UINT32)Sz2,
                ((4 + 2 + StrSize ((CHAR16 *)(V2 + 6)) + PL2) <= Sz2) ? 1 : 0));
      }
      if (V2 != NULL) FreePool (V2);
    }
  }
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect ()
{
  // Execute Secondary On Demand
  DeviceBootManagerOnDemandConInConnect ();
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  BOOLEAN                      BootManagerFlag;

  // Unregister Key Callback
  UnregisterKeyCallback ();

  // Get Boot Manager Flag
  BootManagerFlag = EnterBootManager ();
  if (BootManagerFlag == FALSE) {
    return;
  }

  // Get Boot Manager Load Option
  Status = MsBootOptionsLibGetBootManagerMenu (&LoadOption, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to get Boot Manager Load Option! Status = %r", __FUNCTION__, Status));
    return;
  }

  // Execute Boot Manager Load Option
  EfiBootManagerBoot           (&LoadOption);
  EfiBootManagerFreeLoadOption (&LoadOption);
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;

  // Verify Combo Message Position
  if (XPos == 0 && YPos == 0) {
    return;
  }

  // Get Timeout Time
  UINT16 Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color
    UINT8 DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    // Set GOP Draw Color
    Color.Red = Color.Green = Color.Blue = DrawColor;

    // Print Combo Message
    AsciiPrintXY (XPos, YPos, &Color, NULL, ComboMessage);
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  EFI_STATUS Status;

  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display No Boot OS Logo! Status = %r\n", __FUNCTION__, Status));
    Print (L"No Operating System Found! Please make sure that an OS is Installed.\n");
  }

  // Wait 10s
  gBS->Stall (10000000);

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  // Do Cpu Dead Loop
  CpuDeadLoop ();
}

VOID
EFIAPI
PlatformBootManagerBdsEntry ()
{
  EFI_STATUS Status;

  // Setup Secure Boot
  Status = SetupSecureBoot ();
  ASSERT_EFI_ERROR (Status);

  // Update ACPI Tables
  UpdateAcpiTables ();

  // Execute Secondary BDS Entry
  DeviceBootManagerBdsEntry ();
}
