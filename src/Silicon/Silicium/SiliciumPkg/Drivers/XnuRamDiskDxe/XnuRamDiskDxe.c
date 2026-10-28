/** @file
  XNU-001a-4: minimal read-only BlockIo producer over an embedded FAT12
  image, plus an immediate (TPL_APPLICATION) connect + file verification.

  No events, no timers, no protocol notify: everything happens in the driver
  entry point, which DXE dispatches at TPL_APPLICATION.

  (XNU-001a-3 tried CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_APPLICATION, ...).
   Mu_Basecore Event.c:436 rejects NotifyTpl <= TPL_APPLICATION with
   EFI_INVALID_PARAMETER, so that event was never created.)
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define XNU_FAT_GUID \
  { 0x7B1C2A44, 0x5E3D, 0x4F6A, { 0x9C, 0x11, 0x8E, 0x2D, 0x4F, 0x6A, 0x1B, 0x23 }}

STATIC EFI_GUID  mXnuFatGuid = XNU_FAT_GUID;
STATIC VOID     *mFatImage   = NULL;
STATIC UINTN     mFatSize    = 0;
STATIC EFI_BLOCK_IO_MEDIA    mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo;
STATIC EFI_HANDLE            mHandle = NULL;

STATIC CONST CHAR8 mExpected[] =
  "XNU-000 RAM FAT probe file.\n"
  "Generated for the OnePlus 8T kebab UEFI bring-up.\n"
  "This file is read-only test data.\n";
#define EXPECTED_LEN  (sizeof (mExpected) - 1)

STATIC UINT32 Fnv1a (IN CONST UINT8 *B, IN UINTN N)
{
  UINT32 h = 2166136261u;
  for (UINTN i = 0; i < N; i++) { h ^= B[i]; h *= 16777619u; }
  return h;
}

STATIC EFI_STATUS EFIAPI BlkReset (IN EFI_BLOCK_IO_PROTOCOL *T, IN BOOLEAN V)
{ return EFI_SUCCESS; }

STATIC EFI_STATUS EFIAPI BlkRead (
  IN EFI_BLOCK_IO_PROTOCOL *T, IN UINT32 MediaId, IN EFI_LBA Lba,
  IN UINTN BufferSize, OUT VOID *Buffer)
{
  UINT64 Blocks;
  UINT64 Offset;

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;               // nothing to transfer
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((BufferSize % mMedia.BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Blocks = (UINT64)(BufferSize / mMedia.BlockSize);

  //
  // Validate the whole request BEFORE computing any byte offset, so a huge
  // LBA cannot wrap around and alias into the image.
  //
  if (Lba > mMedia.LastBlock) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Blocks - 1) > ((UINT64)mMedia.LastBlock - (UINT64)Lba)) {
    return EFI_INVALID_PARAMETER;
  }

  Offset = MultU64x32 ((UINT64)Lba, mMedia.BlockSize);   // Lba is bounded now
  if ((Offset + BufferSize) > (UINT64)mFatSize) {
    return EFI_INVALID_PARAMETER;                        // defensive
  }
  CopyMem (Buffer, (UINT8 *)mFatImage + (UINTN)Offset, BufferSize);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI BlkWrite (
  IN EFI_BLOCK_IO_PROTOCOL *T, IN UINT32 MediaId, IN EFI_LBA Lba,
  IN UINTN BufferSize, IN VOID *Buffer)
{ return EFI_WRITE_PROTECTED; }

STATIC EFI_STATUS EFIAPI BlkFlush (IN EFI_BLOCK_IO_PROTOCOL *T)
{ return EFI_SUCCESS; }

STATIC EFI_STATUS ProbeVolume (IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs)
{
  EFI_STATUS St;
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  UINT8 Buf[EXPECTED_LEN + 64];
  UINTN ReadSize = sizeof (Buf);

  St = Sfs->OpenVolume (Sfs, &Root);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] OpenVolume      = %r\n", St));
  if (EFI_ERROR (St)) return St;

  St = Root->Open (Root, &File, L"\\XNU-TEST.TXT", EFI_FILE_MODE_READ, 0);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] FileOpen        = %r\n", St));
  if (EFI_ERROR (St)) { Root->Close (Root); return St; }

  ZeroMem (Buf, sizeof (Buf));
  St = File->Read (File, &ReadSize, Buf);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] FileRead        = %r (%d bytes)\n", St, (UINT32)ReadSize));

  if (!EFI_ERROR (St) && ReadSize == EXPECTED_LEN &&
      CompareMem (Buf, mExpected, EXPECTED_LEN) == 0) {
    DEBUG ((EFI_D_ERROR, "[XNU-RAM] HashMatch       = 1 (fnv1a=0x%08x)\n", Fnv1a (Buf, ReadSize)));
  } else {
    DEBUG ((EFI_D_ERROR, "[XNU-RAM] HashMatch       = 0 (fnv1a=0x%08x)\n", Fnv1a (Buf, ReadSize)));
  }
  File->Close (File);
  Root->Close (Root);
  return EFI_SUCCESS;
}

STATIC BOOLEAN ProbeAllVolumes (VOID)
{
  EFI_STATUS St;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs = NULL;
  BOOLEAN Found = FALSE;

  St = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] SimpleFsReady   = %d handle(s), status %r\n", (UINT32)Count, St));
  if (EFI_ERROR (St) || Handles == NULL) return FALSE;

  for (UINTN i = 0; i < Count && !Found; i++) {
    Sfs = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs)))
      continue;
    DEBUG ((EFI_D_ERROR, "[XNU-RAM] Trying Sfs[%d] = %p\n", (UINT32)i, Handles[i]));
    if (!EFI_ERROR (ProbeVolume (Sfs))) Found = TRUE;
  }
  FreePool (Handles);
  return Found;
}

EFI_STATUS EFIAPI XnuRamDiskDxeEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS St;
  EFI_DEVICE_PATH_PROTOCOL *Dp;
  VENDOR_DEVICE_PATH *Vn;
  EFI_TPL Tpl;

  DEBUG ((EFI_D_ERROR, "[XNU-RAM] XnuRamDiskDxe entry (BlockIo mode)\n"));

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] EntryTpl        = %d (4=APPLICATION 8=CALLBACK 16=NOTIFY 31=HIGH)\n", (UINT32)Tpl));

  St = GetSectionFromAnyFv (&mXnuFatGuid, EFI_SECTION_RAW, 0, &mFatImage, &mFatSize);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] ImageFound      = %r\n", St));
  if (EFI_ERROR (St) || mFatImage == NULL) { DEBUG ((EFI_D_ERROR, "[XNU-RAM] ImageSize       = 0 (abort)\n")); return EFI_SUCCESS; }
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] ImageSize       = %d bytes @ %p\n", (UINT32)mFatSize, mFatImage));

  if (mFatSize < 512) { DEBUG ((EFI_D_ERROR, "[XNU-RAM] ImageSize       = too small (abort)\n")); return EFI_SUCCESS; }

  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId          = 0x584E5531;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.BlockSize        = 512;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (EFI_LBA)(mFatSize / 512) - 1;

  mBlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBlockIo.Media       = &mMedia;
  mBlockIo.Reset       = BlkReset;
  mBlockIo.ReadBlocks  = BlkRead;
  mBlockIo.WriteBlocks = BlkWrite;
  mBlockIo.FlushBlocks = BlkFlush;

  //
  // CreateDeviceNode() allocates a single node WITHOUT a terminating node.
  // Installing that raw buffer as EFI_DEVICE_PATH_PROTOCOL would make every
  // device-path walk run past the allocation. AppendDevicePathNode(NULL, ..)
  // returns node + END_DEVICE_PATH (24 bytes total for a vendor node).
  //
  Vn = (VENDOR_DEVICE_PATH *)CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  if (Vn == NULL) {
    return EFI_SUCCESS;
  }
  CopyGuid (&Vn->Guid, &mXnuFatGuid);
  Dp = AppendDevicePathNode (NULL, (EFI_DEVICE_PATH_PROTOCOL *)Vn);
  FreePool (Vn);              // node is copied into Dp
  Vn = NULL;
  if (Dp == NULL) {
    return EFI_SUCCESS;
  }
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] DevicePathSize  = %d bytes (needs 24)\n", (UINT32)GetDevicePathSize (Dp)));

  St = gBS->InstallMultipleProtocolInterfaces (
         &mHandle,
         &gEfiDevicePathProtocolGuid, Dp,
         &gEfiBlockIoProtocolGuid,    &mBlockIo,
         NULL);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] RegisterStatus  = %r (BlockIo installed)\n", St));
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] DevicePath      = %p handle=%p\n", Dp, mHandle));
  if (EFI_ERROR (St)) return EFI_SUCCESS;

  //
  // DiskIo/Partition/Fat are in the APRIORI list, so their Driver Bindings
  // are already installed when a normal DXE driver runs. Connect only our
  // own controller; Recursive lets DiskIo -> Partition -> Fat cascade.
  //
  St = gBS->ConnectController (mHandle, NULL, NULL, TRUE);
  DEBUG ((EFI_D_ERROR, "[XNU-RAM] ConnectStatus   = %r (scoped to RAM disk)\n", St));

  if (!ProbeAllVolumes ()) {
    DEBUG ((EFI_D_ERROR, "[XNU-RAM] FileRead        = NO_VOLUME\n"));
  }
  return EFI_SUCCESS;
}
