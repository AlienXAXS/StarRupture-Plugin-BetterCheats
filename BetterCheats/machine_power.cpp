#include "machine_power.h"
#include "aob_patterns.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#include "Chimera_classes.hpp"
#include "Chimera_parameters.hpp"
#include "Engine_classes.hpp"
#include "MassSpawner_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace BetterCheats::Panels::Power
{
	namespace
	{
		struct PowerEntry
		{
			std::string                      packageName;
			std::string                      assetName;
			std::string                      name;
			SDK::ECrMassElectricityAgentType type;
			float                             value;
		};

		// g_entries is only ever touched from RenderImGui() (the render thread).
		// Scans run on the game thread and publish results through g_pendingEntries
		// for AdoptPendingIfReady() to pick up.
		std::vector<PowerEntry> g_entries;
		bool                    g_loaded = false;

		std::mutex              g_pendingMutex;
		std::vector<PowerEntry> g_pendingEntries;
		bool                    g_pendingReady = false;
		std::atomic<bool>       g_refreshInFlight{ false };

		const char* AgentTypeName(SDK::ECrMassElectricityAgentType type)
		{
			switch (type)
			{
			case SDK::ECrMassElectricityAgentType::Producer:  return "Producer";
			case SDK::ECrMassElectricityAgentType::Consumer:  return "Consumer";
			case SDK::ECrMassElectricityAgentType::Conductor: return "Conductor";
			default:                                          return "None";
			}
		}

		// -------------------------------------------------------------------------
		// Raw CoreUObject object/package lookup functions — same force-load
		// approach as the Item Spawner (player_items.cpp): the asset registry only
		// gives us asset paths, so building data assets and their (soft-referenced)
		// Mass entity config assets need to be force-loaded to read their values,
		// regardless of whether the current world has spawned anything that
		// references them yet.
		// -------------------------------------------------------------------------
		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		StaticFindObjectByNameFn g_staticFindObjectByName = nullptr;
		FindPackageFn            g_findPackage             = nullptr;
		PackageFullyLoadFn       g_packageFullyLoad        = nullptr;
		LoadPackageFn            g_loadPackage             = nullptr;
		bool                     g_engineFnsResolveTried   = false;

		bool ResolveEngineLookupFunctions()
		{
			if (g_engineFnsResolveTried)
				return g_staticFindObjectByName && g_findPackage && g_packageFullyLoad && g_loadPackage;

			g_engineFnsResolveTried = true;

			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
			{
				LOG_WARN("Power: engine events interface unavailable - cannot resolve object/package lookup functions.");
				return false;
			}

			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				g_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				g_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				g_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				g_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!g_staticFindObjectByName || !g_findPackage || !g_packageFullyLoad || !g_loadPackage)
			{
				LOG_WARN("Power: failed to resolve object/package lookup functions "
					"(StaticFindObject=%p FindPackage=%p FullyLoad=%p LoadPackage=%p) - building list will be incomplete.",
					reinterpret_cast<void*>(g_staticFindObjectByName), reinterpret_cast<void*>(g_findPackage),
					reinterpret_cast<void*>(g_packageFullyLoad), reinterpret_cast<void*>(g_loadPackage));
				return false;
			}

			return true;
		}

		std::wstring ToWide(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());
		}

		SDK::UPackage* ForceLoadPackage(const std::wstring& packageNameW)
		{
			SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
			if (package)
				g_packageFullyLoad(package);
			else
				package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);
			return package;
		}

		// -------------------------------------------------------------------------
		// Asset registry plumbing — mirrors player_items.cpp's CallGetAssetRegistry
		// / CallGetAssetsByClass helpers.
		// -------------------------------------------------------------------------
		SDK::FTopLevelAssetPath MakeBuildingDataClassPath()
		{
			SDK::FString packagePath(L"/Script/Chimera");
			SDK::FString className(L"CrBuildingData");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		SDK::FTopLevelAssetPath MakeMassEntityConfigClassPath()
		{
			SDK::FString packagePath(L"/Script/MassSpawner");
			SDK::FString className(L"MassEntityConfigAsset");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		SDK::UObject* CallGetAssetRegistry()
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo) return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!func)
			{
				LOG_WARN("Power: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		bool CallGetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject) return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LOG_WARN("Power: could not resolve IAssetRegistry::GetAssetsByClass.");
				return false;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName    = classPath;
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			outAssets = std::move(parms.OutAssetData);
			return parms.ReturnValue;
		}

		// -------------------------------------------------------------------------
		// Walks Config.Traits, then Config.Parent, looking for a CrElectricityTrait
		// instance. Building configs can inherit traits from a parent config asset.
		// -------------------------------------------------------------------------
		SDK::UCrElectricityTrait* FindElectricityTrait(SDK::UMassEntityConfigAsset* configAsset, int depth = 0)
		{
			if (!configAsset || depth > 8) return nullptr;

			for (int i = 0; i < configAsset->Config.Traits.Num(); ++i)
			{
				SDK::UMassEntityTraitBase* trait = configAsset->Config.Traits[i];
				if (trait && trait->Class && trait->Class->GetName() == "CrElectricityTrait")
					return static_cast<SDK::UCrElectricityTrait*>(trait);
			}

			return FindElectricityTrait(configAsset->Config.Parent, depth + 1);
		}

		// -------------------------------------------------------------------------
		// Re-resolves a MassEntityConfigAsset by package/asset name. Pointers cached
		// from a previous scan can go stale (the engine reloads/reconstructs these
		// assets, e.g. when "unlock all buildings" rescans available buildings),
		// so edits must look the asset up fresh immediately before mutating it or
		// calling native template functions on it.
		// -------------------------------------------------------------------------
		SDK::UMassEntityConfigAsset* ResolveConfigAsset(const std::string& packageName, const std::string& assetName)
		{
			if (packageName.empty() || !ResolveEngineLookupFunctions())
				return nullptr;

			SDK::UPackage* package = ForceLoadPackage(ToWide(packageName));
			if (!package)
				return nullptr;

			SDK::UObject* obj = g_staticFindObjectByName(SDK::UMassEntityConfigAsset::StaticClass(), package, ToWide(assetName).c_str(), false);
			return obj ? static_cast<SDK::UMassEntityConfigAsset*>(obj) : nullptr;
		}

		// -------------------------------------------------------------------------
		// Turns a Mass entity config asset name like "DA_WindPowerGeneratorTier2"
		// into a readable fallback display name ("Wind Power Generator Tier 2") for
		// configs that aren't referenced by any CrBuildingData (so have no
		// localized BuildingName to fall back to).
		// -------------------------------------------------------------------------
		std::string PrettifyAssetName(const std::string& assetName)
		{
			std::string s = assetName;
			for (const char* prefix : { "DA_", "BD_" })
			{
				const size_t len = std::strlen(prefix);
				if (s.size() > len && s.compare(0, len, prefix) == 0)
				{
					s = s.substr(len);
					break;
				}
			}

			std::string out;
			for (size_t i = 0; i < s.size(); ++i)
			{
				const char c = s[i];
				if (i > 0)
				{
					const char prev = s[i - 1];
					const bool upperAfterLower = std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(prev));
					const bool digitAfterAlpha = std::isdigit(static_cast<unsigned char>(c)) && std::isalpha(static_cast<unsigned char>(prev));
					const bool alphaAfterDigit = std::isalpha(static_cast<unsigned char>(c)) && std::isdigit(static_cast<unsigned char>(prev));
					if (upperAfterLower || digitAfterAlpha || alphaAfterDigit)
						out += ' ';
				}
				out += c;
			}
			return out;
		}

		// -------------------------------------------------------------------------
		// Builds a map of Mass entity config package path -> localized building
		// name by enumerating CrBuildingData assets via the asset registry and
		// reading their (soft-referenced, not necessarily loaded) EntityConfig
		// path. Used purely for display names; the electricity scan below walks
		// MassEntityConfigAsset assets directly so it doesn't depend on this
		// resolving for every building.
		// -------------------------------------------------------------------------
		void BuildConfigNameMap(SDK::IAssetRegistry* registry, std::unordered_map<std::string, std::string>& outNames)
		{
			SDK::TArray<SDK::FAssetData> assetData;
			if (!CallGetAssetsByClass(registry, MakeBuildingDataClassPath(), assetData))
			{
				LOG_WARN("Power: IAssetRegistry::GetAssetsByClass(CrBuildingData) failed.");
				return;
			}

			const int rawCount = assetData.Num();
			LOG_INFO("Power: asset registry reports %d CrBuildingData asset(s).", rawCount);

			for (int i = 0; i < rawCount; ++i)
			{
				const std::string assetName   = assetData[i].AssetName.ToString();
				const std::string packageName = assetData[i].PackageName.GetRawString();
				if (packageName.empty()) continue;

				SDK::UPackage* package = ForceLoadPackage(ToWide(packageName));
				if (!package) continue;

				SDK::UObject* obj = g_staticFindObjectByName(SDK::UCrBuildingData::StaticClass(), package, ToWide(assetName).c_str(), false);
				if (!obj) continue;

				auto* building = static_cast<SDK::UCrBuildingData*>(obj);

				const SDK::FName& cfgPackageName = building->EntityType.EntityConfig.ObjectID.AssetPath.PackageName;
				if (cfgPackageName.IsNone()) continue;

				std::string name = SDK::UKismetTextLibrary::Conv_TextToString(building->BuildingName).ToString();
				if (name.empty() || name == "<MISSING STRING TABLE ENTRY>" || name == "None")
					continue;

				outNames[cfgPackageName.ToString()] = std::move(name);
			}
		}

		// -------------------------------------------------------------------------
		// Enumerates every MassEntityConfigAsset via the asset registry (mirrors
		// UCrUW_CheatItemsTab-style force-loading) and records every config whose
		// CrElectricityTrait has a Type other than None, so its
		// Parameters.ElectricityValue can be edited. This is the source of truth
		// for both producers and consumers — CrBuildingData is only used to look
		// up a friendlier display name.
		//
		// Runs on the game thread (posted via RequestRescan) and publishes its
		// result through g_pendingEntries for RenderImGui() to adopt.
		// -------------------------------------------------------------------------
		void ScanEntriesOnGameThread(void* /*context*/)
		{
			std::vector<PowerEntry> entries;

			try
			{
				if (ResolveEngineLookupFunctions())
				{
					SDK::UObject* registryObject = CallGetAssetRegistry();
					SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;
					if (!registry)
					{
						LOG_WARN("Power: could not resolve the asset registry.");
					}
					else
					{
						std::unordered_map<std::string, std::string> configNames;
						BuildConfigNameMap(registry, configNames);

						SDK::TArray<SDK::FAssetData> assetData;
						if (!CallGetAssetsByClass(registry, MakeMassEntityConfigClassPath(), assetData))
						{
							LOG_WARN("Power: IAssetRegistry::GetAssetsByClass(MassEntityConfigAsset) failed.");
						}

						const int rawCount = assetData.Num();
						LOG_INFO("Power: asset registry reports %d MassEntityConfigAsset asset(s).", rawCount);

						for (int i = 0; i < rawCount; ++i)
						{
							const std::string assetName   = assetData[i].AssetName.ToString();
							const std::string packageName = assetData[i].PackageName.GetRawString();
							if (packageName.empty()) continue;

							SDK::UPackage* package = ForceLoadPackage(ToWide(packageName));
							if (!package)
							{
								LOG_DEBUG("Power:   [%s] could not find or load package '%s'.", assetName.c_str(), packageName.c_str());
								continue;
							}

							SDK::UObject* obj = g_staticFindObjectByName(SDK::UMassEntityConfigAsset::StaticClass(), package, ToWide(assetName).c_str(), false);
							if (!obj)
							{
								LOG_DEBUG("Power:   [%s] StaticFindObject could not find MassEntityConfigAsset in package '%s'.", assetName.c_str(), packageName.c_str());
								continue;
							}

							auto* configAsset = static_cast<SDK::UMassEntityConfigAsset*>(obj);

							SDK::UCrElectricityTrait* trait = FindElectricityTrait(configAsset);
							if (!trait || trait->Parameters.Type == SDK::ECrMassElectricityAgentType::None)
								continue;

							PowerEntry entry{};
							entry.packageName = packageName;
							entry.assetName   = assetName;

							auto it = configNames.find(packageName);
							entry.name = (it != configNames.end()) ? it->second : PrettifyAssetName(assetName);

							entry.type  = trait->Parameters.Type;
							entry.value = trait->Parameters.ElectricityValue;

							// Reloading a save reloads this asset from disk, discarding any
							// in-memory edit from a previous session — re-apply any
							// persisted override now so newly-spawned entities pick it up.
							if (BetterCheatsConfig::Config::HasPowerOverride(entry.assetName))
							{
								const float overrideValue = BetterCheatsConfig::Config::GetPowerOverride(entry.assetName, entry.value);
								if (overrideValue != trait->Parameters.ElectricityValue)
								{
									LOG_INFO("Power:   [%s] re-applying persisted override %.2f -> %.2f.",
										entry.name.c_str(), trait->Parameters.ElectricityValue, overrideValue);
									trait->Parameters.ElectricityValue = overrideValue;
								}
								entry.value = overrideValue;
							}

							LOG_INFO("Power:   [%s] type=%s value=%.2f trait=%p configAsset=%p (asset '%s')",
								entry.name.c_str(), AgentTypeName(entry.type), entry.value,
								static_cast<void*>(trait), static_cast<void*>(configAsset), assetName.c_str());

							entries.push_back(std::move(entry));
						}
					}
				}
			}
			catch (...)
			{
				LOG_DEBUG("Power: exception while scanning electricity-using buildings.");
			}

			std::sort(entries.begin(), entries.end(), [](const PowerEntry& a, const PowerEntry& b)
			{
				return a.name < b.name;
			});

			LOG_INFO("Power: found %zu electricity-using building type(s).", entries.size());

			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				g_pendingEntries = std::move(entries);
				g_pendingReady   = true;
				g_refreshInFlight.store(false, std::memory_order_release);
			}
		}

		// Triggers an asynchronous rescan on the game thread. Safe to call
		// repeatedly from RenderImGui() — coalesces into a single in-flight request.
		void RequestRescan()
		{
			bool expected = false;
			if (!g_refreshInFlight.compare_exchange_strong(expected, true))
				return;

			IPluginHooks* hooks = GetHooks();
			if (!hooks)
			{
				g_refreshInFlight.store(false, std::memory_order_release);
				return;
			}

			LOG_INFO("{POSTING_TO_GAME_THREAD} Power: posting building scan to game thread.");
			hooks->Engine->PostToGameThread(&ScanEntriesOnGameThread, nullptr);
		}

		// Adopts a freshly-scanned entry list, if one is ready, into g_entries —
		// called only from RenderImGui(), so g_entries needs no locking once adopted.
		void AdoptPendingIfReady()
		{
			std::vector<PowerEntry> incoming;
			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				if (!g_pendingReady)
					return;

				incoming = std::move(g_pendingEntries);
				g_pendingEntries.clear();
				g_pendingReady = false;
			}

			g_entries = std::move(incoming);
			g_loaded  = true;
		}

		// -------------------------------------------------------------------------
		// FMassEntityConfig::DestroyEntityTemplate / GetOrCreateEntityTemplate —
		// the Mass entity template for a config bakes a content-hashed copy of
		// FCrElectricityParameters into a const-shared-fragment the first time it's
		// built (see UCrElectricityTrait::BuildTemplate), and that cached copy is
		// what newly-spawned entities actually use — not the live trait we edit.
		// Destroying and rebuilding the cached template forces it to re-run
		// BuildTemplate against the trait's current (just-edited) Parameters.
		// -------------------------------------------------------------------------
		using DestroyEntityTemplateFn     = void       (__fastcall*)(SDK::FMassEntityConfig*, const SDK::UWorld*);
		using GetOrCreateEntityTemplateFn = const void* (__fastcall*)(SDK::FMassEntityConfig*, const SDK::UWorld*);

		DestroyEntityTemplateFn     g_destroyEntityTemplate     = nullptr;
		GetOrCreateEntityTemplateFn g_getOrCreateEntityTemplate = nullptr;
		bool                        g_templateFnsResolveTried   = false;

		bool ResolveTemplateFunctions()
		{
			if (g_templateFnsResolveTried)
				return g_destroyEntityTemplate && g_getOrCreateEntityTemplate;

			g_templateFnsResolveTried = true;

			IPluginScanner* scanner = GetScanner();
			if (!scanner)
			{
				LOG_WARN("Power: scanner unavailable - cannot resolve Mass entity template functions.");
				return false;
			}

			if (uintptr_t address = scanner->FindPatternInMainModule(AOB::FMassEntityConfig_DestroyEntityTemplate))
				g_destroyEntityTemplate = reinterpret_cast<DestroyEntityTemplateFn>(address);
			else
				LOG_WARN("Power: FMassEntityConfig::DestroyEntityTemplate pattern not found.");

			if (uintptr_t address = scanner->FindPatternInMainModule(AOB::FMassEntityConfig_GetOrCreateEntityTemplate))
				g_getOrCreateEntityTemplate = reinterpret_cast<GetOrCreateEntityTemplateFn>(address);
			else
				LOG_WARN("Power: FMassEntityConfig::GetOrCreateEntityTemplate pattern not found.");

			return g_destroyEntityTemplate && g_getOrCreateEntityTemplate;
		}

		// Forces the cached Mass entity template for configAsset to be rebuilt, so
		// newly-spawned entities of this type pick up the just-edited
		// FCrElectricityParameters instead of the stale baked copy.
		void RebuildEntityTemplate(SDK::UMassEntityConfigAsset* configAsset, const char* assetName)
		{
			if (!configAsset)
			{
				LOG_WARN("Power: RebuildEntityTemplate skipped for '%s' - null configAsset.", assetName);
				return;
			}

			if (!ResolveTemplateFunctions())
			{
				LOG_WARN("Power: RebuildEntityTemplate skipped for '%s' - template functions unresolved "
					"(Destroy=%p GetOrCreate=%p).", assetName,
					reinterpret_cast<void*>(g_destroyEntityTemplate), reinterpret_cast<void*>(g_getOrCreateEntityTemplate));
				return;
			}

			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
				{
					LOG_WARN("Power: no world loaded - skipping template rebuild for '%s'.", assetName);
					return;
				}

				LOG_INFO("Power: rebuilding Mass entity template for '%s' (configAsset=%p, world=%p)...",
					assetName, static_cast<void*>(configAsset), static_cast<void*>(world));

				g_destroyEntityTemplate(&configAsset->Config, world);
				LOG_INFO("Power: DestroyEntityTemplate done for '%s'.", assetName);

				g_getOrCreateEntityTemplate(&configAsset->Config, world);
				LOG_INFO("Power: rebuilt Mass entity template for '%s'.", assetName);
			}
			catch (...)
			{
				LOG_DEBUG("Power: exception while rebuilding Mass entity template for '%s'.", assetName);
			}
		}

		// -------------------------------------------------------------------------
		// Patching already-placed buildings.
		//
		// UCrElectricityTrait::BuildTemplate doesn't give each entity its own copy
		// of FCrElectricityParameters - it hashes the struct's contents and stores
		// the result in FMassEntityManager's ConstSharedFragmentsContainer
		// (TSharedFragmentsContainer<FConstSharedStruct>), deduplicated by content
		// hash. Every already-spawned building of a given config holds a
		// FConstSharedStruct pointing at that *same* shared block.
		//
		// RebuildEntityTemplate (above) only affects the template used for
		// *future* spawns - it computes a brand-new hash from the trait's edited
		// Parameters and registers a separate block. Already-placed buildings keep
		// referencing the old block under the old hash.
		//
		// To update those buildings too, we re-derive the OLD hash (from a copy of
		// Parameters captured before the edit) via UE::StructUtils::
		// GetStructInstanceCrc32, look up that existing block via
		// TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd, and overwrite
		// its ElectricityValue directly. "Const" here is a C++-level contract, not
		// a hardware one - this updates every entity sharing that block in place,
		// with no entity iteration or BeginPlay hook required.
		// -------------------------------------------------------------------------
		using GetMassEntitySubsystemFn = void*    (__fastcall*)(SDK::UWorld*);
		using FindOrAddConstSharedFn   = void*    (__fastcall*)(void*, uint32_t, const SDK::UScriptStruct*, const uint8_t*);
		using GetStructInstanceCrc32Fn = uint32_t (__fastcall*)(const SDK::UScriptStruct*, const uint8_t*, uint32_t);

		GetMassEntitySubsystemFn g_getMassEntitySubsystem  = nullptr;
		FindOrAddConstSharedFn   g_findOrAddConstShared    = nullptr;
		GetStructInstanceCrc32Fn g_getStructInstanceCrc32  = nullptr;
		bool                     g_sharedFragmentFnsResolveTried = false;

		bool ResolveSharedFragmentFunctions()
		{
			if (g_sharedFragmentFnsResolveTried)
				return g_getMassEntitySubsystem && g_findOrAddConstShared && g_getStructInstanceCrc32;

			g_sharedFragmentFnsResolveTried = true;

			IPluginScanner* scanner = GetScanner();
			if (!scanner)
			{
				LOG_WARN("Power: scanner unavailable - cannot resolve shared-fragment functions.");
				return false;
			}

			if (uintptr_t address = scanner->FindPatternInMainModule(AOB::UWorld_GetMassEntitySubsystem))
				g_getMassEntitySubsystem = reinterpret_cast<GetMassEntitySubsystemFn>(address);
			else
				LOG_WARN("Power: UWorld::GetSubsystem<UMassEntitySubsystem> pattern not found.");

			if (uintptr_t address = scanner->FindPatternInMainModule(AOB::FMassEntityManager_ConstSharedFragments_FindOrAdd))
				g_findOrAddConstShared = reinterpret_cast<FindOrAddConstSharedFn>(address);
			else
				LOG_WARN("Power: TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd pattern not found.");

			if (uintptr_t address = scanner->FindPatternInMainModule(AOB::StructUtils_GetStructInstanceCrc32))
				g_getStructInstanceCrc32 = reinterpret_cast<GetStructInstanceCrc32Fn>(address);
			else
				LOG_WARN("Power: UE::StructUtils::GetStructInstanceCrc32 pattern not found.");

			return g_getMassEntitySubsystem && g_findOrAddConstShared && g_getStructInstanceCrc32;
		}

		// FCrElectricityParameters is a native (non-Blueprint) ScriptStruct, so
		// dumper-7 doesn't generate a StaticStruct() for it - resolve it once via
		// the Chimera native package, the same way trait/config assets are
		// resolved by name elsewhere in this file.
		SDK::UScriptStruct* ResolveElectricityParametersStruct()
		{
			static SDK::UScriptStruct* s_struct = nullptr;
			static bool                s_tried  = false;
			if (s_tried)
				return s_struct;
			s_tried = true;

			if (!ResolveEngineLookupFunctions())
				return nullptr;

			SDK::UPackage* chimeraPackage = g_findPackage(nullptr, L"/Script/Chimera");
			if (!chimeraPackage)
			{
				LOG_WARN("Power: could not find /Script/Chimera package for CrElectricityParameters lookup.");
				return nullptr;
			}

			SDK::UObject* obj = g_staticFindObjectByName(SDK::UScriptStruct::StaticClass(), chimeraPackage, L"CrElectricityParameters", false);
			s_struct = obj ? static_cast<SDK::UScriptStruct*>(obj) : nullptr;
			if (!s_struct)
				LOG_WARN("Power: could not resolve UScriptStruct for CrElectricityParameters.");

			return s_struct;
		}

		// Patches the shared FCrElectricityParameters block that already-placed
		// buildings of this config currently reference, so they pick up the new
		// ElectricityValue immediately without reloading the save.
		void PatchExistingSharedFragment(const SDK::FCrElectricityParameters& oldParams, float newValue, const char* assetName)
		{
			if (!ResolveSharedFragmentFunctions())
			{
				LOG_WARN("Power: PatchExistingSharedFragment skipped for '%s' - shared-fragment functions unresolved.", assetName);
				return;
			}

			try
			{
				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
				{
					LOG_WARN("Power: no world loaded - skipping shared-fragment patch for '%s'.", assetName);
					return;
				}

				void* massEntitySubsystem = g_getMassEntitySubsystem(world);
				if (!massEntitySubsystem)
				{
					LOG_WARN("Power: UMassEntitySubsystem unavailable - skipping shared-fragment patch for '%s'.", assetName);
					return;
				}

				// UMassEntitySubsystem::EntityManager is a TSharedPtr<FMassEntityManager>
				// at offset 0x38; its first 8 bytes are the raw FMassEntityManager*.
				void* entityManager = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(massEntitySubsystem) + 0x38);
				if (!entityManager)
				{
					LOG_WARN("Power: FMassEntityManager unavailable - skipping shared-fragment patch for '%s'.", assetName);
					return;
				}

				SDK::UScriptStruct* paramsStruct = ResolveElectricityParametersStruct();
				if (!paramsStruct)
					return;

				// FMassEntityManager::ConstSharedFragmentsContainer sits at offset 0x120.
				void* container = reinterpret_cast<uint8_t*>(entityManager) + 0x120;

				const uint32_t oldHash = g_getStructInstanceCrc32(paramsStruct, reinterpret_cast<const uint8_t*>(&oldParams), 0);

				void* element = g_findOrAddConstShared(container, oldHash, paramsStruct, reinterpret_cast<const uint8_t*>(&oldParams));
				if (!element)
				{
					LOG_WARN("Power: FindOrAdd returned null - skipping shared-fragment patch for '%s'.", assetName);
					return;
				}

				// FConstSharedStruct (16 bytes) is a TSharedPtr<FStructSharedMemory>;
				// its first 8 bytes are the FStructSharedMemory* whose first member
				// is the UScriptStruct*, immediately followed by the struct's data.
				void* sharedMemory = *reinterpret_cast<void**>(element);
				if (!sharedMemory)
				{
					LOG_WARN("Power: shared fragment block has null memory - skipping patch for '%s'.", assetName);
					return;
				}

				auto* liveParams = reinterpret_cast<SDK::FCrElectricityParameters*>(reinterpret_cast<uint8_t*>(sharedMemory) + 0x08);

				const float before = liveParams->ElectricityValue;
				liveParams->ElectricityValue = newValue;

				LOG_INFO("Power: patched existing shared ElectricityValue %.2f -> %.2f for '%s' (hash=%08X, block=%p).",
					before, newValue, assetName, oldHash, sharedMemory);
			}
			catch (...)
			{
				LOG_DEBUG("Power: exception while patching existing shared electricity fragment for '%s'.", assetName);
			}
		}

		// -------------------------------------------------------------------------
		// Writing the edited output value back to the trait must happen on the game
		// thread — the trait pointer is only ever dereferenced there. The render
		// thread only edits the local snapshot value in g_entries.
		// -------------------------------------------------------------------------
		struct ApplyValueContext
		{
			std::string packageName;
			std::string assetName;
			float       value;
		};

		void ApplyValueOnGameThread(void* context)
		{
			std::unique_ptr<ApplyValueContext> ctx(static_cast<ApplyValueContext*>(context));

			try
			{
				// Cached pointers from a previous scan can go stale (the engine
				// reloads/reconstructs these assets, e.g. when "unlock all
				// buildings" rescans available buildings) - re-resolve fresh
				// before mutating anything or calling native template functions.
				SDK::UMassEntityConfigAsset* configAsset = ResolveConfigAsset(ctx->packageName, ctx->assetName);
				SDK::UCrElectricityTrait*    trait        = configAsset ? FindElectricityTrait(configAsset) : nullptr;

				if (trait)
				{
					const SDK::FCrElectricityParameters oldParams = trait->Parameters;

					trait->Parameters.ElectricityValue = ctx->value;
					LOG_INFO("Power: set ElectricityValue %.2f -> %.2f on trait %p ('%s').",
						oldParams.ElectricityValue, ctx->value, static_cast<void*>(trait), ctx->assetName.c_str());

					// Update buildings of this type already placed in the world -
					// they reference the old shared FCrElectricityParameters block
					// directly and won't pick up a template rebuild.
					PatchExistingSharedFragment(oldParams, ctx->value, ctx->assetName.c_str());
				}
				else
				{
					LOG_WARN("Power: ApplyValueOnGameThread could not re-resolve trait for '%s' (configAsset=%p).",
						ctx->assetName.c_str(), static_cast<void*>(configAsset));
				}

				// Persist the override so it can be re-applied after a save reload
				// reloads the asset from disk and discards this in-memory edit.
				BetterCheatsConfig::Config::SetPowerOverride(ctx->assetName, ctx->value);
				LOG_INFO("Power: persisted override '%s' = %.2f.", ctx->assetName.c_str(), ctx->value);

				// Force the cached Mass entity template to rebuild from the trait's
				// new value, so newly-spawned buildings of this type pick it up
				// immediately instead of the stale baked value.
				if (trait)
					RebuildEntityTemplate(configAsset, ctx->assetName.c_str());
			}
			catch (...)
			{
				LOG_DEBUG("Power: exception while applying electricity value.");
			}
		}

		void RequestApplyValue(const std::string& packageName, const std::string& assetName, float value)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks)
			{
				LOG_WARN("Power: RequestApplyValue skipped (hooks unavailable).");
				return;
			}

			LOG_INFO("{POSTING_TO_GAME_THREAD} Power: posting ElectricityValue=%.2f write for '%s'.", value, assetName.c_str());
			hooks->Engine->PostToGameThread(&ApplyValueOnGameThread, new ApplyValueContext{ packageName, assetName, value });
		}
	}

	void Initialize()
	{
		ResolveTemplateFunctions();
		ResolveSharedFragmentFunctions();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		AdoptPendingIfReady();

		imgui->SeparatorText("Power Output / Consumption");

		if (!g_loaded)
			RequestRescan();

		if (imgui->Button("Rescan"))
			RequestRescan();

		imgui->TextWrapped(
			"Enter a value and click Apply to commit it. Applied edits update the "
			"building's template for newly-placed buildings, and also patch "
			"buildings already placed in the world.");

		if (g_entries.empty())
		{
			imgui->TextDisabled(g_loaded ? "No electricity-using buildings found." : "Scanning...");
			return;
		}

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		auto renderSection = [&](const char* heading, const char* tableId, SDK::ECrMassElectricityAgentType filterType, const char* emptyText)
		{
			imgui->SeparatorText(heading);

			if (imgui->BeginTable(tableId, 4, kTableFlags))
			{
				imgui->TableSetupColumn("Building", 0, 0.0f);
				imgui->TableSetupColumn("Type",     kColumnWidthFixed, 90.0f);
				imgui->TableSetupColumn("Output",   kColumnWidthFixed, 140.0f);
				imgui->TableSetupColumn("",         kColumnWidthFixed, 60.0f);

				bool any = false;
				for (auto& entry : g_entries)
				{
					const bool isProducer = entry.type == SDK::ECrMassElectricityAgentType::Producer;
					if ((filterType == SDK::ECrMassElectricityAgentType::Producer) != isProducer)
						continue;

					any = true;
					imgui->TableNextRow(0, 0.0f);

					imgui->TableSetColumnIndex(0);
					imgui->Text(entry.name.c_str());

					imgui->TableSetColumnIndex(1);
					imgui->Text(AgentTypeName(entry.type));

					imgui->PushIDStr(entry.name.c_str());

					imgui->TableSetColumnIndex(2);
					imgui->SetNextItemWidth(-1.0f);
					imgui->InputFloat("##value", &entry.value, 0.0f, 0.0f, "%.2f");

					imgui->TableSetColumnIndex(3);
					if (imgui->Button("Apply"))
						RequestApplyValue(entry.packageName, entry.assetName, entry.value);

					imgui->PopID();
				}

				imgui->EndTable();

				if (!any)
					imgui->TextDisabled(emptyText);
			}
		};

		renderSection("Producers", "##power_table_producers", SDK::ECrMassElectricityAgentType::Producer, "No power-producing buildings found.");
		renderSection("Consumers", "##power_table_consumers", SDK::ECrMassElectricityAgentType::Consumer, "No power-consuming buildings found.");
	}
}
