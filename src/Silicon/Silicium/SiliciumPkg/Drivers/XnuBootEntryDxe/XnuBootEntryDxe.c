/** @file
  XNU-002a-2: append a boot option for the UniXNU application.

  Differences from the official UniXNUDxe:
    - the option is appended at the END of BootOrder;
    - an existing option for the same file is reused, never duplicated;
    - nothing else in BootOrder/BootNext is touched.

  Clean-up (from an OS or a UEFI shell):
    remove the "UniXNU" entry from BootOrder, then delete Boot#### .
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Protocol/FirmwareVolume2.h>
#include <Guid/GlobalVariable.h>
#include <Guid/EventGroup.h>

#define UNIXNU_APP_GUID \
  { 0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }}

STATIC EFI_GUID  mAppGuid   = UNIXNU_APP_GUID;
STATIC CHAR16    mDesc[]    = L"UniXNU";
STATIC CHAR8     mOptional[] = "unixnu:firmware";
STATIC EFI_EVENT mReadyToBoot = NULL;
STATIC BOOLEAN   mDone = FALSE;

STATIC
EFI_STATUS
FindAppFv (
  OUT EFI_HANDLE *FvHandle
  )
{
  EFI_STATUS                     St;
  EFI_HANDLE                    *Handles = NULL;
  UINTN                          Count = 0;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv = NULL;
  EFI_FV_FILETYPE                Type;
  EFI_FV_FILE_ATTRIBUTES         Attr;
  UINT32                         Auth;
  UINTN                          Size = 0;

  *FvHandle = NULL;
  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    return St;
  }
  for (UINTN i = 0; i < Count; i++) {
    Fv = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv)) || (Fv == NULL)) {
      continue;
    }
    if (!EFI_ERROR (Fv->ReadFile (Fv, &mAppGuid, NULL, &Size, &Type, &Attr, &Auth))) {
      *FvHandle = Handles[i];
      break;
    }
  }
  FreePool (Handles);
  return (*FvHandle != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
EFI_DEVICE_PATH_PROTOCOL *
BuildFilePath (IN EFI_HANDLE FvHandle)
{
  EFI_DEVICE_PATH_PROTOCOL *FvPath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *Node;
  EFI_DEVICE_PATH_PROTOCOL *Out;

  FvPath = DevicePathFromHandle (FvHandle);
  if (FvPath == NULL) {
    return NULL;
  }
  Node = AllocateZeroPool (sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH));
  if (Node == NULL) {
    return NULL;
  }
  Node->Header.Type    = MEDIA_DEVICE_PATH;
  Node->Header.SubType = 0x06;   /* PIWG firmware file */
  SetDevicePathNodeLength (&Node->Header, sizeof (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH));
  CopyGuid (&Node->FvFileName, &mAppGuid);

  Out = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)Node);
  FreePool (Node);
  return Out;
}

STATIC
EFI_STATUS
GetBootOrder (
  OUT UINT16 **Order,
  OUT UINTN   *Count
  )
{
  EFI_STATUS St;
  UINTN      Size = 0;

  *Order = NULL;
  *Count = 0;
  St = gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
  if (St == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (St != EFI_BUFFER_TOO_SMALL) {
    return St;
  }
  *Order = AllocatePool (Size);
  if (*Order == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  St = gRT->GetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid, NULL, &Size, *Order);
  if (EFI_ERROR (St)) {
    FreePool (*Order);
    *Order = NULL;
    return St;
  }
  *Count = Size / sizeof (UINT16);
  return EFI_SUCCESS;
}

//
// Move Number to the END of BootOrder (append if absent).  BDS registers the
// platform options (Internal Storage / USB Storage / FFU Mode) in
// PlatformBootManagerBeforeConsole(), i.e. AFTER every DXE driver has run,
// so appending from a DXE entry point would actually put UniXNU FIRST.
// This runs at ReadyToBoot, when the platform list already exists.
//
STATIC
EFI_STATUS
MakeLast (
  IN UINT16 Number
  )
{
  EFI_STATUS St;
  UINT16    *Order = NULL;
  UINTN      Count = 0;
  UINT16    *New = NULL;
  UINTN      Out = 0;
  BOOLEAN    Changed = FALSE;

  St = GetBootOrder (&Order, &Count);
  if (EFI_ERROR (St)) {
    return St;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderBefore = %d entries, first = %04x\n",
          (UINT32)Count, (Count > 0) ? Order[0] : 0xFFFF));

  New = AllocatePool ((Count + 1) * sizeof (UINT16));
  if (New == NULL) {
    if (Order != NULL) FreePool (Order);
    return EFI_OUT_OF_RESOURCES;
  }
  for (UINTN i = 0; i < Count; i++) {
    if (Order[i] == Number) {
      Changed = TRUE;                 // drop it from its current position
      continue;
    }
    New[Out++] = Order[i];
  }
  if ((Count == 0) || (Out != Count) || !(Count > 0 && Order[Count - 1] == Number)) {
    Changed = TRUE;
  }
  New[Out++] = Number;

  if (Changed) {
    St = gRT->SetVariable (EFI_BOOT_ORDER_VARIABLE_NAME, &gEfiGlobalVariableGuid,
                           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                           Out * sizeof (UINT16), New);
  } else {
    St = EFI_SUCCESS;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderWrite  = %r (%s)\n", St, Changed ? "moved to end" : "already last"));

  if (!EFI_ERROR (St)) {
    DEBUG ((EFI_D_ERROR, "[XNU-BOOT] BootOrderAfter  = %d entries, first = %04x\n",
            (UINT32)Out, New[0]));
    DEBUG ((EFI_D_ERROR, "[XNU-BOOT] DefaultPreserved= %s\n",
            ((Count > 0) && (New[0] == Order[0])) ? "1" : ((Count == 0) ? "1 (was empty)" : "0")));
  }

  FreePool (New);
  if (Order != NULL) FreePool (Order);
  return St;
}

STATIC
VOID
EFIAPI
XnuBootEntryReadyToBoot (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  EFI_STATUS                St;
  EFI_HANDLE                FvHandle = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Dp;
  UINTN                     DpSize;
  UINT8                    *Option;
  UINTN                     OptionSize;
  UINT16                    Number;
  CHAR16                    Name[9];
  UINT32                    Attr = 0;

  if (mDone) {
    DEBUG ((EFI_D_ERROR, "[XNU-BOOT] ReadyToBoot    = repeat (ignored)\n"));
    return;
  }
  mDone = TRUE;

  St = FindAppFv (&FvHandle);
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] AppFv          = %r\n", St));
  if (EFI_ERROR (St)) {
    return;
  }

  Dp = BuildFilePath (FvHandle);
  if (Dp == NULL) {
    return;
  }
  DpSize = GetDevicePathSize (Dp);

  OptionSize = sizeof (UINT32) + sizeof (UINT16) + sizeof (mDesc) + DpSize + sizeof (mOptional);
  Option     = AllocateZeroPool (OptionSize);
  if (Option == NULL) {
    FreePool (Dp);
    return;
  }
  Attr = LOAD_OPTION_ACTIVE;
  CopyMem (Option, &Attr, sizeof (UINT32));
  *(UINT16 *)(Option + sizeof (UINT32)) = (UINT16)DpSize;
  CopyMem (Option + sizeof (UINT32) + sizeof (UINT16), mDesc, sizeof (mDesc));
  CopyMem (Option + sizeof (UINT32) + sizeof (UINT16) + sizeof (mDesc), Dp, DpSize);
  CopyMem (Option + sizeof (UINT32) + sizeof (UINT16) + sizeof (mDesc) + DpSize, mOptional, sizeof (mOptional));

  //
  // Reuse an option that already carries our description, otherwise take the
  // first free number in the private range.  Without this, every boot would
  // create a new Boot#### and the list would grow without bound.
  //
  Number = 0xFFFF;
  for (UINT16 N = 0x9800; N < 0x9900; N++) {
    UINTN  VarSize = 0;
    UINT8 *Var = NULL;
    BOOLEAN Ours = FALSE;

    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", N);
    St = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &VarSize, NULL);
    if (St == EFI_NOT_FOUND) {
      if (Number == 0xFFFF) {
        Number = N;                    // remember the first free slot
      }
      continue;
    }
    if ((St != EFI_BUFFER_TOO_SMALL) || (VarSize < 8)) {
      continue;
    }
    Var = AllocatePool (VarSize);
    if (Var == NULL) {
      continue;
    }
    if (!EFI_ERROR (gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &VarSize, Var))) {
      if (((UINT16)StrSize (mDesc) + 8) <= VarSize) {
        if (StrCmp ((CHAR16 *)(Var + 6), mDesc) == 0) {
          Ours = TRUE;
        }
      }
    }
    FreePool (Var);
    if (Ours) {
      Number = N;                      // reuse, do not duplicate
      break;
    }
  }
  if (Number == 0xFFFF) {
    FreePool (Option);
    FreePool (Dp);
    return;
  }
  UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);

  St = gRT->SetVariable (Name, &gEfiGlobalVariableGuid,
                         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                         OptionSize, Option);
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] Create %S      = %r\n", Name, St));
  if (!EFI_ERROR (St)) {
    MakeLast (Number);
  }

  FreePool (Option);
  FreePool (Dp);
}

EFI_STATUS
EFIAPI
XnuBootEntryDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS St;

  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] XnuBootEntryDxe entry (ReadyToBoot hook)\n"));

  //
  // Register at ReadyToBoot, not now: BDS registers the platform options in
  // PlatformBootManagerBeforeConsole() (BootManager.c:48), which runs after
  // every DXE driver.  TPL_CALLBACK is legal here (Event.c only rejects
  // NotifyTpl <= TPL_APPLICATION) and no driver connection happens in the
  // callback, so the TPL is safe.
  //
  St = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, XnuBootEntryReadyToBoot,
                           NULL, &gEfiEventReadyToBootGuid, &mReadyToBoot);
  DEBUG ((EFI_D_ERROR, "[XNU-BOOT] ReadyToBootEvt  = %r\n", St));
  return EFI_SUCCESS;
}
