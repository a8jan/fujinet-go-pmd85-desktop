# ROM images carried by this repository

The PMD 85 system monitors and stock ROM modules are **not** here: they come
from the pinned GPMD85Emulator checkout (`rom/`), staged into
`core/gpmd85-generated/rom/` by `cmake/StageGPMD85.cmake`.

Only what GPMD85Emulator does not ship lives here.

## `fujinet-config-loader.rmm`

The FujiNet CONFIG loader for the PMD 85: a ROM module image that the
PMD 85-2 and 2A monitors auto-run (it starts with `CALL 8C00h` and the monitor's
module header). On the other models with a module slot it is attached the same
way and started from the monitor. It shows the FujiNet banner, copies the bundled FujiNet CONFIG
program from the module into RAM and starts it. This is what the app boots
into by default, loaded as page 0 of the MegaModule.

- Source: https://github.com/a8jan/fujinet-config-loader-pmd85
  (`make clean dist` -> `dist/autorun.rmm`), commit `4d403ba`
- MD5 `93590256e778ba3f154c77f324113401`, 28644 bytes
- Licence: the loader is MIT (Copyright (c) 2024 Jan Krupa); the CONFIG
  program it carries is FujiNet's `fujinet-config` (GPL-3.0-or-later),
  built for the PMD 85 target.

See `COMPLIANCE.md`.
