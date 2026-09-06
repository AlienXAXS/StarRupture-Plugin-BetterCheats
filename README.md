# BetterCheats -- StarRupture Plugin

A modular in-game cheat menu for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/), built on the StarRupture ModLoader. Provides a categorized ImGui panel for player, world, machine, and enemy cheats, with per-save persistence of your settings.

**Target:** Game client only

---

## What It Does

BetterCheats registers an in-game ImGui panel with a sidebar of cheat categories. The menu only appears during a single-player `ChimeraMain` world session -- it stays hidden on the main menu and other game modes.

| Category | Sub-tabs | Examples |
|---|---|---|
| World | Environment | World/environment manipulation, wave control |
| Player | Self, Item Spawner, Inventory, Weapon, Movement, Teleport, Building, Skills, Tools | God mode / attribute locks, spawn items, resize the inventory grid, weapon tweaks, no-clip flight, teleport, free building, skill unlocks, tool tweaks |
| Machinery | Crafters, Power, Logistic Drones, Rail Drones | Instant crafting, infinite machine power, drone control |
| Enemies | Enemies | Enemy-related cheats |
| Misc | -- | Miscellaneous cheats |

Continuous effects (e.g. attribute locks, power overrides, wave control) are enforced every engine tick via `OnEngineTick`, so they keep applying even while the menu is closed.

### No Clip

**Player -> Movement** turns the character's capsule collision off and puts it into flying mode, so you pass through terrain and buildings. Steer as usual to fly horizontally, hold **Space** to rise and **Left Ctrl** to descend, and adjust the fly speed multiplier in the panel. It also has its own keybind (**F9** by default) so it can be toggled without opening the menu -- click the key next to **Toggle No Clip** in the panel to rebind it, holding Ctrl, Shift or Alt for a combo.

### Inventory grid

**Player -> Inventory** sets the player inventory grid to any columns x rows you want, down to the game's own minimum of 8 x 8.

The inventory frame doesn't scroll, so a taller grid just runs off the bottom of it. **Shrink slots to fit the inventory frame** scales every slot widget down against that 8 x 8 reference: 8 x 8 draws at 100%, 16 x 16 at 50%, 20 x 20 at 40%. The same factor applies to both axes, so slots stay square and a wide, short grid leaves space below it rather than stretching. Slots stop shrinking at 40%; past 20 x 20 the grid overflows rather than becoming unreadable.

The grid and the fit setting are saved per session and re-applied on load. The same resize is available from the mod console as `bc_invsize <columns> <rows>`.

The game refuses to shrink a grid below the slots that are currently in use, so empty the inventory out first if a smaller grid doesn't take.

### Per-save settings

Cheat toggles that should persist are saved to a JSON file per save session (`Plugins\BetterCheats\<SessionName>.json`). When a save finishes loading, BetterCheats reloads that session's config and re-applies any saved cheat states automatically -- including on plugin hot-reload into an already-active session.

---

## Configuration

Config is stored in `Plugins\config\BetterCheats.ini` and is generated on first launch.

| Section | Key | Default | Description |
|---|---|---|---|
| `General` | `Enabled` | `true` | `true` or `false` -- enables the plugin |
| `Menu` | `ToggleKey` | `F10` | Key to open / close the BetterCheats menu |
| `Keybinds` | `NoClipKey` | `F9` | Key to toggle No Clip on / off without opening the menu |

Both keys can be rebound at runtime via the modloader's keybind settings, and the No Clip key can also be rebound from the **Player -> Movement** panel -- either way the change takes effect immediately and is written back to the .ini.

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
