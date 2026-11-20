// Checks which message the cheat menu shows for each net mode and setting.
//
// A console program: no game, no loader. It is not part of the plugin project.
// Build and run it from a Visual Studio developer prompt, with <sdk> the
// StarRupture-Plugin-SDK checkout:
//
//   cl /nologo /EHsc /std:c++17 /I BetterCheats /I <sdk>\include tests\cheat_gate_tests.cpp
//   cheat_gate_tests.exe
#include "cheat_gate.h"

#include <cstdio>

using BetterCheats::CheatGate;
using BetterCheats::ClassifyCheatGate;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                    \
	do {                                                               \
		++g_checks;                                                    \
		if (!(cond)) {                                                 \
			++g_failed;                                                \
			printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                              \
	} while (0)

static void RunTest(const char* name, void (*fn)())
{
	const int before = g_failed;
	printf("[ RUN  ] %s\n", name);
	fn();
	printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

static void SinglePlayerIsAllowed()
{
	CHECK(ClassifyCheatGate(false, EPluginNetMode::Standalone) == CheatGate::Allowed);
	CHECK(ClassifyCheatGate(true,  EPluginNetMode::Standalone) == CheatGate::Allowed);
}

static void RealMultiplayerIsRefusedWithTheSinglePlayerMessage()
{
	CHECK(ClassifyCheatGate(false, EPluginNetMode::DedicatedServer) == CheatGate::SinglePlayerOnly);
	CHECK(ClassifyCheatGate(false, EPluginNetMode::ListenServer)    == CheatGate::SinglePlayerOnly);
	CHECK(ClassifyCheatGate(false, EPluginNetMode::Client)          == CheatGate::SinglePlayerOnly);
}

static void UnknownWaitsForTheWorld()
{
	CHECK(ClassifyCheatGate(false, EPluginNetMode::Unknown) == CheatGate::WaitingForWorld);
}

static void TheMultiplayerSettingAllowsEveryMode()
{
	CHECK(ClassifyCheatGate(true, EPluginNetMode::DedicatedServer) == CheatGate::Allowed);
	CHECK(ClassifyCheatGate(true, EPluginNetMode::ListenServer)    == CheatGate::Allowed);
	CHECK(ClassifyCheatGate(true, EPluginNetMode::Client)          == CheatGate::Allowed);
	CHECK(ClassifyCheatGate(true, EPluginNetMode::Unknown)         == CheatGate::Allowed);
}

static void WaitingBecomesAllowedOnceTheModeIsKnown()
{
	CHECK(ClassifyCheatGate(false, EPluginNetMode::Unknown)    == CheatGate::WaitingForWorld);
	CHECK(ClassifyCheatGate(false, EPluginNetMode::Standalone) == CheatGate::Allowed);
}

int main()
{
	RunTest("SinglePlayerIsAllowed", SinglePlayerIsAllowed);
	RunTest("RealMultiplayerIsRefusedWithTheSinglePlayerMessage", RealMultiplayerIsRefusedWithTheSinglePlayerMessage);
	RunTest("UnknownWaitsForTheWorld", UnknownWaitsForTheWorld);
	RunTest("TheMultiplayerSettingAllowsEveryMode", TheMultiplayerSettingAllowsEveryMode);
	RunTest("WaitingBecomesAllowedOnceTheModeIsKnown", WaitingBecomesAllowedOnceTheModeIsKnown);

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
