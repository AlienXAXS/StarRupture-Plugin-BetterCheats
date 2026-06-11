# BetterCheats -- StarRupture Plugin

A modular in-game cheat menu for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/), built on the StarRupture ModLoader. Provides a categorized ImGui panel for player, world, machine, and enemy cheats, with per-save persistence of your settings.

**Target:** Game client only

---

## What It Does

BetterCheats registers an in-game ImGui panel with a sidebar of cheat categories. The menu only appears during a single-player `ChimeraMain` world session -- it stays hidden on the main menu and other game modes.

| Category | Sub-tabs | Examples |
|---|---|---|
| World | Environment | World/environment manipulation, wave control |
| Player | Self, Item Spawner, Weapon, Movement, Teleport, Building, Skills, Tools | God mode / attribute locks, spawn items, weapon tweaks, movement speed, teleport, free building, skill unlocks, tool tweaks |
| Machinery | Crafters, Power, Logistic Drones, Rail Drones | Instant crafting, infinite machine power, drone control |
| Enemies | Enemies | Enemy-related cheats |
| Misc | -- | Miscellaneous cheats |

Continuous effects (e.g. attribute locks, power overrides, wave control) are enforced every engine tick via `OnEngineTick`, so they keep applying even while the menu is closed.

### Per-save settings

Cheat toggles that should persist are saved to a JSON file per save session (`Plugins\BetterCheats\<SessionName>.json`). When a save finishes loading, BetterCheats reloads that session's config and re-applies any saved cheat states automatically -- including on plugin hot-reload into an already-active session.

---

## Configuration

Config is stored in `Plugins\config\BetterCheats.ini` and is generated on first launch.

| Section | Key | Default | Description |
|---|---|---|---|
| `General` | `Enabled` | `true` | `true` or `false` -- enables the plugin |
| `Menu` | `ToggleKey` | `F10` | Key to open / close the BetterCheats menu |

The toggle key can be rebound at runtime via the modloader's keybind settings -- BetterCheats picks up the change automatically.

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page: `BetterCheats_Plugin-Client-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder -- it will sit alongside your existing `dwmapi.dll`.

3. Launch the game, then press **F10** (default) in-game to open the menu.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Menu doesn't appear | The menu only shows in a single-player world session. Confirm you're loaded into a save, and that `Enabled=true` in `BetterCheats.ini`. |
| Toggle key does nothing | Check the keybind in the modloader settings -- it may have been rebound. |
| Saved cheat settings not restored | Check `modloader.log` for errors loading `Plugins\BetterCheats\<SessionName>.json`. |
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Game updated, plugin broken | Some features use byte-pattern scanning. A game update may shift the patterns -- wait for a plugin update. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) and [StarRupture-Game-SDK](https://github.com/AlienXAXS/StarRupture-Game-SDK), checked out alongside this repo.

Clone the repo, open `StarRupture-Plugin-BetterCheats.sln`, and build the `Client Release|x64` configuration:

```
msbuild StarRupture-Plugin-BetterCheats.sln /p:Configuration="Client Release" /p:Platform=x64
```

The output DLL is placed in `build\Client Release\Plugins\BetterCheats.dll`.

---

## Disclaimer

This is a single-player cheat menu intended for personal use. Use at your own risk -- the authors are not responsible for any damage caused by using this software, including loss of save data or issues caused by using cheats in multiplayer/server contexts.
