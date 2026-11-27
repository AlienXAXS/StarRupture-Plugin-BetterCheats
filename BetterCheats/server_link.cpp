#include "server_link.h"

#include "game_context.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace BetterCheats::ServerLink
{
	namespace Protocol = ServerProtocol;

	namespace
	{
		constexpr uint64_t    kRetryMs   = 1000;
		constexpr uint64_t    kCapAgainMs = 10000;
		constexpr size_t      kOpCount   = static_cast<size_t>(Op::Count);
		constexpr const char* kChecking  = "Asking the server what it can do...";
		constexpr const char* kSending   = "Waiting for the server...";
		constexpr const char* kDone      = "Done.";
		constexpr size_t      kMaxKept   = 32;

		enum class Kind { Click, Cap, State, List };

		struct What
		{
			Op       op    = Op::Give;
			Kind     kind  = Kind::Click;
			uint64_t order = 0;
			// A cheat.power click that puts every building back.
			bool     resetsAllPower = false;
		};

		struct Queued
		{
			Op               op;
			Protocol::Fields fields;
		};

		enum class Cap { NotSent, Waiting, Got, Failed };

		std::mutex               g_mutex;
		std::atomic<uint32_t>    g_connection{ 0 };
		// Counts from the clock, so a RELOAD does not hand out the same ids again
		// while a reply to one of them may still arrive.
		Protocol::Waiting<What>  g_waiting{ static_cast<uint32_t>(GetTickCount64() % Protocol::kMaxId) };
		std::vector<Queued>      g_queued;
		std::array<Result, kOpCount> g_results;
		// Never reset, so no stamp or send order is used twice.
		uint64_t                 g_stamp          = 0;
		uint64_t                 g_order          = 0;
		// What is on for this player is asked after every cap answer and every
		// change of a feature flag.
		Protocol::StateRequest   g_state;
		uint32_t                 g_flags          = 0;
		bool                     g_flagsSeen      = false;
		// The server's last word on Unlock All Buildings. The server tells every
		// player, whatever their own rules, so it is not part of a slot's result.
		std::atomic<bool>        g_unlockHeld{ false };
		uint64_t                 g_unlockOrder    = 0;
		// The edited power values, asked for while the power page is shown.
		PowerList                g_power;
		bool                     g_powerEdited    = false;
		bool                     g_powerDue       = false;
		bool                     g_powerWaiting   = false;
		// A list sent at or before this send order is out of date.
		uint64_t                 g_powerFloor     = 0;
		uint64_t                 g_powerSentMs    = 0;
		std::atomic<bool>        g_powerShown{ false };
		Cap                      g_cap = Cap::NotSent;
		Protocol::Reply          g_capReply;
		std::string              g_capFailure;
		bool                     g_capRefused     = false;
		uint64_t                 g_capRetryMs     = 0;
		uint64_t                 g_notReadySince  = 0;
		bool                     g_wasReady       = false;
		bool                     g_neverReady     = false;
		bool                     g_capStale       = false;
		uint64_t                 g_capSentMs      = 0;
		// From the ready callback, which fires once per connection, so a world
		// ending must not forget it.
		std::string              g_serverTag;
		bool                     g_serverTagSeen  = false;

		IPluginSelf*      g_self = nullptr;
		std::atomic<bool> g_canSend{ false };

		struct OpInfo
		{
			const char*               name;
			const PluginMap::Feature* feature;
			// What cheat.state reports for this slot. Empty: nothing.
			std::optional<Protocol::Held> held;
			// The op's rule in the cap reply's build.rules. Null: the op has none.
			const char* rule = nullptr;
		};
		using Held = Protocol::Held;

		// In Op order.
		const OpInfo kOps[] = {
			{ "cheat.give",   &Features::Items, {} },
			{ "cheat.wave",   &Features::Wave, Held::Wave },
			{ "cheat.stack",  &Features::StackSize, Held::Stack },
			{ "cheat.corp",   &Features::Corporations, Held::Corp },
			{ "cheat.power",  &Features::Power, Held::Power },
			{ "cheat.enemy",  &Features::Enemies, Held::Enemy },
			{ "cheat.weapon", &Features::Weapons, Held::Weapon },
			{ "cheat.grid",   &Features::Inventory, Held::Grid },
			{ "cheat.plant",  &Features::Inventory, Held::Plant },
			{ "cheat.mine",   &Features::Mining, Held::Mine },
			{ "cheat.stat",   &Features::Stats, Held::Stat },
			{ "cheat.build",  &Features::BuildFree, Held::BuildParts, "free" },
			{ "cheat.build",  &Features::BuildAnywhere, Held::BuildParts, "anywhere" },
			{ "cheat.build",  &Features::BuildUnlock, Held::BuildUnlock, "unlock" },
			{ "cheat.move",   &Features::Movement, Held::Move },
		};
		static_assert(sizeof(kOps) / sizeof(kOps[0]) == kOpCount);

		// False when a server's rules or the loader switched every feature off:
		// nothing is sent then.
		bool ModOn()
		{
			return Features::WithAll([](const auto&... features) { return (features.On() || ...); });
		}

		// One bit per feature, to see that one changed.
		uint32_t FlagBits()
		{
			return Features::WithAll([](const auto&... features)
			{
				uint32_t bits = 0;
				((bits = (bits << 1) | (features.On() ? 1u : 0u)), ...);
				return bits;
			});
		}

		const char* OpName(Op op) { return kOps[static_cast<size_t>(op)].name; }

		const PluginMap::Feature& FeatureOf(Op op) { return *kOps[static_cast<size_t>(op)].feature; }

		Result& ResultOf(Op op) { return g_results[static_cast<size_t>(op)]; }

		IPluginNetworkChannel* Network()
		{
			return g_self && g_self->hooks ? g_self->hooks->Network : nullptr;
		}

		void ResetLocked()
		{
			++g_connection;
			g_waiting.Clear();
			g_queued.clear();
			g_results = {};
			for (Result& result : g_results)
				result.stamp = ++g_stamp;
			g_state.Clear();
			g_flagsSeen   = false;
			g_unlockHeld  = false;
			g_unlockOrder = 0;
			g_power = {};
			g_power.stamp  = ++g_stamp;
			g_powerEdited  = false;
			g_powerDue     = false;
			g_powerWaiting = false;
			g_powerFloor   = 0;
			g_powerSentMs  = 0;
			g_powerShown   = false;
			g_cap = Cap::NotSent;
			g_capReply = {};
			g_capFailure.clear();
			g_capRefused    = false;
			g_capRetryMs    = 0;
			g_notReadySince = 0;
			g_wasReady      = false;
			g_neverReady    = false;
			g_capStale      = false;
			g_capSentMs     = 0;
		}

		// What was listed at or before send order `order` no longer holds. It is
		// asked again only when some building still has an edited value.
		void PowerChangedLocked(uint64_t order, bool anyEdited)
		{
			g_power = {};
			g_power.stamp = ++g_stamp;
			g_powerEdited = g_powerDue = anyEdited;
			g_powerFloor  = order;
		}

		bool ServerListsLocked(const char* op)
		{
			if (g_cap != Cap::Got)
				return false;
			const Protocol::Handler handler = Protocol::HandlerOf(g_capReply, op);
			return handler.version && handler.on;
		}

		// What the cap reply says about an op, its build rule included.
		Protocol::Handler HandlerLocked(Op op)
		{
			return Protocol::HandlerOf(g_capReply, OpName(op), kOps[static_cast<size_t>(op)].rule);
		}

		// The server listed the op as on and now can't run it, so what it can do
		// has changed.
		void NoteRefusalLocked(const Protocol::Reply& reply, const char* op)
		{
			if (reply.reason == Protocol::kNotOnThisBuild && ServerListsLocked(op))
				g_capStale = true;
		}

		bool ServerLoaderTooOldLocked()
		{
			return g_serverTagSeen && Protocol::IsLoaderTagTooOld(g_serverTag.c_str());
		}

		// What was clicked and not sent yet won't be, so its row stops waiting.
		void DropQueuedLocked()
		{
			for (const Queued& queued : g_queued)
				ResultOf(queued.op).text.clear();
			g_queued.clear();
		}

		void FailCapLocked(std::string why)
		{
			g_cap = Cap::Failed;
			g_capFailure = std::move(why);
			DropQueuedLocked();
		}

		// A request that left no entry waiting: the send did not queue.
		void SendFailedLocked(const What& what, int result)
		{
			const char* text = Protocol::SendFailureText(result);
			const bool cap = what.kind == Kind::Cap;
			if (result == -1)
				FailCapLocked(text);
			else if (what.kind == Kind::State)
			{
				if (result == 0)
					g_state.Want();     // not ready after all: asked again shortly
			}
			else if (what.kind == Kind::List)
			{
				g_powerWaiting = false;
				if (result == 0)
					g_powerDue = g_powerEdited;
				else
					g_power.note = text;
			}
			else if (cap && g_cap == Cap::Got)
				g_capStale = true;      // asking again failed: the last answer stands for now
			else if (cap && result == 0)
				g_cap = Cap::NotSent;   // not ready after all: asked again shortly
			else if (cap)
				FailCapLocked(text);

			if (what.kind == Kind::Click)
				ResultOf(what.op).text = text;
		}

		void Send(uint32_t id, const What& what, const std::string& message)
		{
			IPluginNetworkChannel* network = Network();
			const auto send = PluginOptSendToServerPlugin(network);
			const int result = send
				? send(g_self, Protocol::kTarget, Protocol::kTag,
					reinterpret_cast<const uint8_t*>(message.data()), message.size())
				: -2;
			if (result == 1)
				return;

			std::lock_guard<std::mutex> lock(g_mutex);
			if (g_waiting.Take(id))
				SendFailedLocked(what, result);
		}

		void OnServerReady(const char* serverBuildTag)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			ResetLocked();
			g_serverTag = serverBuildTag ? Protocol::Clip(serverBuildTag, 64) : std::string();
			g_serverTagSeen = true;
		}

		// A cheat.state answer. A slot it does not report is not on, unless a
		// request sent after this one has already been answered for it.
		void ApplyStateLocked(const Protocol::Reply& state, uint64_t order)
		{
			for (size_t i = 0; i < kOpCount; ++i)
			{
				Result& result = g_results[i];
				if (!kOps[i].held || !kOps[i].feature->On() || order <= result.order)
					continue;
				result.fields = Protocol::HeldFields(state, *kOps[i].held);
				result.order    = order;
				result.stamp    = ++g_stamp;
				result.readBack = true;
				if (static_cast<Op>(i) == Op::Power)
				{
					const std::string edited = result.Value("edited");
					PowerChangedLocked(order, !edited.empty() && edited != "0");
				}
			}
			// Not behind this player's own rules. A missing key is off.
			if (order > g_unlockOrder)
			{
				const std::string* unlock = state.Find("unlock");
				g_unlockHeld  = unlock && *unlock == "on";
				g_unlockOrder = order;
			}
		}

		void OnMessage(const char* /*pluginName*/, const char* /*typeTag*/, const uint8_t* data, size_t size)
		{
			auto reply = Protocol::ParseReply(data, size);
			if (!reply)
				return;

			std::lock_guard<std::mutex> lock(g_mutex);
			const auto entry = g_waiting.Take(reply->id);
			if (!entry)
				return;

			if (entry->what.kind == Kind::State)
			{
				if (reply->status == Protocol::Status::Ok)
				{
					ApplyStateLocked(*reply, entry->what.order);
					return;
				}
				// Refused: what earlier replies reported stands.
				if (reply->reason == Protocol::kRateLimited)
					g_state.Unanswered(entry->id);
				NoteRefusalLocked(*reply, Protocol::kStateOp);
				return;
			}

			if (entry->what.kind == Kind::List)
			{
				g_powerWaiting = false;
				if (entry->what.order <= g_powerFloor)
					return;
				if (reply->status != Protocol::Status::Ok)
				{
					// Not asked again by itself: the page says why and has Rescan.
					g_power.note = reply->reason.empty() ? std::string(Protocol::kNoReason) : reply->reason;
					NoteRefusalLocked(*reply, OpName(Op::Power));
					return;
				}
				Protocol::PowerList list = Protocol::ParsePowerList(*reply);
				g_power.values = std::move(list.entries);
				g_power.more   = list.more;
				g_power.note.clear();
				g_power.stamp  = ++g_stamp;
				return;
			}

			if (entry->what.kind == Kind::Cap)
			{
				if (reply->status == Protocol::Status::Ok)
				{
					g_capReply = std::move(*reply);
					g_cap = Cap::Got;
					g_state.Want();
				}
				else if (g_cap == Cap::Got)
				{
					g_capStale = true;   // asking again failed: the last answer stands for now
				}
				else
				{
					g_capRefused = reply->status == Protocol::Status::Refused;
					FailCapLocked(reply->reason);
				}
				return;
			}

			Result& result = ResultOf(entry->what.op);
			result.text = reply->reason.empty() ? kDone : reply->reason;
			if (reply->status != Protocol::Status::Ok)
			{
				NoteRefusalLocked(*reply, OpName(entry->what.op));
				return;
			}

			if (entry->what.op == Op::BuildUnlock && entry->what.order > g_unlockOrder)
				if (const std::string* on = reply->Find("on"); on && *on == "on")
				{
					g_unlockHeld  = true;
					g_unlockOrder = entry->what.order;
				}
			if (entry->what.op == Op::Power && entry->what.order > g_powerFloor)
				PowerChangedLocked(entry->what.order, !entry->what.resetsAllPower);

			// The reply names one stat, so the list of locks is asked for again.
			if (entry->what.op == Op::Stat)
				g_state.Want();

			if (entry->what.order < result.order)
				return;
			result.order = entry->what.order;
			// The count of edited power values only comes with cheat.state, so a
			// change makes the one held out of date.
			result.fields.erase(std::remove_if(result.fields.begin(), result.fields.end(),
				[](const auto& field) { return field.first == "edited"; }), result.fields.end());

			for (auto& [key, value] : reply->fields)
			{
				auto kept = std::find_if(result.fields.begin(), result.fields.end(),
					[&](const auto& field) { return field.first == key; });
				if (kept != result.fields.end())
					kept->second = std::move(value);
				else if (result.fields.size() < kMaxKept)
					result.fields.emplace_back(std::move(key), std::move(value));
			}
		}
	}

	void Initialize(IPluginSelf* self)
	{
		g_self = self;
		IPluginNetworkChannel* network = Network();
		// Null below interface 83: nothing is registered and nothing is sent.
		if (!PluginOptSendToServerPlugin(network))
			return;

		g_canSend = true;
		network->RegisterMessageHandler(self, Protocol::kTag, &OnMessage);
		network->RegisterServerReadyCallback(self, &OnServerReady);
	}

	void Shutdown()
	{
		IPluginNetworkChannel* network = Network();
		if (g_canSend.exchange(false) && network)
		{
			network->UnregisterServerReadyCallback(g_self, &OnServerReady);
			network->UnregisterMessageHandler(g_self, Protocol::kTag, &OnMessage);
		}
		Reset();
		g_self = nullptr;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		ResetLocked();
	}

	void DropQueued()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		DropQueuedLocked();
	}

	void Tick()
	{
		if (!g_canSend)
			return;
		if (!GameContext::IsJoinedClient())
		{
			Reset();
			return;
		}

		IPluginNetworkChannel* network = Network();
		if (!network)
			return;
		const bool ready = network->IsServerReady();
		const uint64_t now = GetTickCount64();

		struct Outgoing { uint32_t id; What what; std::string message; };
		std::vector<Outgoing> outgoing;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!ready)
			{
				if (g_wasReady)
					ResetLocked();
				if (g_notReadySince == 0)
					g_notReadySince = now;
				g_neverReady = now - g_notReadySince >= Protocol::kTimeoutMs;
				return;
			}
			g_wasReady   = true;
			g_neverReady = false;

			// A deny puts the server's toggles back, so what it reported is void.
			for (size_t i = 0; i < kOpCount; ++i)
				if (!kOps[i].feature->On())
					g_results[i] = {};
			if (!Features::Power.On())
				PowerChangedLocked(g_order, false);

			const uint32_t flags = FlagBits();
			if (g_flagsSeen && flags != g_flags)
				g_state.Want();
			g_flags     = flags;
			g_flagsSeen = true;

			const bool modOn = ModOn();
			if (!modOn)
				g_state.Drop();
			// A server without cheat.state can't say the unlock still stands for a
			// player whose own rules switched it off.
			if (!modOn || (!Features::BuildUnlock.On() && !ServerListsLocked(Protocol::kStateOp)))
				g_unlockHeld = false;

			for (const auto& expired : g_waiting.Expire(now))
			{
				// An unanswered cheat.state changes nothing and no row waits on it.
				if (expired.what.kind == Kind::State)
				{
					g_state.Unanswered(expired.id);
					continue;
				}
				if (expired.what.kind == Kind::List)
				{
					g_powerWaiting = false;
					if (expired.what.order > g_powerFloor)
						g_power.note = Protocol::kNoAnswer;
					continue;
				}
				if (expired.what.kind == Kind::Cap && g_cap == Cap::Got)
					g_capStale = true;
				else if (expired.what.kind == Kind::Cap)
					FailCapLocked(Protocol::kNoCapAnswer);
				else
					ResultOf(expired.what.op).text = Protocol::kNoAnswer;
			}

			const bool firstCap = g_cap == Cap::NotSent && now >= g_capRetryMs && !ServerLoaderTooOldLocked();
			const bool capAgain = g_cap == Cap::Got && g_capStale && now - g_capSentMs >= kCapAgainMs;
			if (modOn && (firstCap || capAgain))
			{
				const What what{ Op::Give, Kind::Cap, ++g_order };
				if (const uint32_t id = g_waiting.Add(what, now))
				{
					if (auto message = Protocol::BuildRequest(id, "cap", {}))
					{
						// Asking again keeps the last answer until the new one is in.
						if (firstCap)
							g_cap = Cap::Waiting;
						g_capStale  = false;
						g_capSentMs = now;
						g_capRetryMs = now + kRetryMs;
						outgoing.push_back({ id, what, std::move(*message) });
					}
				}
			}

			// Ahead of the clicks, so their replies are the newer ones.
			if (g_cap == Cap::Got && g_state.Due(now))
			{
				if (!modOn || !ServerListsLocked(Protocol::kStateOp))
					g_state.Drop();     // an older server: nothing shows as on
				else if (!g_waiting.Full())
				{
					const What what{ Op::Give, Kind::State, ++g_order };
					const uint32_t id = g_waiting.Add(what, now);
					if (auto message = Protocol::BuildRequest(id, Protocol::kStateOp, {}))
					{
						outgoing.push_back({ id, what, std::move(*message) });
						g_state.Sent(id, now);
					}
				}
			}

			for (Queued& queued : g_queued)
			{
				// Denied since the click: nothing is sent.
				if (!FeatureOf(queued.op).On())
				{
					ResultOf(queued.op).text.clear();
					continue;
				}
				const bool resetsAllPower = queued.op == Op::Power
					&& std::any_of(queued.fields.begin(), queued.fields.end(),
						[](const auto& field) { return field.first == "do" && field.second == "reset-all"; });
				const What what{ queued.op, Kind::Click, ++g_order, resetsAllPower };
				const uint32_t id = g_waiting.Add(what, now);
				if (id == 0)
				{
					ResultOf(queued.op).text = Protocol::kTooManyWaiting;
					continue;
				}
				auto message = Protocol::BuildRequest(id, OpName(queued.op), queued.fields);
				if (!message)
				{
					g_waiting.Take(id);
					ResultOf(queued.op).text = Protocol::SendFailureText(-2);
					continue;
				}
				outgoing.push_back({ id, what, std::move(*message) });
			}
			g_queued.clear();

			// After the clicks, so the list holds what they changed. Only while the
			// page is shown, and one at a time.
			if (g_powerShown.exchange(false) && g_powerDue && !g_powerWaiting && Features::Power.On()
				&& now - g_powerSentMs >= Protocol::kStateSpacingMs && !g_waiting.Full()
				&& ServerListsLocked(Protocol::kStateOp) && ServerListsLocked(OpName(Op::Power)))
			{
				const What what{ Op::Power, Kind::List, ++g_order };
				const uint32_t id = g_waiting.Add(what, now);
				if (auto message = Protocol::BuildRequest(id, OpName(Op::Power), { { "do", "list" } }))
				{
					outgoing.push_back({ id, what, std::move(*message) });
					g_powerDue     = false;
					g_powerWaiting = true;
					g_powerSentMs  = now;
				}
			}
		}

		// Outside the lock: the loader is not called while it is held.
		for (const Outgoing& request : outgoing)
			Send(request.id, request.what, request.message);
	}

	uint32_t Connection()
	{
		return g_connection;
	}

	std::string Blocker(Op op)
	{
		if (!g_canSend)
			return Protocol::kNeedsClientLoader;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_wasReady && ServerLoaderTooOldLocked())
			return Protocol::kNeedsServerLoader;

		switch (g_cap)
		{
		case Cap::Failed:
			return g_capFailure;
		case Cap::Got:
		{
			const Protocol::Handler handler = HandlerLocked(op);
			return handler.version && handler.on ? std::string() : std::string(Protocol::kNotOnThisBuild);
		}
		default:
			// A server that never acknowledges the loader has none that can.
			return g_neverReady ? Protocol::kNeedsServerLoader : kChecking;
		}
	}

	Route RouteOf(Op op)
	{
		if (!GameContext::IsJoinedClient())
			return Route::Local;

		const std::string blocker = Blocker(op);
		if (blocker.empty())
			return Route::Server;
		if (blocker == kChecking)
			return Route::Checking;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_cap == Cap::Failed)
			return g_capRefused ? Route::Refused : Route::Local;
		const Protocol::Handler handler = HandlerLocked(op);
		return g_cap == Cap::Got && handler.version && !handler.on ? Route::Refused : Route::Local;
	}

	std::optional<std::string> StatLocks()
	{
		if (RouteOf(Op::Stat) != Route::Server)
			return std::nullopt;

		std::lock_guard<std::mutex> lock(g_mutex);
		for (const auto& [key, value] : ResultOf(Op::Stat).fields)
			if (key == "locks")
				return value;
		return std::nullopt;
	}

	std::optional<Protocol::Movement> ServerMovement()
	{
		if (RouteOf(Op::Move) != Route::Server)
			return std::nullopt;

		const Result result = LastResult(Op::Move);
		return Protocol::MovementOf(result.Value("fly"), result.Value("speed"));
	}

	bool ServerOwns(Row row)
	{
		if (!GameContext::IsJoinedClient())
			return false;
		return row.without == Without::Grey || RouteOf(row.op) != Route::Local;
	}

	void Queue(Op op, Protocol::Fields fields)
	{
		// Taken first: a click judged against one connection never joins the next.
		const uint32_t connection = g_connection;
		if (!FeatureOf(op).On() || !Blocker(op).empty())
			return;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (connection != g_connection)
			return;
		if (g_queued.size() + g_waiting.Size() >= Protocol::kMaxWaiting)
		{
			ResultOf(op).text = Protocol::kTooManyWaiting;
			return;
		}
		g_queued.push_back({ op, std::move(fields) });
		ResultOf(op).text = kSending;
	}

	Result LastResult(Op op)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return ResultOf(op);
	}

	bool HoldsUnlock()
	{
		return g_unlockHeld && ModOn();
	}

	PowerList ShowPowerList()
	{
		g_powerShown = true;
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_power;
	}

	void RescanPowerList()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_powerDue = g_powerEdited;
		g_power.note.clear();
	}

	void Say(IModLoaderImGui* imgui, const std::string& text)
	{
		imgui->TextWrapped(text.c_str());   // the loader prints it through "%s"
	}
}
