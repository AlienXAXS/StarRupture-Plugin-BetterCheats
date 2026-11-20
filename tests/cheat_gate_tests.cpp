// Checks which message the cheat menu shows for each net mode.
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
	CHECK(ClassifyCheatGate(EPluginNetMode::Standalone) == CheatGate::Allowed);
}

static void RealMultiplayerIsRefusedWithTheSinglePlayerMessage()
{
	CHECK(ClassifyCheatGate(EPluginNetMode::DedicatedServer) == CheatGate::SinglePlayerOnly);
	CHECK(ClassifyCheatGate(EPluginNetMode::ListenServer)    == CheatGate::SinglePlayerOnly);
	CHECK(ClassifyCheatGate(EPluginNetMode::Client)          == CheatGate::SinglePlayerOnly);
}

static void UnknownWaitsForTheWorld()
{
	CHECK(ClassifyCheatGate(EPluginNetMode::Unknown) == CheatGate::WaitingForWorld);
}

int main()
{
	RunTest("SinglePlayerIsAllowed", SinglePlayerIsAllowed);
	RunTest("RealMultiplayerIsRefusedWithTheSinglePlayerMessage", RealMultiplayerIsRefusedWithTheSinglePlayerMessage);
	RunTest("UnknownWaitsForTheWorld", UnknownWaitsForTheWorld);

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
