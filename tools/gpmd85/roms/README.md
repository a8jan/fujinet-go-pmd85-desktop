# ROM images carried by this repository

The PMD 85 system monitors and stock ROM modules are **not** here: they come
from the pinned GPMD85Emulator checkout (`rom/`), staged into
`core/gpmd85-generated/rom/` by `cmake/StageGPMD85.cmake`.

Only what GPMD85Emulator does not ship lives here.

## `fujinet-config.rmm`

FujiNet CONFIG for the PMD 85, as a ROM module image. The PMD 85-2 and 2A
monitors auto-run it (it starts with `CALL 8C00h` and the monitor's module
header). On the other models with a module slot it is attached the same way
and started from the monitor. A small loader in front shows the FujiNet logo
and a progress bar while it copies CONFIG from the module into RAM, then
starts it. This is what the app boots into by default.

- Built by https://github.com/a8jan/fujinet-config-loader-pmd85, commit
  `c294cd9` (`make clean dist` -> `dist/autorun.rmm`), around `config.bin`
  from https://github.com/FujiNetWIFI/fujinet-config, commit `968d482` on
  `main` (`make clean pmd85` -> `r2r/pmd85/config.bin`), the two checkouts
  side by side
- MD5 `cd63bc06d1bb008ceae996450cc7080d`, 28503 bytes
- Earlier builds, all named `fujinet-config-loader.rmm`. A user's ROM folder
  still holding one of them exactly loses it at the next start, and gets
  this one (`superseded_roms` in `core/src/paths.c`, where each replaced
  image goes):
  - `3887f6a590ea5a0712387f7d34967fe7`, 28505 bytes: the loader's progress
    bar work, CONFIG from `pmd85-keyboard` at `f07e9a0`
  - `7fd0723d9b52595b06641b5d2fc13ca4`, 28481 bytes: the same loader work,
    CONFIG from `pmd85-keyboard` before it was committed (on `be99dd5`)
  - `93590256e778ba3f154c77f324113401`, 28644 bytes: the 2024 build, `4d403ba`
- Licence: the loader is MIT (Copyright (c) 2024 Jan Krupa); the CONFIG
  program it carries is FujiNet's `fujinet-config` (GPL-3.0-or-later),
  built for the PMD 85 target.

See `COMPLIANCE.md`.
