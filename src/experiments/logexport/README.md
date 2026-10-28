# LogExport experiment (archived, NOT wired into the build)

Goal: get the UEFI boot log onto a PC over USB, so screenshots are unnecessary.

## Status

| Piece | Result |
|---|---|
| Capture all DEBUG output into a RAM buffer | PASS (verified on device: "spliced 0x45F bytes of log") |
| Pre-built FAT16 image embedded in FV (FREEFORM + PCD) | PASS |
| Splice captured log into the image at a fixed file offset | PASS |
| RAM-backed EFI_BLOCK_IO_PROTOCOL | PASS ([DIAG] BLOCK_IO = 1) |
| UEFI mounts it as a filesystem | PASS ([DIAG] SIMPLE_FS = 1) |
| Hand off to EFI_USB_MSD_PROTOCOL | FAIL (UsbMsdStartDevice: "USB FN IO protocol: Not Found") |
| Windows enumerates the device | FAIL |

## Why it is not in the build

`LogExportDxe` installs a BlockIo device during APRIORI. On this platform that
**broke the BDS Boot Manager** (menu became unreachable). Removing the driver
restored it. Root cause not proven at the instruction level, but the correlation
was confirmed in both directions on real hardware.

Also, without EFI_USBFN_IO_PROTOCOL the export can never complete, so the
driver currently has zero benefit and a known risk.

## To re-enable (do NOT do this without fixing USB first)

1. Copy `LogExportDxe.{c,inf}` to
   `Silicon/Silicium/SiliciumPkg/Drivers/LogExportDxe/`
2. Generate the disk image:
   `python3 make_logdisk.py Silicon/Silicium/SiliciumPkg/Include/Resources/LogDisk.img`
   (the script prints the file data offset; it must match `LOG_FILE_OFFSET`)
3. Add to `SiliciumPkg/SiliciumPkg.dec`:
   `gSiliciumPkgTokenSpaceGuid.PcdLogDiskImageFile|{ 0x51,0x9B,0x2E,0x7A,0xC4,0x18,0x4F,0x63,0x9D,0x22,0x8B,0x41,0xE7,0x36,0x05,0xAF }|VOID*|0x0000A403`
4. Add to `SiliciumPkg/Extra.fdf.inc`:
   ```
   FILE FREEFORM = PCD (gSiliciumPkgTokenSpaceGuid.PcdLogDiskImageFile) {
     SECTION RAW = SiliciumPkg/Include/Resources/LogDisk.img
     SECTION UI = "LogDisk.img"
   }
   ```
5. Add `INF SiliciumPkg/Drivers/LogExportDxe/LogExportDxe.inf` to
   `SiliciumPkg/Extra.fdf.inc` and to `QcomPkg.dsc.inc` [Components].
6. Add the log-capture hook (see `log-capture.txt`).

## Disk image layout (generated)

4 MiB, FAT16, 512 B clusters, root dir at sector 65, data at sector 97.
`BOOTLOG.TXT` is pre-allocated at 512 KiB; its data starts at **0xC200**.
