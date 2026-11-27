// Checks BetterCheats/server_protocol.h without the game.
//   cl /nologo /std:c++20 /EHsc /W4 tests\server_protocol_test.cpp && server_protocol_test.exe

#include "../BetterCheats/server_protocol.h"

#include <mutex>
#include <set>
#include <thread>

using namespace BetterCheats::ServerProtocol;

static int g_failed = 0;
#define CHECK(x) ((x) ? void() : (++g_failed, void(std::printf("FAIL line %d: %s\n", __LINE__, #x))))

// '|' stands for the separator.
static std::string Wire(std::string text)
{
	std::replace(text.begin(), text.end(), '|', kSeparator);
	return text;
}

static std::optional<Reply> Parse(const std::string& text)
{
	const std::string wire = Wire(text);
	return ParseReply(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
}

int main()
{
	CHECK(BuildRequest(42, "cheat.give", { { "item", "DA_IronOre" }, { "count", "500" } })
		== Wire("req|1|42|cheat.give|item=DA_IronOre|count=500"));
	CHECK(BuildRequest(41, "cap", {}) == Wire("req|1|41|cap"));
	CHECK(!BuildRequest(0, "cap", {}));
	CHECK(!BuildRequest(1, "cap", { { "a", Wire("x|y") } }));
	CHECK(!BuildRequest(1, "cap", { { "a", "x\x1Ey" } }));
	CHECK(!BuildRequest(1, "cap", { { "a", std::string(kMaxMessage, 'x') } }));

	const auto given = Parse("res|1|42|ok|Gave 500 Iron Ore.|count=500|item=a=b|new=1|junk");
	CHECK(given && given->id == 42 && given->status == Status::Ok && given->reason == "Gave 500 Iron Ore.");
	CHECK(given && given->Find("item") && *given->Find("item") == "a=b" && !given->Find("junk"));

	const auto odd = Parse("res|1|7|maybe|");
	CHECK(odd && odd->status == Status::Refused && odd->reason == kNoReason);
	CHECK(Parse("res|1|7|error|%s%n")->reason == "%s%n");
	CHECK(Parse("res|1|7|refused|" + std::string(500, 'a'))->reason.size() == kMaxReason);
	CHECK(Clip("ab\xC3\xA9", 3) == "ab");

	for (const char* bad : { "", "res|1|7|ok", "req|1|7|ok|x", "res|2|7|ok|x", "res|1|0|ok|x",
		"res|1|-7|ok|x", "res|1|2147483648|ok|x", "res|1|99999999999999999999|ok|x", "res|1|7|ok|a\nb" })
		CHECK(!Parse(bad));
	CHECK(!Parse("res|1|7|ok|" + std::string(kMaxMessage, 'a')));
	CHECK(!ParseReply(nullptr, 4));

	const auto cap = Parse("res|1|41|ok||su=0.20.0|cheat.give=1,on|cheat.move=1,off|cheat.wave=x,on|cheat.corp=2");
	CHECK(cap && HandlerOf(*cap, "cheat.give").version == 1 && HandlerOf(*cap, "cheat.give").on);
	CHECK(cap && HandlerOf(*cap, "cheat.move").version == 1 && !HandlerOf(*cap, "cheat.move").on);
	CHECK(cap && HandlerOf(*cap, "cheat.wave").version == 0 && HandlerOf(*cap, "cheat.corp").version == 0);
	CHECK(cap && HandlerOf(*cap, "cheat.stat").version == 0);

	CHECK(IsLoaderTagTooOld("v1.27.3") && IsLoaderTagTooOld("1.23.2-gss.4") && !IsLoaderTagTooOld("1.28.0-rc.6"));
	CHECK(!IsLoaderTagTooOld("1.27.0-dev.4") && IsLoaderTagTooOld(""));
	CHECK(!IsLoaderTagTooOld("dev") && IsLoaderTagTooOld(nullptr));
	CHECK(!IsLoaderTagTooOld("v1.28.0") && IsLoaderTagTooOld("1.27.0"));
	CHECK(!IsLoaderTagTooOld("dev"));

	CHECK(!SendFailureText(1));
	for (int result : { 0, -1, -2, -3, 99 })
		CHECK(SendFailureText(result) && *SendFailureText(result));

	Waiting<int> waiting;
	std::set<uint32_t> ids;
	for (int i = 0; i < static_cast<int>(kMaxWaiting); ++i)
		ids.insert(waiting.Add(i, 1000 + i));
	CHECK(ids.size() == kMaxWaiting && !ids.count(0) && waiting.Add(8, 2000) == 0);
	const uint32_t first = *ids.begin();
	CHECK(waiting.Take(first).has_value() && !waiting.Take(first) && !waiting.Take(12345));
	CHECK(waiting.Expire(1000 + kTimeoutMs).empty());
	CHECK(waiting.Expire(1001 + kTimeoutMs).size() == 1 && waiting.Size() == 6);
	waiting.Clear();
	CHECK(waiting.Size() == 0 && !waiting.Take(*ids.rbegin()));
	CHECK(waiting.Add(0, 3000) > *ids.rbegin());

	Waiting<int> seeded(500);
	CHECK(seeded.Add(0, 0) == 501 && !seeded.Take(1));
	Waiting<int> last(kMaxId);
	CHECK(last.Add(0, 0) == 1);

	// Sender, receiver and timer on their own threads under the owner's lock:
	// every request ends exactly once.
	Waiting<int> shared;
	std::mutex lock;
	int sent = 0, answered = 0, timedOut = 0;
	bool done = false;
	std::vector<uint32_t> replies;
	std::thread sender([&] {
		while (sent < 2000)
		{
			std::lock_guard<std::mutex> hold(lock);
			if (const uint32_t id = shared.Add(sent, 0))
			{
				++sent;
				if (id % 3)
					replies.push_back(id);
			}
		}
	});
	std::thread receiver([&] {
		for (bool stop = false; !stop;)
		{
			std::lock_guard<std::mutex> hold(lock);
			for (uint32_t id : replies)
				answered += shared.Take(id).has_value() + shared.Take(id).has_value();
			replies.clear();
			stop = done;
		}
	});
	std::thread timer([&] {
		for (bool stop = false; !stop;)
		{
			std::lock_guard<std::mutex> hold(lock);
			if (shared.Full() && replies.empty())
				timedOut += static_cast<int>(shared.Expire(kTimeoutMs).size());
			stop = done;
		}
	});
	sender.join();
	{
		std::lock_guard<std::mutex> hold(lock);
		done = true;
	}
	receiver.join();
	timer.join();
	timedOut += static_cast<int>(shared.Expire(kTimeoutMs).size());
	CHECK(answered + timedOut == sent && shared.Size() == 0);

	std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
	return g_failed ? 1 : 0;
}
