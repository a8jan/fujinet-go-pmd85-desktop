# Licence compliance

FujiNet Go PMD 85 is distributed under the GPL-3.0-or-later. Every component
that enters the build, where it comes from, its licence, and how it gets in:

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This repository | fujinet-go-pmd85-desktop | GPL-3.0-or-later | source |
| Session/FujiNet/paths/settings code and the debugger's expression evaluator, prompt, symbol table, memory tools and DriveWire trace (`core/debugger/`), derived from fujinet-go-coco-desktop | Thomas Cherryhomes | GPL-3.0-or-later | adapted source |
| GPMD85Emulator machine sources (`src/Chip*`, `SystemPIO`, `Iif*`, `Pmd32`, `Mif85`, `Mouse602`, `RomModule`, `RomMegaModule`, `CommonUtils`, `globals.h`, `sigslot.hpp`) | https://github.com/mborik/GPMD85Emulator, pinned in `cmake/Dependencies.cmake` | MIT (Roman Borik, Martin Borik); `sigslot.hpp` MIT (Pierre-Antoine Lacaze) | staged and compiled unmodified (`cmake/StageGPMD85.cmake`) |
| Logic ported from GPMD85Emulator's TEmulator/ScreenPMD85/SoundDriver into `core/pmd85/pmd85_host.cpp` | same | MIT | adapted source, attribution in the file header |
| SAASound (`saa/`) | via GPMD85Emulator | BSD-style (see `saa/LICENCE`) | staged and compiled |
| Becker port device (`core/pmd85/pmd85_becker.{h,cpp}`) | Jan Krupa's `becker-port` branch of GPMD85Emulator (written against the older, GPL-licensed tree; upstream has since moved to MIT) | MIT (Jan Krupa, FujiNet Go contributors), matching GPMD85Emulator | adapted source |
| fujinet-firmware | https://github.com/FujiNetWIFI/fujinet-firmware (fork pin, see `cmake/Dependencies.cmake`) | GPL-3.0 | built as `libfujinet`, loaded at run time |
| SDL3 | libsdl.org | zlib | linked (audio) |
| GTK4, libadwaita (GNOME frontend) | GNOME | LGPL-2.1-or-later | linked |

## Firmware images

Embedded into the binaries by `tools/gpmd85/embed-roms.py` and written to the
user's ROM folder on first start (a user's own file of the same name wins):

- **The GPMD85Emulator `rom/` set**: the system monitors of the PMD 85-1, -2,
  -2A, -3, Mato, Didaktik Alfa/Alfa 2 and Consul 2717, and the stock ROM-module
  contents (BASIC-G, Pascal, demos, the PMD 32 booter). These are 1980s
  Czechoslovak firmware (Tesla Piešťany, Didaktik Skalica, Consul and
  community authors) that GPMD85Emulator has redistributed for years; its
  `rom/README.md` documents each image but states no licence. This project
  bundles them on the same basis as the upstream emulator does. Any rights
  holder who objects should open an issue and the image will be removed from
  the embedded set (the machine then reads a user-supplied copy from the ROM
  folder).
- **`fujinet-config-loader.rmm`**: the FujiNet CONFIG loader module,
  https://github.com/a8jan/fujinet-config-loader-pmd85 (MIT, Jan Krupa),
  carrying FujiNet's `fujinet-config` (GPL-3.0). Provenance and checksum in
  `tools/gpmd85/roms/README.md`.

## Monitor symbols

`core/debugger/monitor_symbols.c` holds label names and addresses only (no
comments, no code) of the PMD 85-1 and 85-2 system monitors, extracted by
`tools/gpmd85/extract-monitor-symbols.py` from two publicly available
commented ROM listings, and each verified against the ROM images above:

- *Monitor PMD-85-1* (HTML), whose names follow the ÚVSSM manual "Popis
  základního programového vybavení mikropočítače PMD-85";
- *Komentovaný výpis monitoru PMD-85-2* (PDF, MACRO-80 listing) by ing. Vít
  Libovický and Jiří Olmer.

Their licence is not stated. Only the names are used (the identifiers the
original authors gave the routines, as one would cite them); a rights holder
who objects should open an issue and the table will be removed.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the sibling
apps') on the PMD 85 case's dark grey, its centre disc in the blue of the
function keys (`tools/icons/make-icons.py`; only the two colours were sampled from a 3D
render of the machine, nothing of the render is included). "PMD 85" and
"Tesla" name the machines emulated and their maker; the project is not
affiliated with or endorsed by any holder of those names.

## Deliberately not used

- GPMD85Emulator's Dear ImGui user interface (`gui/`), its SDL2/OpenGL screen
  and sound drivers and its settings layer: not compiled; the native
  frontends and `pmd85_host.cpp` replace them.
