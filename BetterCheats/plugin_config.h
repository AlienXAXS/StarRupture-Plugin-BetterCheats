#pragma once

#include "plugin_interface.h"

#include <string>

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

		// Returns the current toggle keybind string (e.g. "F10", "Ctrl+F10").
		// The modloader re-registers the keybind automatically when the user changes it.
		static const char* GetToggleKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "ToggleKey", buffer, sizeof(buffer), "F10"))
				return buffer;
			return "F10";
		}

		// Persisted per-building electricity output/consumption overrides, keyed by
		// the building's Mass entity config asset name (e.g. "DA_WindPowerGenerator").
		// Reloading a save reloads these assets from disk, discarding any in-memory
		// edit, so the Power panel re-applies these overrides on every rescan.
		static bool HasPowerOverride(const std::string& key)
		{
			return s_self ? s_self->config->ReadBool(s_self, "PowerOverrides", (key + "_set").c_str(), false) : false;
		}

		static float GetPowerOverride(const std::string& key, float defaultValue)
		{
			return s_self ? s_self->config->ReadFloat(s_self, "PowerOverrides", key.c_str(), defaultValue) : defaultValue;
		}

		static void SetPowerOverride(const std::string& key, float value)
		{
			if (!s_self) return;
			s_self->config->WriteFloat(s_self, "PowerOverrides", key.c_str(), value);
			s_self->config->WriteBool(s_self, "PowerOverrides", (key + "_set").c_str(), true);
		}

	private:
		static IPluginSelf* s_self;
	};
}
