/** @file
  XNU-004 stage 2b: put the UFS streams into SMMU BYPASS.

  Runs from APRIORI *before* UFSDxe.  Deliberately prints nothing: at that
  point the console does not exist yet, and Print() asserts (that mistake cost
  us one boot cycle already).  The resulting state is re-read later from
  PlatformBootManagerAfterConsole(), where printing is safe.

  Why: ABL leaves SID 0x4E0 translated through context bank 0.  UFSDxe, once
  its redundant SMMU configuration is skipped, allocates fresh DMA rings that
  are not mapped in that context, so the first transfer faults.  Switching the
  UFS streams to BYPASS makes DMA use physical addresses, which those new
  buffers are.

  Copyright (c) 2026. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define SMMU_BASE     0x15000000u
#define SMMU_ID1      0x24
#define SMMU_SMR(n)   (0x800 + (n) * 4)
#define SMMU_S2CR(n)  (0xC00 + (n) * 4)

#define SMR_VALID     (1u << 31)
#define S2CR_TYPE_SHIFT  16
#define S2CR_TYPE_MASK   0x3
#define S2CR_TYPE_BYPASS 1

//
// UFS stream IDs on SM8250: the vendor DAL IORT maps ARID 0x0 -> 0x4E0 and
// ARID 0x10 -> 0xE0, and the upstream SM8250 DT lists the same two IDs.
//
STATIC CONST UINT16  mUfsSids[] = { 0x04E0, 0x00E0 };

STATIC
UINT32
Rd32 (
  IN UINTN A
  )
{
  return *(volatile UINT32 *)A;
}

STATIC
VOID
Wr32 (
  IN UINTN  A,
  IN UINT32 V
  )
{
  *(volatile UINT32 *)A = V;
  (VOID)Rd32 (A + 4);                 // flush
}

EFI_STATUS
EFIAPI
SmmuBypassEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32 Id1    = Rd32 (SMMU_BASE + SMMU_ID1);
  UINT32 Groups = Rd32 (SMMU_BASE + 0x20) & 0x3F;
  UINTN  Slots  = (Groups > 64) ? 64 : Groups;

  (VOID)Id1;

  for (UINTN s = 0; s < Slots; s++) {
    UINT32        Smr  = Rd32 (SMMU_BASE + SMMU_SMR (s));
    UINT32        Sid  = Smr & 0xFFFF;
    UINT32        Mask = (Smr >> 16) & 0xFFFF;
    UINT32        S2cr;
    UINTN         Match;

    if ((Smr & SMR_VALID) == 0) {
      continue;
    }
    //
    // Require an exact match (mask 0) so a wildcard entry shared with other
    // devices is never silently turned into a bypass for them.
    //
    if (Mask != 0) {
      continue;
    }

    Match = 0;
    for (UINTN i = 0; i < ARRAY_SIZE (mUfsSids); i++) {
      if (Sid == mUfsSids[i]) {
        Match = 1;
        break;
      }
    }
    if (Match == 0) {
      continue;
    }

    S2cr  = Rd32 (SMMU_BASE + SMMU_S2CR (s));
    S2cr &= ~(S2CR_TYPE_MASK << S2CR_TYPE_SHIFT);
    S2cr |=  (S2CR_TYPE_BYPASS << S2CR_TYPE_SHIFT);
    Wr32 (SMMU_BASE + SMMU_S2CR (s), S2cr);
  }

  return EFI_SUCCESS;
}
