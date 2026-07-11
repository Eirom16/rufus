Rufus Qt — Linux Port
=====================

A Qt6/C++ port of [Rufus](https://rufus.ie), the reliable USB formatting utility,
for Linux.

Build Dependencies
-----------------
- CMake ≥ 3.16
- Qt 6 (Base, Core, Widgets, LinguistTools)
- GCC or Clang with C++17 support

Runtime Dependencies
--------------------
| Tool            | Purpose                        | Package              |
|-----------------|--------------------------------|----------------------|
| `parted`        | Partition table creation       | `parted`             |
| `partprobe`     | Kernel partition table refresh | `parted`             |
| `udevadm`       | Wait for new device nodes      | `systemd` / `udev`   |
| `mkfs.fat`      | FAT filesystem formatting      | `dosfstools`         |
| `mkfs.ntfs`     | NTFS formatting                | `ntfs-3g`            |
| `ntfs-3g`       | NTFS mounting/copying fallback | `ntfs-3g`            |
| `mkfs.ext2/3/4` | ext2/3/4 formatting            | `e2fsprogs`          |
| `dd`            | Raw ISO/image writing          | `coreutils`          |
| `badblocks`     | Bad block scanning             | `e2fsprogs`          |
| `mount`/`umount`| Mounting ISO/partitions        | `util-linux`         |
| `grub-install`  | GRUB BIOS boot (ISO mode, MBR) | `grub`               |
| `sync`          | Cache flush                    | `coreutils`          |
| `cp`            | File copy (ISO extraction)     | `coreutils`          |

> `grub-install` is only needed for ISO extraction mode targeting MBR/BIOS boot;
> GPT/UEFI boot does not require it.

Building
--------
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo ./build/src/rufus-qt
```

Translations are compiled automatically during build via `lrelease`.

Usage Notes
-----------
- Run with `sudo` (required for `parted`, `dd`, `mount`, etc.).
- The image mode selector (ISO vs DD) determines how ISOs are written:
  - **ISO Image mode** (default): extracts ISO contents to a formatted FAT32
    partition and installs GRUB for BIOS boot on MBR targets. Best for Linux ISOs
    and general-purpose bootable USBs.
  - **DD Image mode**: writes the ISO image byte-for-byte with `dd`. Best for
    Windows ISOs and disk images that need raw sector layout.
- Theme: System/Light/Dark toggles under **Settings → Settings**.
