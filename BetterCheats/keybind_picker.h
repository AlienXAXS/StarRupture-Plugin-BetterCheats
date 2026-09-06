#pragma once

#include "plugin_interface.h"

#include <cstddef>

// A small "click, then press a key" rebind control for the cheat menu.
//
// The modloader exposes key press/release events but no key-capture UI, so the
// press is read straight from the OS with GetAsyncKeyState while the control is
// armed. That is safe from the render thread, which is where every ImGui
// callback runs.
//
// Combos are produced in the modloader's own format ("F9", "Ctrl+Shift+F9"), so
// the result can go straight to RegisterKeybindByName and into the .ini, where
// the loader's config UI shows and parses the same string.

namespace BetterCheats::Keybind
{
	// Draws the current combo plus a rebind button. Returns true on the frame the
	// user finishes picking a new combo, with newCombo filled in.
	//
	// id must be unique within the window; it is only used for widget identity.
	bool RenderPicker(IModLoaderImGui* imgui,
	                  const char* id,
	                  const char* currentCombo,
	                  char* newCombo,
	                  size_t newComboSize);

	// True while a picker is waiting for a key press. Callers that read the
	// keyboard themselves should stand down for the duration.
	bool IsCapturing();

	// Drops any in-progress capture. Call when the menu closes.
	void CancelCapture();
}
