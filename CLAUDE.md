# BetterCheats — Development Rules

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
