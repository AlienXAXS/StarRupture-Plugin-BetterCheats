#include "player_movement.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "cheat_menu.h"
#include "keybind_picker.h"
#include "game_context.h"
#include "session_config.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

// No-clip is the classic UE ghost cheat, done entirely through the generated
// SDK — no AOB patterns, no detours:
//
//   * the capsule stops colliding, so nothing blocks the character
//   * the movement mode becomes MOVE_Flying, which skips gravity and the floor
//     checks that would otherwise drop the character out of the world
//   * bCheatFlying tells the movement component this flying is deliberate, so a
//     listen host does not correct it back
//
// Horizontal movement still comes from the game's own input; the game only
// steers on the horizontal plane, so Space / Left Ctrl are fed in here as an
// extra vertical axis.
//
// Everything that touches a UObject runs on the game thread (Tick).
// RenderImGui() runs on the render thread and only ever reads the snapshot.

namespace BetterCheats::Panels::Movement
{
	namespace
	{
		constexpr float kDefaultFlySpeedMultiplier = 3.0f;
		constexpr float kMinFlySpeedMultiplier     = 1.0f;
		constexpr float kMaxFlySpeedMultiplier     = 25.0f;

		// Used only if the movement component ships with no fly speed of its own —
		// UCharacterMovementComponent's own default, in cm/s.
		constexpr float kFallbackMaxFlySpeed = 600.0f;

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);
		constexpr int kColumnFixed = 1 << 4; // ImGuiTableColumnFlags_WidthFixed

		// ---------------------------------------------------------------------
		// Wanted state — written from the render thread and the keybind callback,
		// read on the game thread.
		// ---------------------------------------------------------------------
		std::atomic<bool>  g_noClipWanted{ false };
		std::atomic<float> g_flySpeedMultiplier{ kDefaultFlySpeedMultiplier };

		// ---------------------------------------------------------------------
		// Applied state — game thread only.
		//
		// g_appliedTo is compared, never dereferenced blind: the pawn is replaced
		// on respawn and the old one is destroyed, so state belonging to a
		// character that is no longer the local pawn is dropped rather than
		// restored.
		// ---------------------------------------------------------------------
		bool                         g_active    = false;
		SDK::ACrCharacterPlayerBase* g_appliedTo = nullptr;

		SDK::ECollisionEnabled g_savedCollision      = SDK::ECollisionEnabled::QueryAndPhysics;
		float                  g_savedMaxFlySpeed    = kFallbackMaxFlySpeed;
		float                  g_savedFlyingBraking  = 0.0f;
		bool                   g_savedCheatFlying    = false;

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->Pawn) return nullptr;

			// See player_attributes.cpp's GetLocalCharacter — the pawn isn't a
			// Chimera character until it's actually possessed.
			SDK::UClass* characterClass = SDK::ACrCharacterPlayerBase::StaticClass();
			if (!characterClass || !pc->Pawn->IsA(characterClass)) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}

		// Key state is polled rather than bound: the modloader exposes press and
		// release events, not "is held", and holding Space is exactly what
		// ascending needs. Only listen while this process owns the foreground
		// window, or the character climbs while the player is in another app.
		bool IsGameForeground()
		{
			HWND foreground = GetForegroundWindow();
			if (!foreground) return false;

			DWORD pid = 0;
			GetWindowThreadProcessId(foreground, &pid);
			return pid == GetCurrentProcessId();
		}

		bool IsKeyHeld(int virtualKey)
		{
			return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
		}

		void Enable(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;
			if (!move || !capsule)
				return;

			g_savedCollision     = capsule->GetCollisionEnabled();
			g_savedMaxFlySpeed   = move->MaxFlySpeed > 0.0f ? move->MaxFlySpeed : kFallbackMaxFlySpeed;
			g_savedFlyingBraking = move->BrakingDecelerationFlying;
			g_savedCheatFlying   = move->bCheatFlying != 0;

			capsule->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
			move->bCheatFlying = 1;
			move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			g_active    = true;
			g_appliedTo = character;

			LOG_INFO("Movement: no-clip enabled.");
		}

		void Disable(SDK::ACrCharacterPlayerBase* character)
		{
			g_active    = false;
			g_appliedTo = nullptr;

			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;

			if (capsule)
				capsule->SetCollisionEnabled(g_savedCollision);

			if (move)
			{
				move->MaxFlySpeed               = g_savedMaxFlySpeed;
				move->BrakingDecelerationFlying = g_savedFlyingBraking;
				move->bCheatFlying              = g_savedCheatFlying ? 1 : 0;

				// Falling rather than walking: the character is usually in mid-air
				// when no-clip goes off, and the engine drops it into walking as
				// soon as it finds a floor.
				move->SetMovementMode(SDK::EMovementMode::MOVE_Falling, 0);
			}

			LOG_INFO("Movement: no-clip disabled.");
		}

		void MaintainNoClip(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			if (!move)
				return;

			// Zipline, slide and dash all drive the movement mode themselves, so
			// take it back whenever something else has changed it.
			if (move->MovementMode != SDK::EMovementMode::MOVE_Flying)
				move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			const float maxSpeed = g_savedMaxFlySpeed * g_flySpeedMultiplier.load();
			move->MaxFlySpeed = maxSpeed;

			// Flying braking ships at 0 on most components, which means coasting
			// forever once you let go. Brake hard instead so the character parks
			// where the player stopped steering.
			move->BrakingDecelerationFlying = maxSpeed * 4.0f;

			if (CheatMenu::IsOpen() || !IsGameForeground())
				return;

			float vertical = 0.0f;
			if (IsKeyHeld(VK_SPACE))                                 vertical += 1.0f;
			if (IsKeyHeld(VK_LCONTROL) || IsKeyHeld(VK_RCONTROL))    vertical -= 1.0f;

			if (vertical != 0.0f)
				character->AddMovementInput(SDK::FVector(0.0, 0.0, 1.0), vertical, true);
		}

		// ---------------------------------------------------------------------
		// Snapshot — populated on the game thread (Tick), read on the ImGui
		// render thread. Never touch SDK objects from RenderImGui().
		// ---------------------------------------------------------------------
		struct MovementSnapshot
		{
			bool  characterFound = false;
			bool  active         = false;
			float baseFlySpeed   = kFallbackMaxFlySpeed;
		};

		std::mutex       g_snapshotMutex;
		MovementSnapshot g_snapshot;

		void RefreshSnapshot(SDK::ACrCharacterPlayerBase* character)
		{
			MovementSnapshot snap;
			snap.characterFound = character != nullptr;
			snap.active         = g_active;
			snap.baseFlySpeed   = g_active ? g_savedMaxFlySpeed : kFallbackMaxFlySpeed;

			if (!g_active && character && character->CharacterMovement && character->CharacterMovement->MaxFlySpeed > 0.0f)
				snap.baseFlySpeed = character->CharacterMovement->MaxFlySpeed;

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// The combo we actually registered with. Config::GetNoClipKey() hands back
		// a shared static buffer, so it is read once here rather than from the
		// render thread every frame — and unregistering must use the same string.
		// Written by the rebind control on the render thread, read there and on
		// shutdown, hence the lock.
		std::mutex g_keyMutex;
		char       g_noClipKey[64] = "";

		void ReadNoClipKey(char* out, size_t outSize)
		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(out, outSize, "%s", g_noClipKey);
		}

		void OnNoClipKeyPressed(EModKey /*key*/, EModKeyEvent /*event*/)
		{
			if (!GameContext::IsInChimeraMain() || !GameContext::AreCheatsAllowed())
				return;

			g_noClipWanted.store(!g_noClipWanted.load());
		}

		// Swaps the live registration and persists the new combo. The loader
		// matches an unregistration against the name the bind was created with, so
		// handing it the string we last registered is enough even if it has been
		// rebound from the loader's own config UI in the meantime.
		void ApplyNoClipKey(const char* combo)
		{
			IPluginSelf* self = GetSelf();
			if (!self || !self->hooks || !self->hooks->Input || !combo || !*combo)
				return;

			char previous[64];
			{
				std::lock_guard<std::mutex> lock(g_keyMutex);
				if (strcmp(g_noClipKey, combo) == 0)
					return;

				snprintf(previous, sizeof(previous), "%s", g_noClipKey);
				snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", combo);
			}

			if (previous[0])
				self->hooks->Input->UnregisterKeybindByName(previous, EModKeyEvent::Pressed, &OnNoClipKeyPressed);

			self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
			BetterCheatsConfig::Config::SetNoClipKey(combo);

			LOG_INFO("Movement: no-clip keybind is now %s.", combo);
		}
	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		if (!self || !self->hooks || !self->hooks->Input)
			return;

		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", BetterCheatsConfig::Config::GetNoClipKey());
		}

		char combo[64];
		ReadNoClipKey(combo, sizeof(combo));

		self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
	}

	void Shutdown()
	{
		IPluginSelf* self = GetSelf();
		if (self && self->hooks && self->hooks->Input)
		{
			char combo[64];
			ReadNoClipKey(combo, sizeof(combo));

			if (combo[0])
				self->hooks->Input->UnregisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
		}

		// Leaving a character permanently non-colliding would outlive the plugin,
		// so put it back — but only if the pawn we changed is still the live one.
		try
		{
			if (g_active)
			{
				SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
				if (character && character == g_appliedTo)
					Disable(character);
			}
		}
		catch (...) {}

		g_active    = false;
		g_appliedTo = nullptr;
		g_noClipWanted.store(false);
	}

	void Tick(float /*deltaSeconds*/)
	{
		try
		{
			SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();

			// Respawn or a world change replaces the pawn; the one we altered is
			// gone, so forget it rather than writing through a freed pointer.
			if (g_appliedTo && g_appliedTo != character)
			{
				g_active    = false;
				g_appliedTo = nullptr;
			}

			if (!character)
			{
				RefreshSnapshot(nullptr);
				return;
			}

			const bool wanted = g_noClipWanted.load();
			if (wanted && !g_active)
				Enable(character);
			else if (!wanted && g_active)
				Disable(character);

			if (g_active)
				MaintainNoClip(character);

			RefreshSnapshot(character);
		}
		catch (...)
		{
			LOG_ERROR("Movement: exception while driving no-clip.");
		}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		float speed = SessionConfig::Get("playerMovement.flySpeedMultiplier", kDefaultFlySpeedMultiplier);
		if (speed < kMinFlySpeedMultiplier) speed = kMinFlySpeedMultiplier;
		if (speed > kMaxFlySpeedMultiplier) speed = kMaxFlySpeedMultiplier;

		g_flySpeedMultiplier.store(speed);
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		MovementSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		imgui->SeparatorText("No Clip");

		if (!snap.characterFound)
		{
			imgui->TextDisabled("Player character not found.");
			return;
		}

		char buffer[160];
		char currentKey[64];
		ReadNoClipKey(currentKey, sizeof(currentKey));

		bool  noClip = g_noClipWanted.load();
		float speed  = g_flySpeedMultiplier.load();

		if (imgui->BeginTable("##noclip_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option", 0,            0.85f);
			imgui->TableSetupColumn("Value",  kColumnFixed, 190.0f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			snprintf(buffer, sizeof(buffer), "No Clip  (%s)", currentKey[0] ? currentKey : "unbound");
			imgui->Text(buffer);
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##noclip", &noClip))
				g_noClipWanted.store(noClip);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Fly Speed");
			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->SliderFloat("##fly_speed", &speed, kMinFlySpeedMultiplier, kMaxFlySpeedMultiplier, "%.1fx"))
			{
				g_flySpeedMultiplier.store(speed);
				SessionConfig::Set("playerMovement.flySpeedMultiplier", speed);
			}

			imgui->EndTable();
		}

		imgui->Spacing();

		snprintf(buffer, sizeof(buffer), "Top speed: %.0f cm/s  (base %.0f)", snap.baseFlySpeed * speed, snap.baseFlySpeed);
		imgui->TextDisabled(buffer);

		imgui->Spacing();
		imgui->TextWrapped("Look and move as usual to fly horizontally; hold Space to rise and Left Ctrl to "
			"descend. Vertical input is ignored while this menu is open.");

		imgui->Spacing();
		imgui->TextColored(1.0f, 0.3f, 0.3f, 1.0f,
			"Note: Turning No Clip off drops the character. Come back above ground "
			"first, or you will fall through the world until the game respawns you.");

		imgui->Spacing();
		imgui->SeparatorText("Keybind");

		imgui->AlignTextToFramePadding();
		imgui->Text("Toggle No Clip");
		imgui->SameLine(0.0f, 8.0f);

		char picked[64];
		if (Keybind::RenderPicker(imgui, "noclip_key", currentKey, picked, sizeof(picked)))
			ApplyNoClipKey(picked);

		imgui->SameLine(0.0f, 8.0f);
		if (imgui->Button("Reset"))
		{
			Keybind::CancelCapture();
			ApplyNoClipKey(BetterCheatsConfig::kDefaultNoClipKey);
		}

		imgui->Spacing();
		imgui->TextDisabled("Modifiers count: hold Ctrl, Shift or Alt while pressing the key to bind a combo.");
	}
}
