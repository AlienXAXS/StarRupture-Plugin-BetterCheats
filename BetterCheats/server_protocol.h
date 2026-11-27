#pragma once

// The text protocol BetterCheats speaks to ServerUtility's server part, and the
// bookkeeping of requests still waiting for a reply. No game or loader types, so
// it builds and is checked without the game. Not thread safe: the owner locks.
// Everything received is untrusted text.

#include <algorithm>
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
	inline constexpr uint32_t    kMaxId        = 2147483647u;
	inline constexpr const char* kTag          = "su.cheat";
	inline constexpr const char* kTarget       = "ServerUtility";

	inline constexpr const char* kNeedsClientLoader = "Needs ModLoader 1.28.0 or newer.";
	inline constexpr const char* kNoServerUtility   = "This server does not run ServerUtility.";
	inline constexpr const char* kNeedsServerLoader = "This server needs ModLoader 1.28.0 or newer.";
	inline constexpr const char* kNeedsServerUtility = "This server needs ServerUtility 0.19.0 or newer.";
	inline constexpr const char* kNotOnThisBuild    = "Not available on this server's game build.";
	inline constexpr const char* kNoAnswer          = "No answer from the server.";
	inline constexpr const char* kTooManyWaiting    = "Still waiting for the server. Try again in a moment.";
	inline constexpr const char* kNoReason          = "The server gave no reason.";

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

	// For a UI call that takes a printf format: received text must never be one.
	inline std::string EscapeFormat(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (char c : text)
		{
			out += c;
			if (c == '%')
				out += '%';
		}
		return out;
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

	// True only for a readable "major.minor.patch" (a leading v and a suffix are
	// ignored) that is older than 1.28.0.
	inline bool IsLoaderTagTooOld(const char* tag)
	{
		if (!tag)
			return false;
		if (*tag == 'v')
			++tag;
		unsigned major = 0, minor = 0, patch = 0;
		if (sscanf_s(tag, "%u.%u.%u", &major, &minor, &patch) != 3)
			return false;
		return major < 1 || (major == 1 && minor < 28);
	}

	// A cap request nobody answered. An old server loader and an old
	// ServerUtility look the same, so the build tag picks the text.
	inline const char* CapTimeoutText(const char* serverTag)
	{
		unsigned major = 0, minor = 0, patch = 0;
		const char* digits = serverTag && *serverTag == 'v' ? serverTag + 1 : serverTag;
		const bool readable = digits && sscanf_s(digits, "%u.%u.%u", &major, &minor, &patch) == 3;
		return readable && !IsLoaderTagTooOld(serverTag) ? kNeedsServerUtility : kNeedsServerLoader;
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
		uint32_t           m_lastId = 0;
	};
}
