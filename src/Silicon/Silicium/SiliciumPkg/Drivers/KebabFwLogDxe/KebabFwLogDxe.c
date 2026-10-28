/** @file
  Kebab firmware log reservation.

  Reserves KEBAB_FWLOG_PA as EfiReservedMemoryType and marks it with the KFLG
  magic, so the framebuffer serial port library may start appending and the
  kernel can find the buffer at a fixed physical address.

  This is read/write memory only: no RPMh/RSC access, no protocol calls.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <KebabFwLog.h>

EFI_STATUS
EFIAPI
KebabFwLogDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;
  KEBAB_FWLOG           *Log;

  Address = KEBAB_FWLOG_PA;
  Status  = gBS->AllocatePages (AllocateAddress, EfiReservedMemoryType,
                  KEBAB_FWLOG_PAGES, &Address);
  if (EFI_ERROR (Status)) {
    //
    // Expected: the range is described as EfiReservedMemoryType by the
    // "Kebab FwLog" entry in MemoryMapLib, so the page is already owned and
    // AllocatePages cannot hand it out.  The descriptor is the contract, so
    // keep going and publish the header; the alternative - bailing out here -
    // is what made the ring invisible on the first hardware test.
    //
    DEBUG ((DEBUG_ERROR,
      "[KFLG] %lx already reserved by the memory map (%r); using it\n",
      (UINT64)KEBAB_FWLOG_PA, Status));
  }

  Log = (KEBAB_FWLOG *)(UINTN)Address;
  Log->Magic = 0;
  Log->Size  = KEBAB_FWLOG_DATA_SIZE;
  Log->Head  = 0;
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
  Log->Magic = KEBAB_FWLOG_MAGIC;

  DEBUG ((DEBUG_ERROR,
    "[KFLG] reserved %lx size %x, log starts here\n",
    (UINT64)Address, KEBAB_FWLOG_DATA_SIZE));
  return EFI_SUCCESS;
}
