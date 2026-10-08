# RX 580 vBIOS Loader

A UEFI application for initializing an AMD Polaris / RX 580 GPU when its SPI
vBIOS flash chip (or the electrical path to it) is unavailable.

The loader reads a **user-supplied full ROM dump** from a FAT filesystem,
emulates the ROM reads required during initialization, and can expose the ROM
to the operating system through PCI/ACPI mechanisms.

## Quick Setup

```bash
git clone https://github.com/aryan-nikzad/rx580_vbios_loader_git.git
cd rx580_vbios_loader_git
sudo apt install gnu-efi build-essential
make
```

### Add your own vBIOS

Copy one or more `.rom` vBIOS files into the `vbioses/` folder (the installer copies it to the EFI partition):

```text
rx580_vbios_loader_git/
├── vbioses/
│   ├── my_rx580.rom
│   ├── backup.rom
│   └── another_vbios.rom
└── ...
```

You can add **multiple ROMs, from different card brands**. For every card the loader tries the ROMs until one initializes it. With `smart_order` (default) ROM files whose name contains the card's brand (`xfx`, `gigabyte`, `asus`, `msi`, `sapphire`, ...) are tried first. You can also pin a ROM to a specific card (see below).

You must provide your **own vBIOS dump** or a legally obtained compatible ROM. Do not add proprietary ROMs to this repository.

### Install

```bash
sudo ./install_linux.sh
```

> **Requirements:** The system must be booted in UEFI mode and GRUB must be installed. The installation script adds the vBIOS loader to the EFI/GRUB boot process.

Reboot the system after installation.

**Done.** On the next boot a status page shows every RX 580 in the machine, initializes the ones that need it, and then the normal OS boot continues.


> **Important:** This repository intentionally contains **no GPU vBIOS dumps,
> GOP images, extracted firmware, or prebuilt `.efi` loader**. Firmware is
> hardware/vendor material and should be obtained and used by the end user.

## What it does at boot

1. Finds every supported AMD GPU (multi-GPU, any mix of brands) and every `*.rom` file.
2. Shows a **status page** (graphical, or the classic text console) with one tile per card.
3. For each card, in PCI order:
   - already initialised and trained -> **green**, left untouched;
   - otherwise ROMs are tried (pinned ROM, remembered working ROM, then smart order) until one brings the card up -> **yellow**;
   - no ROM worked -> **red**, with the reason.
4. Publishes one ROM per card to the OS through the ACPI VFCT table (the OS picks the image by PCI address), then continues booting.

Under the hood it parses the PCI option-ROM structure, runs the included AtomBIOS interpreter, serves ROM-chip reads from your file during initialization, and remembers results per card in UEFI NVRAM.

Colours: green = ready (already initialised), yellow = initialised by the loader, red = no ROM worked, grey = waiting / disabled.

For the important initialization path, a **full SPI-chip dump** is preferable.
The original project testing found that the Polaris memory-controller data may
live beyond the normal BIOS image. Do not trim a hardware dump just because
the visible BIOS image is smaller.

## 1. Dump your own ROM

If your card's SPI flash is still readable, make a complete dump before doing
anything else. Use a programmer such as a CH341A with the appropriate voltage
adapter, or another reliable SPI programmer.

**Keep the original dump private. Do not commit it to Git.**

The loader does not require a particular filename. Rename your dump to any
simple `.rom` name, for example:

```text
my_rx580_original.rom
```

Then place it here:

```text
vbioses/my_rx580_original.rom
```

If you have multiple dumps, put all of them in `vbioses/`. The loader sorts them
alphabetically. If you want one attempted first, give it a prefix such as:

```text
00_original_full_dump.rom
01_matching_stock_rom.rom
02_fallback_rom.rom
```

A `00_` filename is only an ordering convention; it does not make the ROM more
compatible.

### Where to obtain a stock ROM

If your original dump is unavailable, you can research a matching stock image
from the [TechPowerUp VGA BIOS Collection](https://www.techpowerup.com/vgabios/?model=RX+580).
It contains many RX 580 vendor/model/memory variants. Match the exact board,
memory size, memory vendor, subsystem ID, and preferably the BIOS version.

A downloaded stock ROM is **not equivalent to your own full SPI dump**. In
particular, this loader may need data located outside the normal BIOS image.
Prefer a full dump from your own card whenever possible.

You can also use a ROM dumped by another owner as a diagnostic/compatibility
candidate, but verify that the board and memory configuration are actually
compatible. The project does not endorse arbitrary ROM flashing.

## 2. Build the loader

On Debian/Ubuntu:

```bash
sudo apt install build-essential gnu-efi efibootmgr
make
```

This produces:

```text
vbios_loader.efi
```

No ROM is embedded during the build.

The source tree deliberately does not ship prebuilt firmware-derived `.efi`
GOP files. If you need to extract a UEFI PE image for development/testing, the
optional `tools/extract_pe.py` helper can process a ROM you supply locally.

## 3. Prepare the EFI partition

Everything lives in one folder on the FAT EFI System Partition:

```text
EFI/
└── vbios_loader/
    ├── vbios_loader.efi
    ├── vbios_loader.cfg          optional - see below
    ├── vbioses/
    │   ├── 00_original_full_dump.rom
    │   └── 01_matching_rom.rom
    ├── loader_trace.txt          written by the loader (flushed per line)
    └── regdump_*.bin             only if you request a register dump
```

The loader can also find a `vbioses` directory next to the `.efi`, in the old beta location `EFI/vbios/vbioses`, or in common EFI locations.

From Linux, the included installer copies the loader, your local ROMs, and creates a commented `vbios_loader.cfg` if none exists (an existing one is never overwritten):

```bash
sudo sh install_linux.sh
```

It expects `/boot/efi` by default. Use `ESP=/your/mountpoint` if needed.

## 4. Using the loader

`vbios_loader.efi` is started through a UEFI boot entry (the installer creates one and puts it first), a UEFI shell, or GRUB `chainloader`.

### Status page keys

| Key | Action |
|---|---|
| ARROWS | select a card |
| ENTER / A | start AUTO (before the run) - continue boot (after the run) / run the failed cards again |
| C or SPACE | card page: run AUTO or one ROM on this card now, **P** pin a ROM to the card, **U** unpin, disable the card |
| S | settings page |
| L | log page |
| D | dump the selected card's registers to the loader folder |
| X | disable / enable the selected card |
| R | forget remembered ROMs and per-ROM results of all cards |
| ESC | skip the loader (before the run) |

Any key during the start countdown stops it and keeps the page open.

### Settings

All settings are optional. Priority: **built-in default < `vbios_loader.cfg` < settings page (NVRAM)**. The loader never writes the `.cfg`; the settings page stores only what you change in a UEFI variable. `DEL` on a setting returns it to the file/default value. See `vbios_loader.cfg.example` for every option.

| Setting | Default | Meaning |
|---|---|---|
| `ui_mode` | auto | `gui` (GOP), `text`, or `auto` |
| `ui_scale` | 0 | GUI font magnification (0 = automatic) |
| `boot_mode` | auto | `auto` starts after `menu_timeout`; `menu` waits for you |
| `menu_timeout` | 5 | seconds before AUTO starts (0 = immediately) |
| `finish_secs` | 8 | seconds the final page stays before the OS boots |
| `hold_on_fail` | 1 | wait for a key if a card failed |
| `skip_initialized` | 1 | leave already-initialised cards alone |
| `reset_mode` | gpu | after a failed ROM: `gpu` (AMD config reset of that card), `pcie` (secondary-bus reset, experimental), `reboot` (cold reboot, the next boot continues with the next ROM), `none` |
| `smart_order` | 1 | prefer ROM files named after the card's brand |
| `pin_fallback` | 1 | if a pinned ROM fails, continue with the others |
| `retry_failed` | 0 | retry exhausted cards every boot |
| `engine` | atom | `atom` (built-in interpreter) or `gop` (firmware driver) |
| `loop_ms`, `gop_timeout`, `vfct_only`, `post_dump`, `trace` | | diagnostics, as in the first beta (V/F/P/E keys became settings) |
| `rom_dir` | | ROM folder on the loader's partition |
| `card.BB:DD.F.rom` / `.skip` / `.name` | | per-card ROM pin / ignore / label |

### Multi-GPU notes

- With several 8 GB cards, enable **Above 4G Decoding** in the motherboard BIOS. Without it the firmware may not give every card a register BAR; the card then shows red with `no MMIO BAR`.
- Cards are processed one after another in PCI order; each gets its own history, so mixed brands can use different ROMs.
- A card that is already initialised (for example because its SPI chip works) is detected per card and skipped.

The exact behavior depends on the GPU, ROM, firmware environment, and the
state of the card. A failed initialization can hang or reset the machine.

## 5. Full-dump requirement

The normal BIOS image inside an RX 580 ROM may be around 118–120 KiB while a
physical SPI chip can contain a 256 KiB image.

For this project, do **not** automatically cut a 256 KiB programmer dump down to
the visible BIOS image. Keep the complete dump when available.

If the selected ROM lacks the Polaris memory-controller block, the loader can
look for a usable MC block in another supplied full dump. This is one reason
keeping your own original full dump as the first `00_...rom` candidate is useful.

## Firmware safety

This is low-level firmware/GPU initialization software. Test with a recovery
path available. Do not assume that an RX 580 ROM is interchangeable merely
because both cards are called "RX 580".

The project is intended to **load/use** a ROM for recovery or initialization.
It is not a recommendation to flash an incompatible ROM onto the card.

## Repository hygiene

Do not commit:

- your personal SPI dumps;
- downloaded vendor vBIOS files;
- modified/mining ROMs;
- extracted GOP/PE firmware;
- generated `.efi` binaries;
- diagnostic register dumps.

These patterns are covered by `.gitignore`, but check `git status` before
pushing.

## Licensing

Original project code is released under the MIT License in `LICENSE`.

The `atomlib/` directory contains AMD AtomBIOS-derived code with its own AMD
MIT-style copyright and permission notices. Those notices remain in the source;
see `LICENSES-AMD-ATOMBIOS.txt`.

ROM dumps, vBIOS images, GOP images, and other vendor firmware are intentionally
excluded from this repository and are **not covered by this project's MIT
license**.

## Interface licence note

The GUI font is a bitmap conversion of the Terminus Font (SIL OFL 1.1), see `LICENSES-FONT-TERMINUS.txt`.

## Development

See [`AGENTS.md`](AGENTS.md) for the project-specific development workflow,
architecture notes, build rules, and firmware-handling requirements.
