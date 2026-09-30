#pragma once

#include "Chimera_classes.hpp"

// Shared by every feature module that needs the local player character.
// Game-thread only -- call this from Tick(), never from RenderImGui(), which
// runs on a different thread where UWorld::GetWorld() and UObject access
// intermittently crash inside the renderer (FD3D12DynamicRHI::
// HandleFailedD3D12Result, no useful callstack).
namespace BetterCheats
{
	inline SDK::ACrCharacterPlayerBase* GetLocalCharacter()
	{
		SDK::UWorld* world = nullptr;
		try { world = SDK::UWorld::GetWorld(); }
		catch (...) { return nullptr; }
		if (!world) return nullptr;

		SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
		if (!pc || !pc->Pawn) return nullptr;

		// The local pawn is only an ACrCharacterPlayerBase once the real character
		// has been possessed. During a level transition or a multiplayer join it can
		// still be some other pawn class, and the attribute-set pointers below then
		// come from past the end of the object.
		SDK::UClass* characterClass = SDK::ACrCharacterPlayerBase::StaticClass();
		if (!characterClass || !pc->Pawn->IsA(characterClass)) return nullptr;

		return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
	}
}
