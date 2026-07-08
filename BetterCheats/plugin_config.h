#pragma once

#include "plugin_interface.h"

namespace BetterCheatsConfig
{
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

	private:
		static IPluginSelf* s_self;
	};
}
