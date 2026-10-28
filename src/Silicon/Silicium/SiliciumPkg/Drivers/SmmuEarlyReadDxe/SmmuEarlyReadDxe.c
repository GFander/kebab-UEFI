/** @file
  XNU-004 discriminator: read two SMMU registers at APRIORI time, write nothing.

  Earlier images told us that a driver which both reads and writes the SMMU at
  this point crashes the device.  That leaves two hypotheses:

    (1) writing the SMMU from the non-secure side is fatal;
    (2) merely touching the SMMU this early is fatal (clock/power not up).

  This driver only reads ID0/ID1, stores the values in a UEFI variable so the
  BDS-stage diagnostics can print them, and returns.  It prints nothing and
  writes no hardware register.

  Copyright (c) 2026. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define SMMU_BASE   0x15000000u
#define SMMU_ID0    0x20
#define SMMU_ID1    0x24

STATIC EFI_GUID  mVendorGuid = { 0x7B1C2A44, 0x5E3D, 0x4F6A, { 0x9C, 0x11, 0x8E, 0x2D, 0x4F, 0x6A, 0x1B, 0x23 } };

EFI_STATUS
EFIAPI
SmmuEarlyReadEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32 Data[2];

  Data[0] = *(volatile UINT32 *)(UINTN)(SMMU_BASE + SMMU_ID0);
  Data[1] = *(volatile UINT32 *)(UINTN)(SMMU_BASE + SMMU_ID1);

  gRT->SetVariable (
         L"XnuSmmuEarlyRead",
         &mVendorGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS,
         sizeof (Data),
         Data
         );
  return EFI_SUCCESS;
}
