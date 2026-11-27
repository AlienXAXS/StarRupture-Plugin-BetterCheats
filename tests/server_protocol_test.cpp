// Checks BetterCheats/server_protocol.h without the game.
//   cl /nologo /std:c++20 /EHsc /W4 tests\server_protocol_test.cpp && server_protocol_test.exe

#include "../BetterCheats/server_protocol.h"

#include <clocale>
#include <limits>

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

	// ListHas: whole names only.
	CHECK(ListHas("free", "free") && ListHas("free,anywhere,unlock", "anywhere") && ListHas("a,,unlock", "unlock"));
	for (const char* list : { "", ",", "fre", "freer", " free", "free ", "FREE", "anywhere;free" })
		CHECK(!ListHas(list, "free"));

	// HandlerOf with a build rule: only a rule list that leaves it out turns it off.
	const auto ruled = [](const char* fields, const char* rule)
	{
		const auto reply = Parse(std::string("res|1|41|ok|") + fields);
		return reply ? HandlerOf(*reply, "cheat.build", rule) : Handler{ 99, true };
	};
	for (const char* rule : { "free", "anywhere", "unlock" })
	{
		CHECK(ruled("|cheat.build=1,on", rule).on);          // an older server: no list
		CHECK(ruled("|cheat.build=1,on|build.rules=free,anywhere,unlock", rule).on);
		CHECK(!ruled("|cheat.build=1,off|build.rules=free,anywhere,unlock", rule).on);
		CHECK(ruled("|build.rules=free,anywhere,unlock", rule).version == 0);
		for (const char* none : { "|cheat.build=1,on|build.rules=", "|cheat.build=1,on|build.rules=none",
			"|cheat.build=1,on|build.rules=,,,", "|cheat.build=1,on|build.rules=teleport,FREE, unlock,anywhere " })
			CHECK(!ruled(none, rule).on && ruled(none, rule).version == 1);
	}
	const char* const some = "|cheat.build=1,on|build.rules=later,unlock,unlock|later=on";
	CHECK(ruled(some, "unlock").on && !ruled(some, "free").on && !ruled(some, "anywhere").on);
	CHECK(ruled(some, nullptr).on);                              // an op with no rule
	CHECK(!ruled(("|cheat.build=1,on|build.rules=" + std::string(900, 'f')).c_str(), "free").on);

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

	// NumberOr: the whole text, plain notation, inside the range.
	CHECK(NumberOr("2.50", 1.0f, 5.0f, 9.0f) == 2.5f);
	CHECK(NumberOr("1", 1.0f, 5.0f, 9.0f) == 1.0f && NumberOr("5", 1.0f, 5.0f, 9.0f) == 5.0f);
	for (const char* bad : { "", "0.99", "5.01", "2x", " 2", "2 ", "1e0", "nan", "inf", "%s", "0x2", "+2" })
		CHECK(NumberOr(bad, 1.0f, 5.0f, 9.0f) == 9.0f);

	// HeldFields: a cheat.state reply under the names the op replies use.
	const auto held = [](const char* fields, Held what)
	{
		const auto reply = Parse(std::string("res|1|1|ok|State.") + fields);
		return reply ? HeldFields(*reply, what) : Fields{ { "unparsed", "" } };
	};
	const Held all[] = { Held::Stack, Held::Wave, Held::Power, Held::Corp, Held::Enemy, Held::Weapon,
		Held::Grid, Held::Plant, Held::Mine, Held::Stat, Held::BuildParts, Held::BuildUnlock };
	for (Held what : all)
		CHECK(held("", what).empty());                    // missing keys
	for (Held what : all)
		CHECK(held("|later=on", what).empty());           // unknown keys

	CHECK((held("|stack=250", Held::Stack) == Fields{ { "size", "250" } }));
	CHECK((held("|stack=off", Held::Stack) == Fields{ { "size", "0" } }));
	CHECK(held("|stack=10000", Held::Stack).empty() && held("|stack=-1", Held::Stack).empty());
	CHECK((held("|wave=none", Held::Wave) == Fields{ { "state", "idle" } }));
	CHECK((held("|wave=paused", Held::Wave) == Fields{ { "state", "paused" } }));
	CHECK(held("|wave=%n", Held::Wave).empty());
	CHECK((held("|power=3", Held::Power) == Fields{ { "edited", "3" } }));
	CHECK((held("|power=off", Held::Power) == Fields{ { "edited", "0" } }));
	CHECK(held("|power=1000001", Held::Power).empty());
	CHECK((held("|ai=on|onehit=off", Held::Enemy) == Fields{ { "ai", "on" }, { "onehit", "off" } }));
	CHECK((held("|ai=yes|onehit=on", Held::Enemy) == Fields{ { "onehit", "on" } }));
	CHECK((held("|weapon=2.00,1.50", Held::Weapon) == Fields{ { "damage", "2.00" }, { "rate", "1.50" } }));
	CHECK((held("|weapon=off", Held::Weapon) == Fields{ { "damage", "1" }, { "rate", "1" } }));
	for (const char* bad : { "|weapon=2", "|weapon=2,9", "|weapon=26,1", "|weapon=2,1,1", "|weapon=," })
		CHECK(held(bad, Held::Weapon).empty());
	CHECK((held("|grid=10x12", Held::Grid) == Fields{ { "cols", "10" }, { "rows", "12" } }));
	CHECK(held("|grid=7x12", Held::Grid).empty() && held("|grid=10x41", Held::Grid).empty()
		&& held("|grid=off", Held::Grid).empty());
	CHECK((held("|plant=3", Held::Plant) == Fields{ { "mult", "3" } }));
	CHECK((held("|plant=off", Held::Plant) == Fields{ { "mult", "1" } }));
	CHECK(held("|plant=6", Held::Plant).empty());
	CHECK((held("|mine=4.00|heat=on", Held::Mine) == Fields{ { "boost", "4.00" }, { "heat", "on" } }));
	CHECK((held("|mine=off|heat=1", Held::Mine) == Fields{ { "boost", "1" } }));
	std::string every;
	for (const char* stat : kStats)
		every += (every.empty() ? "" : ",") + std::string(stat);
	CHECK(every == "health,energy,shield,hydration,calories,oxygen,toxicity,radiation,heat,drain,"
		"corrosion,infection,temperature");
	CHECK((held(("|lock=" + every).c_str(), Held::Stat) == Fields{ { "locks", every } }));
	CHECK((held("|lock=none", Held::Stat) == Fields{ { "locks", "" } }));
	// Keys that follow the list in the same reply stay out of it.
	CHECK((held("|lock=health,temperature|fly=off|speed=off", Held::Stat) == Fields{ { "locks", "health,temperature" } }));
	CHECK((held("|lock=none|fly=off|speed=off", Held::Stat) == Fields{ { "locks", "" } }));
	CHECK(held("|fly=off|speed=off", Held::Stat).empty());
	CHECK((held("|lock=drain,mana,health,drain,", Held::Stat) == Fields{ { "locks", "health,drain" } }));
	for (const char* bad : { "|lock=", "|lock=,", "|lock=mana", "|lock=on", "|lock=Health", "|lock= health",
		"|lock=healthy,heal", "|lock=health;drain", "|lock=%s%n", "|locks=health", "|lock" })
		CHECK(held(bad, Held::Stat).empty());
	CHECK(held(("|lock=" + std::string(900, 'h')).c_str(), Held::Stat).empty());
	CHECK((held(("|lock=" + std::string(900, ',') + "heat").c_str(), Held::Stat) == Fields{ { "locks", "heat" } }));
	CHECK((held("|build=free,anywhere", Held::BuildParts) == Fields{ { "parts", "free,anywhere" } }));
	CHECK((held("|build=none", Held::BuildParts) == Fields{ { "parts", "" } }));
	CHECK((held("|unlock=on", Held::BuildUnlock) == Fields{ { "on", "on" } }));
	CHECK(held("|unlock=maybe", Held::BuildUnlock).empty());
	CHECK((held("|points=0", Held::Corp) == Fields{ { "points", "0" } }));
	CHECK((held("|points=999999999", Held::Corp) == Fields{ { "points", "999999999" } }));
	for (const char* bad : { "|points=1000000000", "|points=-1", "|points=1.5", "|points=", "|points=off" })
		CHECK(held(bad, Held::Corp).empty());

	// StatValueFields: the request that sets a stat's value.
	CHECK((StatValueFields("health", 80.0f) == Fields{ { "stat", "health" }, { "value", "80.00" } }));
	CHECK((StatValueFields("temperature", -12.5f) == Fields{ { "stat", "temperature" }, { "value", "-12.50" } }));
	CHECK((StatValueFields("heat", 1.0e9f) == Fields{ { "stat", "heat" }, { "value", "1000000000.00" } }));
	for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), 1.0e10f, -1.0e10f })
		CHECK(!StatValueFields("health", bad));
	std::setlocale(LC_ALL, "de-DE");
	CHECK((StatValueFields("health", 2.5f) == Fields{ { "stat", "health" }, { "value", "2.50" } }));
	std::setlocale(LC_ALL, "C");

	// ParsePowerList: a cheat.power list reply.
	const auto listed = [](const std::string& fields)
	{
		const auto reply = Parse("res|1|1|ok|List." + fields);
		CHECK(reply.has_value());
		return reply ? ParsePowerList(*reply) : PowerList{};
	};
	const PowerList two = listed("|count=2|b0=DA_Wind~wind~500~100|b1=DA_Solar~~2.5~2.5");
	CHECK(two.entries.size() == 2 && !two.more);
	CHECK(two.entries.size() == 2 && two.entries[0].asset == "DA_Wind" && two.entries[0].value == 500.0f
		&& two.entries[0].normal == 100.0f && two.entries[1].asset == "DA_Solar" && two.entries[1].value == 2.5f);
	CHECK(listed("|count=0").entries.empty());
	CHECK(listed("|b0=DA_Wind~wind~1~1|more=1").more && !listed("|more=0").more);
	for (const char* bad : { "|b0=DA_Wind~wind~1", "|b0=DA_Wind~wind~1~1~1", "|b0=~wind~1~1",
		"|b0=DA_Wind~wind~x~1", "|b0=DA_Wind~wind~1~-1", "|b0=DA_Wind~wind~inf~1", "|b0=DA_Wind~wind~1e10~1",
		"|b=DA_Wind~wind~1~1", "|bx=DA_Wind~wind~1~1", "|x0=DA_Wind~wind~1~1" })
		CHECK(listed(bad).entries.empty());
	CHECK(listed("|b0=" + std::string(kMaxPowerAsset, 'a') + "~t~1~1").entries.size() == 1);
	CHECK(listed("|b0=" + std::string(kMaxPowerAsset + 1, 'a') + "~t~1~1").entries.empty());
	CHECK(listed("|b0=bad|b1=DA_Wind~t~1~1").entries.size() == 1);

	// StateRequest: when the next cheat.state is due.
	StateRequest state;
	CHECK(!state.Due(100000));
	state.Want();
	CHECK(state.Due(100000));
	state.Sent(1, 100000);
	CHECK(!state.Due(200000));                                  // answered, or still waiting
	state.Want();
	CHECK(!state.Due(100000 + kStateSpacingMs - 1) && state.Due(100000 + kStateSpacingMs));
	state.Sent(2, 101000);
	state.Unanswered(1);                                        // an older one no longer matters
	CHECK(!state.Due(101000 + kStateRetryMs));
	state.Unanswered(2);
	CHECK(!state.Due(101000 + kStateRetryMs - 1) && state.Due(101000 + kStateRetryMs));
	state.Drop();
	CHECK(!state.Due(1000000));
	state.Want();
	state.Clear();
	CHECK(!state.Due(1000000));

	std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
	return g_failed ? 1 : 0;
}
