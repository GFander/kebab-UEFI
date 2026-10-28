/** @file
  Kebab RPMh probe (read-only).

  Locates the RPMh protocol published by RpmhDxe, prints the interface pointer
  and the first entries of its function table, converting each pointer to an
  image RVA so the static analysis can be checked at runtime.  Nothing is
  written to RPMh/RSC and no protocol function is called.

  Output uses DEBUG at ERROR level: the platform sets
  PcdDebugPrintErrorLevel to 0x80000000, so only errors are emitted.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/LoadedImage.h>
#include <Guid/EventGroup.h>

STATIC EFI_GUID mKebabRpmhProtocolGuid = {
  0xe1d0b573, 0xf29f, 0x495d,
  { 0x93, 0x52, 0xca, 0x0d, 0xd4, 0x2a, 0x0a, 0x74 }
};

STATIC EFI_EVENT mProbeEvent;

/*
 * Direct producer for the firmware log ring (0xF44C0000, 64 pages, header:
 * u32 magic, u32 size, u64 head, then the byte ring).  The ring is reserved by
 * MemoryMapLib, so it is ours to write; the kernel reads it later.
 */
#define KEBAB_FWLOG_PA        0xF44C0000ULL
#define KEBAB_FWLOG_MAGIC     0x474C464Bu
#define KEBAB_FWLOG_DATA_SIZE 0x3FFF0u

STATIC
VOID
KebabRingWrite (
  IN CONST CHAR8  *Text,
  IN UINTN        Length
  )
{
  volatile UINT32  *Magic = (volatile UINT32 *)(UINTN)KEBAB_FWLOG_PA;
  volatile UINT32  *Size  = (volatile UINT32 *)(UINTN)(KEBAB_FWLOG_PA + 4);
  volatile UINT64  *Head  = (volatile UINT64 *)(UINTN)(KEBAB_FWLOG_PA + 8);
  volatile UINT8   *Data  = (volatile UINT8  *)(UINTN)(KEBAB_FWLOG_PA + 16);
  UINT64            head;
  UINT32            size;
  UINTN             index;

  if (*Magic != KEBAB_FWLOG_MAGIC) {
    return;
  }
  size = *Size;
  if ((size == 0) || (size > KEBAB_FWLOG_DATA_SIZE)) {
    return;
  }
  head = __atomic_load_n (Head, __ATOMIC_RELAXED);
  for (index = 0; index < Length; index++) {
    Data[(head + index) % size] = (UINT8)Text[index];
  }
  __atomic_store_n (Head, head + Length, __ATOMIC_RELEASE);
}

STATIC
UINT64
ImageRvaOf (
  IN UINT64  Address
  )
{
  EFI_STATUS  Status;
  UINTN       HandleCount;
  EFI_HANDLE  *Handles;
  UINTN       Index;
  UINT64      Rva = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadedImageProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return 0;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_LOADED_IMAGE_PROTOCOL  *Image;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid,
                    (VOID **)&Image);
    if (EFI_ERROR (Status) || Image == NULL || Image->ImageBase == NULL) {
      continue;
    }

    if (Address >= (UINT64)(UINTN)Image->ImageBase &&
        Address < (UINT64)(UINTN)Image->ImageBase + Image->ImageSize) {
      Rva = Address - (UINT64)(UINTN)Image->ImageBase;
      break;
    }
  }

  FreePool (Handles);
  return Rva;
}

STATIC
VOID
KrpPrint (
  IN CONST CHAR8  *Text
  )
{
  UINTN  length;

  for (length = 0; Text[length] != 0; length++) {
  }
  KebabRingWrite (Text, length);
  KebabRingWrite ("\n", 1);
  DEBUG ((DEBUG_ERROR, "%a\n", Text));
}

STATIC
VOID
EFIAPI
KebabRpmhProbeNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *Interface;
  UINT64      *Table;
  UINTN       Index;
  CHAR8       Line[128];

  gBS->CloseEvent (Event);

  Status = gBS->LocateProtocol (&mKebabRpmhProtocolGuid, NULL, &Interface);
  if (EFI_ERROR (Status)) {
    KrpPrint ("KRP: protocol not found");
    return;
  }

  KrpPrint ("KRP: rpmh protocol found");

  AsciiSPrint (Line, sizeof (Line), "KRP: iface=%p rva=%x", Interface,
    ImageRvaOf ((UINT64)(UINTN)Interface));
  KrpPrint (Line);

  Table = (UINT64 *)Interface;
  for (Index = 0; Index < 24; Index++) {
    UINT64  Fn = Table[Index];

    if (Fn == 0) {
      AsciiSPrint (Line, sizeof (Line), "KRP: [%u] null", (UINT32)Index);
      KrpPrint (Line);
      continue;
    }

    AsciiSPrint (Line, sizeof (Line), "KRP: [%u] %p rva=%x",
      (UINT32)Index, (VOID *)(UINTN)Fn, ImageRvaOf (Fn));
    KrpPrint (Line);
  }
}

EFI_STATUS
EFIAPI
KebabRpmhProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  KebabRingWrite ("KRP: entry reached\n", 19);

  /*
   * Same pattern as the [DIAG] dump that is known to reach the log: arm a
   * 5 second timer and print from its callback, i.e. after BDS has connected
   * the console.  The RPMh protocol is still installed there, because
   * RpmhDxe only uninstalls it in its ExitBootServices callback.
   */
  KebabRingWrite ("KRP: entry reached\n", 19);

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, KebabRpmhProbeNotify,
                  NULL, &mProbeEvent);
  if (EFI_ERROR (Status)) {
    KrpPrint ("KRP: CreateEvent failed");
    return Status;
  }

  /*
   * Dump now instead of waiting for the 5 s timer: the timer never fires
   * before the payload takes over, and this driver's DEBUG() output does not
   * reach the ring.  The APRIORI list places this driver after RpmhDxe, so the
   * protocol is already installed when we get here.
   */
  KebabRpmhProbeNotify (mProbeEvent, NULL);
  return EFI_SUCCESS;
}
