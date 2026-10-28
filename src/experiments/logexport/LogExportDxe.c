/** @file
  Export the captured UEFI boot log to a PC as a USB mass-storage device.

  ARCHIVED EXPERIMENT -- see README.md. This driver is NOT part of the build.
  It installs a BlockIo device during APRIORI which was observed to make the
  BDS Boot Manager unreachable on kebab. It also cannot complete because
  EFI_USBFN_IO_PROTOCOL is never installed on this platform.
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/BlockIo.h>
#include <Protocol/EFIUsbMsd.h>

#define LOG_REGION_BASE   ((UINTN)0x9FFD0000)
#define LOG_REGION_SIZE   0x00020000U
#define LOG_REGION_MAGIC  0x474F4C55U
#define LOG_FILE_OFFSET   0x0000C200U
#define LOG_FILE_MAX      0x00080000U
#define LOG_DISK_BLOCK    512U
#define LOG_DEFER_US      60000000ULL

typedef struct {
  UINT32 Magic;
  UINT32 Total;
  UINT32 Cursor;
  UINT32 Size;
  UINT8  Data[LOG_REGION_SIZE - 16];
} LOG_REGION_HEADER;

STATIC EFI_BLOCK_IO_MEDIA     mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo;
STATIC UINT8                 *mDiskImage = NULL;
STATIC UINTN                  mDiskSize  = 0;
STATIC EFI_HANDLE             mDiskHandle = NULL;

STATIC EFI_STATUS EFIAPI LogReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Ext) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI LogFlush (EFI_BLOCK_IO_PROTOCOL *This) { return EFI_SUCCESS; }

STATIC EFI_STATUS EFIAPI
LogReadBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
               UINTN BufferSize, VOID *Buffer)
{
  UINTN Offset;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (BufferSize == 0) return EFI_SUCCESS;
  Offset = (UINTN)Lba * LOG_DISK_BLOCK;
  if ((Offset + BufferSize) > mDiskSize) return EFI_INVALID_PARAMETER;
  CopyMem (Buffer, mDiskImage + Offset, BufferSize);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
LogWriteBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
                UINTN BufferSize, VOID *Buffer)
{
  UINTN Offset;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (BufferSize == 0) return EFI_SUCCESS;
  Offset = (UINTN)Lba * LOG_DISK_BLOCK;
  if ((Offset + BufferSize) > mDiskSize) return EFI_INVALID_PARAMETER;
  CopyMem (mDiskImage + Offset, Buffer, BufferSize);
  return EFI_SUCCESS;
}

STATIC VOID
SpliceLogIntoImage (IN OUT UINT8 *Image, IN UINTN ImageSize)
{
  LOG_REGION_HEADER *Log = (LOG_REGION_HEADER *)LOG_REGION_BASE;
  UINTN Length;

  if ((Log->Magic != LOG_REGION_MAGIC) || (Log->Size != sizeof (Log->Data)) ||
      (Log->Cursor > Log->Size)) {
    DEBUG ((EFI_D_ERROR, "LogExport: log region not initialised\n"));
    return;
  }
  Length = Log->Cursor;
  if (Length > LOG_FILE_MAX) Length = LOG_FILE_MAX;
  if ((LOG_FILE_OFFSET + Length) > ImageSize) {
    DEBUG ((EFI_D_ERROR, "LogExport: image too small\n"));
    return;
  }
  CopyMem (Image + LOG_FILE_OFFSET, Log->Data, Length);
  DEBUG ((EFI_D_ERROR, "LogExport: spliced 0x%x bytes of log\n", (UINT32)Length));
}

STATIC VOID EFIAPI
UsbMsdPoll (IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_USB_MSD_PROTOCOL *Msd = (EFI_USB_MSD_PROTOCOL *)Context;
  if (Msd != NULL) Msd->EventHandler (Msd);
}

STATIC VOID EFIAPI
LogExportDeferredStart (IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_STATUS            Status;
  EFI_USB_MSD_PROTOCOL *Msd   = NULL;
  EFI_EVENT             Timer = NULL;

  gBS->CloseEvent (Event);

  Status = gBS->LocateProtocol (&gEfiUsbMsdProtocolGuid, NULL, (VOID **)&Msd);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport[D]: MSD protocol not found: %r\n", Status));
    return;
  }
  Status = Msd->AssignBlkIoHandle (Msd, &mBlockIo, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport[D]: AssignBlkIoHandle failed: %r\n", Status));
    return;
  }
  Status = Msd->StartDevice (Msd);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport[D]: StartDevice failed: %r\n", Status));
    return;
  }
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, UsbMsdPoll, Msd, &Timer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport[D]: poll event failed: %r\n", Status));
    return;
  }
  gBS->SetTimer (Timer, TimerPeriodic, 100000);
  DEBUG ((EFI_D_ERROR, "LogExport[D]: USB mass storage STARTED, disk=0x%x\n", (UINT32)mDiskSize));
}

EFI_STATUS EFIAPI
LogExportEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  VOID      *FileData = NULL;
  UINTN      FileSize = 0;
  EFI_EVENT  Timer    = NULL;

  Status = GetSectionFromAnyFv (PcdGetPtr (PcdLogDiskImageFile), EFI_SECTION_RAW,
                                0, &FileData, &FileSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport: image not found in FV: %r\n", Status));
    return EFI_SUCCESS;
  }

  mDiskImage = AllocatePool (FileSize);
  if (mDiskImage == NULL) {
    DEBUG ((EFI_D_ERROR, "LogExport: out of memory\n"));
    return EFI_SUCCESS;
  }
  CopyMem (mDiskImage, FileData, FileSize);
  mDiskSize = FileSize;
  SpliceLogIntoImage (mDiskImage, mDiskSize);

  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId          = 0;
  mMedia.RemovableMedia   = TRUE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = FALSE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = LOG_DISK_BLOCK;
  mMedia.IoAlign          = 0;
  mMedia.LastBlock        = (EFI_LBA)(mDiskSize / LOG_DISK_BLOCK) - 1;

  mBlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION3;
  mBlockIo.Media       = &mMedia;
  mBlockIo.Reset       = LogReset;
  mBlockIo.ReadBlocks  = LogReadBlocks;
  mBlockIo.WriteBlocks = LogWriteBlocks;
  mBlockIo.FlushBlocks = LogFlush;

  Status = gBS->InstallMultipleProtocolInterfaces (&mDiskHandle,
                  &gEfiBlockIoProtocolGuid, &mBlockIo, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport: install BlockIo failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, LogExportDeferredStart, NULL, &Timer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LogExport: CreateEvent failed: %r\n", Status));
    return EFI_SUCCESS;
  }
  gBS->SetTimer (Timer, TimerRelative, LOG_DEFER_US);

  DEBUG ((EFI_D_ERROR, "LogExport: disk 0x%x prepared, USB start deferred\n", (UINT32)mDiskSize));
  return EFI_SUCCESS;
}
