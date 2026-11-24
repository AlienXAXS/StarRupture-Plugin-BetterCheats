#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Some building features now live in another plugin. While that plugin runs
// and has the feature working in this game build, BetterCheats leaves the
// feature to it: the detours pass through and the menu row is greyed out. The
// saved setting is never touched, so the feature comes back by itself when the
// other plugin goes away or loses the feature.
//
// DeferredTo() is the one place that decides. To defer another feature, add a
// row to kDeferTable.
namespace BetterCheats::Defer
{
	struct Row
	{
		const char* featureId;   // the id the feature is declared with in map_fields.h
		const char* plugin;      // the other plugin's name, as it reports it in PluginInfo
		const char* theirId;     // the id the other plugin declares the feature under
	};

	inline constexpr Row kDeferTable[] = {
		{ "build-anywhere",      "Best Builder", "build-anywhere" },
		{ "deconstruct-windows", "Best Builder", "deconstruct-windows" },
	};
	inline constexpr size_t kRows = sizeof(kDeferTable) / sizeof(kDeferTable[0]);

	// How often Tick() asks again.
	inline constexpr uint64_t kRefreshMs = 3000;

	// Room for the loader's declared-features text. What does not fit is cut
	// off by the loader, and only the whole rows in it are read.
	inline constexpr size_t kFeatureTextSize = 16 * 1024;

	enum class FeatureState { Missing, Off, On };

	// Finds "feature<TAB>plugin<TAB>id<TAB>on|off<TAB>title" in the loader's
	// text. Rows end in a line feed and a plugin name can hold a space, so
	// split on tabs only. A last row with no line feed (cut off by a small
	// buffer) is ignored.
	inline FeatureState FindFeature(std::string_view text, std::string_view plugin, std::string_view id)
	{
		for (size_t end; (end = text.find('\n')) != std::string_view::npos; text.remove_prefix(end + 1))
		{
			std::string_view fields[4];
			std::string_view row = text.substr(0, end);
			size_t count = 0;
			while (count < 4)
			{
				const size_t tab = row.find('\t');
				fields[count++] = row.substr(0, tab);
				if (tab == std::string_view::npos)
					break;
				row.remove_prefix(tab + 1);
			}
			if (count == 4 && fields[0] == "feature" && fields[1] == plugin && fields[2] == id)
				return fields[3] == "on" ? FeatureState::On : FeatureState::Off;
		}
		return FeatureState::Missing;
	}

	namespace detail
	{
		inline std::atomic<bool>     g_deferred[kRows];
		inline std::atomic<uint64_t> g_lastAskMs{ 0 };
		inline std::atomic<bool>     g_asked{ false };
	}

	// Answers whether the named plugin is running. Supplied by the caller so
	// this header stays free of loader types.
	using IsRunningFn = bool (*)(const char* plugin);

	// Copies the loader's declared-features text into buf and returns its full
	// length, or -1 before the loader has finished starting the plugins
	// (IPluginRegistry::GetDeclaredFeatures).
	using ReadFeaturesFn = int (*)(char* buf, size_t size);

	// Game thread only (call from the engine tick, never from a draw
	// callback). Asks the loader at most every kRefreshMs and caches the
	// answers in atomics that DeferredTo() reads from any thread.
	//
	// A row is deferred while its plugin runs and declares the feature "on".
	// Until the loader has finished starting (-1) every row of a running
	// plugin is deferred, so the two plugins never act at once.
	inline void Tick(uint64_t nowMs, IsRunningFn isRunning, ReadFeaturesFn readFeatures)
	{
		if (!isRunning || !readFeatures)
			return;
		if (detail::g_asked.load() && nowMs - detail::g_lastAskMs.load() < kRefreshMs)
			return;

		detail::g_lastAskMs = nowMs;
		detail::g_asked = true;

		static char text[kFeatureTextSize];
		bool asked = false;
		int length = -1;
		for (size_t i = 0; i < kRows; ++i)
		{
			if (!isRunning(kDeferTable[i].plugin))
			{
				detail::g_deferred[i] = false;
				continue;
			}
			if (!asked)
			{
				length = readFeatures(text, sizeof(text));
				asked = true;
			}
			if (length < 0)
			{
				detail::g_deferred[i] = true;
				continue;
			}
			const bool cut = static_cast<size_t>(length) >= sizeof(text);
			const FeatureState state = FindFeature(std::string_view(text, cut ? sizeof(text) - 1 : length),
			                                       kDeferTable[i].plugin, kDeferTable[i].theirId);
			// A missing row in cut-off text may be in the part that did not
			// fit, so keep the last answer.
			if (state != FeatureState::Missing || !cut)
				detail::g_deferred[i] = (state == FeatureState::On);
		}
	}

	// The plugin a feature is left to, or null when it has no row.
	inline const char* PluginFor(const char* featureId)
	{
		for (size_t i = 0; i < kRows; ++i)
		{
			if (std::strcmp(kDeferTable[i].featureId, featureId) == 0)
				return kDeferTable[i].plugin;
		}
		return nullptr;
	}

	// True while the feature is left to another plugin. Reads the cache only.
	inline bool DeferredTo(const char* featureId)
	{
		for (size_t i = 0; i < kRows; ++i)
		{
			if (std::strcmp(kDeferTable[i].featureId, featureId) == 0)
				return detail::g_deferred[i].load();
		}
		return false;
	}
}
