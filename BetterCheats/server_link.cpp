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

		enum class Kind { Click, Cap, State };

		struct What
		{
			Op       op    = Op::Give;
			Kind     kind  = Kind::Click;
			uint64_t order = 0;
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
		// Set by every cap answer: what is on for this player is asked next.
		bool                     g_stateWanted    = false;
		uint64_t                 g_stateSentMs    = 0;
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
		};
		using Held = Protocol::Held;

		// In Op order.
		const OpInfo kOps[] = {
			{ "cheat.give",   &Features::Items, {} },
			{ "cheat.wave",   &Features::Wave, Held::Wave },
			{ "cheat.stack",  &Features::StackSize, Held::Stack },
			{ "cheat.corp",   &Features::Corporations, {} },
			{ "cheat.power",  &Features::Power, Held::Power },
			{ "cheat.enemy",  &Features::Enemies, Held::Enemy },
			{ "cheat.weapon", &Features::Weapons, Held::Weapon },
			{ "cheat.grid",   &Features::Inventory, Held::Grid },
			{ "cheat.plant",  &Features::Inventory, Held::Plant },
			{ "cheat.mine",   &Features::Mining, Held::Mine },
			{ "cheat.build",  &Features::BuildFree, Held::BuildParts },
			{ "cheat.build",  &Features::BuildAnywhere, Held::BuildParts },
			{ "cheat.build",  &Features::BuildUnlock, Held::BuildUnlock },
		};
		static_assert(sizeof(kOps) / sizeof(kOps[0]) == kOpCount);

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
			g_stateWanted = false;
			g_stateSentMs = 0;
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
				g_stateWanted = result == 0;   // not ready after all: asked again shortly
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
				// Refused: what earlier replies reported stands.
				if (reply->status == Protocol::Status::Ok)
					ApplyStateLocked(*reply, entry->what.order);
				return;
			}

			if (entry->what.kind == Kind::Cap)
			{
				if (reply->status == Protocol::Status::Ok)
				{
					g_capReply = std::move(*reply);
					g_cap = Cap::Got;
					g_stateWanted = true;
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
				// The server listed this op as on and now can't run it, so what
				// it can do has changed.
				if (reply->reason == Protocol::kNotOnThisBuild && g_cap == Cap::Got
					&& Protocol::HandlerOf(g_capReply, OpName(entry->what.op)).on)
					g_capStale = true;
				return;
			}

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

			for (const auto& expired : g_waiting.Expire(now))
			{
				// An unanswered cheat.state changes nothing and no row waits on it.
				if (expired.what.kind == Kind::State)
					continue;
				if (expired.what.kind == Kind::Cap && g_cap == Cap::Got)
					g_capStale = true;
				else if (expired.what.kind == Kind::Cap)
					FailCapLocked(Protocol::kNoCapAnswer);
				else
					ResultOf(expired.what.op).text = Protocol::kNoAnswer;
			}

			const bool firstCap = g_cap == Cap::NotSent && now >= g_capRetryMs && !ServerLoaderTooOldLocked();
			const bool capAgain = g_cap == Cap::Got && g_capStale && now - g_capSentMs >= kCapAgainMs;
			if (firstCap || capAgain)
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
			if (g_stateWanted && g_cap == Cap::Got && now - g_stateSentMs >= kRetryMs)
			{
				const Protocol::Handler handler = Protocol::HandlerOf(g_capReply, Protocol::kStateOp);
				const bool anyAllowed = std::any_of(std::begin(kOps), std::end(kOps),
					[](const OpInfo& op) { return op.held && op.feature->On(); });
				if (!handler.version || !handler.on || !anyAllowed)
					g_stateWanted = false;   // an older server: nothing shows as on
				else if (!g_waiting.Full())
				{
					const What what{ Op::Give, Kind::State, ++g_order };
					const uint32_t id = g_waiting.Add(what, now);
					if (auto message = Protocol::BuildRequest(id, Protocol::kStateOp, {}))
					{
						outgoing.push_back({ id, what, std::move(*message) });
						g_stateWanted = false;
						g_stateSentMs = now;
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
				const What what{ queued.op, Kind::Click, ++g_order };
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
			const Protocol::Handler handler = Protocol::HandlerOf(g_capReply, OpName(op));
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
		const Protocol::Handler handler = Protocol::HandlerOf(g_capReply, OpName(op));
		return g_cap == Cap::Got && handler.version && !handler.on ? Route::Refused : Route::Local;
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

	void Say(IModLoaderImGui* imgui, const std::string& text)
	{
		imgui->TextWrapped(text.c_str());   // the loader prints it through "%s"
	}
}
