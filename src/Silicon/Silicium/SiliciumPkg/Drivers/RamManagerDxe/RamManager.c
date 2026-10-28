/**
  Copyright (c) 2015-2018, 2020, The Linux Foundation. All rights reserved.
  Copyright (c) 2022. Sunflower2333. All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  - Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials provided
    with the distribution.

  - Neither the name of The Linux Foundation nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
  ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/EfiMemoryMapUtilsLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RamManagerLib.h>
#include <Library/SortLib.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PciIo.h>
#include <Protocol/UsbFunctionIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Guid/EventGroup.h>

STATIC
VOID
DumpProtocolCount (
  IN CONST CHAR8 *Name,
  IN EFI_GUID    *Guid
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles = NULL;
  UINTN       Count   = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Count = 0;
  }

  DEBUG ((EFI_D_ERROR, "[DIAG] %a = %d\n", Name, (UINT32)Count));

  if (Handles != NULL) {
    FreePool (Handles);
  }
}

STATIC EFI_GUID mUsbfnPrereq[] = {
  { 0x18A031AB, 0xB443, 0x4D1A, { 0xA5, 0xC0, 0x0C, 0x09, 0x26, 0x1E, 0x9F, 0x71 }},
  { 0x76B95DAD, 0x1E66, 0x4D98, { 0x9C, 0xE7, 0xB2, 0x94, 0xD9, 0x59, 0x68, 0x95 }},
  { 0x157A5C45, 0x21B2, 0x43C5, { 0xBA, 0x7C, 0x82, 0x2F, 0xEE, 0x5F, 0xE5, 0x99 }},
  { 0x665E3FF6, 0x46CC, 0x11D4, { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }},
  { 0x26BACCB1, 0x6F42, 0x11D4, { 0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }},
  { 0x26BACCB2, 0x6F42, 0x11D4, { 0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }},
  { 0x1DA97072, 0xBDDC, 0x4B30, { 0x99, 0xF1, 0x72, 0xA0, 0xB5, 0x6F, 0xFF, 0x2A }},
  { 0x27CFAC87, 0x46CC, 0x11D4, { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }},
  { 0x27CFAC88, 0x46CC, 0x11D4, { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }},
  { 0xB7DFB4E1, 0x052F, 0x449F, { 0x87, 0xBE, 0x98, 0x18, 0xFC, 0x91, 0xB7, 0x33 }},
  { 0xA46423E3, 0x4617, 0x49F1, { 0xB9, 0xFF, 0xD1, 0xBF, 0xA9, 0x11, 0x58, 0x39 }},
  { 0x26BACCB3, 0x6F42, 0x11D4, { 0xBC, 0xE7, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }},
  { 0x6441F818, 0x6362, 0x4E44, { 0xB5, 0x70, 0x7D, 0xBA, 0x31, 0xDD, 0x24, 0x53 }},
  { 0x1E5668E2, 0x8481, 0x11D4, { 0xBC, 0xF1, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }},
  { 0x665E3FF5, 0x46CC, 0x11D4, { 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }},
  { 0xF4E5C7D0, 0xD239, 0x47CB, { 0xAA, 0xCD, 0x7F, 0x66, 0xEF, 0x76, 0x32, 0x38 }}
};

STATIC
VOID
DumpUsbfnPrereq (VOID)
{
  EFI_STATUS Status;
  VOID      *Ifc = NULL;
  UINT32     Missing = 0;

  for (UINTN i = 0; i < ARRAY_SIZE (mUsbfnPrereq); i++) {
    Status = gBS->LocateProtocol (&mUsbfnPrereq[i], NULL, &Ifc);
    if (EFI_ERROR (Status)) {
      Missing++;
      DEBUG ((EFI_D_ERROR, "[DBG] USBFN prereq #%d MISSING (%08X-%04X-%04X)\n",
              (UINT32)i, mUsbfnPrereq[i].Data1, mUsbfnPrereq[i].Data2, mUsbfnPrereq[i].Data3));
    }
  }

  DEBUG ((EFI_D_ERROR, "[DBG] USBFN prereq: %d of %d missing\n",
          Missing, (UINT32)ARRAY_SIZE (mUsbfnPrereq)));
}

STATIC
VOID
DumpUsbCfgCandidates (VOID);

STATIC
VOID
UsbDevReport (CONST CHAR8 *Tag);

STATIC
VOID
UsbDevArmReadyToBoot (VOID);

STATIC
VOID
UsbDevConfigFields (CONST CHAR8 *Tag);

STATIC
VOID
UsbDevConfigOpenDetails (CONST CHAR8 *Tag);

STATIC
VOID
UsbDevArmConfigNotify (VOID);

STATIC EFI_GUID mUsbCfgCand[] = {
  { 0xB898D8DC, 0x080A, 0x40F7, { 0x99, 0xE3, 0x31, 0x62, 0x7B, 0x80, 0x6A, 0x5A }},
  { 0x0A134F0E, 0x075E, 0x40B3, { 0x9C, 0x63, 0x3B, 0x39, 0x06, 0x80, 0x46, 0x63 }},
  { 0x27ABF055, 0xB1B8, 0x4C26, { 0x80, 0x48, 0x74, 0x8F, 0x37, 0xBA, 0xA2, 0xDF }},
  { 0xBEDAEABC, 0x5E70, 0x4D66, { 0x97, 0x33, 0x21, 0x3D, 0x07, 0x2B, 0x9D, 0x04 }},
  { 0x2DA15AC7, 0x4909, 0x4C4C, { 0xB8, 0xC3, 0xA7, 0x42, 0x19, 0x5E, 0xA4, 0x6E }},
  { 0xE722B03F, 0xB250, 0x42CE, { 0x8E, 0xBD, 0x5B, 0xD5, 0x18, 0x12, 0xD0, 0x37 }},
  { 0xD2B2B828, 0x0826, 0x48A7, { 0xB3, 0xDF, 0x98, 0x3C, 0x00, 0x60, 0x24, 0xF0 }}
};

STATIC
VOID
DumpUsbCfgCandidates (VOID)
{
  EFI_STATUS Status;
  VOID      *Ifc = NULL;
  UINT32     Present = 0;

  for (UINTN i = 0; i < ARRAY_SIZE (mUsbCfgCand); i++) {
    Status = gBS->LocateProtocol (&mUsbCfgCand[i], NULL, &Ifc);
    if (!EFI_ERROR (Status)) {
      Present++;
      DEBUG ((EFI_D_ERROR, "[USBCFG] cand#%d PRESENT (%08X)\n",
              (UINT32)i, mUsbCfgCand[i].Data1));
    }
  }

  DEBUG ((EFI_D_ERROR, "[USBCFG] %d of %d present\n",
          Present, (UINT32)ARRAY_SIZE (mUsbCfgCand)));
}

//
// ---- Gate-2C-A2: USBFN Driver Binding runtime diagnostics ----
//
// Task 0: compile-time verification of the EFI_BOOT_SERVICES layout.
// EFI_BOOT_SERVICES starts with EFI_TABLE_HEADER (0x18 bytes). Member offsets
// are therefore 0x18 + index*8, NOT index*8. These asserts fail the build if
// the local ABI ever diverges from the standard layout.
//
typedef char UsbdAssertTableHeader[((sizeof (EFI_TABLE_HEADER)) == 0x18) ? 1 : -1];
typedef char UsbdAssertInstallProto[((OFFSET_OF (EFI_BOOT_SERVICES, InstallProtocolInterface)) == 0x80) ? 1 : -1];
typedef char UsbdAssertOpenProto[((OFFSET_OF (EFI_BOOT_SERVICES, OpenProtocol)) == 0x118) ? 1 : -1];
typedef char UsbdAssertLocateProto[((OFFSET_OF (EFI_BOOT_SERVICES, LocateProtocol)) == 0x140) ? 1 : -1];
typedef char UsbdAssertInstallMulti[((OFFSET_OF (EFI_BOOT_SERVICES, InstallMultipleProtocolInterfaces)) == 0x148) ? 1 : -1];

#define USBD_MEDIA_FV_FILEPATH_SUBTYPE  0x06

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL Header;
  EFI_GUID                 Name;
} USBD_FV_FILEPATH_NODE;

// FFS FILE_GUID of UsbfnDwc3Dxe (from its INF)
STATIC EFI_GUID mUsbdUsbfnFileGuid = { 0x94F8A6A7, 0xDC34, 0x4101, { 0x88, 0xC1, 0x99, 0x17, 0x9C, 0xCE, 0xAE, 0x83 }};
// Qualcomm USB Config protocol (reverse engineered, see Gate-2C-A1)
STATIC EFI_GUID mUsbdUsbConfigGuid = { 0xE722B03F, 0xB250, 0x42CE, { 0x8E, 0xBD, 0x5B, 0xD5, 0x18, 0x12, 0xD0, 0x37 }};
STATIC EFI_GUID mUsbdUsbfnIoGuid   = { 0x32D2963A, 0xFE5D, 0x4F30, { 0xB6, 0x33, 0x6E, 0x5D, 0xC5, 0x58, 0x03, 0xCC }};

STATIC
BOOLEAN
UsbdFileIs (
  IN EFI_DEVICE_PATH_PROTOCOL *Path,
  IN EFI_GUID                 *FileGuid
  )
{
  EFI_DEVICE_PATH_PROTOCOL *Node;

  if (Path == NULL) {
    return FALSE;
  }

  for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == USBD_MEDIA_FV_FILEPATH_SUBTYPE)) {
      if (CompareGuid (&((USBD_FV_FILEPATH_NODE *)Node)->Name, FileGuid)) {
        return TRUE;
      }
    }
  }

  return FALSE;
}

STATIC
EFI_HANDLE
UsbdFindUsbfnImage (VOID)
{
  EFI_STATUS                   Status;
  EFI_HANDLE                  *Handles = NULL;
  UINTN                        Count   = 0;
  EFI_HANDLE                   Found   = NULL;
  EFI_DRIVER_BINDING_PROTOCOL *Db;
  EFI_LOADED_IMAGE_PROTOCOL   *Li;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiDriverBindingProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return NULL;
  }

  for (UINTN i = 0; i < Count; i++) {
    Db = NULL;
    Li = NULL;
    //
    // Use OpenProtocol(GET_PROTOCOL) with our own AgentHandle: HandleProtocol
    // opens as gDxeCoreImageHandle and our CloseProtocol would not match.
    //
    Status = gBS->OpenProtocol (
                    Handles[i],
                    &gEfiDriverBindingProtocolGuid,
                    (VOID **)&Db,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status) || (Db == NULL) || (Db->ImageHandle == NULL)) {
      gBS->CloseProtocol (Handles[i], &gEfiDriverBindingProtocolGuid, gImageHandle, NULL);
      continue;
    }

    Status = gBS->OpenProtocol (
                    Db->ImageHandle,
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&Li,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (!EFI_ERROR (Status) && (Li != NULL) && UsbdFileIs (Li->FilePath, &mUsbdUsbfnFileGuid)) {
      Found = Db->ImageHandle;
    }

    gBS->CloseProtocol (Db->ImageHandle, &gEfiLoadedImageProtocolGuid, gImageHandle, NULL);
    gBS->CloseProtocol (Handles[i], &gEfiDriverBindingProtocolGuid, gImageHandle, NULL);

    if (Found != NULL) {
      break;
    }
  }

  FreePool (Handles);
  return Found;
}

STATIC
VOID
UsbDevReport (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS                           Status;
  EFI_HANDLE                           UsbfnImage = NULL;
  EFI_HANDLE                          *CfgHandles = NULL;
  UINTN                                CfgCount   = 0;
  UINTN                                TotalOpen  = 0;
  UINTN                                ByFnOpen   = 0;
  UINTN                                InfoCount  = 0;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info       = NULL;
  VOID                                *Io         = NULL;

  UsbfnImage = UsbdFindUsbfnImage ();

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &mUsbdUsbConfigGuid,
                  NULL,
                  &CfgCount,
                  &CfgHandles
                  );
  if (EFI_ERROR (Status)) {
    CfgCount   = 0;
    CfgHandles = NULL;
  }

  if (CfgHandles != NULL) {
    for (UINTN i = 0; i < CfgCount; i++) {
      Info      = NULL;
      InfoCount = 0;
      Status    = gBS->OpenProtocolInformation (
                         CfgHandles[i],
                         &mUsbdUsbConfigGuid,
                         &Info,
                         &InfoCount
                         );
      if (EFI_ERROR (Status)) {
        continue;
      }

      for (UINTN j = 0; j < InfoCount; j++) {
        TotalOpen++;
        if ((UsbfnImage != NULL) && (Info[j].AgentHandle == UsbfnImage) &&
            ((Info[j].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0)) {
          ByFnOpen++;
        }
      }

      if (Info != NULL) {
        FreePool (Info);
      }
    }

    FreePool (CfgHandles);
  }

  Status = gBS->LocateProtocol (&mUsbdUsbfnIoGuid, NULL, &Io);

  DEBUG ((EFI_D_ERROR, "[USBDEV%s] BindingFound       = %d\n", Tag, (UINT32)((UsbfnImage != NULL) ? 1 : 0)));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigHandles      = %d\n", Tag, (UINT32)CfgCount));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigOpenEntries  = %d\n", Tag, (UINT32)TotalOpen));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigOpenedByUsbFn = %d\n", Tag, (UINT32)ByFnOpen));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] USBFN_IO           = %d\n", Tag, (UINT32)((Io != NULL) ? 1 : 0)));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] BindingStart       = UNKNOWN (needs vendor patch)\n", Tag));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] CoreBase/Role      = UNKNOWN (no public API)\n", Tag));
}

//
// Gate-2C-A4: read the two fields that UsbfnDwc3Dxe!DriverBindingSupported
// compares against (see Gate-2C-A3):
//     USBConfig[+0x88] <= 3   and   USBConfig[+0x8C] == 4
//
// Strictly read-only:
//   - LocateHandleBuffer + HandleProtocol (BY_HANDLE_PROTOCOL semantics)
//   - never opens BY_DRIVER, never calls any USB Config vtable method
//   - does not modify the interface
//   - releases its own open with CloseProtocol
//
STATIC
VOID
UsbDevConfigFields (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles = NULL;
  UINTN       Count   = 0;
  UINT8      *Ifc;
  UINT32      F88;
  UINT32      F8C;
  EFI_STATUS  CloseStatus;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &mUsbdUsbConfigGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgEnumStatus = %r (no handles)\n", Tag, Status));
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    Ifc    = NULL;
    //
    // Use explicit OpenProtocol(GET_PROTOCOL) with our own AgentHandle.
    // CoreHandleProtocol() internally opens with gDxeCoreImageHandle, so a
    // later CloseProtocol(gImageHandle) would not match and would leak an
    // open record (EFI_NOT_FOUND).
    //
    Status = gBS->OpenProtocol (
                    Handles[i],
                    &mUsbdUsbConfigGuid,
                    (VOID **)&Ifc,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigHandle[%d] = %p\n", Tag, (UINT32)i, Handles[i]));
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigStatus[%d] = %r\n", Tag, (UINT32)i, Status));

    if (!EFI_ERROR (Status) && (Ifc != NULL)) {
      F88 = ReadUnaligned32 ((UINT32 *)(Ifc + 0x88));
      F8C = ReadUnaligned32 ((UINT32 *)(Ifc + 0x8C));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgField88[%d]   = 0x%08x (need <= 3)\n", Tag, (UINT32)i, F88));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgField8C[%d]   = 0x%08x (need == 4)\n", Tag, (UINT32)i, F8C));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgWouldBind[%d] = %d\n", Tag, (UINT32)i,
              (UINT32)(((F88 <= 3) && (F8C == 4)) ? 1 : 0)));
      CloseStatus = gBS->CloseProtocol (
                           Handles[i],
                           &mUsbdUsbConfigGuid,
                           gImageHandle,
                           NULL
                           );
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigClose[%d]  = %r\n", Tag, (UINT32)i, CloseStatus));
    } else {
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgField88[%d]   = UNKNOWN\n", Tag, (UINT32)i));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgField8C[%d]   = UNKNOWN\n", Tag, (UINT32)i));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgWouldBind[%d] = UNKNOWN\n", Tag, (UINT32)i));
    }
  }

  FreePool (Handles);
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgFieldScanned = %d\n", Tag, (UINT32)Count));
  DEBUG ((EFI_D_ERROR, "[USBDEV%s] CfgVtableCalls  = 0 (never invoked)\n", Tag));
}

//
// Gate-2C-A10b: passive diagnostics, corrected.
//   - BASELINE snapshot separates pre-existing instances from real notifications
//     (EDK2 Notify.c sets ProtNotify->Position = &ProtEntry->Protocols, so the
//      first callback re-walks every existing instance).
//   - All identity lookups use OpenProtocol(GET_PROTOCOL)/CloseProtocol with a
//     matched AgentHandle; no HandleProtocol (which opens as gDxeCoreImageHandle).
//   - Strictly passive: no Supported()/Start()/ConnectController/BY_DRIVER.
//
#define USBD_BASELINE_MAX  8

STATIC UINT32     mUsbCfgNotifySeq   = 0;   // notifications that actually fired
STATIC UINT32     mUsbCfgSeenCount   = 0;   // handles reported total
STATIC UINT32     mUsbCfgNewCount    = 0;   // handles NOT in baseline
STATIC EFI_HANDLE mUsbCfgBaseline[USBD_BASELINE_MAX];
STATIC UINT32     mUsbCfgBaselineCount = 0;
STATIC EFI_EVENT  mUsbCfgNotifyEvt   = NULL;
STATIC EFI_GUID   mUsbdDriverBindingGuid = { 0x18A031AB, 0xB443, 0x4D1A, { 0xA5, 0xC0, 0x0C, 0x09, 0x26, 0x1E, 0x9F, 0x71 }};

STATIC
BOOLEAN
UsbdIsBaseline (
  IN EFI_HANDLE Handle
  )
{
  for (UINTN i = 0; i < mUsbCfgBaselineCount; i++) {
    if (mUsbCfgBaseline[i] == Handle) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
VOID
UsbdPrintCoreFields (
  IN CONST CHAR8 *Kind,
  IN UINT32       Seq,
  IN EFI_HANDLE   Handle
  )
{
  EFI_STATUS Status;
  UINT8     *Ifc = NULL;
  UINT32     F88;
  UINT32     F8C;
  EFI_STATUS CloseSt;

  Status = gBS->OpenProtocol (Handle, &mUsbdUsbConfigGuid, (VOID **)&Ifc,
                              gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status) || (Ifc == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/%a] Seq=%d Handle=%p OpenStatus=%r (no read)\n",
            Kind, Seq, Handle, Status));
    return;
  }

  F88 = ReadUnaligned32 ((UINT32 *)(Ifc + 0x88));
  F8C = ReadUnaligned32 ((UINT32 *)(Ifc + 0x8C));
  DEBUG ((EFI_D_ERROR, "[USBCFG/%a] Seq=%d Handle=%p Core=%d Field88=0x%08x Field8C=0x%08x\n",
          Kind, Seq, Handle, F88, F88, F8C));

  CloseSt = gBS->CloseProtocol (Handle, &mUsbdUsbConfigGuid, gImageHandle, NULL);
  if (EFI_ERROR (CloseSt)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/%a] Seq=%d Close=%r  <== UNBALANCED\n", Kind, Seq, CloseSt));
  }
}

STATIC
VOID
UsbDevPrintAgent (
  IN CONST CHAR8 *Tag,
  IN UINT32       Idx,
  IN UINT32       Ent,
  IN EFI_HANDLE   Agent
  )
{
  EFI_STATUS                   St;
  EFI_STATUS                   CloseSt;
  EFI_LOADED_IMAGE_PROTOCOL   *Li = NULL;
  EFI_DRIVER_BINDING_PROTOCOL *Db = NULL;
  EFI_GUID                    *FileGuid = NULL;
  EFI_DEVICE_PATH_PROTOCOL    *Node;

  DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentHandle[%d,%d]      = %p\n", Tag, Idx, Ent, Agent));

  //
  // LoadedImage: OpenProtocol(GET_PROTOCOL) with our own agent handle.
  //
  St = gBS->OpenProtocol (Agent, &gEfiLoadedImageProtocolGuid, (VOID **)&Li,
                          gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (!EFI_ERROR (St) && (Li != NULL) && (Li->FilePath != NULL)) {
    for (Node = Li->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
          (DevicePathSubType (Node) == USBD_MEDIA_FV_FILEPATH_SUBTYPE)) {
        FileGuid = &((USBD_FV_FILEPATH_NODE *)Node)->Name;
        break;
      }
    }
    if (FileGuid != NULL) {
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentFileGuid[%d,%d]   = %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
              Tag, Idx, Ent,
              FileGuid->Data1, FileGuid->Data2, FileGuid->Data3,
              FileGuid->Data4[0], FileGuid->Data4[1], FileGuid->Data4[2], FileGuid->Data4[3],
              FileGuid->Data4[4], FileGuid->Data4[5], FileGuid->Data4[6], FileGuid->Data4[7]));
    } else {
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentFileGuid[%d,%d]   = UNKNOWN (no FV node)\n", Tag, Idx, Ent));
    }
  } else {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentFileGuid[%d,%d]   = UNKNOWN (status %r)\n", Tag, Idx, Ent, St));
  }
  CloseSt = gBS->CloseProtocol (Agent, &gEfiLoadedImageProtocolGuid, gImageHandle, NULL);
  if (EFI_ERROR (CloseSt)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentLiClose[%d,%d]    = %r  <== UNBALANCED\n", Tag, Idx, Ent, CloseSt));
  }

  //
  // Is the agent a DriverBinding? Report both handles; do NOT equate them.
  //
  St = gBS->OpenProtocol (Agent, &mUsbdDriverBindingGuid, (VOID **)&Db,
                          gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (!EFI_ERROR (St) && (Db != NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentIsDriverBinding[%d,%d] = 1 ImageHandle=%p DriverBindingHandle=%p\n",
            Tag, Idx, Ent, Db->ImageHandle, Db->DriverBindingHandle));
  } else {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentIsDriverBinding[%d,%d] = 0 (status %r)\n", Tag, Idx, Ent, St));
  }
  CloseSt = gBS->CloseProtocol (Agent, &mUsbdDriverBindingGuid, gImageHandle, NULL);
  if (EFI_ERROR (CloseSt)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] AgentDbClose[%d,%d]    = %r  <== UNBALANCED\n", Tag, Idx, Ent, CloseSt));
  }
}

STATIC
VOID
UsbDevConfigOpenDetails (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS                           St;
  EFI_HANDLE                          *Handles = NULL;
  UINTN                                Count   = 0;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info    = NULL;
  UINTN                                InfoCount;

  St = gBS->LocateHandleBuffer (ByProtocol, &mUsbdUsbConfigGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] OpenDetailEnum = %r\n", Tag, St));
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    Info      = NULL;
    InfoCount = 0;
    St = gBS->OpenProtocolInformation (Handles[i], &mUsbdUsbConfigGuid, &Info, &InfoCount);
    DEBUG ((EFI_D_ERROR, "[USBDEV%s] ConfigHandle[%d] = %p  OpenEntryCount[%d] = %d\n",
            Tag, (UINT32)i, Handles[i], (UINT32)i, (UINT32)InfoCount));
    if (EFI_ERROR (St)) {
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] OpenDetailStatus[%d] = %r\n", Tag, (UINT32)i, St));
      continue;
    }
    for (UINTN j = 0; j < InfoCount; j++) {
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] ControllerHandle[%d,%d] = %p\n", Tag, (UINT32)i, (UINT32)j, Info[j].ControllerHandle));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] Attributes[%d,%d]       = 0x%08x%s\n", Tag, (UINT32)i, (UINT32)j,
              Info[j].Attributes,
              ((Info[j].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) != 0) ? " (BY_DRIVER)" : ""));
      DEBUG ((EFI_D_ERROR, "[USBDEV%s] OpenCount[%d,%d]        = %d\n", Tag, (UINT32)i, (UINT32)j, (UINT32)Info[j].OpenCount));
      UsbDevPrintAgent (Tag, (UINT32)i, (UINT32)j, Info[j].AgentHandle);
    }
    if (Info != NULL) {
      FreePool (Info);
    }
  }
  FreePool (Handles);
}

STATIC
VOID
UsbCfgBaselineSnapshot (
  VOID
  )
{
  EFI_STATUS  St;
  EFI_HANDLE *Handles = NULL;
  UINTN       Count   = 0;

  mUsbCfgBaselineCount = 0;

  St = gBS->LocateHandleBuffer (ByProtocol, &mUsbdUsbConfigGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/BASELINE] none or error %r\n", St));
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    if (mUsbCfgBaselineCount < USBD_BASELINE_MAX) {
      mUsbCfgBaseline[mUsbCfgBaselineCount++] = Handles[i];
    }
    UsbdPrintCoreFields ("BASELINE", (UINT32)(i + 1), Handles[i]);
  }
  DEBUG ((EFI_D_ERROR, "[USBCFG/BASELINE] count=%d (pre-existing, registered before any callback)\n",
          (UINT32)mUsbCfgBaselineCount));

  FreePool (Handles);
}

STATIC
VOID
EFIAPI
UsbCfgNotifyCallback (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  EFI_STATUS  St;
  EFI_HANDLE *Handles = NULL;
  UINTN       Count   = 0;

  mUsbCfgNotifySeq++;
  DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Callback=%d begin\n", mUsbCfgNotifySeq));

  //
  // Do NOT use LocateHandle(ByRegisterNotify): its SearchKey must be the
  // Registration pointer returned by RegisterProtocolNotify, not the Event.
  // Passing the Event trips ASSERT in DxeCore Locate.c(355) and halts boot.
  // Enumerating by protocol and diffing against the baseline is equivalent
  // here, safe, and also handles several instances installed at once.
  //
  St = gBS->LocateHandleBuffer (ByProtocol, &mUsbdUsbConfigGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Callback=%d enum failed %r\n", mUsbCfgNotifySeq, St));
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    mUsbCfgSeenCount++;

    if (UsbdIsBaseline (Handles[i])) {
      DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Callback=%d Handle=%p -> pre-existing (see BASELINE), not a new install\n",
              mUsbCfgNotifySeq, Handles[i]));
      continue;
    }

    mUsbCfgNewCount++;
    if (mUsbCfgBaselineCount < USBD_BASELINE_MAX) {
      mUsbCfgBaseline[mUsbCfgBaselineCount++] = Handles[i];
    }
    DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Callback=%d Handle=%p -> NEW install (#%d)\n",
            mUsbCfgNotifySeq, Handles[i], mUsbCfgNewCount));
    UsbdPrintCoreFields ("NOTIFY", mUsbCfgNotifySeq, Handles[i]);
  }

  FreePool (Handles);

  DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Callback=%d end (seenTotal=%d newTotal=%d)\n",
          mUsbCfgNotifySeq, mUsbCfgSeenCount, mUsbCfgNewCount));
}

STATIC
VOID
UsbDevArmConfigNotify (
  VOID
  )
{
  EFI_STATUS  St;
  VOID       *Registration = NULL;

  //
  // Snapshot what already exists BEFORE registering, so the first callback's
  // re-walk of the whole protocol list cannot be mistaken for new installs.
  //
  UsbCfgBaselineSnapshot ();

  St = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UsbCfgNotifyCallback, NULL, &mUsbCfgNotifyEvt);
  if (EFI_ERROR (St)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] CreateEvent failed: %r\n", St));
    return;
  }
  St = gBS->RegisterProtocolNotify (&mUsbdUsbConfigGuid, mUsbCfgNotifyEvt, &Registration);
  if (EFI_ERROR (St)) {
    DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] Register failed: %r\n", St));
    return;
  }
  DEBUG ((EFI_D_ERROR, "[USBCFG/NOTIFY] registered (passive, read-only). NOTE: a handle installed between the baseline snapshot and this registration would be counted as NEW; that window is unavoidable from a DXE driver.\n"));
}

//
// Gate-2C-A12: strictly read-only runtime probe.
//   - LocateProtocol / LocateHandleBuffer / memory reads only
//   - NO vtable calls (no StartDevice, StartController, ConfigureEnableEndpoints)
//   - NO BlockIo, NO ConnectController, NO new notify cursors or timers
//
STATIC EFI_GUID mUsbdUsbFnIoGuid = { 0x32D2963A, 0xFE5D, 0x4F30, { 0xB6, 0x33, 0x6E, 0x5D, 0xC5, 0x58, 0x03, 0xCC }};
STATIC EFI_GUID mUsbdMsdGuid     = { 0xC8591FAF, 0xDBCC, 0x479E, { 0x9E, 0xF2, 0xFD, 0x08, 0x5B, 0xC3, 0x7B, 0xC7 }};
STATIC EFI_GUID mUsbdUsbDevGuid  = { 0x3299A266, 0x15F0, 0x4346, { 0x83, 0x18, 0x71, 0x63, 0x36, 0x73, 0x6D, 0x3E }};

#define USBD_USBDEV_ACTIVATION_RVA  0xA3D0

STATIC
VOID
UsbDevProbeProtocols (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS  St;
  VOID       *Ifc;
  EFI_HANDLE *Handles;
  UINTN       Count;
  UINT32      Rev;
  UINT32      Magic;
  UINT8       State;

  //
  // ---- USBFN_IO ----
  //
  Ifc = NULL;
  St  = gBS->LocateProtocol (&mUsbdUsbFnIoGuid, NULL, &Ifc);
  DEBUG ((EFI_D_ERROR, "[USBPROBE] t=%a USBFN_Status = %r\n", Tag, St));
  if (!EFI_ERROR (St) && (Ifc != NULL) && (((UINTN)Ifc & 0x7) == 0)) {
    Rev = ReadUnaligned32 ((UINT32 *)Ifc);
    DEBUG ((EFI_D_ERROR, "[USBPROBE] t=%a USBFN_Revision = 0x%08x\n", Tag, Rev));
    Handles = NULL;
    Count   = 0;
    St      = gBS->LocateHandleBuffer (ByProtocol, &mUsbdUsbFnIoGuid, NULL, &Count, &Handles);
    DEBUG ((EFI_D_ERROR, "[USBPROBE] t=%a USBFN_HandleCount = %d (status %r)\n", Tag, (UINT32)Count, St));
    if (Handles != NULL) {
      FreePool (Handles);
    }
  } else {
    DEBUG ((EFI_D_ERROR, "[USBPROBE] t=%a USBFN_Revision = UNKNOWN\n", Tag));
    DEBUG ((EFI_D_ERROR, "[USBPROBE] t=%a USBFN_HandleCount = UNKNOWN\n", Tag));
  }

  //
  // ---- MSD ----
  //
  Ifc = NULL;
  St  = gBS->LocateProtocol (&mUsbdMsdGuid, NULL, &Ifc);
  DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_Status = %r\n", Tag, St));
  if (!EFI_ERROR (St) && (Ifc != NULL) && (((UINTN)Ifc & 0x7) == 0)) {
    Rev   = ReadUnaligned32 ((UINT32 *)Ifc);
    Magic = ReadUnaligned32 ((UINT32 *)((UINT8 *)Ifc - 8));
    DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_Revision = 0x%08x\n", Tag, Rev));
    if ((Magic == 0x6D627375u) && (Rev == 0x00010002u)) {
      DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_Magic = 0x%08x OK\n", Tag, Magic));
      State = *(volatile UINT8 *)((UINT8 *)Ifc + 0x6B0);   // private Base + 0x6B8
      DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_State = %d\n", Tag, State));
    } else {
      DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_Magic = 0x%08x MISMATCH -> MSD_State = UNKNOWN\n", Tag, Magic));
    }
  } else {
    DEBUG ((EFI_D_ERROR, "[MSDPROBE] t=%a MSD_Revision = UNKNOWN  MSD_Magic = UNKNOWN  MSD_State = UNKNOWN\n", Tag));
  }
}

STATIC
VOID
UsbDevProbeUsbDeviceDxe (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS                 St;
  EFI_STATUS                 CloseSt;
  EFI_HANDLE                *Handles = NULL;
  UINTN                      Count   = 0;
  EFI_LOADED_IMAGE_PROTOCOL *Li      = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_GUID                  *FileGuid;
  UINT64                     ImageBase = 0;
  UINT64                     ImageSize = 0;
  UINT64                     Target;
  UINT64                     Slot;
  BOOLEAN                    Found = FALSE;

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (St) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a Loaded = UNKNOWN (enum %r)\n", Tag, St));
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    Li = NULL;
    St = gBS->OpenProtocol (Handles[i], &gEfiLoadedImageProtocolGuid, (VOID **)&Li,
                            gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR (St) || (Li == NULL)) {
      gBS->CloseProtocol (Handles[i], &gEfiLoadedImageProtocolGuid, gImageHandle, NULL);
      continue;
    }

    FileGuid = NULL;
    if (Li->FilePath != NULL) {
      for (Node = Li->FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
        if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
            (DevicePathSubType (Node) == USBD_MEDIA_FV_FILEPATH_SUBTYPE)) {
          FileGuid = &((USBD_FV_FILEPATH_NODE *)Node)->Name;
          break;
        }
      }
    }

    if ((FileGuid != NULL) && CompareGuid (FileGuid, &mUsbdUsbDevGuid)) {
      ImageBase = (UINT64)(UINTN)Li->ImageBase;
      ImageSize = (UINT64)Li->ImageSize;
      Found     = TRUE;
    }

    CloseSt = gBS->CloseProtocol (Handles[i], &gEfiLoadedImageProtocolGuid, gImageHandle, NULL);
    if (EFI_ERROR (CloseSt)) {
      DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a LiClose = %r <== UNBALANCED\n", Tag, CloseSt));
    }
    if (Found) {
      break;
    }
  }
  FreePool (Handles);

  if (!Found) {
    DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a Loaded = 0 (identity not found)\n", Tag));
    DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a UsbfnGlobal = UNKNOWN\n", Tag));
    return;
  }

  DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a Loaded = 1\n", Tag));
  DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a ImageBase = 0x%lx  ImageSize = 0x%lx\n", Tag, ImageBase, ImageSize));

  //
  // Range-validate before dereferencing the private global.
  //
  if ((ImageBase != 0) && (ImageSize > USBD_USBDEV_ACTIVATION_RVA + 8)) {
    Target = ImageBase + USBD_USBDEV_ACTIVATION_RVA;
    if ((Target > ImageBase) && ((Target + 8) <= (ImageBase + ImageSize))) {
      DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a IdentityVerified = 1\n", Tag));
      Slot = *(volatile UINT64 *)(UINTN)Target;
      DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a UsbfnGlobal = %a (%p)\n",
              Tag, (Slot == 0) ? "NULL" : "NON_NULL", (VOID *)(UINTN)Slot));
    } else {
      DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a UsbfnGlobal = UNKNOWN (range check failed)\n", Tag));
    }
  } else {
    DEBUG ((EFI_D_ERROR, "[USBDEVICE] t=%a UsbfnGlobal = UNKNOWN (image too small)\n", Tag));
  }
}

STATIC
VOID
EFIAPI
UsbDevReadyToBoot (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  UsbDevReport ("/R2B");
  UsbDevConfigFields ("/R2B");
  UsbDevConfigOpenDetails ("/R2B");
  UsbDevProbeProtocols ("/R2B");
  UsbDevProbeUsbDeviceDxe ("/R2B");
}

STATIC
VOID
UsbDevArmReadyToBoot (
  VOID
  )
{
  EFI_STATUS Status;
  EFI_EVENT  Event = NULL;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  UsbDevReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[USBDEV] ReadyToBoot event failed: %r\n", Status));
  }
}

STATIC
VOID
DiagDumpProtocols (VOID);


STATIC EFI_GUID mKebabRpmhProtocolGuid = {
  0xe1d0b573, 0xf29f, 0x495d,
  { 0x93, 0x52, 0xca, 0x0d, 0xd4, 0x2a, 0x0a, 0x74 }
};

STATIC
UINT64
KebabRvaOf (
  IN UINT64  Address
  )
{
  EFI_STATUS  Status;
  UINTN       Count;
  EFI_HANDLE  *Handles;
  UINTN       Index;
  UINT64      Rva = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid,
                  NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return 0;
  }
  for (Index = 0; Index < Count; Index++) {
    EFI_LOADED_IMAGE_PROTOCOL *Li;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
        &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) || Li == NULL ||
        Li->ImageBase == NULL) {
      continue;
    }
    if (Address >= (UINT64)(UINTN)Li->ImageBase &&
        Address < (UINT64)(UINTN)Li->ImageBase + Li->ImageSize) {
      Rva = Address - (UINT64)(UINTN)Li->ImageBase;
      break;
    }
  }
  FreePool (Handles);
  return Rva;
}

STATIC
VOID
KebabDumpRpmh (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *Interface;
  UINT64      *Table;
  UINTN       Index;

  DEBUG ((EFI_D_ERROR, "[KRP] ---- rpmh protocol ----\n"));
  Status = gBS->LocateProtocol (&mKebabRpmhProtocolGuid, NULL, &Interface);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[KRP] not found: %r\n", Status));
    return;
  }
  DEBUG ((EFI_D_ERROR, "[KRP] iface=%p rva=%lx\n", Interface,
    KebabRvaOf ((UINT64)(UINTN)Interface)));
  Table = (UINT64 *)Interface;
  for (Index = 0; Index < 24; Index++) {
    UINT64 Fn = Table[Index];
    DEBUG ((EFI_D_ERROR, "[KRP] [%u] %p rva=%lx\n", (UINT32)Index,
      (VOID *)(UINTN)Fn, KebabRvaOf (Fn)));
  }
}

STATIC
VOID
EFIAPI
DiagTimerNotify (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  gBS->CloseEvent (Event);
  DEBUG ((EFI_D_ERROR, "\n[DIAG] ==== deferred (post-BDS) ====\n"));
  DiagDumpProtocols ();
  KebabDumpRpmh ();
  DumpUsbfnPrereq ();
  DEBUG ((EFI_D_ERROR, "[DIAG] ============================\n\n"));
}

STATIC
VOID
DiagArmDeferredDump (VOID)
{
  EFI_STATUS Status;
  EFI_EVENT  Event;

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, DiagTimerNotify, NULL, &Event);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[DIAG] CreateEvent failed: %r\n", Status));
    return;
  }

  // 5 seconds, 100ns units
  Status = gBS->SetTimer (Event, TimerRelative, 50000000ULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "[DIAG] SetTimer failed: %r\n", Status));
    gBS->CloseEvent (Event);
  }
}

STATIC
VOID
DiagDumpProtocols (VOID)
{
  DEBUG ((EFI_D_ERROR, "\n[DIAG] ---- protocol presence ----\n"));
  DumpProtocolCount ("USB2_HC  ", &gEfiUsb2HcProtocolGuid);
  DumpProtocolCount ("USB_IO   ", &gEfiUsbIoProtocolGuid);
  DumpProtocolCount ("BLOCK_IO ", &gEfiBlockIoProtocolGuid);
  DumpProtocolCount ("SIMPLE_FS", &gEfiSimpleFileSystemProtocolGuid);
  DumpProtocolCount ("PCI_IO   ", &gEfiPciIoProtocolGuid);
  DumpProtocolCount ("QSEECOM  ", &gQcomQseecomProtocolGuid);
  DumpProtocolCount ("USBFN_IO ", &gEfiUsbFunctionIoProtocolGuid);
  DEBUG ((EFI_D_ERROR, "[DIAG] ----------------------------\n\n"));
}

//
// Global Variables
//
STATIC EFI_MEMORY_DESCRIPTOR *EfiMemoryMap     = NULL;
STATIC UINTN                  EfiMemoryMapSize = 0;
STATIC UINTN                  DescriptorSize   = 0;

VOID
HandleRamRange (
  IN EFI_PHYSICAL_ADDRESS Base,
  IN UINT64               Length)
{
  EFI_STATUS Status;

  // Set RAM Range End
  EFI_PHYSICAL_ADDRESS End = Base + Length;

  // Map RAM Range
  Status = MapMemoryRegion (Base, Length, EfiConventionalMemory);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Map RAM Range: 0x%p - 0x%p! Status = %r\n", Base, End, Status));
    return;
  }

  // Show Mapped RAM Range
  DEBUG ((EFI_D_ERROR, "Successfully Mapped RAM Range: 0x%p - 0x%p\n", Base, End));
}

VOID
ParseMemoryRange (
  IN EFI_PHYSICAL_ADDRESS RangeBase,
  IN EFI_PHYSICAL_ADDRESS RangeEnd)
{
  // Set Inital Memory Range Base
  EFI_PHYSICAL_ADDRESS CurrentRangeBase = RangeBase;

  // Go thru each Memory Map Region
  for (UINT16 i = 0; i < MEMORY_DESCRIPTOR_LIST_SIZE (EfiMemoryMapSize, DescriptorSize); i++) {
    // Get new Memory Descriptor
    EFI_MEMORY_DESCRIPTOR *Descriptor = GET_MEMORY_DESCRIPTOR (EfiMemoryMap, i, DescriptorSize);

    // Save Memory Region Details
    EFI_PHYSICAL_ADDRESS RegionBase = Descriptor->PhysicalStart;
    EFI_PHYSICAL_ADDRESS RegionEnd  = Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);

    // Verify Memory Range End
    if (RegionBase >= RangeEnd) {
      break;
    }

    // Verify Memory Region End
    if (RegionEnd <= CurrentRangeBase) {
      continue;
    }

    // Check Memory Region Base
    if (RegionBase > CurrentRangeBase) {
      // Handle Free Memory Space
      HandleRamRange (CurrentRangeBase, RegionBase - CurrentRangeBase);
    }

    // Set new Memory Range Base
    if (RegionEnd > CurrentRangeBase) {
      CurrentRangeBase = RegionEnd;
    }
  }

  // Check Current Memory Range Base
  if (CurrentRangeBase < RangeEnd) {
    // Handle Free Memory Space
    HandleRamRange (CurrentRangeBase, RangeEnd - CurrentRangeBase);
  }
}

INTN
EFIAPI
CompareMemoryRegions (
  IN CONST VOID *Input1,
  IN CONST VOID *Input2)
{
  // Populate Memory Region Structure
  EFI_MEMORY_DESCRIPTOR *Region1 = (EFI_MEMORY_DESCRIPTOR *)Input1;
  EFI_MEMORY_DESCRIPTOR *Region2 = (EFI_MEMORY_DESCRIPTOR *)Input2;

  // Compare Base Addresses
  if (Region1->PhysicalStart < Region2->PhysicalStart) {
    return -1;
  } else if (Region1->PhysicalStart > Region2->PhysicalStart) {
    return 1;
  }

  return 0;
}

INTN
EFIAPI
CompareMemoryRanges (
  IN CONST VOID *Input1,
  IN CONST VOID *Input2)
{
  // Populate Memory Range Structure
  EFI_MEMORY_RANGE *Range1 = (EFI_MEMORY_RANGE *)Input1;
  EFI_MEMORY_RANGE *Range2 = (EFI_MEMORY_RANGE *)Input2;

  // Compare Base Addresses
  if (Range1->Address < Range2->Address) {
    return -1;
  } else if (Range1->Address > Range2->Address) {
    return 1;
  }

  return 0;
}

EFI_STATUS
GetEfiMemoryMap ()
{
  EFI_STATUS Status;

  // Get EFI Memory Map Size
  Status = gBS->GetMemoryMap (&EfiMemoryMapSize, EfiMemoryMap, NULL, NULL, NULL);
  if (EFI_ERROR (Status) && Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((EFI_D_ERROR, "Failed to get EFI Memory Map Size! Status = %r\n", Status));
    return Status;
  }

  // Add 1 KiB
  EfiMemoryMapSize += SIZE_1KB;

  // Allocate Memory
  EfiMemoryMap = AllocateZeroPool (EfiMemoryMapSize);
  if (EfiMemoryMap == NULL) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for EFI Memory Map!\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  // Get EFI Memory Map
  Status = gBS->GetMemoryMap (&EfiMemoryMapSize, EfiMemoryMap, NULL, &DescriptorSize, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to get EFI Memory Map! Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ManageRam (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS        Status;
  EFI_MEMORY_RANGE *MemoryRange;
  UINT8             MemoryRangeCount;

  // Dump protocol presence (diagnostic for USB / storage bring-up)
  DiagDumpProtocols ();
  DumpUsbfnPrereq ();
  DumpUsbCfgCandidates ();
  UsbDevReport ("#1-early");
  UsbDevConfigFields ("#1-early");
  UsbDevConfigOpenDetails ("#1-early");
  UsbDevProbeProtocols ("#1-early");
  UsbDevProbeUsbDeviceDxe ("#1-early");
  UsbDevArmConfigNotify ();
  UsbDevArmReadyToBoot ();

  // Arm a one-shot dump that fires after BDS has connected all drivers
  DiagArmDeferredDump ();

  // Get EFI Memory Map
  Status = GetEfiMemoryMap ();
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Get usable Memory Ranges
  Status = GetUsableMemoryRanges (&MemoryRange, &MemoryRangeCount);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Sort Memory Map & Memory Ranges
  PerformQuickSort (EfiMemoryMap, MEMORY_DESCRIPTOR_LIST_SIZE (EfiMemoryMapSize, DescriptorSize), DescriptorSize,            CompareMemoryRegions);
  PerformQuickSort (MemoryRange,  MemoryRangeCount,                                               sizeof (EFI_MEMORY_RANGE), CompareMemoryRanges);

  // Go thru each Memory Range
  for (UINT8 i = 0; i < MemoryRangeCount; i++) {
    // Parse Memory Range
    ParseMemoryRange (MemoryRange[i].Address, (EFI_PHYSICAL_ADDRESS)(MemoryRange[i].Address + MemoryRange[i].Length));
  }

cleanup:
  // Free EFI Memory Map Buffer
  if (EfiMemoryMap != NULL) {
    FreePool (EfiMemoryMap);
  }

  // Free Memory Range Buffer
  if (MemoryRange != NULL) {
    FreePool (MemoryRange);
  }

  return Status;
}
