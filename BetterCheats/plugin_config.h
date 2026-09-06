#pragma once

#include "plugin_interface.h"

namespace BetterCheatsConfig
{
	// F9 rather than a higher function key: F11 is the OS-level fullscreen
	// toggle in most windowed contexts, and F12 is Steam's screenshot bind.
	constexpr const char* kDefaultNoClipKey = "F9";

	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable BetterCheats"
		},
		{
			"General",
			"EnableCheatsInMultiplayer",
			ConfigValueType::Boolean,
			"false",
			"Bypasses the single-player check so the menu opens in multiplayer. Cheats are NOT supported in multiplayer and may cause crashes or other undesired effects."
		},
		{
			"Menu",
			"ToggleKey",
			ConfigValueType::Keybind,
			"F10",
			"Key to open / close the BetterCheats menu"
		},
		{
			"Keybinds",
			"NoClipKey",
			ConfigValueType::Keybind,
			kDefaultNoClipKey,
			"Key to toggle No Clip on / off without opening the menu"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;

			// Initialize config from schema - creates file with defaults if missing
			if (s_self)
			{
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		// Cheats are NOT supported in multiplayer and may cause crashes or other undesired effects when enabled.
		static bool IsCheatsInMultiplayerEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "EnableCheatsInMultiplayer", false) : false;
		}

		// Returns the current toggle keybind string (e.g. "F10", "Ctrl+F10").
		// The modloader re-registers the keybind automatically when the user changes it.
		static const char* GetToggleKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "ToggleKey", buffer, sizeof(buffer), "F10"))
				return buffer;
			return "F10";
		}

		// Returns the current No Clip keybind string (e.g. "F9", "Ctrl+F9").
		static const char* GetNoClipKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Keybinds", "NoClipKey", buffer, sizeof(buffer), kDefaultNoClipKey))
				return buffer;
			return kDefaultNoClipKey;
		}

		// Persists a new No Clip keybind. The caller still owns re-registering it
		// with the loader — writing the .ini only changes what the next launch,
		// and the loader's own config UI, will show.
		static bool SetNoClipKey(const char* combo)
		{
			if (!s_self || !combo || !*combo)
				return false;

			return s_self->config->WriteString(s_self, "Keybinds", "NoClipKey", combo);
		}

	private:
		static IPluginSelf* s_self;
	};
}
