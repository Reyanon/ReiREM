<p align="center">
<img width="840" height="500" alt="ReiREM" src="vita/sce_sys_src/bg.png" />
</p>

<h1 align="center">ReiREM</h1>

<p align="center">
  <b>Irem arcade board emulator for PlayStation Vita and PSP</b><br>
  FinalBurn Neo based &middot; 10 Irem boards &middot; 196 sets &middot; full speed on a 333&nbsp;MHz PSP
</p>

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [What's New](#2-whats-new)
3. [Highlights](#3-highlights)
4. [Supported Boards and Games](#4-supported-boards-and-games)
5. [Download](#5-download)
6. [Installation](#6-installation)
7. [Usage](#7-usage)
8. [Building from Source](#8-building-from-source)
9. [Project Layout](#9-project-layout)
10. [Accuracy and Testing](#10-accuracy-and-testing)
11. [License and Credits](#11-license-and-credits)

---

## 1. Introduction

ReiREM is a dedicated emulator for Irem arcade hardware, built on the Irem drivers and CPU/sound cores of
[FinalBurn Neo](https://github.com/finalburnneo/FBNeo). It started as a single-game port of
*Ninja Baseball Bat Man* (Irem M92) and grew into a multi-board emulator covering every Irem board family
that FinalBurn Neo supports, from the 1982 M52 (*Moon Patrol*) to the 1993-1995 M107 (*Dream Soccer '94*).

The goal is arcade-perfect speed on handheld hardware: a locked 60 fps with clean audio on the
PlayStation Vita, and on the original PSP-3000 / PSP go, whose single 333 MHz MIPS core is far slower
than the hardware these games were written for.

Both ports share one emulation core (`src/core`). Frame and audio hashes are checked against a desktop
reference harness so that every optimization keeps the emulation bit-exact.

## 2. What's New

### v0.1.400 (PS Vita)

- Two players and external controllers: each player (1P / 2P) picks a device, the Vita itself or a controller.
- New Controls screen: press a button to assign it, *Set all controls* in one go, L2 / R2 / L3 / R3 as keys
  of their own, combo keys made of any two buttons, turbo per device.
- Control settings are stored per device (Vita, DualShock 3, DualShock 4, DualSense, Xbox, Switch Pro, other), so
  each controller keeps its own layout.
- Input engine setting: Native, RePad (Bluetooth plugin bundled with the app) or VitaControl.
  See [Controls and controllers](#controls-and-controllers-ps-vita).
- Touch buttons can press a pair of keys and can be duplicated.
- Fixed help text running out of the left edge of the menu panel.

No changes on PSP in this version.

### v0.1.347

Changes since v0.1.279.

#### PS Vita

- New interface: game list with board tabs and Playable / All / Favorites filters, in-game menu, loading screen
  and six color themes (Graphite by default).
- Games are switched inside the app: *Select game* goes back to the game list without restarting.
  ROMs load on a background thread while the loading screen animates.
- The PS button opens the in-game menu (kernel helper bundled with the app, nothing to install).
- Built-in updates: the app checks GitHub at start and can download and install a new version
  (see [Updates](#updates-ps-vita)).
- Touch buttons: place on-screen buttons that press any key, with a layout editor.
- Smooth 60 fps scrolling in the game list.
- Louder output with a soft limiter.
- Default border artwork follows the theme color.
- M72 board family optimized: the sound CPU runs on its own core next to the main CPU, and the V35 interpreter
  is faster (also helps M90 and the M107 sound CPU).
- New defaults: 4:3, smooth filter, scanlines off, FPS display off, turbo off.

#### PSP

- The same new interface as on the Vita, drawn with the GE and a bundled bitmap font (Noto Sans) made for the PSP screen.
- Games are switched inside the app without restarting the EBOOT; ROMs load on a background thread
  while the loading screen animates.
- More stable game switching (the Media Engine stays ready between games).
- Louder output with a soft limiter.
- Default border artwork follows the theme color.

## 3. Highlights

**Emulation**
- 10 Irem board families, 196 sets (parents, clones and hacks): M52, M57, M58, M62, M63, M72, M75/77, M90/97, M92, M107.
- Cycle-accurate NEC V30/V33/V35 timing, including the prefetch queue model, kept identical to the FBNeo interpreter.
- Deterministic main/sound pipeline: sound-to-main events are applied at a fixed scanline delay, so results
  do not depend on thread scheduling.

**PSP port (MIPS Allegrex @ 333 MHz)**
- Dynamic recompilers (dynarec) for NEC V33, V30 and V35, and for the Z80, with lazy flag evaluation,
  block linking and a self-growing code cache.
- Media Engine (second CPU) runs sound (V35 + YM2151 + GA20) or tile/sprite rendering, depending on the board.
- GE (GPU) renders tile layers directly for M107 and M72-family boards; CLUT index output for the rest.
- Automatic idle-loop detection and batched CPU execution that preserve exact results.
- Safe sleep/resume: the Media Engine is shut down before suspend and restored, with the game state, on wake.

**Vita port (Cortex-A9 @ 444 MHz)**
- Three-core layout: main CPU emulation, sound CPU + synthesis, and rendering on separate cores.
- NEON tile drawing, dynamic audio rate control (+-0.5 %) and vsync-locked frame pacing.
- PS button menu, touch buttons and built-in updates.

**Frontend (both platforms)**
- Game list with per-board tabs, Playable / All / Favorites filters and six color themes.
- Game switching inside the app, with a loading screen while the ROMs load.
- Save states (10 slots plus quick save), DIP switches, button mapping with turbo, cheats (FBNeo `.ini` format).
- Display options: aspect ratio, filter, scanlines, border color or user artwork.
- Per-game settings: save the current display and control settings for one game only.

## 4. Supported Boards and Games

All sets are the FinalBurn Neo sets of the same name. ROMs must match the FinalBurn Neo romset
(non-merged or split; clone sets read missing files from the parent zip).

**Performance**: *Playable*, *Partial* or *Not working*, and the average frame rate. Full speed is the board rate
(for example 55 fps on M72). A rate with `~` is estimated from tested games on the same board, not measured for that set.
Click a board to expand its list.

### PS Vita (444 MHz)

<details>
<summary><b>M92</b> (67 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `bmaster` |  | 1991 | Blade Master (World) | Playable<br>~60 fps |
| `crossbld` | `bmaster` | 1991 | Cross Blades! (Japan) | Playable<br>~60 fps |
| `dsoccr94j` | `dsoccr94` | 1994 | Dream Soccer '94 (Japan, M92 hardware) | Playable<br>~60 fps |
| `gunforc2` |  | 1994 | Gun Force II (US) | Playable<br>~60 fps |
| `geostorm` | `gunforc2` | 1994 | Geo Storm (Japan, 014 custom sound CPU) | Playable<br>~60 fps |
| `gunforce` |  | 1991 | Gunforce - Battle Fire Engulfed Terror Island (World) | Playable<br>~60 fps |
| `gunforcej` | `gunforce` | 1991 | Gunforce - Battle Fire Engulfed Terror Island (Japan) | Playable<br>~60 fps |
| `gunforceu` | `gunforce` | 1991 | Gunforce - Battle Fire Engulfed Terror Island (US) | Playable<br>~60 fps |
| `hook` |  | 1992 | Hook (World) | Playable<br>~60 fps |
| `hook1v4` | `hook` | 2016 | Hook (1v4, Hack) | Playable<br>~60 fps |
| `hook6h` | `hook` | 2022 | Hook (6th Emperor, Hack) | Playable<br>~60 fps |
| `hookbo` | `hook` | 2024 | Hook (Gai Gai, Hack) | Playable<br>~60 fps |
| `hookdj` | `hook` | 2025 | Hook (Item, Hack) | Playable<br>~60 fps |
| `hookdszz` | `hook` | 2023 | Hook (Summit War, Hack) | Playable<br>~60 fps |
| `hookdw` | `hook` | 2016 | Hook (Warriors, Hack) | Playable<br>~60 fps |
| `hookgs` | `hook` | 2022 | Hook (The Expendables, Hack) | Playable<br>~60 fps |
| `hookgs2` | `hook` | 2023 | Hook (The Expendables II, Hack) | Playable<br>~60 fps |
| `hookhd` | `hook` | 2024 | Hook (Pirate Wars, Hack) | Playable<br>~60 fps |
| `hookhd2` | `hook` | 2024 | Hook (Pirate Wars II, Hack) | Playable<br>~60 fps |
| `hookj` | `hook` | 1992 | Hook (Japan) | Playable<br>~60 fps |
| `hookjy` | `hook` | 2024 | Hook (Elite, Hack) | Playable<br>~60 fps |
| `hookjyds` | `hook` | 2025 | Hook (Elite Competition, Hack) | Playable<br>~60 fps |
| `hooknx` | `hook` | 2022 | Hook (Hook's Reversal, Hack) | Playable<br>~60 fps |
| `hooknx2` | `hook` | 2022 | Hook (Hook's Reversal II, Hack) | Playable<br>~60 fps |
| `hookpj` | `hook` | 2024 | Hook (POK KAI, Hack) | Playable<br>~60 fps |
| `hookpls` | `hook` | 2024 | Hook (Plus, Hack) | Playable<br>~60 fps |
| `hooksw` | `hook` | 2022 | Hook (SW, Hack) | Playable<br>~60 fps |
| `hooksw2` | `hook` | 2025 | Hook (SW2, Hack) | Playable<br>~60 fps |
| `hooku` | `hook` | 1992 | Hook (US) | Playable<br>~60 fps |
| `hookx2` | `hook` | 2018 | Hook (X2, Hack) | Playable<br>~60 fps |
| `hookxr` | `hook` | 2018 | Hook (Xin Er, Hack) | Playable<br>~60 fps |
| `hookxx` | `hook` | 2025 | Hook (XX, Hack) | Playable<br>~60 fps |
| `ppan` | `hook` | 1992 | Peter Pan (bootleg of Hook) | Playable<br>~60 fps |
| `inthunt` |  | 1993 | In The Hunt (World) | Playable<br>~60 fps |
| `inthuntk` | `inthunt` | 1993 | In The Hunt (Korea?) | Playable<br>~60 fps |
| `inthuntu` | `inthunt` | 1993 | In The Hunt (US) | Playable<br>~60 fps |
| `kaiteids` | `inthunt` | 1993 | Kaitei Daisensou (Japan) | Playable<br>~60 fps |
| `lethalth` |  | 1991 | Lethal Thunder (World) | Playable<br>~60 fps |
| `thndblst` | `lethalth` | 1991 | Thunder Blaster (Japan) | Playable<br>~60 fps |
| `majtitl2` |  | 1992 | Major Title 2 (World, set 1) | Playable<br>~60 fps |
| `majtitl2a` | `majtitl2` | 1992 | Major Title 2 (World, set 1, alt sound CPU) | Playable<br>~60 fps |
| `majtitl2b` | `majtitl2` | 1992 | Major Title 2 (World, set 2) | Playable<br>~60 fps |
| `majtitl2j` | `majtitl2` | 1992 | Major Title 2 (Japan) | Playable<br>~60 fps |
| `skingame` | `majtitl2` | 1992 | The Irem Skins Game (US set 1) | Playable<br>~60 fps |
| `skingame2` | `majtitl2` | 1992 | The Irem Skins Game (US set 2) | Playable<br>~60 fps |
| `mysticri` |  | 1992 | Mystic Riders (World) | Playable<br>~60 fps |
| `gunhohki` | `mysticri` | 1992 | Mahou Keibitai Gun Hohki (Japan) | Playable<br>~60 fps |
| `mysticrib` | `mysticri` | 1992 | Mystic Riders (bootleg?) | Playable<br>~60 fps |
| `nbbatman` |  | 1993 | Ninja Baseball Bat Man (World) | Playable<br>60 fps |
| `leaguemn` | `nbbatman` | 1993 | Yakyuu Kakutou League-Man (Japan, set 1) | Playable<br>60 fps |
| `leaguemna` | `nbbatman` | 1993 | Yakyuu Kakutou League-Man (Japan, set 2) | Playable<br>60 fps |
| `nbbat1v2` | `nbbatman` | 2020 | Ninja Baseball Batman (1v2, Hack) | Playable<br>60 fps |
| `nbbat1v4` | `nbbatman` | 2020 | Ninja Baseball Batman (1v4, Hack) | Playable<br>60 fps |
| `nbbat1v8` | `nbbatman` | 2020 | Ninja Baseball Batman (1v8, Hack) | Playable<br>60 fps |
| `nbbatmano` | `nbbatman` | 2020 | Ninja Baseball Batman (One Key Edition, Hack) | Playable<br>60 fps |
| `nbbatmanu` | `nbbatman` | 1993 | Ninja Baseball Bat Man (US) | Playable<br>60 fps |
| `nbbatmbh` | `nbbatman` | 2016 | Ninja Baseball Batman (Transform, Hack) | Playable<br>60 fps |
| `nbbatmx2` | `nbbatman` | 2018 | Ninja Baseball Batman (X2, Hack) | Playable<br>60 fps |
| `rtypeleo` |  | 1992 | R-Type Leo (World) | Playable<br>~60 fps |
| `rtypeleoj` | `rtypeleo` | 1992 | R-Type Leo (Japan) | Playable<br>~60 fps |
| `ssoldier` |  | 1993 | Superior Soldiers (US) | Playable<br>~60 fps |
| `psoldier` | `ssoldier` | 1993 | Perfect Soldiers (Japan) | Playable<br>~60 fps |
| `uccops` |  | 1992 | Undercover Cops (World) | Playable<br>~60 fps |
| `uccopsar` | `uccops` | 1992 | Undercover Cops - Alpha Renewal Version (World) | Playable<br>~60 fps |
| `uccopsaru` | `uccops` | 1992 | Undercover Cops - Alpha Renewal Version (US) | Playable<br>~60 fps |
| `uccopsj` | `uccops` | 1992 | Undercover Cops (Japan) | Playable<br>~60 fps |
| `uccopsu` | `uccops` | 1992 | Undercover Cops (US) | Playable<br>~60 fps |

</details>

<details>
<summary><b>M107</b> (4 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `airass` |  | 1993 | Air Assault (World) | Playable<br>60 fps |
| `firebarr` | `airass` | 1993 | Fire Barrel (Japan) | Playable<br>60 fps |
| `dsoccr94` |  | 1994 | Dream Soccer '94 (World, M107 hardware) | Playable<br>60 fps |
| `dsoccr94k` | `dsoccr94` | 1994 | Dream Soccer '94 (Korea, M107 hardware) | Playable<br>60 fps |

</details>

<details>
<summary><b>M90/97</b> (14 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `bbmanw` |  | 1992 | Bomber Man World / New Dyna Blaster - Global Quest | Playable<br>60 fps |
| `bbmanwj` | `bbmanw` | 1992 | Bomber Man World (Japan) | Playable<br>60 fps |
| `bbmanwja` | `bbmanw` | 1992 | Bomber Man World (Japan, revised sound hardware) | Playable<br>60 fps |
| `newapunk` | `bbmanw` | 1992 | New Atomic Punk - Global Quest (US) | Playable<br>60 fps |
| `dicegame` |  | 1991 | Dice - The Dice Game! | Playable<br>~60 fps |
| `dynablst` |  | 1991 | Dynablaster / Bomber Man | Playable<br>~60 fps |
| `atompunk` | `dynablst` | 1991 | Atomic Punk (US) | Playable<br>~60 fps |
| `bombrman` | `dynablst` | 1991 | Bomber Man (Japan) | Playable<br>~60 fps |
| `hasamu` |  | 1991 | Hasamu (Japan) | Playable<br>~60 fps |
| `matchit2` |  | 1993 | Match It II | Playable<br>~60 fps |
| `shisen2` | `matchit2` | 1993 | Shisensho II | Playable<br>~60 fps |
| `quizf1` |  | 1992 | Quiz F1 1-2 Finish (Japan) | Playable<br>~60 fps |
| `riskchal` |  | 1993 | Risky Challenge | Playable<br>~60 fps |
| `gussun` | `riskchal` | 1993 | Gussun Oyoyo (Japan) | Playable<br>~60 fps |

</details>

<details>
<summary><b>M72</b> (48 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `airduel` |  | 1990 | Air Duel (World, M82 hardware) | Playable<br>~55 fps |
| `airdueljm72` | `airduel` | 1990 | Air Duel (Japan, M72 hardware) | Playable<br>~55 fps |
| `airduelm72` | `airduel` | 1990 | Air Duel (World, M72 hardware) | Playable<br>~55 fps |
| `airduelu` | `airduel` | 1990 | Air Duel (US location test, M82 hardware) | Playable<br>~55 fps |
| `bchopper` |  | 1987 | Battle Chopper (World) | Playable<br>~55 fps |
| `mrheli` | `bchopper` | 1987 | Mr. HELI no Daibouken (Japan) | Playable<br>~55 fps |
| `cosmccop` |  | 1991 | Cosmic Cop (World) | Playable<br>~55 fps |
| `gallop` | `cosmccop` | 1991 | Gallop - Armed Police Unit (Japan, M84 hardware) | Playable<br>~55 fps |
| `gallopm72` | `cosmccop` | 1991 | Gallop - Armed Police Unit (Japan, M72 hardware) | Playable<br>~55 fps |
| `dbreed` |  | 1989 | Dragon Breed (World, M81 hardware) | Playable<br>55 fps |
| `dbreedjm72` | `dbreed` | 1989 | Dragon Breed (Japan, M72 hardware) | Playable<br>55 fps |
| `dbreedm72` | `dbreed` | 1989 | Dragon Breed (World, M72 hardware) | Playable<br>55 fps |
| `hharry` |  | 1990 | Hammerin' Harry (World, M81 hardware) | Playable<br>~55 fps |
| `dkgensan` | `hharry` | 1990 | Daiku no Gensan (Japan, M84 hardware) | Playable<br>~55 fps |
| `dkgensanm72` | `hharry` | 1990 | Daiku no Gensan (Japan, M72 hardware) | Playable<br>~55 fps |
| `dkgensanm82` | `hharry` | 1990 | Daiku no Gensan (Japan, M82 hardware) | Playable<br>~55 fps |
| `hharryb` | `hharry` | 1990 | Hammerin' Harry (World, M84 hardware bootleg) | Playable<br>~55 fps |
| `hharryu` | `hharry` | 1990 | Hammerin' Harry (US, M84 hardware) | Playable<br>~55 fps |
| `imgfight` |  | 1988 | Image Fight (World) | Playable<br>~55 fps |
| `imgfightj` | `imgfight` | 1988 | Image Fight (Japan) | Playable<br>~55 fps |
| `imgfightjb` | `imgfight` | 1988 | Image Fight (Japan, bootleg) | Playable<br>~55 fps |
| `loht` |  | 1989 | Legend of Hero Tonma (World) | Playable<br>~55 fps |
| `lohtb` | `loht` | 1989 | Legend of Hero Tonma (Playmark unprotected bootleg) | Playable<br>~55 fps |
| `lohtb2` | `loht` | 1989 | Legend of Hero Tonma (Japan, bootleg with i8751) | Playable<br>~55 fps |
| `lohtb3` | `loht` | 1997 | Legend of Hero Tonma (World, bootleg with i8751) | Playable<br>~55 fps |
| `lohtj` | `loht` | 1989 | Legend of Hero Tonma (Japan) | Playable<br>~55 fps |
| `ltswords` |  | 1991 | Lightning Swords (World) | Playable<br>~55 fps |
| `kengo` | `ltswords` | 1991 | Ken-Go (World) | Playable<br>~55 fps |
| `kengoj` | `ltswords` | 1991 | Ken-Go (Japan) | Playable<br>~55 fps |
| `majtitle` |  | 1990 | Major Title (World) | Playable<br>~55 fps |
| `majtitlej` | `majtitle` | 1990 | Major Title (Japan) | Playable<br>~55 fps |
| `nspirit` |  | 1988 | Ninja Spirit (World) | Playable<br>~55 fps |
| `nspiritj` | `nspirit` | 1988 | Saigo no Nindou (Japan) | Playable<br>~55 fps |
| `poundfor` |  | 1990 | Pound for Pound (World) | Playable<br>~55 fps |
| `poundforj` | `poundfor` | 1990 | Pound for Pound (Japan) | Playable<br>~55 fps |
| `poundforu` | `poundfor` | 1990 | Pound for Pound (US) | Playable<br>~55 fps |
| `rtype` |  | 1987 | R-Type (World) | Playable<br>55 fps |
| `rtypeb` | `rtype` | 1987 | R-Type (World bootleg) | Playable<br>55 fps |
| `rtypej` | `rtype` | 1987 | R-Type (Japan) | Playable<br>55 fps |
| `rtypejp` | `rtype` | 1987 | R-Type (Japan prototype) | Playable<br>55 fps |
| `rtypem82b` | `rtype` | 2009 | R-Type (Japan, bootleg M82 conversion) | Playable<br>55 fps |
| `rtypeu` | `rtype` | 1987 | R-Type (US) | Playable<br>55 fps |
| `rtype2` |  | 1989 | R-Type II (World) | Playable<br>~55 fps |
| `rtype2j` | `rtype2` | 1989 | R-Type II (Japan) | Playable<br>~55 fps |
| `rtype2jc` | `rtype2` | 1989 | R-Type II (Japan, revision C) | Playable<br>~55 fps |
| `rtype2m82b` | `rtype2` | 1997 | R-Type II (Japan, bootleg M82 conversion) | Playable<br>~55 fps |
| `xmultipl` |  | 1989 | X Multiply (World, M81 hardware) | Playable<br>55 fps |
| `xmultiplm72` | `xmultipl` | 1989 | X Multiply (Japan, M72 hardware) | Playable<br>55 fps |

</details>

<details>
<summary><b>M75/77</b> (13 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `buccanrs` |  | 1989 | Buccaneers (set 1) | Playable<br>~56 fps |
| `buccanrsa` | `buccanrs` | 1989 | Buccaneers (set 2) | Playable<br>~56 fps |
| `buccanrsb` | `buccanrs` | 1989 | Buccaneers (set 3, harder) | Playable<br>~56 fps |
| `kikcubic` |  | 1988 | Meikyu Jima (Japan) | Playable<br>~56 fps |
| `kikcubicb` | `kikcubic` | 1988 | Kickle Cubele | Playable<br>~56 fps |
| `vigilant` |  | 1988 | Vigilante (World, Rev E) | Playable<br>56 fps |
| `vigilanta` | `vigilant` | 1988 | Vigilante (World, Rev A) | Playable<br>56 fps |
| `vigilantb` | `vigilant` | 1988 | Vigilante (US, Rev B) | Playable<br>56 fps |
| `vigilantbl` | `vigilant` | 1988 | Vigilante (bootleg) | Playable<br>56 fps |
| `vigilantc` | `vigilant` | 1988 | Vigilante (World, Rev C) | Playable<br>56 fps |
| `vigilantd` | `vigilant` | 1988 | Vigilante (Japan, Rev D) | Playable<br>56 fps |
| `vigilantg` | `vigilant` | 1988 | Vigilante (US, Rev G) | Playable<br>56 fps |
| `vigilanto` | `vigilant` | 1988 | Vigilante (US) | Playable<br>56 fps |

</details>

<details>
<summary><b>M62</b> (25 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `battroad` |  | 1984 | The Battle-Road | Playable<br>~55 fps |
| `horizon` |  | 1985 | Horizon (Irem) | Playable<br>~55 fps |
| `kidniki` |  | 1986 | Kid Niki - Radical Ninja (World) | Playable<br>55 fps |
| `kidnikiu` | `kidniki` | 1986 | Kid Niki - Radical Ninja (US) | Playable<br>55 fps |
| `lithero` | `kidniki` | 1987 | Little Hero | Playable<br>55 fps |
| `yanchamr` | `kidniki` | 1986 | Kaiketsu Yanchamaru (Japan) | Playable<br>55 fps |
| `kungfum` |  | 1984 | Kung-Fu Master (World) | Playable<br>55 fps |
| `kungfub` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 1) | Playable<br>55 fps |
| `kungfub2` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 2) | Playable<br>55 fps |
| `kungfub3` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 3) | Playable<br>55 fps |
| `kungfub3s` | `kungfum` | 1984 | Kung-Fu Senjyo (bootleg, Spanish) | Playable<br>55 fps |
| `kungfumd` | `kungfum` | 1984 | Kung-Fu Master (US) | Playable<br>55 fps |
| `spartanx` | `kungfum` | 1984 | Spartan X (Japan) | Playable<br>55 fps |
| `ldrun` |  | 1984 | Lode Runner (set 1) | Playable<br>~55 fps |
| `ldruna` | `ldrun` | 1984 | Lode Runner (set 2) | Playable<br>~55 fps |
| `ldrun2` |  | 1984 | Lode Runner II - The Bungeling Strikes Back | Playable<br>~55 fps |
| `ldrun3` |  | 1985 | Lode Runner III - The Golden Labyrinth | Playable<br>~55 fps |
| `ldrun3j` | `ldrun3` | 1985 | Lode Runner III - Majin no Fukkatsu (Japan, rev. A) | Playable<br>~55 fps |
| `ldrun3jc` | `ldrun3` | 1985 | Lode Runner III - Majin no Fukkatsu (Japan, rev. C) | Playable<br>~55 fps |
| `ldrun4` |  | 1986 | Lode Runner IV - Teikoku Karano Dasshutsu (Japan) | Playable<br>~55 fps |
| `lotlot` |  | 1985 | Lot Lot | Playable<br>~55 fps |
| `spelunk2` |  | 1986 | Spelunker II - 23 no Kagi (Japan) | Playable<br>~55 fps |
| `spelunkr` |  | 1985 | Spelunker | Playable<br>~55 fps |
| `spelunkrj` | `spelunkr` | 1985 | Spelunker (Japan) | Playable<br>~55 fps |
| `youjyudn` |  | 1986 | Youjyuden (Japan) | Playable<br>~55 fps |

</details>

<details>
<summary><b>M63</b> (4 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `fghtbskt` |  | 1984 | Fighting Basketball | Playable<br>~60 fps |
| `wilytowr` |  | 1984 | Wily Tower | Playable<br>~60 fps |
| `atomboy` | `wilytowr` | 1985 | Atomic Boy (revision B) | Playable<br>~60 fps |
| `atomboya` | `wilytowr` | 1985 | Atomic Boy (revision A) | Playable<br>~60 fps |

</details>

<details>
<summary><b>M58</b> (6 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `10yard` |  | 1983 | 10-Yard Fight (World, set 1) | Playable<br>~60 fps |
| `10yard85` | `10yard` | 1985 | 10-Yard Fight '85 (US, Taito license) | Playable<br>~60 fps |
| `10yardj` | `10yard` | 1983 | 10-Yard Fight (Japan) | Playable<br>~60 fps |
| `vs10yard` | `10yard` | 1984 | Vs 10-Yard Fight (World, 11/05/84) | Playable<br>~60 fps |
| `vs10yardj` | `10yard` | 1984 | Vs 10-Yard Fight (Japan) | Playable<br>~60 fps |
| `vs10yardu` | `10yard` | 1984 | Vs 10-Yard Fight (US, Taito license) | Playable<br>~60 fps |

</details>

<details>
<summary><b>M57</b> (2 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `troangel` |  | 1983 | Tropical Angel | Playable<br>~57 fps |
| `newtangl` | `troangel` | 1983 | New Tropical Angel | Playable<br>~57 fps |

</details>

<details>
<summary><b>M52</b> (13 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `alpha1v` |  | 1988 | Alpha One (Vision Electronics) | Playable<br>~57 fps |
| `mpatrol` |  | 1982 | Moon Patrol | Playable<br>~57 fps |
| `mpatrolut` | `mpatrol` | 2025 | Moon Patrol Unknown Territories | Playable<br>~57 fps |
| `mpatrolw` | `mpatrol` | 1982 | Moon Patrol (Williams) | Playable<br>~57 fps |
| `mranger` | `mpatrol` | 1982 | Moon Ranger (bootleg of Moon Patrol) | Playable<br>~57 fps |
| `shtrider` |  | 1985 | Shot Rider | Playable<br>~57 fps |
| `shtridera` | `shtrider` | 1984 | Shot Rider (Sigma license) | Playable<br>~57 fps |
| `travrusa` |  | 1983 | Traverse USA / Zippy Race | Playable<br>57 fps |
| `motorace` | `travrusa` | 1983 | MotoRace USA | Playable<br>57 fps |
| `mototour` | `travrusa` | 1983 | MotoTour / Zippy Race (Tecfri license) | Playable<br>57 fps |
| `mototoura` | `travrusa` | 1983 | MotoTour / Zippy Race (Assa version of Tecfri license) | Playable<br>57 fps |
| `travrusab` | `travrusa` | 1983 | Traverse USA (bootleg, set 1) | Playable<br>57 fps |
| `travrusab2` | `travrusa` | 1983 | Traverse USA (bootleg, set 2) | Playable<br>57 fps |

</details>

### PSP-3000 / PSP go (333 MHz)

<details>
<summary><b>M92</b> (67 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `bmaster` |  | 1991 | Blade Master (World) | Playable<br>60 fps |
| `crossbld` | `bmaster` | 1991 | Cross Blades! (Japan) | Playable<br>60 fps |
| `dsoccr94j` | `dsoccr94` | 1994 | Dream Soccer '94 (Japan, M92 hardware) | Playable<br>~60 fps |
| `gunforc2` |  | 1994 | Gun Force II (US) | Playable<br>60 fps |
| `geostorm` | `gunforc2` | 1994 | Geo Storm (Japan, 014 custom sound CPU) | Playable<br>60 fps |
| `gunforce` |  | 1991 | Gunforce - Battle Fire Engulfed Terror Island (World) | Playable<br>60 fps |
| `gunforcej` | `gunforce` | 1991 | Gunforce - Battle Fire Engulfed Terror Island (Japan) | Playable<br>60 fps |
| `gunforceu` | `gunforce` | 1991 | Gunforce - Battle Fire Engulfed Terror Island (US) | Playable<br>60 fps |
| `hook` |  | 1992 | Hook (World) | Playable<br>60 fps |
| `hook1v4` | `hook` | 2016 | Hook (1v4, Hack) | Playable<br>60 fps |
| `hook6h` | `hook` | 2022 | Hook (6th Emperor, Hack) | Playable<br>60 fps |
| `hookbo` | `hook` | 2024 | Hook (Gai Gai, Hack) | Playable<br>60 fps |
| `hookdj` | `hook` | 2025 | Hook (Item, Hack) | Playable<br>60 fps |
| `hookdszz` | `hook` | 2023 | Hook (Summit War, Hack) | Playable<br>60 fps |
| `hookdw` | `hook` | 2016 | Hook (Warriors, Hack) | Playable<br>60 fps |
| `hookgs` | `hook` | 2022 | Hook (The Expendables, Hack) | Playable<br>60 fps |
| `hookgs2` | `hook` | 2023 | Hook (The Expendables II, Hack) | Playable<br>60 fps |
| `hookhd` | `hook` | 2024 | Hook (Pirate Wars, Hack) | Playable<br>60 fps |
| `hookhd2` | `hook` | 2024 | Hook (Pirate Wars II, Hack) | Playable<br>60 fps |
| `hookj` | `hook` | 1992 | Hook (Japan) | Playable<br>60 fps |
| `hookjy` | `hook` | 2024 | Hook (Elite, Hack) | Playable<br>60 fps |
| `hookjyds` | `hook` | 2025 | Hook (Elite Competition, Hack) | Playable<br>60 fps |
| `hooknx` | `hook` | 2022 | Hook (Hook's Reversal, Hack) | Playable<br>60 fps |
| `hooknx2` | `hook` | 2022 | Hook (Hook's Reversal II, Hack) | Playable<br>60 fps |
| `hookpj` | `hook` | 2024 | Hook (POK KAI, Hack) | Playable<br>60 fps |
| `hookpls` | `hook` | 2024 | Hook (Plus, Hack) | Playable<br>60 fps |
| `hooksw` | `hook` | 2022 | Hook (SW, Hack) | Playable<br>60 fps |
| `hooksw2` | `hook` | 2025 | Hook (SW2, Hack) | Playable<br>60 fps |
| `hooku` | `hook` | 1992 | Hook (US) | Playable<br>60 fps |
| `hookx2` | `hook` | 2018 | Hook (X2, Hack) | Playable<br>60 fps |
| `hookxr` | `hook` | 2018 | Hook (Xin Er, Hack) | Playable<br>60 fps |
| `hookxx` | `hook` | 2025 | Hook (XX, Hack) | Playable<br>60 fps |
| `ppan` | `hook` | 1992 | Peter Pan (bootleg of Hook) | Playable<br>60 fps |
| `inthunt` |  | 1993 | In The Hunt (World) | Playable<br>~60 fps |
| `inthuntk` | `inthunt` | 1993 | In The Hunt (Korea?) | Playable<br>~60 fps |
| `inthuntu` | `inthunt` | 1993 | In The Hunt (US) | Playable<br>~60 fps |
| `kaiteids` | `inthunt` | 1993 | Kaitei Daisensou (Japan) | Playable<br>~60 fps |
| `lethalth` |  | 1991 | Lethal Thunder (World) | Playable<br>60 fps |
| `thndblst` | `lethalth` | 1991 | Thunder Blaster (Japan) | Playable<br>60 fps |
| `majtitl2` |  | 1992 | Major Title 2 (World, set 1) | Playable<br>~60 fps |
| `majtitl2a` | `majtitl2` | 1992 | Major Title 2 (World, set 1, alt sound CPU) | Playable<br>~60 fps |
| `majtitl2b` | `majtitl2` | 1992 | Major Title 2 (World, set 2) | Playable<br>~60 fps |
| `majtitl2j` | `majtitl2` | 1992 | Major Title 2 (Japan) | Playable<br>~60 fps |
| `skingame` | `majtitl2` | 1992 | The Irem Skins Game (US set 1) | Playable<br>~60 fps |
| `skingame2` | `majtitl2` | 1992 | The Irem Skins Game (US set 2) | Playable<br>~60 fps |
| `mysticri` |  | 1992 | Mystic Riders (World) | Playable<br>60 fps |
| `gunhohki` | `mysticri` | 1992 | Mahou Keibitai Gun Hohki (Japan) | Playable<br>60 fps |
| `mysticrib` | `mysticri` | 1992 | Mystic Riders (bootleg?) | Playable<br>60 fps |
| `nbbatman` |  | 1993 | Ninja Baseball Bat Man (World) | Playable<br>60 fps |
| `leaguemn` | `nbbatman` | 1993 | Yakyuu Kakutou League-Man (Japan, set 1) | Playable<br>60 fps |
| `leaguemna` | `nbbatman` | 1993 | Yakyuu Kakutou League-Man (Japan, set 2) | Playable<br>60 fps |
| `nbbat1v2` | `nbbatman` | 2020 | Ninja Baseball Batman (1v2, Hack) | Playable<br>60 fps |
| `nbbat1v4` | `nbbatman` | 2020 | Ninja Baseball Batman (1v4, Hack) | Playable<br>60 fps |
| `nbbat1v8` | `nbbatman` | 2020 | Ninja Baseball Batman (1v8, Hack) | Playable<br>60 fps |
| `nbbatmano` | `nbbatman` | 2020 | Ninja Baseball Batman (One Key Edition, Hack) | Playable<br>60 fps |
| `nbbatmanu` | `nbbatman` | 1993 | Ninja Baseball Bat Man (US) | Playable<br>60 fps |
| `nbbatmbh` | `nbbatman` | 2016 | Ninja Baseball Batman (Transform, Hack) | Playable<br>60 fps |
| `nbbatmx2` | `nbbatman` | 2018 | Ninja Baseball Batman (X2, Hack) | Playable<br>60 fps |
| `rtypeleo` |  | 1992 | R-Type Leo (World) | Playable<br>60 fps |
| `rtypeleoj` | `rtypeleo` | 1992 | R-Type Leo (Japan) | Playable<br>60 fps |
| `ssoldier` |  | 1993 | Superior Soldiers (US) | Playable<br>~60 fps |
| `psoldier` | `ssoldier` | 1993 | Perfect Soldiers (Japan) | Playable<br>~60 fps |
| `uccops` |  | 1992 | Undercover Cops (World) | Playable<br>60 fps |
| `uccopsar` | `uccops` | 1992 | Undercover Cops - Alpha Renewal Version (World) | Playable<br>60 fps |
| `uccopsaru` | `uccops` | 1992 | Undercover Cops - Alpha Renewal Version (US) | Playable<br>60 fps |
| `uccopsj` | `uccops` | 1992 | Undercover Cops (Japan) | Playable<br>60 fps |
| `uccopsu` | `uccops` | 1992 | Undercover Cops (US) | Playable<br>60 fps |

</details>

<details>
<summary><b>M107</b> (4 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `airass` |  | 1993 | Air Assault (World) | Playable<br>60 fps |
| `firebarr` | `airass` | 1993 | Fire Barrel (Japan) | Playable<br>60 fps |
| `dsoccr94` |  | 1994 | Dream Soccer '94 (World, M107 hardware) | Playable<br>59 fps |
| `dsoccr94k` | `dsoccr94` | 1994 | Dream Soccer '94 (Korea, M107 hardware) | Playable<br>59 fps |

</details>

<details>
<summary><b>M90/97</b> (14 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `bbmanw` |  | 1992 | Bomber Man World / New Dyna Blaster - Global Quest | Playable<br>~60 fps |
| `bbmanwj` | `bbmanw` | 1992 | Bomber Man World (Japan) | Playable<br>~60 fps |
| `bbmanwja` | `bbmanw` | 1992 | Bomber Man World (Japan, revised sound hardware) | Playable<br>~60 fps |
| `newapunk` | `bbmanw` | 1992 | New Atomic Punk - Global Quest (US) | Playable<br>~60 fps |
| `dicegame` |  | 1991 | Dice - The Dice Game! | Playable<br>~60 fps |
| `dynablst` |  | 1991 | Dynablaster / Bomber Man | Playable<br>60 fps |
| `atompunk` | `dynablst` | 1991 | Atomic Punk (US) | Playable<br>60 fps |
| `bombrman` | `dynablst` | 1991 | Bomber Man (Japan) | Playable<br>60 fps |
| `hasamu` |  | 1991 | Hasamu (Japan) | Playable<br>60 fps |
| `matchit2` |  | 1993 | Match It II | Playable<br>~60 fps |
| `shisen2` | `matchit2` | 1993 | Shisensho II | Playable<br>~60 fps |
| `quizf1` |  | 1992 | Quiz F1 1-2 Finish (Japan) | Playable<br>~60 fps |
| `riskchal` |  | 1993 | Risky Challenge | Playable<br>60 fps |
| `gussun` | `riskchal` | 1993 | Gussun Oyoyo (Japan) | Playable<br>60 fps |

</details>

<details>
<summary><b>M72</b> (48 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `airduel` |  | 1990 | Air Duel (World, M82 hardware) | Playable<br>55 fps |
| `airdueljm72` | `airduel` | 1990 | Air Duel (Japan, M72 hardware) | Playable<br>55 fps |
| `airduelm72` | `airduel` | 1990 | Air Duel (World, M72 hardware) | Playable<br>55 fps |
| `airduelu` | `airduel` | 1990 | Air Duel (US location test, M82 hardware) | Playable<br>55 fps |
| `bchopper` |  | 1987 | Battle Chopper (World) | Playable<br>~55 fps |
| `mrheli` | `bchopper` | 1987 | Mr. HELI no Daibouken (Japan) | Playable<br>~55 fps |
| `cosmccop` |  | 1991 | Cosmic Cop (World) | Playable<br>55 fps |
| `gallop` | `cosmccop` | 1991 | Gallop - Armed Police Unit (Japan, M84 hardware) | Playable<br>55 fps |
| `gallopm72` | `cosmccop` | 1991 | Gallop - Armed Police Unit (Japan, M72 hardware) | Playable<br>55 fps |
| `dbreed` |  | 1989 | Dragon Breed (World, M81 hardware) | Playable<br>55 fps |
| `dbreedjm72` | `dbreed` | 1989 | Dragon Breed (Japan, M72 hardware) | Playable<br>55 fps |
| `dbreedm72` | `dbreed` | 1989 | Dragon Breed (World, M72 hardware) | Playable<br>55 fps |
| `hharry` |  | 1990 | Hammerin' Harry (World, M81 hardware) | Playable<br>55 fps |
| `dkgensan` | `hharry` | 1990 | Daiku no Gensan (Japan, M84 hardware) | Playable<br>55 fps |
| `dkgensanm72` | `hharry` | 1990 | Daiku no Gensan (Japan, M72 hardware) | Playable<br>55 fps |
| `dkgensanm82` | `hharry` | 1990 | Daiku no Gensan (Japan, M82 hardware) | Playable<br>55 fps |
| `hharryb` | `hharry` | 1990 | Hammerin' Harry (World, M84 hardware bootleg) | Playable<br>55 fps |
| `hharryu` | `hharry` | 1990 | Hammerin' Harry (US, M84 hardware) | Playable<br>55 fps |
| `imgfight` |  | 1988 | Image Fight (World) | Playable<br>55 fps |
| `imgfightj` | `imgfight` | 1988 | Image Fight (Japan) | Playable<br>55 fps |
| `imgfightjb` | `imgfight` | 1988 | Image Fight (Japan, bootleg) | Playable<br>55 fps |
| `loht` |  | 1989 | Legend of Hero Tonma (World) | Playable<br>~55 fps |
| `lohtb` | `loht` | 1989 | Legend of Hero Tonma (Playmark unprotected bootleg) | Playable<br>~55 fps |
| `lohtb2` | `loht` | 1989 | Legend of Hero Tonma (Japan, bootleg with i8751) | Playable<br>~55 fps |
| `lohtb3` | `loht` | 1997 | Legend of Hero Tonma (World, bootleg with i8751) | Playable<br>~55 fps |
| `lohtj` | `loht` | 1989 | Legend of Hero Tonma (Japan) | Playable<br>~55 fps |
| `ltswords` |  | 1991 | Lightning Swords (World) | Playable<br>55 fps |
| `kengo` | `ltswords` | 1991 | Ken-Go (World) | Playable<br>55 fps |
| `kengoj` | `ltswords` | 1991 | Ken-Go (Japan) | Playable<br>55 fps |
| `majtitle` |  | 1990 | Major Title (World) | Playable<br>55 fps |
| `majtitlej` | `majtitle` | 1990 | Major Title (Japan) | Playable<br>55 fps |
| `nspirit` |  | 1988 | Ninja Spirit (World) | Playable<br>~55 fps |
| `nspiritj` | `nspirit` | 1988 | Saigo no Nindou (Japan) | Playable<br>~55 fps |
| `poundfor` |  | 1990 | Pound for Pound (World) | Playable<br>~55 fps |
| `poundforj` | `poundfor` | 1990 | Pound for Pound (Japan) | Playable<br>~55 fps |
| `poundforu` | `poundfor` | 1990 | Pound for Pound (US) | Playable<br>~55 fps |
| `rtype` |  | 1987 | R-Type (World) | Playable<br>55 fps |
| `rtypeb` | `rtype` | 1987 | R-Type (World bootleg) | Playable<br>55 fps |
| `rtypej` | `rtype` | 1987 | R-Type (Japan) | Playable<br>55 fps |
| `rtypejp` | `rtype` | 1987 | R-Type (Japan prototype) | Playable<br>55 fps |
| `rtypem82b` | `rtype` | 2009 | R-Type (Japan, bootleg M82 conversion) | Playable<br>55 fps |
| `rtypeu` | `rtype` | 1987 | R-Type (US) | Playable<br>55 fps |
| `rtype2` |  | 1989 | R-Type II (World) | Playable<br>55 fps |
| `rtype2j` | `rtype2` | 1989 | R-Type II (Japan) | Playable<br>55 fps |
| `rtype2jc` | `rtype2` | 1989 | R-Type II (Japan, revision C) | Playable<br>55 fps |
| `rtype2m82b` | `rtype2` | 1997 | R-Type II (Japan, bootleg M82 conversion) | Playable<br>55 fps |
| `xmultipl` |  | 1989 | X Multiply (World, M81 hardware) | Playable<br>55 fps |
| `xmultiplm72` | `xmultipl` | 1989 | X Multiply (Japan, M72 hardware) | Playable<br>55 fps |

</details>

<details>
<summary><b>M75/77</b> (13 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `buccanrs` |  | 1989 | Buccaneers (set 1) | Playable<br>~56 fps |
| `buccanrsa` | `buccanrs` | 1989 | Buccaneers (set 2) | Playable<br>~56 fps |
| `buccanrsb` | `buccanrs` | 1989 | Buccaneers (set 3, harder) | Playable<br>~56 fps |
| `kikcubic` |  | 1988 | Meikyu Jima (Japan) | Playable<br>~56 fps |
| `kikcubicb` | `kikcubic` | 1988 | Kickle Cubele | Playable<br>~56 fps |
| `vigilant` |  | 1988 | Vigilante (World, Rev E) | Playable<br>56 fps |
| `vigilanta` | `vigilant` | 1988 | Vigilante (World, Rev A) | Playable<br>56 fps |
| `vigilantb` | `vigilant` | 1988 | Vigilante (US, Rev B) | Playable<br>56 fps |
| `vigilantbl` | `vigilant` | 1988 | Vigilante (bootleg) | Playable<br>56 fps |
| `vigilantc` | `vigilant` | 1988 | Vigilante (World, Rev C) | Playable<br>56 fps |
| `vigilantd` | `vigilant` | 1988 | Vigilante (Japan, Rev D) | Playable<br>56 fps |
| `vigilantg` | `vigilant` | 1988 | Vigilante (US, Rev G) | Playable<br>56 fps |
| `vigilanto` | `vigilant` | 1988 | Vigilante (US) | Playable<br>56 fps |

</details>

<details>
<summary><b>M62</b> (25 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `battroad` |  | 1984 | The Battle-Road | Playable<br>~55 fps |
| `horizon` |  | 1985 | Horizon (Irem) | Playable<br>~55 fps |
| `kidniki` |  | 1986 | Kid Niki - Radical Ninja (World) | Playable<br>~55 fps |
| `kidnikiu` | `kidniki` | 1986 | Kid Niki - Radical Ninja (US) | Playable<br>~55 fps |
| `lithero` | `kidniki` | 1987 | Little Hero | Playable<br>~55 fps |
| `yanchamr` | `kidniki` | 1986 | Kaiketsu Yanchamaru (Japan) | Playable<br>~55 fps |
| `kungfum` |  | 1984 | Kung-Fu Master (World) | Playable<br>55 fps |
| `kungfub` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 1) | Playable<br>55 fps |
| `kungfub2` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 2) | Playable<br>55 fps |
| `kungfub3` | `kungfum` | 1984 | Kung-Fu Master (bootleg set 3) | Playable<br>55 fps |
| `kungfub3s` | `kungfum` | 1984 | Kung-Fu Senjyo (bootleg, Spanish) | Playable<br>55 fps |
| `kungfumd` | `kungfum` | 1984 | Kung-Fu Master (US) | Playable<br>55 fps |
| `spartanx` | `kungfum` | 1984 | Spartan X (Japan) | Playable<br>55 fps |
| `ldrun` |  | 1984 | Lode Runner (set 1) | Playable<br>55 fps |
| `ldruna` | `ldrun` | 1984 | Lode Runner (set 2) | Playable<br>55 fps |
| `ldrun2` |  | 1984 | Lode Runner II - The Bungeling Strikes Back | Playable<br>~55 fps |
| `ldrun3` |  | 1985 | Lode Runner III - The Golden Labyrinth | Playable<br>~55 fps |
| `ldrun3j` | `ldrun3` | 1985 | Lode Runner III - Majin no Fukkatsu (Japan, rev. A) | Playable<br>~55 fps |
| `ldrun3jc` | `ldrun3` | 1985 | Lode Runner III - Majin no Fukkatsu (Japan, rev. C) | Playable<br>~55 fps |
| `ldrun4` |  | 1986 | Lode Runner IV - Teikoku Karano Dasshutsu (Japan) | Playable<br>~55 fps |
| `lotlot` |  | 1985 | Lot Lot | Playable<br>~55 fps |
| `spelunk2` |  | 1986 | Spelunker II - 23 no Kagi (Japan) | Playable<br>~55 fps |
| `spelunkr` |  | 1985 | Spelunker | Playable<br>~55 fps |
| `spelunkrj` | `spelunkr` | 1985 | Spelunker (Japan) | Playable<br>~55 fps |
| `youjyudn` |  | 1986 | Youjyuden (Japan) | Playable<br>~55 fps |

</details>

<details>
<summary><b>M63</b> (4 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `fghtbskt` |  | 1984 | Fighting Basketball | Playable<br>~60 fps |
| `wilytowr` |  | 1984 | Wily Tower | Playable<br>59 fps |
| `atomboy` | `wilytowr` | 1985 | Atomic Boy (revision B) | Playable<br>59 fps |
| `atomboya` | `wilytowr` | 1985 | Atomic Boy (revision A) | Playable<br>59 fps |

</details>

<details>
<summary><b>M58</b> (6 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `10yard` |  | 1983 | 10-Yard Fight (World, set 1) | Playable<br>~60 fps |
| `10yard85` | `10yard` | 1985 | 10-Yard Fight '85 (US, Taito license) | Playable<br>~60 fps |
| `10yardj` | `10yard` | 1983 | 10-Yard Fight (Japan) | Playable<br>~60 fps |
| `vs10yard` | `10yard` | 1984 | Vs 10-Yard Fight (World, 11/05/84) | Playable<br>~60 fps |
| `vs10yardj` | `10yard` | 1984 | Vs 10-Yard Fight (Japan) | Playable<br>~60 fps |
| `vs10yardu` | `10yard` | 1984 | Vs 10-Yard Fight (US, Taito license) | Playable<br>~60 fps |

</details>

<details>
<summary><b>M57</b> (2 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `troangel` |  | 1983 | Tropical Angel | Playable<br>~57 fps |
| `newtangl` | `troangel` | 1983 | New Tropical Angel | Playable<br>~57 fps |

</details>

<details>
<summary><b>M52</b> (13 sets)</summary>

| Set | Parent | Year | Title | Performance |
|---|---|---|---|---|
| `alpha1v` |  | 1988 | Alpha One (Vision Electronics) | Playable<br>~57 fps |
| `mpatrol` |  | 1982 | Moon Patrol | Playable<br>~57 fps |
| `mpatrolut` | `mpatrol` | 2025 | Moon Patrol Unknown Territories | Playable<br>~57 fps |
| `mpatrolw` | `mpatrol` | 1982 | Moon Patrol (Williams) | Playable<br>~57 fps |
| `mranger` | `mpatrol` | 1982 | Moon Ranger (bootleg of Moon Patrol) | Playable<br>~57 fps |
| `shtrider` |  | 1985 | Shot Rider | Playable<br>~57 fps |
| `shtridera` | `shtrider` | 1984 | Shot Rider (Sigma license) | Playable<br>~57 fps |
| `travrusa` |  | 1983 | Traverse USA / Zippy Race | Playable<br>57 fps |
| `motorace` | `travrusa` | 1983 | MotoRace USA | Playable<br>57 fps |
| `mototour` | `travrusa` | 1983 | MotoTour / Zippy Race (Tecfri license) | Playable<br>57 fps |
| `mototoura` | `travrusa` | 1983 | MotoTour / Zippy Race (Assa version of Tecfri license) | Playable<br>57 fps |
| `travrusab` | `travrusa` | 1983 | Traverse USA (bootleg, set 1) | Playable<br>57 fps |
| `travrusab2` | `travrusa` | 1983 | Traverse USA (bootleg, set 2) | Playable<br>57 fps |

</details>

## 5. Download

Prebuilt binaries are published on the [Releases](../../releases) page:

| File | Platform | Contents |
|---|---|---|
| `ReiREM_vX.Y.Z_vita.zip` | PS Vita / PS TV | `ReiREM_vX.Y.Z_vita.vpk` and an empty `data/reirem/roms/` folder |
| `ReiREM_vX.Y.Z_psp.zip` | PSP-3000 / PSP go | `PSP/GAME/REIREM/EBOOT.PBP` and an empty `PSP/GAME/REIREM/roms/` folder |

The releases contain the emulator only. **No ROMs, BIOS files or game artwork are included or provided.**
You must supply ROM images of games you own.

## 6. Installation

### PS Vita

Requirements: a PS Vita or PS TV that can install homebrew VPK files with [VitaShell](https://github.com/TheOfficialFloW/VitaShell).

**Install the VPK**

1. Extract `ReiREM_vX.Y.Z_vita.zip` on your PC.
2. Start VitaShell on the Vita and connect it to the PC:
   - USB: press **SELECT** in VitaShell, connect the cable, and the memory card appears as a drive; or
   - FTP: press **SELECT** in VitaShell and connect an FTP client to the address shown on screen.
3. Copy `ReiREM_vX.Y.Z_vita.vpk` to `ux0:` (any folder, for example `ux0:downloads/`).
4. Disconnect, open the folder in VitaShell, select the `.vpk` and press **X** to install.
   Accept the extended permissions prompt. A **ReiREM** bubble appears on the home screen (title ID `M92A00001`).
5. The `.vpk` file can be deleted after installation.

The first start after each boot restarts the app once: it loads its kernel helper (`rei_kctrl.skprx`, inside the app)
that lets the PS button open the menu and installs updates. No plugin needs to be installed.

**Add ROMs**

1. Copy the `data` folder from the zip to the root of `ux0:` so that the empty folder is
   `ux0:data/reirem/roms/` (ReiREM also creates it on first launch).
2. Put your ROM zips in that folder, unchanged and not extracted, for example
   `ux0:data/reirem/roms/nbbatman.zip`.
3. Start ReiREM. The game list shows every recognized set, grouped by board.

<a id="updates-ps-vita"></a>
**Updates (PS Vita)**

When the app starts it checks the GitHub releases in the background. If a newer version is out, the game list
shows its changes with **Update**, **Close** or **Skip this version**. *Update* downloads the new package, installs it
after the app closes and returns to the home screen; start ReiREM again afterwards. Without a connection nothing
happens. Versions older than this one have to be updated by hand once.

The PSP version has no built-in updater: replace `EBOOT.PBP` with the one from the new release.

### PSP

Requirements: a PSP-3000 or PSP go that can run homebrew. The emulator uses the Media Engine and the 64 MB memory of these models; PSP-1000/2000 are not supported.

**Install**

1. Connect the PSP to the PC in USB mode (or remove the Memory Stick and use a card reader).
2. Extract `ReiREM_vX.Y.Z_psp.zip` to the root of the Memory Stick, keeping the folders, so that you get
   `ms0:/PSP/GAME/REIREM/EBOOT.PBP` and `ms0:/PSP/GAME/REIREM/roms/`.
   On the PSP go internal storage the same path is under `ef0:`.
3. ReiREM appears under *Game > Memory Stick* in the XMB.

**Add ROMs**

1. Put your ROM zips, unchanged and not extracted, in `PSP/GAME/REIREM/roms/`,
   for example `ms0:/PSP/GAME/REIREM/roms/nbbatman.zip`.
2. Start ReiREM. The game list shows every recognized set, grouped by board.

### ROM sets

- ROMs must be **FinalBurn Neo** compatible sets with the set names listed in [section 4](#4-supported-boards-and-games)
  (MAME sets of the same name usually match).
- Keep each set as one zip named after the set, for example `rtype.zip`. Do not rename or extract them.
- Clone sets (for example `nbbatmanu`) read missing files from the parent zip (`nbbatman.zip`), so keep the
  parent in the same folder, or use non-merged sets.
- The *Playable* list shows only sets whose ROM zip was found; press SELECT in the list to show all supported sets.

### Optional files

| What | Vita | PSP |
|---|---|---|
| Cheats (FBNeo `.ini`) | `ux0:data/reirem/cheats/<set>.ini` | `PSP/GAME/REIREM/cheats/<set>.ini` |
| Border artwork | `ux0:data/reirem/artwork/<set>.png` | `PSP/GAME/REIREM/artwork/<set>.png` |

## 7. Usage

**Game list**

| Action | Buttons |
|---|---|
| Start the selected game | Cross (or Circle, see Controls) |
| Change board tab | L / R |
| Add / remove favorite | Square |
| Filter: Playable / All / Favorites | SELECT |
| Options (Display, Controls) | START |
| About | Triangle |

Options opened from the game list edit the global settings.

**In game**

| Action | Buttons |
|---|---|
| Open the in-game menu | SELECT + START (Vita: also the PS button) |
| Quick save / quick load | SELECT + L / SELECT + R |

The in-game menu provides: Resume, Reset, Save states (10 slots), Display (aspect ratio, filter, scanlines, border,
theme), Controls (button mapping, turbo; Vita: touch buttons), DIP switches, Cheats, per-game settings and
*Select game*, which goes back to the game list without restarting the app.

**Turbo.** Pick one *Turbo key* and the game button it fires (*Turbo button*), plus the speed
(15, 10 or 7.5 presses per second). While the turbo key is held, that game button fires repeatedly.

<a id="controls-and-controllers-ps-vita"></a>
**Controls and controllers (PS Vita)**

*Controls* has these rows:

| Row | What it does |
|---|---|
| 1P Controller / 2P Controller | Opens the controls of that player. The value is the device the player uses. 2P is shown when a second device is connected. |
| Input engine | How controllers are read: Native, RePad or VitaControl (see below). |
| Touch controls | Touch buttons on the screen and their layout editor. |
| Menu confirm button | Cross or Circle. |
| Restore defaults | Default controls for every device. |

A player's page has *Device* (Auto, Vita, a connected controller or None), *Analog stick to d-pad*,
*Set all controls*, the game buttons, Coin and Start, *Turbo*, *Combo keys* and *Reset to default controls*.

- To assign a key, select its row, press Cross (or Circle), then press the button you want. The d-pad, the PS button
  or 5 seconds without input cancel. Left / right still step through the values, Square sets the row to None.
- *Set all controls* asks for every row in order, from the first button to Start.
- L2, R2, L3 and R3 of a controller are keys of their own.
- *Combo keys*: one button presses two others together. Any two of Square, Triangle, Cross, Circle, L, R, L2, R2, L3
  and R3 can be paired; pick the first key, then the second. Square clears a combo.
- Controls belong to the device, not to the player: the Vita, a DualShock 4 and a Switch Pro controller each keep
  their own layout, whichever player uses them. Per-game settings store them the same way.
- When a player's controller is switched off, the player uses a free device until it comes back.
  Devices that connect or disconnect while the menu is open show up within half a second.

*Input engine*:

- **Native** (default): the system's own controller support and any controller plugin you already use.
  This is the right choice in most cases.
- **RePad**: use this for Bluetooth controllers the Vita does not support by itself, such as the Nintendo Switch Pro
  controller and other unofficial pads (DualShock 4 and DualSense work too). RePad is a kernel plugin bundled with the
  app and loaded by it; nothing has to be added to the plugin list. With RePad the Vita's own controls are 1P and the
  controller is 2P.
- **VitaControl**: offered when the VitaControl plugin is in your plugin list. Enough if you only play alone (1P);
  the app may ask to restart the Vita when that plugin has to be switched on.

**Per-game settings.** *Save game settings* stores the current display and control settings for the running
set only (`<set>_game.cfg`); global settings are not touched. When a per-game file exists it is loaded
automatically, and the menu shows *Delete game settings* instead.

The last played game is remembered and started on the next launch.

## 8. Building from Source

Builds are done on Linux or WSL (Ubuntu 24.04 is used for development).

### Prerequisites

```bash
sudo apt install -y build-essential cmake ninja-build git python3 zlib1g-dev
```

- PS Vita: [VitaSDK](https://vitasdk.org/) with `vita2d` (`vdpm` install-all), `VITASDK` set.
- PSP: [pspdev](https://github.com/pspdev/pspdev) toolchain in `~/pspdev` (or `PSPDEV` set), plus
  [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core) installed into pspdev:

  ```bash
  git clone https://github.com/mcidclan/psp-media-engine-custom-core
  cd psp-media-engine-custom-core
  psp-cmake -B build && cmake --build build && cmake --install build
  ```

### PS Vita

```bash
bash tools/build.sh vita
# output: build-vita/reirem.vpk, build-vita/eboot.bin
```

### PSP

```bash
# release build (no logging) for playing
BUILD_DIR=build-psp-play bash tools/build.sh psp Release -DREI_LOG=OFF
# output: build-psp-play/EBOOT.PBP

# development build with logging over PSPLink
bash tools/build.sh psp
```

### Desktop test harness

A headless harness runs the shared core on a PC and prints per-frame video and audio hashes. It is used
as the reference for every optimization.

```bash
cmake -S tools/harness -B build-pc && cmake --build build-pc -j
./build-pc/harness --rom path/to/nbbatman.zip --frames 3600
```

## 9. Project Layout

```
src/core/      FinalBurn Neo subset: Irem drivers, NEC/Z80 cores, YM2151, GA20, AY, MSM5205 + shim
src/romprep/   ROM preprocessing: graphics pre-decoding, sound program decryption, cache
vita/          PS Vita port (frontend, platform code, LiveArea assets)
psp/           PSP port (frontend, Media Engine code, MIPS dynarec glue, XMB assets)
tools/         Build script, desktop harness, audio checks
```

## 10. Accuracy and Testing

- Every performance change is checked against the desktop harness: frame hashes and audio hashes must
  match the unmodified FinalBurn Neo path over thousands of frames for each board.
- The PSP dynarecs are verified with a MIPS user-mode emulator (QEMU) that runs the real PSP code path
  and compares hashes with the interpreter.
- Intentional differences (for example the fixed sound-to-main latency) are kept deterministic and documented.

## 11. License and Credits

**License.** ReiREM is derived from FinalBurn Neo and is distributed under the
[FinalBurn Neo license](LICENSE-FBNeo.txt). **Non-commercial use only**: the emulator may not be sold or
bundled with commercial products. The original copyright headers are kept in all imported sources.

**Credits**
- [FinalBurn Neo](https://github.com/finalburnneo/FBNeo) team: Irem drivers, NEC V30/V33/V35, Z80,
  YM2151, GA20 and the rest of the emulation core.
- [MAME](https://www.mamedev.org/) developers: original research on the Irem hardware.
- [VitaSDK](https://vitasdk.org/) and [vita2d](https://github.com/xerpi/libvita2d).
- [pspdev / PSPSDK](https://github.com/pspdev/pspdev).
- psp-media-engine-custom-core (MIT) for the Media Engine bootstrap.
- [stb_image](https://github.com/nothings/stb) (public domain), [zlib](https://zlib.net/).
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0) for the Vita update check.
- [Noto Sans](https://fonts.google.com/noto) (SIL Open Font License 1.1, `psp/font/OFL.txt`) for the PSP menu font.
- RePad (MIT, `vita/repad/`): the Bluetooth controller plugin bundled with the Vita version.

**Trademarks.** Irem and all game titles are trademarks of their respective owners. This project is not
affiliated with or endorsed by Irem Software Engineering or any rights holder. No ROM images, game
graphics or logos are included in this repository or in the release packages.
