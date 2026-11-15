# Third-Party Notices

This repository contains excerpted reference sources from GPL-licensed modding
projects. They are documentation/pattern references ONLY and are **excluded from
every build** (see `references/framework-mod.md`, §5.3 "What compiles into what").
They are not required for the skill to function.

## FFB Arcade Plugin (GPL v3+)

- `Examples/Standalone MOD/CrazyTaxi_FFB_ref.cpp`
- `Examples/Standalone MOD/CrazyTaxi_FFB_ref.h`
- `Examples/Standalone MOD/Game_ref.cpp`
- `Examples/Standalone MOD/Game_ref.h`

Carry the upstream GPL v3+ license header in-file ("This file is part of FFB
Arcade Plugin ... GNU General Public License ... version 3 ... or (at your
option) any later version"). Included here as excerpted pattern references for
force-feedback hook design; the excerpts do not compile standalone (missing
`CrazyTaxi.h` / `Game.h`).

Upstream project: FFB Arcade Plugin (GNU GPL v3 or later). Full license text:
https://www.gnu.org/licenses/ (per the in-file header).

## Engine-fixes pattern reference (GPL-licensed project)

- `Examples/Standalone MOD/EngineFixes_ref.cpp`

An excerpt implementing an `AnalogTriggersFix` using the `Utils/MemoryMgr.h` /
`Utils/Patterns.h` utility headers of a GPL-licensed Crazy Taxi engine-fixes
project. The excerpt carries no in-file license header and the exact upstream
repository is not documented in this workspace; the utility headers identify it
as a GPL-family modding project. Included as a pattern reference only; it does
not compile standalone. If you redistribute this repository, verify the upstream
project and extend this notice with its exact name, author, and license before
doing so beyond private use.

## Everything else

All other files in `Examples/` (Lyra's Framework Mod v1.2 sources, the
LyrasTextureMod sources and guides) and the skill itself are the repository
owner's own work.
