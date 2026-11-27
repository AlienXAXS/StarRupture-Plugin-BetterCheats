#include "server_link.h"

#include "game_context.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace BetterCheats::ServerLink
{
	namespace Protocol = ServerProtocol;

	namespace
	{
		constexpr uint64_t    kRetryMs   = 1000;
		constexpr size_t      kOpCount   = static_cast<size_t>(Op::Count);
		constexpr const char* kChecking  = "Asking the server what it can do...";
		constexpr const char* kSending   = "Waiting for the server...";
		constexpr const char* kDone      = "Done.";

		struct What
		{
			Op   op  = Op::Give;
			bool cap = false;
		};

		struct Queued
		{
			Op               op;
			Protocol::Fields fields;
		};

		enum class Cap { NotSent, Waiting, Got, Failed };

		std::mutex               g_mutex;
		Protocol::Waiting<What>  g_waiting;
		std::vector<Queued>      g_queued;
		std::array<Result, kOpCount> g_results;
		Cap                      g_cap = Cap::NotSent;
		Protocol::Reply          g_capReply;
		std::string              g_capFailure;
		uint64_t                 g_capRetryMs     = 0;
		uint64_t                 g_notReadySince  = 0;
		bool                     g_wasReady       = false;
		bool                     g_neverReady     = false;
		// From the ready callback, which fires once per connection, so a world
		// ending must not forget it.
		std::string              g_serverTag;

		IPluginSelf*      g_self = nullptr;
		std::atomic<bool> g_canSend{ false };

		const char* OpName(Op op)
		{
			switch (op)
			{
			case Op::Give:  return "cheat.give";
			case Op::Wave:  return "cheat.wave";
			case Op::Stack: return "cheat.stack";
			default:        return "cheat.corp";
			}
		}

		const PluginMap::Feature& FeatureOf(Op op)
		{
			switch (op)
			{
			case Op::Give:  return Features::Items;
			case Op::Wave:  return Features::Wave;
			case Op::Stack: return Features::StackSize;
			default:        return Features::Corporations;
			}
		}

		Result& ResultOf(Op op) { return g_results[static_cast<size_t>(op)]; }

		IPluginNetworkChannel* Network()
		{
			return g_self && g_self->hooks ? g_self->hooks->Network : nullptr;
		}

		void ResetLocked()
		{
			g_waiting.Clear();
			g_queued.clear();
			g_results = {};
			g_cap = Cap::NotSent;
			g_capReply = {};
			g_capFailure.clear();
			g_capRetryMs    = 0;
			g_notReadySince = 0;
			g_wasReady      = false;
			g_neverReady    = false;
		}

		void FailCapLocked(std::string why)
		{
			g_cap = Cap::Failed;
			g_capFailure = std::move(why);
			g_queued.clear();
		}

		// A request that left no entry waiting: the send did not queue.
		void SendFailedLocked(const What& what, int result)
		{
			const char* text = Protocol::SendFailureText(result);
			if (result == -1)
				FailCapLocked(text);
			else if (what.cap && result == 0)
				g_cap = Cap::NotSent;   // not ready after all: asked again shortly
			else if (what.cap)
				FailCapLocked(text);

			if (!what.cap)
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

			if (entry->what.cap)
			{
				if (reply->status == Protocol::Status::Ok)
				{
					g_capReply = std::move(*reply);
					g_cap = Cap::Got;
				}
				else
				{
					FailCapLocked(reply->reason);
				}
				return;
			}

			Result& result = ResultOf(entry->what.op);
			result.text = reply->reason.empty() ? kDone : reply->reason;
			if (reply->status != Protocol::Status::Ok)
				return;

			if (const std::string* state = reply->Find("state"))
				if (*state == "idle" || *state == "running" || *state == "paused")
					result.waveState = *state;

			const char* key = entry->what.op == Op::Corp ? "points" : entry->what.op == Op::Stack ? "size" : nullptr;
			if (const std::string* number = key ? reply->Find(key) : nullptr)
				if (const auto value = Protocol::ParseNumber(*number, 999999999u))
					result.value = static_cast<int>(*value);
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
		for (const Queued& queued : g_queued)
			ResultOf(queued.op).text.clear();
		g_queued.clear();
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
				{
					ResetLocked();
					g_serverTag.clear();
				}
				if (g_notReadySince == 0)
					g_notReadySince = now;
				g_neverReady = now - g_notReadySince >= Protocol::kTimeoutMs;
				return;
			}
			g_wasReady   = true;
			g_neverReady = false;

			for (const auto& expired : g_waiting.Expire(now))
			{
				if (expired.what.cap)
					FailCapLocked(Protocol::kNoCapAnswer);
				else
					ResultOf(expired.what.op).text = Protocol::kNoAnswer;
			}

			if (g_cap == Cap::NotSent && now >= g_capRetryMs && !Protocol::IsLoaderTagTooOld(g_serverTag.c_str()))
			{
				const What what{ Op::Give, true };
				if (const uint32_t id = g_waiting.Add(what, now))
				{
					if (auto message = Protocol::BuildRequest(id, "cap", {}))
					{
						g_cap = Cap::Waiting;
						g_capRetryMs = now + kRetryMs;
						outgoing.push_back({ id, what, std::move(*message) });
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
				const What what{ queued.op, false };
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

	std::string Blocker(Op op)
	{
		if (!g_canSend)
			return Protocol::kNeedsClientLoader;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_wasReady && Protocol::IsLoaderTagTooOld(g_serverTag.c_str()))
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

	void Queue(Op op, Protocol::Fields fields)
	{
		if (!FeatureOf(op).On() || !Blocker(op).empty())
			return;

		std::lock_guard<std::mutex> lock(g_mutex);
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
		imgui->TextWrapped(Protocol::EscapeFormat(text).c_str());
	}
}
