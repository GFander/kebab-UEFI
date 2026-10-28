/** @file
  XNU-004 stage 3: Qualcomm UFS host controller adapter for EDK2's UfsPassThruDxe.

  Why this exists: the vendor UFSDxe asserts with a device error once its IOMMU
  step is satisfied, and its diagnostics never reach ConOut.  The controller
  itself is healthy (HCE=1, HCS=0x10F, UTRLRSR=1, no UIC errors) and the SMMU
  turns out to leave UFS untranslated (CBAR0 type = S1_TRANS_S2_BYPASS,
  TTBR0 = 0, stage-1 disabled), i.e. UFS DMA uses plain physical addresses.

  So we reuse the EDK2 UFS stack instead of reimplementing one:

      this adapter  ->  UfsPassThruDxe  ->  ScsiBusDxe  ->  ScsiDiskDxe  -> BlockIo

  The adapter provides the two protocols UfsPassThruDxe consumes and keeps the
  controller state ABL left behind (SkipHceReenable / SkipLinkStartup), because
  re-initialising power, clocks and PHY from DXE is exactly what we cannot do.

  Copyright (c) 2026. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#define QCOM_UFS_HC_BASE   0x01D84000ULL
#define QCOM_UFS_HC_SIZE   0x1C000

//
// Device path for the controller handle.  A vendor node is enough: UfsPassThru
// appends its own UFS/LUN nodes to it.
//
#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} QCOM_UFS_DEVICE_PATH;
#pragma pack ()


//
// Crash-proof trace.  The console does not exist when this driver runs, and a
// failure here can drop the device into Qualcomm crash-dump mode where nothing
// is visible.  So each step records itself in a non-volatile variable; a later
// image can read it back and tell us exactly how far we got.
//
STATIC EFI_GUID  mTraceGuid = { 0x6A1F3C58, 0x2E47, 0x4D90, { 0xB3, 0x8C, 0x51, 0x7A, 0xE2, 0x94, 0x0F, 0x6D } };

STATIC
VOID
QcomUfsTrace (
  IN UINT32 Step
  )
{
  gRT->SetVariable (
         L"XnuUfsTrace",
         &mTraceGuid,
         EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
         sizeof (Step),
         &Step
         );
}

STATIC QCOM_UFS_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x3C1E7A54, 0x9D28, 0x4B67, { 0x8E, 0x15, 0xA2, 0x6F, 0x4D, 0x39, 0xC7, 0x10 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL  mUfsHc;
STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL      mUfsHcPlatform;

//
// --- EDKII_UFS_HOST_CONTROLLER_PROTOCOL ---
//

STATIC
EFI_STATUS
EFIAPI
QcomUfsGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                               *MmioBar
  )
{
  if (MmioBar == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *MmioBar = (UINTN)QCOM_UFS_HC_BASE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsAllocateBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  EFI_ALLOCATE_TYPE                   Type,
  IN  EFI_MEMORY_TYPE                     MemoryType,
  IN  UINTN                               Pages,
  OUT VOID                                **HostAddress,
  IN  UINT64                              Attributes
  )
{
  EFI_STATUS  Status;
  VOID       *Buffer;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = gBS->AllocatePages (AllocateAnyPages, MemoryType == 0 ? EfiBootServicesData : MemoryType, Pages, (EFI_PHYSICAL_ADDRESS *)(UINTN)&Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *HostAddress = Buffer;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN UINTN                               Pages,
  IN VOID                                *HostAddress
  )
{
  return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

//
// The SMMU leaves UFS untranslated, so bus addresses are physical addresses.
// No bounce buffering, no page tables.
//
STATIC
EFI_STATUS
EFIAPI
QcomUfsMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  if ((HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }
  //
  // The SMMU does not translate UFS traffic (CR0.CLIENTPD is set), so the bus
  // address is the CPU physical address.  Cache maintenance still matters:
  // the controller is not guaranteed to snoop our caches.
  //
  switch (Operation) {
    case EdkiiUfsHcOperationBusMasterRead:
      WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    case EdkiiUfsHcOperationBusMasterWrite:
      InvalidateDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    default:
      WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
      break;
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = HostAddress;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN VOID                                *Mapping
  )
{
  //
  // Length is not available here, so invalidate conservatively around the
  // mapping base; EDK2 keeps descriptors page sized.
  //
  if (Mapping != NULL) {
    InvalidateDataCacheRange (Mapping, 64);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  //
  // Push everything we wrote (rings, UTRD/UCD, PRDT) out before the
  // controller is expected to have seen it.
  //
  WriteBackDataCache ();
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINTN  Address;

  if (Buffer == NULL || Count == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (Offset >= QCOM_UFS_HC_SIZE) {
    return EFI_UNSUPPORTED;
  }
  Address = (UINTN)QCOM_UFS_HC_BASE + (UINTN)Offset;

  switch (Width) {
    case EfiUfsHcWidthUint8:
      for (UINTN i = 0; i < Count; i++) {
        ((UINT8 *)Buffer)[i] = MmioRead8 (Address + i);
      }
      break;
    case EfiUfsHcWidthUint16:
      for (UINTN i = 0; i < Count; i++) {
        ((UINT16 *)Buffer)[i] = MmioRead16 (Address + i * 2);
      }
      break;
    case EfiUfsHcWidthUint32:
      for (UINTN i = 0; i < Count; i++) {
        ((UINT32 *)Buffer)[i] = MmioRead32 (Address + i * 4);
      }
      break;
    case EfiUfsHcWidthUint64:
      for (UINTN i = 0; i < Count; i++) {
        *(UINT64 *)((UINT8 *)Buffer + i * 8) = MmioRead64 (Address + i * 8);
      }
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsWrite (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN UINT64                                    Offset,
  IN UINTN                                     Count,
  IN OUT VOID                                  *Buffer
  )
{
  UINTN  Address;

  if (Buffer == NULL || Count == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (Offset >= QCOM_UFS_HC_SIZE) {
    return EFI_UNSUPPORTED;
  }
  Address = (UINTN)QCOM_UFS_HC_BASE + (UINTN)Offset;

  switch (Width) {
    case EfiUfsHcWidthUint8:
      for (UINTN i = 0; i < Count; i++) {
        MmioWrite8 (Address + i, ((UINT8 *)Buffer)[i]);
      }
      break;
    case EfiUfsHcWidthUint16:
      for (UINTN i = 0; i < Count; i++) {
        MmioWrite16 (Address + i * 2, ((UINT16 *)Buffer)[i]);
      }
      break;
    case EfiUfsHcWidthUint32:
      for (UINTN i = 0; i < Count; i++) {
        MmioWrite32 (Address + i * 4, ((UINT32 *)Buffer)[i]);
      }
      break;
    case EfiUfsHcWidthUint64:
      for (UINTN i = 0; i < Count; i++) {
        MmioWrite64 (Address + i * 8, *(UINT64 *)((UINT8 *)Buffer + i * 8));
      }
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

//
// --- EDKII_UFS_HC_PLATFORM_PROTOCOL ---
//

STATIC
EFI_STATUS
EFIAPI
QcomUfsOverrideHcInfo (
  IN     EFI_HANDLE          ControllerHandle,
  IN OUT EDKII_UFS_HC_INFO   *HcInfo
  )
{
  if (HcInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  //
  // Keep whatever the controller reports.  CAP advertises 32 transfer slots
  // and UFSHCI 3.0, which is what UfsPassThru expects.
  //
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QcomUfsPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  //
  // No board-specific sequence: power, clocks, PHY and the link were all set
  // up before we were loaded, and we deliberately do not touch them.
  //
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
QcomUfsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  //
  // Hand the controller a quiescent state.  EDK2's UfsPassThru will rewrite
  // UTRLBA/UTMRLBA and set the Run/Stop bits; doing that while ABL's lists are
  // still running is exactly the handover hazard we must avoid.
  //
  QcomUfsTrace (1);                                    // entry reached
  MmioWrite32 ((UINTN)QCOM_UFS_HC_BASE + 0x0060, 0);   // UTRLRSR = stop
  QcomUfsTrace (2);
  MmioWrite32 ((UINTN)QCOM_UFS_HC_BASE + 0x0080, 0);   // UTMRLRSR = stop
  QcomUfsTrace (3);
  for (UINTN i = 0; i < 100; i++) {
    if ((MmioRead32 ((UINTN)QCOM_UFS_HC_BASE + 0x0060) & 1) == 0) {
      break;
    }
    gBS->Stall (1000);
  }
  QcomUfsTrace (4);                                    // poll finished
  MmioWrite32 ((UINTN)QCOM_UFS_HC_BASE + 0x0058, 0xFFFFFFFF);  // clear doorbells
  MmioWrite32 ((UINTN)QCOM_UFS_HC_BASE + 0x0078, 0xFFFFFFFF);

  QcomUfsTrace (5);                                    // doorbells cleared

  mUfsHc.GetUfsHcMmioBar = QcomUfsGetMmioBar;
  mUfsHc.AllocateBuffer  = QcomUfsAllocateBuffer;
  mUfsHc.FreeBuffer      = QcomUfsFreeBuffer;
  mUfsHc.Map             = QcomUfsMap;
  mUfsHc.Unmap           = QcomUfsUnmap;
  mUfsHc.Flush           = QcomUfsFlush;
  mUfsHc.Read            = QcomUfsRead;
  mUfsHc.Write           = QcomUfsWrite;

  mUfsHcPlatform.Version         = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  mUfsHcPlatform.OverrideHcInfo  = QcomUfsOverrideHcInfo;
  mUfsHcPlatform.Callback        = QcomUfsPlatformCallback;
  mUfsHcPlatform.RefClkFreq      = EdkiiUfsCardRefClkFreq19p2Mhz;
  mUfsHcPlatform.SkipHceReenable = TRUE;
  mUfsHcPlatform.SkipLinkStartup = TRUE;

  QcomUfsTrace (6);                                    // about to install

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &mUfsHc,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  &mUfsHcPlatform,
                  NULL
                  );
  QcomUfsTrace (EFI_ERROR (Status) ? 0x100 : 7);       // installed
  return Status;
}
