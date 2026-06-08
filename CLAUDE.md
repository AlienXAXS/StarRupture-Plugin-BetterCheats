# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# BetterCheats — Development Rules

## Build

This is a Visual Studio 2022 C++ project (no CLI build/test/lint tooling — build via MSBuild or the IDE).

- Solution: `StarRupture-Plugin-BetterCheats.sln`, project: `BetterCheats/BetterCheats.vcxproj`
- Configurations: `Client Debug|x64` and `Client Release|x64` (this is a client-only plugin — `MODLOADER_CLIENT_BUILD` is defined)
- Build from the command line:
  ```
  msbuild StarRupture-Plugin-BetterCheats.sln /p:Configuration="Client Release" /p:Platform=x64
  ```
- Output DLL lands in `build/Client Release/Plugins/BetterCheats.dll`
- `Shared.props` resolves SDK include paths from sibling checkouts (`StarRupture-Game-SDK`, `StarRupture-Plugin-SDK`) — these must exist alongside this repo at `C:\Users\markp\Documents\GitHub\`
- To run/test: copy the built DLL into `<game_dir>\Plugins\` alongside `dwmapi.dll` (the modloader runtime) and launch StarRupture. There are no automated tests; verification is manual in-game.
- CI release workflow (`.github/workflows/release.yml`, templated from `templates/release.yml`) builds `Client Release` and publishes a GitHub release on push to `main`.

## Architecture

BetterCheats is a plugin for the **StarRupture ModLoader** (see `plugin_interface.h`, the SDK contract — never edit it). The plugin is a single DLL that the modloader loads, and which registers an ImGui panel, hooks, and a keybind through `IPluginSelf`.

**Entry point & lifecycle** (`plugin.cpp`): exports `GetPluginInfo`, `PluginInit`, `PluginShutdown`. `PluginInit` stores the global `IPluginSelf*` (accessed elsewhere via `GetSelf()`/`GetHooks()`/`GetConfig()`/`GetScanner()` in `plugin_helpers.h`), initializes config, registers the cheat menu panel, registers the menu toggle keybind, and subscribes to the engine tick (`OnEngineTick` drives continuous effects like attribute locks regardless of whether the menu UI is open).

**Menu shell** (`cheat_menu.h/.cpp`): `CheatMenu` owns the ImGui panel registration, sidebar navigation, and dispatches rendering to category panels based on the `MenuCategory` enum. It also gates menu visibility to single-player `ChimeraMain` world sessions via `World->RegisterOnWorldBeginPlay`/`RegisterOnAfterWorldEndPlay` hooks (see `ShouldShowMenu`/`s_inChimeraMain`) — the menu hides itself outside that world, including correctly picking up an in-progress session on hot reload by probing `SDK::UWorld::GetWorld()` directly.

**Category panels** (`panel_*.h/.cpp`): each is a thin namespace of `RenderXxx(IModLoaderImGui* imgui)` functions dispatched from `cheat_menu.cpp`'s `RenderContent`. Panels are pure UI/dispatch — actual game-state manipulation lives in dedicated feature modules (e.g. `panel_player.cpp`'s "Self" and "Skills" sub-tabs delegate to `player_attributes.cpp`/`player_skills.cpp` rather than touching SDK objects directly).

**Feature modules** (e.g. `player_attributes.cpp`, `player_skills.cpp`): contain the actual SDK interaction — resolving `SDK::ACrCharacterPlayerBase*` from `SDK::UWorld::GetWorld()` → `UGameplayStatics::GetPlayerController` → `Pawn`, reading/writing `FGameplayAttributeData` (both `BaseValue` and `CurrentValue`, since gameplay effects re-evaluate from the base value), and exposing a `Tick(float deltaSeconds)` for per-frame enforcement (e.g. attribute locks) independent of UI render calls. SDK access always goes through generated headers like `Chimera_classes.hpp`, wrapped in `try/catch` since `UWorld::GetWorld()` can throw when no world is loaded.

**Config** (`plugin_config.h/.cpp`): a `BetterCheatsConfig::Config` static class wraps `IPluginConfig` with a typed schema (`CONFIG_ENTRIES`/`SCHEMA`) — this is the *only* sanctioned way to read/write plugin settings; panels must never call `IPluginConfig` directly.

**Logging** (`plugin_helpers.h`): use the `LOG_INFO`/`LOG_WARN`/`LOG_ERROR`/`LOG_DEBUG`/`LOG_TRACE` macros, which route through `GetSelf()->logger` and no-op safely if the plugin isn't initialized.

## 1. Visual Studio Filter File

Every time a file is added, removed, or renamed you **must** update
`BetterCheats/BetterCheats.vcxproj.filters` in the same change.
Both the `<Filter>` definition block and the item-group entry for the file
must be present. Never leave the filters file out of sync with the project.

## 2. Modular Design — File & Filter Layout

Cheat categories are self-contained. Each category lives in its own pair of files:

| Files | VS Filter | Purpose |
|---|---|---|
| `cheat_menu.h/.cpp` | `Cheats\Menu` | Window shell, nav bar, sub-tab dispatch |
| `panel_world.h/.cpp` | `Cheats\World` | World / environment cheats |
| `panel_player.h/.cpp` | `Cheats\Player` | Player cheats (sub-tabs: Self, Weapons, Inventory) |
| `panel_machines.h/.cpp` | `Cheats\Machines` | Machine cheats |
| `panel_enemies.h/.cpp` | `Cheats\Enemies` | Enemy cheats |
| `plugin.h/.cpp` | `Plugin Core` | DLL entry points |
| `plugin_config.h/.cpp` | `Plugin Core\Config` | Config schema & typed accessors |
| `plugin_helpers.h` | `Plugin Core\Helpers` | Logging macros, convenience wrappers |
| `plugin_interface.h` | `SDK` | Modloader SDK (do not edit) |
| `aob_patterns.h` | `Patterns` | All AOB byte patterns (see rule 3) |

When adding a **new category** (e.g. Vehicles):
- Create `panel_vehicles.h` + `panel_vehicles.cpp`
- Add a `Cheats\Vehicles` filter
- Add the files under that filter in the `.filters` file
- Add the files to the `<ItemGroup>` blocks in `.vcxproj`
- Add the enum value in `cheat_menu.h` and a dispatch case in `cheat_menu.cpp`

## 3. AOB Pattern Registry — `aob_patterns.h`

All byte-pattern strings must live in `aob_patterns.h` inside the
`BetterCheats::AOB` namespace. **Never** hard-code a raw pattern string
inside a panel or feature file.

Each entry must include the class+function name and the function signature
as comments immediately above the constant:

```cpp
// Class::Function  AMyClass::DoThing
// Parameters       (AMyClass* self, int count, bool force)
constexpr const char* DoThing = "48 89 5C 24 ?? 57 48 83 EC 20 ?? ?? ?? ??";
```

Use `IPluginScanner::FindPatternInMainModule` (via `GetScanner()`) to
resolve patterns at runtime.

## 4. General Coding Conventions

- Namespace everything under `BetterCheats::` (panels use `BetterCheats::Panels::`)
- No raw ImGui calls outside of panel/menu render functions
- Config keys live in `plugin_config.h` — never read `IPluginConfig` directly from panels
- Keybinds use `RegisterKeybindByName` so the modloader tracks user rebinds automatically
- Comments only where the *why* is non-obvious — no narration of what the code does
- Wrap `SDK::UWorld::GetWorld()` and other SDK access that can run with no world loaded in `try/catch`
- When mutating gameplay attributes, set both `BaseValue` and `CurrentValue` so changes survive gameplay-effect re-evaluation
- Never build the project, the user will always do that.