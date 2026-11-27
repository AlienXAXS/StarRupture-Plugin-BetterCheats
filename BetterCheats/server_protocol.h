#pragma once

// The text protocol BetterCheats speaks to ServerUtility's server part, and the
// bookkeeping of requests still waiting for a reply. No game or loader types, so
// it builds and is checked without the game. Not thread safe: the owner locks.
// Everything received is untrusted text.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace BetterCheats::ServerProtocol
{
	inline constexpr char        kSeparator    = '\x1F';
	inline constexpr size_t      kMaxMessage   = 1024;
	inline constexpr size_t      kMaxReason    = 240;
	inline constexpr size_t      kMaxAssetPath = 256;
	inline constexpr size_t      kMaxWaiting   = 8;
	inline constexpr uint64_t    kTimeoutMs    = 10000;
	// Between two cheat.state requests: for a new reason to ask, and for asking
	// again after one that got no answer.
	inline constexpr uint64_t    kStateSpacingMs = 1000;
	inline constexpr uint64_t    kStateRetryMs   = 10000;
	inline constexpr uint32_t    kMaxId        = 2147483647u;
	inline constexpr const char* kTag          = "su.cheat";
	inline constexpr const char* kTarget       = "ServerUtility";
	inline constexpr const char* kStateOp      = "cheat.state";

	inline constexpr const char* kNeedsClientLoader = "Needs ModLoader 1.28.0 or newer.";
	inline constexpr const char* kNoServerUtility   = "This server does not run ServerUtility.";
	inline constexpr const char* kNeedsServerLoader = "This server needs ModLoader 1.28.0 or newer.";
	// The first ServerUtility release that answers these requests. An older one
	// stays silent.
#define BC_MIN_SERVERUTILITY "0.20.0"
	inline constexpr const char* kNoCapAnswer       = "This server needs ServerUtility " BC_MIN_SERVERUTILITY " or newer.";
	inline constexpr const char* kNotOnThisBuild    = "Not available on this server's game build.";
	inline constexpr const char* kNoAnswer          = "No answer from the server.";
	inline constexpr const char* kTooManyWaiting    = "Still waiting for the server. Try again in a moment.";
	inline constexpr const char* kNoReason          = "The server gave no reason.";
	// The server's own words for a request it turned away for coming too soon.
	inline constexpr const char* kRateLimited       = "Too many requests. Wait a moment.";

	using Fields = std::vector<std::pair<std::string, std::string>>;

	// The text for a send that did not queue. `result` is the loader's: 0 not
	// ready, -1 the server does not list the target, -2 bad arguments, -3 called
	// on the authority. Null for 1, which means queued.
	inline const char* SendFailureText(int result)
	{
		switch (result)
		{
		case 1:  return nullptr;
		case 0:  return "Not connected to the server yet. Try again in a moment.";
		case -1: return kNoServerUtility;
		case -3: return "Only a player who joined a server can send this.";
		default: return "Could not send the request.";
		}
	}

	// No field may hold the separator or any other control byte.
	inline bool IsCleanField(std::string_view text)
	{
		return std::all_of(text.begin(), text.end(),
			[](char c) { return static_cast<unsigned char>(c) >= 0x20; });
	}

	// Empty when a field is not clean or the whole message is over the limit.
	inline std::optional<std::string> BuildRequest(uint32_t id, std::string_view op, const Fields& fields)
	{
		if (id == 0 || id > kMaxId || op.empty() || !IsCleanField(op))
			return std::nullopt;

		std::string out = "req";
		out += kSeparator;
		out += '1';
		out += kSeparator;
		out += std::to_string(id);
		out += kSeparator;
		out += op;
		for (const auto& [key, value] : fields)
		{
			if (key.empty() || key.find('=') != std::string::npos || !IsCleanField(key) || !IsCleanField(value))
				return std::nullopt;
			out += kSeparator;
			out += key;
			out += '=';
			out += value;
		}
		if (out.size() > kMaxMessage)
			return std::nullopt;
		return out;
	}

	enum class Status { Ok, Refused, Error };

	struct Reply
	{
		uint32_t    id     = 0;
		Status      status = Status::Refused;
		std::string reason;
		Fields      fields;

		const std::string* Find(std::string_view key) const
		{
			for (const auto& field : fields)
				if (field.first == key)
					return &field.second;
			return nullptr;
		}
	};

	// Decimal digits only, no sign or space, at most `max`.
	inline std::optional<uint32_t> ParseNumber(std::string_view text, uint32_t max)
	{
		if (text.empty() || text.size() > 10)
			return std::nullopt;
		uint64_t value = 0;
		for (char c : text)
		{
			if (c < '0' || c > '9')
				return std::nullopt;
			value = value * 10 + static_cast<uint64_t>(c - '0');
		}
		if (value > max)
			return std::nullopt;
		return static_cast<uint32_t>(value);
	}

	// Cut to `max` bytes without splitting a UTF-8 sequence.
	inline std::string Clip(std::string_view text, size_t max)
	{
		if (text.size() <= max)
			return std::string(text);
		size_t end = max;
		while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
			--end;
		return std::string(text.substr(0, end));
	}

	// Empty for anything that is not a well formed "res" of protocol 1.
	inline std::optional<Reply> ParseReply(const uint8_t* data, size_t size)
	{
		if (!data || size == 0 || size > kMaxMessage)
			return std::nullopt;

		const std::string_view message(reinterpret_cast<const char*>(data), size);
		std::vector<std::string_view> parts;
		size_t start = 0;
		for (;;)
		{
			const size_t end = message.find(kSeparator, start);
			const std::string_view part = message.substr(start, end == std::string_view::npos ? end : end - start);
			if (!IsCleanField(part))
				return std::nullopt;
			parts.push_back(part);
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}

		if (parts.size() < 5 || parts[0] != "res" || parts[1] != "1")
			return std::nullopt;
		const auto id = ParseNumber(parts[2], kMaxId);
		if (!id || *id == 0)
			return std::nullopt;

		Reply reply;
		reply.id     = *id;
		reply.status = parts[3] == "ok" ? Status::Ok : parts[3] == "error" ? Status::Error : Status::Refused;
		reply.reason = Clip(parts[4], kMaxReason);
		if (reply.reason.empty() && reply.status != Status::Ok)
			reply.reason = kNoReason;

		for (size_t i = 5; i < parts.size(); ++i)
		{
			const size_t equals = parts[i].find('=');
			if (equals == std::string_view::npos || equals == 0)
				continue;
			reply.fields.emplace_back(std::string(parts[i].substr(0, equals)),
				std::string(parts[i].substr(equals + 1)));
		}
		return reply;
	}

	struct Handler
	{
		uint32_t version = 0;   // 0: the server did not list the op
		bool     on      = false;
	};

	// One "<op>=<handler version>,<on|off>" field of a cap reply.
	inline Handler HandlerOf(const Reply& cap, std::string_view op)
	{
		const std::string* value = cap.Find(op);
		if (!value)
			return {};
		const size_t comma = value->find(',');
		if (comma == std::string::npos)
			return {};
		const auto version = ParseNumber(std::string_view(*value).substr(0, comma), 1000000u);
		if (!version || *version == 0)
			return {};
		return { *version, std::string_view(*value).substr(comma + 1) == "on" };
	}

	// Whether a comma-separated list holds exactly this name.
	inline bool ListHas(std::string_view list, std::string_view name)
	{
		for (;;)
		{
			const size_t comma = list.find(',');
			if (list.substr(0, comma) == name)
				return true;
			if (comma == std::string_view::npos)
				return false;
			list.remove_prefix(comma + 1);
		}
	}

	// The same for one rule of an op: off when the cap reply lists its build
	// rules without it. A reply with no list has every rule.
	inline Handler HandlerOf(const Reply& cap, std::string_view op, const char* rule)
	{
		Handler handler = HandlerOf(cap, op);
		if (const std::string* rules = cap.Find("build.rules"); rule && rules && !ListHas(*rules, rule))
			handler.on = false;
		return handler;
	}

	// The stats cheat.stat can lock, in wire order.
	inline constexpr const char* kStats[] = {
		"health", "energy", "shield", "hydration", "calories", "oxygen", "toxicity",
		"radiation", "heat", "drain", "corrosion", "infection", "temperature"
	};

	// What a cheat.state reply reports on. Both build rules that can be switched
	// off share the parts list.
	enum class Held { Stack, Wave, Power, Corp, Enemy, Weapon, Grid, Plant, Mine, Stat, BuildParts, BuildUnlock };

	inline bool IsNumberIn(std::string_view text, float min, float max)
	{
		float value = 0.0f;
		const char* const end = text.data() + text.size();
		const auto [stopped, error] = std::from_chars(text.data(), end, value, std::chars_format::fixed);
		return error == std::errc() && stopped == end && value >= min && value <= max;
	}

	inline float NumberOr(std::string_view text, float min, float max, float fallback)
	{
		if (!IsNumberIn(text, min, max))
			return fallback;
		float value = fallback;
		std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
		return value;
	}

	inline bool IsWholeIn(std::string_view text, uint32_t min, uint32_t max)
	{
		const auto value = ParseNumber(text, max);
		return value && *value >= min;
	}

	// One part of a cheat.state reply, under the names the op's own replies use.
	// A key that is missing or unreadable adds nothing, which shows as not on.
	inline Fields HeldFields(const Reply& state, Held what)
	{
		Fields out;
		const auto onOff = [&](const char* key, const char* as)
		{
			const std::string* value = state.Find(key);
			if (value && (*value == "on" || *value == "off"))
				out.emplace_back(as, *value);
		};
		// "off" reads as the value that switches the op off.
		const auto number = [&](const char* key, const char* as, const char* off, auto valid)
		{
			const std::string* value = state.Find(key);
			if (value && *value == "off")
				out.emplace_back(as, off);
			else if (value && valid(std::string_view(*value)))
				out.emplace_back(as, *value);
		};
		// Two values in one field, split on `separator`, both of them or neither.
		const auto pair = [&](const char* key, char separator, const char* first, const char* second, auto firstOk, auto secondOk)
		{
			const std::string* value = state.Find(key);
			if (!value)
				return;
			const std::string_view text = *value;
			const size_t at = text.find(separator);
			if (at == std::string_view::npos || !firstOk(text.substr(0, at)) || !secondOk(text.substr(at + 1)))
				return;
			out.emplace_back(first, std::string(text.substr(0, at)));
			out.emplace_back(second, std::string(text.substr(at + 1)));
		};
		const auto side = [](std::string_view v) { return IsWholeIn(v, 8, 40); };

		switch (what)
		{
		case Held::Stack:
			number("stack", "size", "0", [](std::string_view v) { return IsWholeIn(v, 0, 9999); });
			break;
		case Held::Wave:
			if (const std::string* wave = state.Find("wave"))
			{
				if (*wave == "none")
					out.emplace_back("state", "idle");
				else if (*wave == "running" || *wave == "paused")
					out.emplace_back("state", *wave);
			}
			break;
		case Held::Power:
			number("power", "edited", "0", [](std::string_view v) { return IsWholeIn(v, 0, 1000000); });
			break;
		case Held::Corp:
			if (const std::string* points = state.Find("points"); points && IsWholeIn(*points, 0, 999999999))
				out.emplace_back("points", *points);
			break;
		case Held::Enemy:
			onOff("ai", "ai");
			onOff("onehit", "onehit");
			break;
		case Held::Weapon:
			if (const std::string* weapon = state.Find("weapon"); weapon && *weapon == "off")
				out = { { "damage", "1" }, { "rate", "1" } };
			else
				pair("weapon", ',', "damage", "rate",
					[](std::string_view v) { return IsNumberIn(v, 0.1f, 25.0f); },
					[](std::string_view v) { return IsNumberIn(v, 0.1f, 5.0f); });
			break;
		case Held::Grid:
			pair("grid", 'x', "cols", "rows", side, side);
			break;
		case Held::Plant:
			number("plant", "mult", "1", [](std::string_view v) { return IsWholeIn(v, 1, 5); });
			break;
		case Held::Mine:
			number("mine", "boost", "1", [](std::string_view v) { return IsNumberIn(v, 1.0f, 10.0f); });
			onOff("heat", "heat");
			break;
		case Held::Stat:
			if (const std::string* lock = state.Find("lock"))
			{
				// The known names, once each. A value with none of them that is
				// not "none" says nothing.
				std::string locked;
				for (const char* stat : kStats)
					if (ListHas(*lock, stat))
						locked += (locked.empty() ? "" : ",") + std::string(stat);
				if (!locked.empty() || *lock == "none")
					out.emplace_back("locks", std::move(locked));
			}
			break;
		case Held::BuildParts:
			if (const std::string* parts = state.Find("build"))
				out.emplace_back("parts", *parts == "none" ? std::string() : *parts);
			break;
		case Held::BuildUnlock:
			onOff("unlock", "on");
			break;
		}
		return out;
	}

	// One building of a cheat.power list reply.
	struct PowerValue
	{
		std::string asset;
		float       value  = 0.0f;
		float       normal = 0.0f;   // before any edit
	};

	struct PowerList
	{
		std::vector<PowerValue> entries;
		bool                    more = false;   // the reply could not hold every building
	};

	inline constexpr size_t kMaxPowerAsset = 128;

	// "b<n>=<asset>~<type>~<value>~<normal>" per building, and "more=1". An entry
	// that can't be read whole is left out. The type is not kept.
	inline PowerList ParsePowerList(const Reply& reply)
	{
		PowerList out;
		const auto number = [](std::string_view text) -> std::optional<float>
		{
			float value = 0.0f;
			const char* const end = text.data() + text.size();
			const auto [stopped, error] = std::from_chars(text.data(), end, value);
			if (error != std::errc() || stopped != end || !(value >= 0.0f && value <= 1.0e9f))
				return std::nullopt;
			return value;
		};
		for (const auto& [key, text] : reply.fields)
		{
			if (key == "more")
				out.more = text == "1";
			if (key.size() < 2 || key[0] != 'b' || !ParseNumber(std::string_view(key).substr(1), 1000000u))
				continue;
			if (std::count(text.begin(), text.end(), '~') != 3)
				continue;
			std::string_view part[4];
			std::string_view rest = text;
			for (std::string_view& one : part)
			{
				const size_t at = rest.find('~');
				one = rest.substr(0, at);
				rest.remove_prefix(at == std::string_view::npos ? rest.size() : at + 1);
			}
			if (part[0].empty() || part[0].size() > kMaxPowerAsset)
				continue;
			const auto value  = number(part[2]);
			const auto normal = number(part[3]);
			if (value && normal)
				out.entries.push_back({ std::string(part[0]), *value, *normal });
		}
		return out;
	}

	// When the next cheat.state is due. The owner sends it and says what came of it.
	class StateRequest
	{
	public:
		// Something changed what the server would answer.
		void Want() { m_wanted = true; }

		void Sent(uint32_t id, uint64_t nowMs)
		{
			m_wanted = false;
			m_again  = false;
			m_latest = id;
			m_sentMs = nowMs;
		}

		// `id` was told to wait, or went unanswered. One older than the last sent
		// no longer matters: that one's answer is on its way.
		void Unanswered(uint32_t id)
		{
			if (id == m_latest)
				m_again = true;
		}

		bool Due(uint64_t nowMs) const
		{
			if (m_wanted)
				return nowMs - m_sentMs >= kStateSpacingMs;
			return m_again && nowMs - m_sentMs >= kStateRetryMs;
		}

		// Nothing to ask for now.
		void Drop() { m_wanted = m_again = false; }

		// A new connection.
		void Clear() { *this = {}; }

	private:
		bool     m_wanted = false;
		bool     m_again  = false;
		uint32_t m_latest = 0;
		uint64_t m_sentMs = 0;
	};

	// True only for a readable "major.minor.patch" (a leading v and a suffix are
	// ignored) that is older than 1.28.0. A "-dev" build is named after the last
	// release while it already holds newer work, so it is never judged.
	inline bool IsLoaderTagTooOld(const char* tag)
	{
		// A server that sends no tag predates the tags this needs.
		if (!tag || !*tag)
			return true;
		if (std::string_view(tag).find("-dev") != std::string_view::npos)
			return false;
		if (*tag == 'v')
			++tag;
		unsigned major = 0, minor = 0, patch = 0;
		if (sscanf_s(tag, "%u.%u.%u", &major, &minor, &patch) != 3)
			return false;
		return major < 1 || (major == 1 && minor < 28);
	}

	// The requests still waiting for a reply. `What` is the owner's note of what
	// each one was for.
	template <class What>
	class Waiting
	{
	public:
		struct Entry
		{
			uint32_t id     = 0;
			uint64_t sentMs = 0;
			What     what{};
		};

		// Ids count up from the one after `lastId`.
		explicit Waiting(uint32_t lastId = 0) : m_lastId(lastId) {}

		bool Full() const { return m_entries.size() >= kMaxWaiting; }
		size_t Size() const { return m_entries.size(); }

		// 0 when eight are already waiting.
		uint32_t Add(const What& what, uint64_t nowMs)
		{
			if (Full())
				return 0;
			m_lastId = m_lastId >= kMaxId ? 1 : m_lastId + 1;
			m_entries.push_back({ m_lastId, nowMs, what });
			return m_lastId;
		}

		// The entry a reply answers. Empty for an id that is not waiting.
		std::optional<Entry> Take(uint32_t id)
		{
			for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
			{
				if (it->id != id)
					continue;
				Entry entry = std::move(*it);
				m_entries.erase(it);
				return entry;
			}
			return std::nullopt;
		}

		// Removes and returns every request unanswered for ten seconds.
		std::vector<Entry> Expire(uint64_t nowMs)
		{
			std::vector<Entry> expired;
			for (auto it = m_entries.begin(); it != m_entries.end();)
			{
				if (nowMs - it->sentMs < kTimeoutMs)
				{
					++it;
					continue;
				}
				expired.push_back(std::move(*it));
				it = m_entries.erase(it);
			}
			return expired;
		}

		// A new connection. Ids keep counting, so a reply from the one before
		// never matches a request of this one.
		void Clear() { m_entries.clear(); }

	private:
		std::vector<Entry> m_entries;
		uint32_t           m_lastId;
	};
}
