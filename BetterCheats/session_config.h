#pragma once

#include "plugin_interface.h"
#include "json.hpp"

#include <string>

namespace BetterCheats::SessionConfig
{
	// Resolves the plugin's config folder (<Plugins>\<pluginName>\, created if
	// missing) — call once during PluginInit.
	void Initialize(IPluginSelf* self);
	void Shutdown();

	// Resolves the active save's session name from UCrSaveSubsystem and loads
	// its JSON config from <Plugins>\<pluginName>\<SessionName>.json. Must run
	// on the game thread (e.g. from OnExperienceLoadComplete). Returns false
	// if no session is currently active.
	bool Reload();

	// True once Reload() has resolved a session and loaded (or created) its config.
	bool IsLoaded();

	std::string GetSessionName();

	// Reads the value at a dot-separated path (e.g. "playerAttributes.maxHealth").
	// Returns defaultValue if no session is loaded or the path doesn't exist.
	nlohmann::json Get(const std::string& path, const nlohmann::json& defaultValue);

	// Writes the value at a dot-separated path and immediately persists the
	// session config to disk. No-op if no session is currently loaded.
	void Set(const std::string& path, const nlohmann::json& value);
}
