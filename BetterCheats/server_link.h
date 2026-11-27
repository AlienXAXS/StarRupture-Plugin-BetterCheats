#pragma once

// The cheats a joined player asks the server to run. Requests are queued from
// the menu, sent from the engine tick and answered on the loader's network
// callback, so everything here is behind one lock.

#include "plugin_interface.h"
#include "map_fields.h"
#include "server_protocol.h"

#include <cstdio>
#include <string>

namespace BetterCheats::ServerLink
{
	// One per server-held feature slot. The three build rules share an op.
	enum class Op { Give, Wave, Stack, Corp, Power, Enemy, Weapon, Grid, Plant, Mine,
		BuildFree, BuildAnywhere, BuildUnlock, Count };

	// What the last request of an op came to. `text` may be the server's own
	// words, so it is never a format string.
	struct Result
	{
		std::string text;
		// Every field the server's replies have reported, the latest value of
		// each. Forgotten on deny and with the connection. Untrusted text.
		ServerProtocol::Fields fields;
		// Changes with each cheat.state answer and with the connection, so a menu
		// draft knows to follow `fields`. The player's own requests leave it.
		uint64_t stamp = 0;
		// The server has answered cheat.state for this slot.
		bool readBack = false;
		// The send order of the newest request whose reply set `fields`. A reply
		// to an older one does not overwrite them.
		uint64_t order = 0;

		// Empty when the server has not reported the key.
		std::string Value(const char* key) const
		{
			for (const auto& field : fields)
				if (field.first == key)
					return field.second;
			return {};
		}

		// The same as a number from a control's range, else `fallback`.
		float NumberOr(const char* key, float min, float max, float fallback) const
		{
			return ServerProtocol::NumberOr(Value(key), min, max, fallback);
		}

		// True once each time `stamp` changes, so a menu draft is reloaded then
		// and left alone while it is being edited.
		bool ChangedSince(uint64_t& seen) const
		{
			if (stamp == seen)
				return false;
			seen = stamp;
			return true;
		}
	};

	// PluginInit / PluginShutdown.
	void Initialize(IPluginSelf* self);
	void Shutdown();

	// Game thread, once per engine tick, behind the cheats gate. The only place
	// that sends.
	void Tick();

	// Any thread. Drops what is queued but not sent.
	void DropQueued();

	// Any thread. A world ended or the connection changed: forgets what the
	// server can do, every waiting request and every result.
	void Reset();

	// Any thread. Changes with every Reset, so a list gathered for one
	// connection can tell it belongs to an earlier one.
	uint32_t Connection();

	// Any thread. Why the server can't run the op, or empty when it can.
	std::string Blocker(Op op);

	// Who applies a cheat that also works from the client: the game here
	// (not joined, or the server can't run the op), nobody yet (still asking),
	// the server, or nobody because the server said no.
	enum class Route { Local, Checking, Server, Refused };

	// Any thread.
	Route RouteOf(Op op);

	// What a joined player's row does while the server can't run its cheat: Grey
	// greys it under the reason, Own leaves it to this plugin's own switch.
	enum class Without { Grey, Own };

	struct Row
	{
		Op      op;
		Without without;
	};

	// The spawner, wave, corporations, enemies, power and stack size rows are not
	// listed: Features::kNeedsServer holds them on every joined client.
	namespace Rows
	{
		inline constexpr Row WeaponDamage  { Op::Weapon,        Without::Grey };
		inline constexpr Row WeaponFireRate{ Op::Weapon,        Without::Grey };
		inline constexpr Row DrillHeat     { Op::Mine,          Without::Grey };
		inline constexpr Row MiningBoost   { Op::Mine,          Without::Grey };
		inline constexpr Row PlantPickup   { Op::Plant,         Without::Grey };
		inline constexpr Row FreeCost      { Op::BuildFree,     Without::Grey };
		inline constexpr Row FreeStability { Op::BuildFree,     Without::Grey };
		inline constexpr Row UnlockAll     { Op::BuildUnlock,   Without::Grey };
		inline constexpr Row Windows       { Op::BuildAnywhere, Without::Own };
		inline constexpr Row Benches       { Op::BuildAnywhere, Without::Own };
		inline constexpr Row Grid          { Op::Grid,          Without::Own };
	}

	// Any thread. Whether this plugin's own switch is held back for the row: only
	// on a joined client, and for an Own row only while the server runs the cheat,
	// is being asked or has said no.
	bool ServerOwns(Row row);

	// Any thread. Ignored while the op is blocked.
	void Queue(Op op, ServerProtocol::Fields fields);

	// Any thread.
	Result LastResult(Op op);

	// A multiplier as a request field: two decimals.
	inline std::string Number(float value)
	{
		char text[32];
		std::snprintf(text, sizeof(text), "%.2f", value);
		return text;
	}

	// Any thread. Text that may hold anything the server sent.
	void Say(IModLoaderImGui* imgui, const std::string& text);

	// Features::Section for a section the server can run: on a joined client it
	// draws `remote` when the server handles the op, and otherwise the greyed
	// section under the reason. A denied feature never reaches either.
	template <class Draw, class Remote>
	void Section(IModLoaderImGui* imgui, const PluginMap::Feature& feature, Op op, Draw draw, Remote remote,
		Features::Why saidAbove = Features::Why::Live)
	{
		if (Features::StateOf(feature) != Features::Why::ServerOnly)
		{
			Features::Section(imgui, feature, draw, saidAbove);
			return;
		}

		const std::string blocker = Blocker(op);
		if (!blocker.empty())
		{
			Say(imgui, blocker);
			imgui->BeginDisabled(true);
			draw(true);
			imgui->EndDisabled();
			return;
		}

		remote();
		const std::string result = LastResult(op).text;
		if (!result.empty())
			Say(imgui, result);
	}
}
