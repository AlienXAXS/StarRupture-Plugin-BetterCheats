#include "keybind_picker.h"

#include <cstdio>
#include <cstring>

namespace BetterCheats::Keybind
{
	namespace
	{
		struct KeyName
		{
			int         vk;
			const char* name;
		};

		// Names must match the modloader's own key table (keybind_registry.cpp) —
		// they are what ParseComboString reads back out of the .ini.
		//
		// Mouse buttons are deliberately absent: the click that arms the picker
		// would otherwise be captured as the new bind. The modifier keys are
		// absent as base keys for the same reason they are useless as one — they
		// are offered as prefixes instead.
		constexpr KeyName kKeys[] = {
			{ VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
			{ VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
			{ VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },

			{ 'A', "A" }, { 'B', "B" }, { 'C', "C" }, { 'D', "D" }, { 'E', "E" },
			{ 'F', "F" }, { 'G', "G" }, { 'H', "H" }, { 'I', "I" }, { 'J', "J" },
			{ 'K', "K" }, { 'L', "L" }, { 'M', "M" }, { 'N', "N" }, { 'O', "O" },
			{ 'P', "P" }, { 'Q', "Q" }, { 'R', "R" }, { 'S', "S" }, { 'T', "T" },
			{ 'U', "U" }, { 'V', "V" }, { 'W', "W" }, { 'X', "X" }, { 'Y', "Y" },
			{ 'Z', "Z" },

			{ '0', "Zero" }, { '1', "One" }, { '2', "Two" }, { '3', "Three" },
			{ '4', "Four" }, { '5', "Five" }, { '6', "Six" }, { '7', "Seven" },
			{ '8', "Eight" }, { '9', "Nine" },

			{ VK_TAB, "Tab" }, { VK_CAPITAL, "CapsLock" }, { VK_SPACE, "SpaceBar" },
			{ VK_RETURN, "Enter" }, { VK_BACK, "BackSpace" }, { VK_DELETE, "Delete" },
			{ VK_INSERT, "Insert" },

			{ VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
			{ VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },

			{ VK_OEM_3, "Tilde" }, { VK_OEM_MINUS, "Hyphen" }, { VK_OEM_PLUS, "Equals" },
			{ VK_OEM_4, "LeftBracket" }, { VK_OEM_6, "RightBracket" }, { VK_OEM_5, "Backslash" },
			{ VK_OEM_1, "Semicolon" }, { VK_OEM_7, "Apostrophe" }, { VK_OEM_COMMA, "Comma" },
			{ VK_OEM_PERIOD, "Period" }, { VK_OEM_2, "Slash" },

			{ VK_NUMPAD0, "NumPadZero" }, { VK_NUMPAD1, "NumPadOne" }, { VK_NUMPAD2, "NumPadTwo" },
			{ VK_NUMPAD3, "NumPadThree" }, { VK_NUMPAD4, "NumPadFour" }, { VK_NUMPAD5, "NumPadFive" },
			{ VK_NUMPAD6, "NumPadSix" }, { VK_NUMPAD7, "NumPadSeven" }, { VK_NUMPAD8, "NumPadEight" },
			{ VK_NUMPAD9, "NumPadNine" },

			{ VK_ADD, "Add" }, { VK_SUBTRACT, "Subtract" }, { VK_MULTIPLY, "Multiply" },
			{ VK_DIVIDE, "Divide" }, { VK_DECIMAL, "Decimal" },
		};

		constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

		// Which picker owns the capture, empty when none does. Everything in here
		// is render-thread only — ImGui callbacks are the only caller.
		char g_capturingId[64] = "";

		// A picker is armed only once every candidate key has been seen released.
		// Without it, a bind triggered from the keyboard captures itself the
		// instant the button is "clicked" by that same still-held key.
		bool g_armed = false;

		bool IsHeld(int vk)
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		bool AnyCandidateHeld()
		{
			for (int i = 0; i < kKeyCount; ++i)
			{
				if (IsHeld(kKeys[i].vk))
					return true;
			}
			return false;
		}

		// Builds "Ctrl+Shift+Alt+Key" in the order the modloader formats it, so a
		// combo written here compares equal to one it wrote itself.
		void FormatCombo(const char* keyName, char* out, size_t outSize)
		{
			char prefix[48] = "";

			if (IsHeld(VK_CONTROL)) strncat_s(prefix, sizeof(prefix), "Ctrl+",  _TRUNCATE);
			if (IsHeld(VK_SHIFT))   strncat_s(prefix, sizeof(prefix), "Shift+", _TRUNCATE);
			if (IsHeld(VK_MENU))    strncat_s(prefix, sizeof(prefix), "Alt+",   _TRUNCATE);

			snprintf(out, outSize, "%s%s", prefix, keyName);
		}
	}

	bool IsCapturing()
	{
		return g_capturingId[0] != '\0';
	}

	void CancelCapture()
	{
		g_capturingId[0] = '\0';
		g_armed = false;
	}

	bool RenderPicker(IModLoaderImGui* imgui,
	                  const char* id,
	                  const char* currentCombo,
	                  char* newCombo,
	                  size_t newComboSize)
	{
		if (!imgui || !id || !newCombo || newComboSize == 0)
			return false;

		const bool capturing = IsCapturing() && strcmp(g_capturingId, id) == 0;

		imgui->PushIDStr(id);

		bool picked = false;

		if (!capturing)
		{
			char label[96];
			snprintf(label, sizeof(label), "%s##rebind",
				(currentCombo && *currentCombo) ? currentCombo : "Unbound");

			if (imgui->Button(label))
			{
				// Another picker may already hold the capture; taking it over is
				// what the user just asked for.
				snprintf(g_capturingId, sizeof(g_capturingId), "%s", id);
				g_armed = false;
			}

			if (imgui->IsItemHovered())
				imgui->SetTooltip("Click, then press the key you want. Escape cancels.");
		}
		else
		{
			imgui->TextColored(1.0f, 0.8f, 0.2f, 1.0f, "Press a key...");

			// Escape always cancels, and is checked before arming so a picker
			// opened with a key still down can still be backed out of.
			if (IsHeld(VK_ESCAPE))
			{
				CancelCapture();
			}
			else if (!g_armed)
			{
				if (!AnyCandidateHeld())
					g_armed = true;
			}
			else
			{
				for (int i = 0; i < kKeyCount; ++i)
				{
					if (!IsHeld(kKeys[i].vk))
						continue;

					FormatCombo(kKeys[i].name, newCombo, newComboSize);
					picked = true;
					CancelCapture();
					break;
				}
			}
		}

		imgui->PopID();

		return picked;
	}
}
