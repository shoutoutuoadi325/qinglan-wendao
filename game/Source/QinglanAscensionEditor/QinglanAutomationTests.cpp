// Qinglan automation specs. Every test is deterministic and headless:
//  - pure functions are called directly (QinglanDamageMath, economy/blessing
//    libraries, the ending-gate mirror over FQinglanNarrativeState);
//  - subsystems are instantiated with NewObject in the transient package
//    (no GameInstance/world required);
//  - combat-component semantics use a private throwaway UWorld whose paused
//    game time is advanced manually, so the specs stay -nullrhi safe.
//
// Expectations are fixed by docs/02 sections 3-7, docs/03 sections 1-6,
// docs/05 section 6, and docs/dev/code-contracts.md summaries.
#include "QinglanAutomationTests.h"

#include "Misc/AutomationTest.h"

#include "Abilities/QinglanBlessingLibrary.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyTypes.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h"
#include "Core/QinglanTrialDirector.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveMigration.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h" // FQinglanBlessing (MakeBlessingEntry out-param)

#include "Dom/JsonObject.h"
#include "EngineDefines.h" // UE 5.8: lives at Runtime/Engine/Public root, not Engine/
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// UE 5.8: EAutomationTestFlags is an enum class; the IMPLEMENT_SIMPLE_AUTOMATION_TEST
	// macro consumes it directly (GetTestFlags/static_asserts).
	constexpr EAutomationTestFlags QinglanTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter;

	/** Raw damage of the docs/02 section 5 prototype: 20 * 1.5 + 5 = 35. */
	float PrototypeRawDamage() { return QinglanDamageMath::ComputeRawDamage(20.f, 1.5f, 5.f); }

	/** Array equality without relying on a templated TestEqual overload. */
	bool SameNameArray(const TArray<FName>& Actual, const TArray<FName>& Expected)
	{
		if (Actual.Num() != Expected.Num()) { return false; }
		for (const FName Value : Expected)
		{
			if (!Actual.Contains(Value)) { return false; }
		}
		return true;
	}

	/** A validated damage request with the prototype numbers (Defense 10 => final 32). */
	FQinglanDamageEvent MakeDamageEvent(int32 Serial, int32 HitOrdinal, EQinglanElement Element, float RawMagnitude)
	{
		FQinglanDamageEvent Event;
		Event.CastId = FQinglanCastId{ Serial, 0 };
		Event.HitOrdinal = HitOrdinal;
		Event.Element = Element;
		Event.RawMagnitude = RawMagnitude;
		return Event;
	}

	/**
	 * Private world + dummy actor + combat component. Time advances only through
	 * AdvancePausedGameTime (paused-game-time semantics, docs/dev/code-contracts.md),
	 * matching how TickComponent accumulates DeltaTime in game builds.
	 */
	class FQinglanCombatWorld
	{
	public:
		FQinglanCombatWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (World == nullptr) { return; }
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			Dummy = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
			if (Dummy == nullptr) { return; }
			Combat = NewObject<UQinglanCombatComponent>(Dummy);
			Dummy->AddOwnedComponent(Combat);
			Combat->RegisterComponent();
		}

		~FQinglanCombatWorld()
		{
			if (World != nullptr)
			{
				World->DestroyWorld(false);
			}
		}

		/** Advances paused game time and ticks the combat component manually. */
		void AdvancePausedGameTime(float Seconds)
		{
			if (World == nullptr || Combat == nullptr) { return; }
			World->TimeSeconds += Seconds;
			Combat->TickComponent(Seconds, LEVELTICK_All, nullptr);
		}

		UWorld* World = nullptr;
		AActor* Dummy = nullptr;
		UQinglanCombatComponent* Combat = nullptr;
	};

	UQinglanSaveGame* MakePopulatedSave()
	{
		UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>(GetTransientPackage());
		Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
		Save->ContentVersion = FName(TEXT("design-1"));
		Save->ProfileId = FName(TEXT("profile_0"));
		Save->SaveSequence = 7;
		Save->CultivationStage = EQinglanStage::FoundationEstablishment;
		Save->CultivationRank = 2;
		Save->Inventory.SpiritShards = 90;
		Save->Inventory.SpiritHerb = 2;
		Save->Inventory.VeinCrystal = 1;
		Save->UnlockedTechniques = { FName(TEXT("jade_sword_arc")), FName(TEXT("flowing_ward")), FName(TEXT("ember_lance")) };
		Save->EquippedTechniques = { FName(TEXT("jade_sword_arc")), FName(TEXT("ember_lance")) };
		Save->CampaignFlags = { FName(TEXT("boss_bamboo_guardian_cleared")), FName(TEXT("foundation_established")) };
		Save->CommittedTransactionIds = { FName(TEXT("tx.profile_0.room01.reward")) };
		Save->bHasCheckpoint = true;
		Save->Checkpoint.RealmSeed = 12345;
		Save->Checkpoint.EncounterStreamState = 11;
		Save->Checkpoint.LootStreamState = 22;
		Save->Checkpoint.BlessingStreamState = 33;
		Save->Checkpoint.RoomIndex = 2;
		Save->Checkpoint.RealmId = FName(TEXT("bamboo_hollow"));
		Save->Checkpoint.AcquiredBlessings = { FName(TEXT("b_sharp_edge")) };
		Save->Checkpoint.PlayerAttributes.Health = 80.f;
		Save->Checkpoint.PlayerAttributes.MaxHealth = 115.f;
		Save->Checkpoint.Loot.SpiritShards = 30;
		Save->Checkpoint.CommittedRoomIds = { 0, 1 };
		Save->Narrative.Revision = FName(TEXT("nar-1"));
		Save->Narrative.CompletedScenes = { FName(TEXT("CH00_S01")) };
		Save->Narrative.Flags = { FName(TEXT("mq_ch00_bell_examined")) };
		Save->Narrative.ChoiceDecisions.Add(FName(TEXT("CH00_S01_LOOK")), FName(TEXT("mq_ch00_observe_first")));
		Save->Narrative.PendingAgreements.Add(FName(TEXT("SQ02")), FName(TEXT("sq02_schedule_b")));
		Save->Narrative.EffectiveAgreements.Add(FName(TEXT("SQ03")), FName(TEXT("sq03_rota_hub")));
		return Save;
	}

	// ---------------------------------------------------------------------------
	// JSON drift-guard support (Qinglan.Narrative.EndingRulesJsonDrift):
	// minimal accessors in the importer's style (QinglanDataImportCommandlet.cpp
	// JStr/JBool/JNameArray; UE 5.8 GetFieldUntyped is the untyped getter).
	// ---------------------------------------------------------------------------

	FString JsonString(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		return Object->HasField(Field) ? Object->GetFieldUntyped(Field)->AsString() : FString();
	}

	bool JsonBool(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		bool bOut = false;
		return Object->HasField(Field) && Object->GetFieldUntyped(Field)->TryGetBool(bOut) && bOut;
	}

	TArray<FName> JsonNameArray(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FName> Out;
		if (!Object->HasField(Field)) { return Out; }
		for (const TSharedPtr<FJsonValue>& Value : Object->GetFieldUntyped(Field)->AsArray())
		{
			if (Value.IsValid() && !Value->IsNull())
			{
				Out.Add(FName(*Value->AsString()));
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> JsonObject(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object->HasField(Field)) { return nullptr; }
		return Object->GetFieldUntyped(Field)->AsObject();
	}

	/**
	 * Payload ending token -> the hardcoded commit flag it must set
	 * (data/story/main-late.json CH06_S04 ending_id values are END_OPEN /
	 * END_SEAL / END_SHARED; QinglanEndingRules.h spells the commit flags in
	 * lowercase). Local mapping, not QinglanEnding::EndingIdToEnum: the runtime
	 * module's namespace functions are not QINGLANASCENSION_API-exported, so -
	 * like QinglanEndingGateMirror above - the spec encodes the mapping beside
	 * itself and compares against the header's inline constants directly.
	 */
	FName EndingFlagForToken(const FString& Token)
	{
		if (Token == TEXT("END_OPEN")) { return FName(QinglanEnding::EndingOpenId); }
		if (Token == TEXT("END_SEAL")) { return FName(QinglanEnding::EndingSealId); }
		if (Token == TEXT("END_SHARED")) { return FName(QinglanEnding::EndingSharedId); }
		return NAME_None;
	}

	/**
	 * Resolves the repo's data/story/main-late.json at runtime. Editor sessions
	 * run from the project dir (game/, with data/ a repo sibling); commandlet
	 * sessions run from game/Binaries/<platform>; the literal relative fallback
	 * covers a process dir matching either. Empty when this machine carries no
	 * repo checkout (packaged builds, some CI), which skips the drift guard.
	 */
	FString FindMainLateJsonPath()
	{
		const FString Candidates[] =
		{
			FPaths::ProjectDir() / TEXT("../data/story/main-late.json"),
			FPaths::LaunchDir() / TEXT("../../../data/story/main-late.json"),
			FString(TEXT("../../../data/story/main-late.json"))
		};
		for (const FString& Candidate : Candidates)
		{
			if (IFileManager::Get().FileExists(*Candidate))
			{
				return Candidate;
			}
		}
		return FString();
	}
} // namespace

// ---------------------------------------------------------------------------
// Qinglan.Combat.DamageMath - docs/02 section 5 prototype + rounding + minimum
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanDamageMath, "Qinglan.Combat.DamageMath", QinglanTestFlags)
bool FAutomationTestQinglanDamageMath::RunTest(const FString& Parameters)
{
	// Prototype check (docs/02 section 5): Power 20, coef 1.5, flat 5, Defense 10 -> 32.
	const float Raw = PrototypeRawDamage();
	TestEqual(TEXT("Raw = 20 * 1.5 + 5 = 35"), Raw, 35.f);
	const float Mitigated = QinglanDamageMath::ComputeMitigatedDamage(Raw, 10.f);
	TestNearlyEqual(TEXT("Mitigated = 35 * 100 / 110"), Mitigated, 35.f * 100.f / 110.f, 1e-4f);
	TestEqual(TEXT("Final = round(31.8181) = 32"), QinglanDamageMath::ComputeFinalDamage(Mitigated, 1.f, QinglanCombatTunables::DefaultDifficultyMultiplier), 32.f);

	// Negative defense never amplifies (clamped at zero). docs/02 section 5.
	TestEqual(TEXT("Negative defense clamps to 0"), QinglanDamageMath::ComputeMitigatedDamage(35.f, -50.f), 35.f);

	// Halves round away from zero. docs/02 section 5.
	TestEqual(TEXT("31.5 -> 32"), QinglanDamageMath::RoundHalfAwayFromZero(31.5f), 32.f);
	TestEqual(TEXT("-31.5 -> -32"), QinglanDamageMath::RoundHalfAwayFromZero(-31.5f), -32.f);
	TestEqual(TEXT("2.5 -> 3"), QinglanDamageMath::RoundHalfAwayFromZero(2.5f), 3.f);
	TestEqual(TEXT("-2.5 -> -3"), QinglanDamageMath::RoundHalfAwayFromZero(-2.5f), -3.f);
	TestEqual(TEXT("31.4 -> 31"), QinglanDamageMath::RoundHalfAwayFromZero(31.4f), 31.f);

	// Any accepted hit deals at least 1 damage. docs/02 section 5.
	TestEqual(TEXT("Minimum damage is 1"), QinglanDamageMath::ComputeFinalDamage(0.3f, 1.f, QinglanCombatTunables::DefaultDifficultyMultiplier), 1.f);
	TestEqual(TEXT("Minimum damage under reaction"), QinglanDamageMath::ComputeFinalDamage(0.1f, 1.25f, QinglanCombatTunables::DefaultDifficultyMultiplier), 1.f);

	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Combat.HitKeyDedupe - (CastId, victim, ordinal) duplicates rejected
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanHitKeyDedupe, "Qinglan.Combat.HitKeyDedupe", QinglanTestFlags)
bool FAutomationTestQinglanHitKeyDedupe::RunTest(const FString& Parameters)
{
	// Pure key behaviour (docs/02 section 3).
	const FQinglanCastId Cast{ 4, 9 };
	const uint64 KeyA = QinglanDamageMath::MakeHitKey(Cast, 42, 0);
	TestEqual(TEXT("Same inputs -> same key"), KeyA, QinglanDamageMath::MakeHitKey(Cast, 42, 0));
	TestNotEqual(TEXT("Different victim -> different key"), KeyA, QinglanDamageMath::MakeHitKey(Cast, 43, 0));
	TestNotEqual(TEXT("Different ordinal -> different key"), KeyA, QinglanDamageMath::MakeHitKey(Cast, 42, 1));
	const FQinglanCastId OtherCast{ 5, 9 };
	TestNotEqual(TEXT("Different serial -> different key"), KeyA, QinglanDamageMath::MakeHitKey(OtherCast, 42, 0));

	// Integration: the combat component rejects a duplicate (cast, victim, ordinal).
	FQinglanCombatWorld World;
	if (!TestNotNull(TEXT("Combat component"), World.Combat))
	{
		return false;
	}
	const float Raw = PrototypeRawDamage(); // final 32 against Defense 10
	TestTrue(TEXT("First hit accepted"), World.Combat->ApplyDamage(MakeDamageEvent(10, 0, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Health after first hit"), World.Combat->GetHealth(), 68.f);
	TestFalse(TEXT("Duplicate hit rejected"), World.Combat->ApplyDamage(MakeDamageEvent(10, 0, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Health unchanged after duplicate"), World.Combat->GetHealth(), 68.f);
	TestTrue(TEXT("Same cast, next ordinal accepted"), World.Combat->ApplyDamage(MakeDamageEvent(10, 1, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Health after second ordinal"), World.Combat->GetHealth(), 36.f);
	TestTrue(TEXT("New cast, ordinal 0 accepted"), World.Combat->ApplyDamage(MakeDamageEvent(11, 0, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Health after new cast"), World.Combat->GetHealth(), 4.f);
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Combat.Conduct - Thunder vs Wet: exactly one 1.25x, consumes Wet
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanConduct, "Qinglan.Combat.ConductSingleApplication", QinglanTestFlags)
bool FAutomationTestQinglanConduct::RunTest(const FString& Parameters)
{
	FQinglanCombatWorld World;
	if (!TestNotNull(TEXT("Combat component"), World.Combat))
	{
		return false;
	}
	const float Raw = PrototypeRawDamage(); // 35 raw, Defense 10 -> 31.8181 mitigated

	World.Combat->ApplyStatus(EQinglanStatus::Wet);
	TestTrue(TEXT("Wet applied"), World.Combat->HasStatus(EQinglanStatus::Wet));

	// Conduct application: 31.8181 * 1.25 = 39.77 -> 40 (docs/02 section 7).
	TestTrue(TEXT("Thunder hit on Wet accepted"), World.Combat->ApplyDamage(MakeDamageEvent(20, 0, EQinglanElement::Lightning, Raw)));
	TestEqual(TEXT("Conduct damage 40"), World.Combat->GetHealth(), 60.f);
	TestFalse(TEXT("Wet consumed by Conduct"), World.Combat->HasStatus(EQinglanStatus::Wet));

	// The reaction never chains: a second Thunder hit deals the plain 32.
	TestTrue(TEXT("Second Thunder hit accepted"), World.Combat->ApplyDamage(MakeDamageEvent(21, 0, EQinglanElement::Lightning, Raw)));
	TestEqual(TEXT("Second Thunder hit deals 32"), World.Combat->GetHealth(), 28.f);
	TestEqual(TEXT("Conduct multiplier is 1.25"), QinglanCombatTunables::ConductMultiplier, 1.25f);
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Combat.Burn - three 4-damage ticks, refresh resets but never stacks
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanBurn, "Qinglan.Combat.BurnThreeTickRefresh", QinglanTestFlags)
bool FAutomationTestQinglanBurn::RunTest(const FString& Parameters)
{
	FQinglanCombatWorld World;
	if (!TestNotNull(TEXT("Combat component"), World.Combat))
	{
		return false;
	}

	// One application: exactly three 4-damage ticks across 3 s (docs/02 section 7).
	World.Combat->ApplyStatus(EQinglanStatus::Burn);
	TestTrue(TEXT("Burn applied"), World.Combat->HasStatus(EQinglanStatus::Burn));
	World.AdvancePausedGameTime(1.f);
	TestEqual(TEXT("Burn tick 1"), World.Combat->GetHealth(), 96.f);
	World.AdvancePausedGameTime(1.f);
	TestEqual(TEXT("Burn tick 2"), World.Combat->GetHealth(), 92.f);
	World.AdvancePausedGameTime(1.f);
	TestEqual(TEXT("Burn tick 3"), World.Combat->GetHealth(), 88.f);
	TestFalse(TEXT("Burn cleared after three ticks"), World.Combat->HasStatus(EQinglanStatus::Burn));

	// Refresh not stack: after re-applying mid-burn the tick counter resets to
	// three; it never accumulates past the authored maximum.
	World.Combat->ApplyStatus(EQinglanStatus::Burn);
	World.AdvancePausedGameTime(1.f);
	TestEqual(TEXT("Second burn application, tick 1"), World.Combat->GetHealth(), 84.f);
	World.AdvancePausedGameTime(1.f);
	TestEqual(TEXT("Second burn application, tick 2"), World.Combat->GetHealth(), 80.f);
	World.Combat->ApplyStatus(EQinglanStatus::Burn); // refresh with one tick left
	World.AdvancePausedGameTime(1.f);
	World.AdvancePausedGameTime(1.f);
	World.AdvancePausedGameTime(1.f);
	// Health was 88 when the second application landed (100 - 3 ticks above);
	// 2 ticks before the refresh + exactly 3 after it = 5 * 4 = 20 -> 68
	// (docs/02 section 7 "maximum three ticks after the latest refresh").
	// A stacking implementation would have dealt 6 ticks (88 - 24 = 64).
	TestEqual(TEXT("Refresh resets to three ticks, never stacks"), World.Combat->GetHealth(), 68.f);
	TestFalse(TEXT("Burn cleared after refreshed ticks"), World.Combat->HasStatus(EQinglanStatus::Burn));
	World.AdvancePausedGameTime(2.f);
	TestEqual(TEXT("No further ticks after expiry"), World.Combat->GetHealth(), 68.f);
	TestEqual(TEXT("Burn tick damage is 4"), QinglanCombatTunables::BurnTickDamage, 4.f);
	TestEqual(TEXT("Burn tick count is 3"), QinglanCombatTunables::BurnTickCount, 3);
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Combat.Barrier - stronger replaces weaker, absorbs before health
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanBarrier, "Qinglan.Combat.BarrierReplaceAbsorb", QinglanTestFlags)
bool FAutomationTestQinglanBarrier::RunTest(const FString& Parameters)
{
	FQinglanCombatWorld World;
	if (!TestNotNull(TEXT("Combat component"), World.Combat))
	{
		return false;
	}
	const float Raw = PrototypeRawDamage(); // final 32 against Defense 10

	// Replace-not-stack (docs/02 section 6).
	World.Combat->ApplyBarrier(EQinglanElement::Water, 20.f, 30.f);
	TestEqual(TEXT("Barrier applied"), World.Combat->GetBarrierAmount(), 20.f);
	World.Combat->ApplyBarrier(EQinglanElement::Earth, 10.f, 30.f);
	TestEqual(TEXT("Weaker barrier ignored"), World.Combat->GetBarrierAmount(), 20.f);
	World.Combat->ApplyBarrier(EQinglanElement::Earth, 50.f, 30.f);
	TestEqual(TEXT("Stronger barrier replaces (no stacking to 70)"), World.Combat->GetBarrierAmount(), 50.f);

	// Absorb before health: a 32-damage hit eats barrier only.
	TestTrue(TEXT("Hit into barrier accepted"), World.Combat->ApplyDamage(MakeDamageEvent(30, 0, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Barrier absorbed 32"), World.Combat->GetBarrierAmount(), 18.f);
	TestEqual(TEXT("Health untouched while barrier holds"), World.Combat->GetHealth(), 100.f);

	// A barrier smaller than the hit spills the remainder into health.
	World.Combat->ApplyBarrier(EQinglanElement::Water, 20.f, 30.f);
	TestEqual(TEXT("Barrier re-applied at 20"), World.Combat->GetBarrierAmount(), 20.f);
	TestTrue(TEXT("Second hit accepted"), World.Combat->ApplyDamage(MakeDamageEvent(31, 0, EQinglanElement::Metal, Raw)));
	TestEqual(TEXT("Barrier fully consumed"), World.Combat->GetBarrierAmount(), 0.f);
	TestEqual(TEXT("Overflow 12 reaches health"), World.Combat->GetHealth(), 88.f);
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Progression.RankCostsAndBreakthroughGates - docs/03 section 1
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanRankCosts, "Qinglan.Progression.RankCostsAndBreakthroughGates", QinglanTestFlags)
bool FAutomationTestQinglanRankCosts::RunTest(const FString& Parameters)
{
	// --- Pure table checks -----------------------------------------------------
	struct FExpected { EQinglanStage Stage; int32 Rank; int64 Shards; const TCHAR* Material; };
	const FExpected ExpectedTable[] =
	{
		{ EQinglanStage::QiRefining, 0, 80,  TEXT("spirit_herb") },
		{ EQinglanStage::QiRefining, 1, 120, TEXT("spirit_herb") },
		{ EQinglanStage::QiRefining, 2, 180, TEXT("spirit_herb") },
		{ EQinglanStage::FoundationEstablishment, 0, 220, TEXT("vein_crystal") },
		{ EQinglanStage::FoundationEstablishment, 1, 300, TEXT("vein_crystal") },
		{ EQinglanStage::FoundationEstablishment, 2, 400, TEXT("vein_crystal") },
	};
	int64 TotalShards = 0;
	for (const FExpected& Expected : ExpectedTable)
	{
		int64 Shards = -1, MaterialCount = -1;
		FName MaterialId(NAME_None);
		TestTrue(TEXT("GetRankCost succeeds"), UQinglanEconomyLibrary::GetRankCost(Expected.Stage, Expected.Rank, Shards, MaterialCount, MaterialId));
		TestEqual(TEXT("Rank shard cost matches docs/03 section 1"), Shards, Expected.Shards);
		TestEqual(TEXT("Exactly one material per rank"), MaterialCount, QinglanProgression::MaterialCountPerRank);
		TestEqual(TEXT("Rank material matches docs/03 section 1"), MaterialId, FName(Expected.Material));
		TotalShards += Shards;
	}
	TestEqual(TEXT("Total pre-final rank cost is 1,300"), TotalShards, QinglanProgression::TotalPreFinalRankShardCost);

	int64 Shards = 0, Count = 0;
	FName Material;
	TestFalse(TEXT("No fourth Qi rank"), UQinglanEconomyLibrary::GetRankCost(EQinglanStage::QiRefining, 3, Shards, Count, Material));
	TestFalse(TEXT("No negative rank"), UQinglanEconomyLibrary::GetRankCost(EQinglanStage::QiRefining, -1, Shards, Count, Material));
	TestFalse(TEXT("Golden Core is rankless"), UQinglanEconomyLibrary::GetRankCost(EQinglanStage::GoldenCore, 0, Shards, Count, Material));

	// Cumulative bonuses: 6 ranks = +30/+12, Foundation +15 HP/+20 Energy, Golden Core +20 HP.
	float HealthBonus = 0.f, EnergyBonus = 0.f, PowerBonus = 0.f;
	UQinglanEconomyLibrary::GetCultivationBonuses(EQinglanStage::FoundationEstablishment, 3, HealthBonus, EnergyBonus, PowerBonus);
	TestEqual(TEXT("Foundation rank 3 health bonus"), HealthBonus, 45.f);
	TestEqual(TEXT("Foundation breakthrough energy bonus"), EnergyBonus, 20.f);
	TestEqual(TEXT("Foundation rank 3 power bonus"), PowerBonus, 12.f);
	UQinglanEconomyLibrary::GetCultivationBonuses(EQinglanStage::GoldenCore, 0, HealthBonus, EnergyBonus, PowerBonus);
	TestEqual(TEXT("Golden Core health bonus"), HealthBonus, 65.f);
	TestFalse(TEXT("Tier 2 locked in Qi Refining"), UQinglanEconomyLibrary::IsTier2Available(EQinglanStage::QiRefining));
	TestTrue(TEXT("Tier 2 available from Foundation"), UQinglanEconomyLibrary::IsTier2Available(EQinglanStage::FoundationEstablishment));

	// --- Subsystem flow: purchases are atomic and gates gate ------------------
	UGameInstance* ProfileOwner = NewObject<UGameInstance>(GetTransientPackage()); // UE 5.8 ClassWithin: subsystems need a GameInstance outer
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(ProfileOwner);
	if (!TestNotNull(TEXT("Profile subsystem"), Profile))
	{
		return false;
	}
	auto Grant = [Profile](const TCHAR* Tx, int64 ShardsToGrant, int64 Herb, int64 Crystal)
	{
		FQinglanInventory GrantInventory;
		GrantInventory.SpiritShards = ShardsToGrant;
		GrantInventory.SpiritHerb = Herb;
		GrantInventory.VeinCrystal = Crystal;
		return Profile->TryGrant(FName(Tx), GrantInventory);
	};

	TestFalse(TEXT("Rank purchase without funds is rejected"), Profile->TryAdvanceRank(FName(TEXT("rank0"))));
	TestEqual(TEXT("Rejected purchase changes nothing"), Profile->GetInventory().SpiritShards, static_cast<int64>(0));

	const int64 QiCosts[3] = { 80, 120, 180 };
	for (int32 RankIndex = 0; RankIndex < 3; ++RankIndex)
	{
		TestTrue(TEXT("Funds granted"), Grant(*FString::Printf(TEXT("funds_qi_%d"), RankIndex), QiCosts[RankIndex], 1, 0));
		TestTrue(TEXT("Rank purchased"), Profile->TryAdvanceRank(*FString::Printf(TEXT("rank_qi_%d"), RankIndex)));
		TestEqual(TEXT("Rank advanced"), Profile->GetCultivationRank(), RankIndex + 1);
		TestEqual(TEXT("Rank purchase spends the shards"), Profile->GetInventory().SpiritShards, static_cast<int64>(0));
		TestEqual(TEXT("Rank purchase spends the material"), Profile->GetInventory().SpiritHerb, static_cast<int64>(0));
	}
	TestFalse(TEXT("No fourth Qi rank purchasable"), Profile->TryAdvanceRank(FName(TEXT("rank_qi_3"))));

	// Breakthrough gates (docs/03 section 1; blueprint section 1.5).
	FText Reason;
	TestFalse(TEXT("Breakthrough blocked without boss proof"), Profile->TryBreakthrough(FName(TEXT("bt_foundation"))));
	TestFalse(TEXT("CanAttemptBreakthrough false"), Profile->CanAttemptBreakthrough(Reason));
	Profile->SetCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag));
	TestFalse(TEXT("Foundation still requires its actual trial"), Profile->CanAttemptBreakthrough(Reason));
	TestFalse(TEXT("Boss proof cannot bypass Foundation trial"), Profile->TryBreakthrough(FName(TEXT("bt_foundation"))));
	Profile->SetCampaignFlag(FName(QinglanProgression::FoundationTrialPassedCampaignFlag));
	TestTrue(TEXT("Gate open with boss and trial proof"), Profile->CanAttemptBreakthrough(Reason));
	TestTrue(TEXT("Foundation breakthrough committed"), Profile->TryBreakthrough(FName(TEXT("bt_foundation"))));
	TestEqual(TEXT("Stage advanced to Foundation"), Profile->GetCultivationStage(), EQinglanStage::FoundationEstablishment);
	TestEqual(TEXT("Rank reset on breakthrough"), Profile->GetCultivationRank(), 0);
	TestTrue(TEXT("Foundation marker flagged"), Profile->HasCampaignFlag(FName(QinglanProgression::FoundationEstablishedFlag)));

	const int64 FoundationCosts[3] = { 220, 300, 400 };
	for (int32 RankIndex = 0; RankIndex < 3; ++RankIndex)
	{
		TestTrue(TEXT("Foundation funds granted"), Grant(*FString::Printf(TEXT("funds_fe_%d"), RankIndex), FoundationCosts[RankIndex], 0, 1));
		TestTrue(TEXT("Foundation rank purchased"), Profile->TryAdvanceRank(*FString::Printf(TEXT("rank_fe_%d"), RankIndex)));
	}
	TestFalse(TEXT("Golden Core blocked without Ashen proof"), Profile->TryBreakthrough(FName(TEXT("bt_core"))));
	Profile->SetCampaignFlag(FName(QinglanProgression::AshenClearedCampaignFlag));
	TestFalse(TEXT("Golden Core still blocked without trial win"), Profile->TryBreakthrough(FName(TEXT("bt_core"))));
	Profile->SetCampaignFlag(FName(QinglanProgression::GoldenCoreTrialWonCampaignFlag));
	TestTrue(TEXT("Golden Core breakthrough with trial proof"), Profile->TryBreakthrough(FName(TEXT("bt_core"))));
	TestEqual(TEXT("Stage advanced to Golden Core"), Profile->GetCultivationStage(), EQinglanStage::GoldenCore);
	TestFalse(TEXT("No breakthrough past Golden Core"), Profile->TryBreakthrough(FName(TEXT("bt_past"))));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Economy.BankingMultipliers - docs/03 section 6 (0.7 floor per stack)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanBanking, "Qinglan.Economy.BankingMultipliers", QinglanTestFlags)
bool FAutomationTestQinglanBanking::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Victory multiplier 1.0"), UQinglanEconomyLibrary::GetBankingMultiplier(true, false), UQinglanEconomyLibrary::BankingMultiplierVictory);
	TestEqual(TEXT("Voluntary retreat multiplier 0.7"), UQinglanEconomyLibrary::GetBankingMultiplier(false, true), UQinglanEconomyLibrary::BankingMultiplierVoluntaryRetreat);
	TestEqual(TEXT("Defeat multiplier 0.5"), UQinglanEconomyLibrary::GetBankingMultiplier(false, false), UQinglanEconomyLibrary::BankingMultiplierDefeat);
	TestEqual(TEXT("Victory wins over retreat flag"), UQinglanEconomyLibrary::GetBankingMultiplier(true, true), 1.0f);

	FQinglanInventory Loot;
	Loot.SpiritShards = 185;
	Loot.SpiritHerb = 1;
	Loot.VeinCrystal = 3;

	FQinglanInventory Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, true, false);
	TestEqual(TEXT("Victory banks everything"), Banked.SpiritShards, static_cast<int64>(185));
	TestEqual(TEXT("Victory herbs"), Banked.SpiritHerb, static_cast<int64>(1));
	TestEqual(TEXT("Victory crystals"), Banked.VeinCrystal, static_cast<int64>(3));

	// Floored per stack: 185*0.7 = 129.5 -> 129, 1*0.7 = 0.7 -> 0, 3*0.7 = 2.1 -> 2.
	Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, false, true);
	TestEqual(TEXT("Retreat shards floored"), Banked.SpiritShards, static_cast<int64>(129));
	TestEqual(TEXT("Retreat floors a 1-herb stack to zero"), Banked.SpiritHerb, static_cast<int64>(0));
	TestEqual(TEXT("Retreat crystals floored"), Banked.VeinCrystal, static_cast<int64>(2));

	// Defeat: 92.5 -> 92, 0.5 -> 0, 1.5 -> 1.
	Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, false, false);
	TestEqual(TEXT("Defeat shards floored"), Banked.SpiritShards, static_cast<int64>(92));
	TestEqual(TEXT("Defeat herbs floored"), Banked.SpiritHerb, static_cast<int64>(0));
	TestEqual(TEXT("Defeat crystals floored"), Banked.VeinCrystal, static_cast<int64>(1));

	// Payout table parity with docs/03 section 2 (base totals include room rewards).
	FQinglanInventory Payout;
	TestTrue(TEXT("Bamboo payout exists"), UQinglanEconomyLibrary::GetExpeditionPayout(FName(UQinglanEconomyLibrary::BambooRealmIdText), true, Payout));
	TestEqual(TEXT("Bamboo first clear shards"), Payout.SpiritShards, static_cast<int64>(300)); // 180 base + 120 bonus
	TestEqual(TEXT("Bamboo first clear herbs"), Payout.SpiritHerb, static_cast<int64>(3));
	TestEqual(TEXT("Bamboo first clear crystals"), Payout.VeinCrystal, static_cast<int64>(1));
	TestTrue(TEXT("Bamboo repeat payout"), UQinglanEconomyLibrary::GetExpeditionPayout(FName(UQinglanEconomyLibrary::BambooRealmIdText), false, Payout));
	TestEqual(TEXT("Bamboo repeat shards"), Payout.SpiritShards, static_cast<int64>(180));
	TestEqual(TEXT("Bamboo repeat herbs"), Payout.SpiritHerb, static_cast<int64>(2));
	TestTrue(TEXT("Ashen first clear payout"), UQinglanEconomyLibrary::GetExpeditionPayout(FName(UQinglanEconomyLibrary::AshenRealmIdText), true, Payout));
	TestEqual(TEXT("Ashen first clear shards"), Payout.SpiritShards, static_cast<int64>(500)); // 300 base + 200 bonus
	TestEqual(TEXT("Ashen first clear crystals"), Payout.VeinCrystal, static_cast<int64>(3));
	UQinglanEconomyLibrary::GetTutorialPayout(Payout);
	TestEqual(TEXT("Tutorial pays 80 shards"), Payout.SpiritShards, static_cast<int64>(80));
	TestEqual(TEXT("Tutorial pays 1 herb"), Payout.SpiritHerb, static_cast<int64>(1));
	TestFalse(TEXT("Unknown realm has no payout"), UQinglanEconomyLibrary::GetExpeditionPayout(FName(TEXT("nowhere")), true, Payout));

	// First-clear idempotency keys are stable and boss-scoped (docs/03 section 6).
	const FName FirstClearA = UQinglanEconomyLibrary::MakeFirstClearTransactionId(FName(TEXT("profile_0")), FName(TEXT("bamboo_guardian")));
	TestEqual(TEXT("First-clear key is stable"), FirstClearA, UQinglanEconomyLibrary::MakeFirstClearTransactionId(FName(TEXT("profile_0")), FName(TEXT("bamboo_guardian"))));
	TestNotEqual(TEXT("First-clear key is boss-scoped"), FirstClearA, UQinglanEconomyLibrary::MakeFirstClearTransactionId(FName(TEXT("profile_0")), FName(TEXT("ashen_guardian"))));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Abilities.Blessings - group mutual exclusion + one-stack no re-offer
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanBlessings, "Qinglan.Abilities.BlessingExclusion", QinglanTestFlags)
bool FAutomationTestQinglanBlessings::RunTest(const FString& Parameters)
{
	// NOTE: QinglanBlessingGroups constants are module-local symbols, so the
	// group ids are spelled out here ("sword_offense" etc., docs/03 section 5).
	const FName GroupIds[4] = { FName(TEXT("sword_offense")), FName(TEXT("casting_economy")), FName(TEXT("protection")), FName(TEXT("elemental_interactions")) };

	TArray<FName> AllIds;
	UQinglanBlessingLibrary::GetAllBlessingIds(AllIds);
	TestEqual(TEXT("Twelve blessings ship"), AllIds.Num(), 12);

	TSet<FName> UniqueIds;
	for (const FName Id : AllIds) { UniqueIds.Add(Id); }
	TestEqual(TEXT("Blessing ids are unique"), UniqueIds.Num(), AllIds.Num());

	// Every blessing belongs to exactly one group of three (mutual exclusion).
	TSet<FName> SeenInAnyGroup;
	for (const FName GroupId : GroupIds)
	{
		TArray<FName> GroupIdsForMember;
		UQinglanBlessingLibrary::GetBlessingsInGroup(GroupId, GroupIdsForMember);
		TestEqual(TEXT("Each group offers three blessings"), GroupIdsForMember.Num(), 3);
		for (const FName MemberId : GroupIdsForMember)
		{
			TestTrue(TEXT("Group member is a known blessing"), UniqueIds.Contains(MemberId));
			TestFalse(TEXT("No blessing in two groups"), SeenInAnyGroup.Contains(MemberId));
			SeenInAnyGroup.Add(MemberId);
		}
	}
	TestEqual(TEXT("Groups cover all blessings"), SeenInAnyGroup.Num(), AllIds.Num());

	// One-stack cap: an owned blessing is never re-offered (docs/03 section 5).
	for (const FName Id : AllIds)
	{
		TestTrue(TEXT("Unowned blessing is offerable"), UQinglanBlessingLibrary::IsBlessingOfferable(Id, TArray<FName>()));
		TestFalse(TEXT("Owned blessing is never re-offered"), UQinglanBlessingLibrary::IsBlessingOfferable(Id, TArray<FName>{ Id }));
	}
	TestFalse(TEXT("Unknown blessing is not offerable"), UQinglanBlessingLibrary::IsBlessingOfferable(FName(TEXT("b_nope")), TArray<FName>()));

	// Modifiers add within a category and apply once (docs/03 section 4).
	// b_sharp_edge (+12% sword) + b_heavy_hand (+20% sword) => +32%, not 1.12*1.20.
	FQinglanBlessingModifiers Modifiers;
	UQinglanBlessingLibrary::ComputeModifiers(TArray<FName>{ FName(TEXT("b_sharp_edge")), FName(TEXT("b_heavy_hand")) }, Modifiers);
	TestEqual(TEXT("Sword percent sums within the category"), Modifiers.SwordDamagePercent, 32.f);

	FQinglanBlessingDef SharpEdge;
	TestTrue(TEXT("b_sharp_edge resolves"), UQinglanBlessingLibrary::FindBlessingDef(FName(TEXT("b_sharp_edge")), SharpEdge));
	TestEqual(TEXT("b_sharp_edge is one-stack capped"), SharpEdge.bOneStackCap, true);
	FQinglanBlessing Entry;
	TestTrue(TEXT("Entry builds for a known blessing"), UQinglanBlessingLibrary::MakeBlessingEntry(FName(TEXT("b_sharp_edge")), Entry));
	TestEqual(TEXT("Entry group matches"), Entry.GroupId, FName(TEXT("sword_offense")));
	TestFalse(TEXT("Entry build rejects unknown blessing"), UQinglanBlessingLibrary::MakeBlessingEntry(FName(TEXT("b_nope")), Entry));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Narrative.EndingGating - shared requirement + effective-only preview
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanEndingGating, "Qinglan.Narrative.EndingGating", QinglanTestFlags)
bool FAutomationTestQinglanEndingGating::RunTest(const FString& Parameters)
{
	using namespace QinglanEndingGateMirror;

	FQinglanNarrativeState Narrative;
	// Nothing set: no ending is available.
	TestFalse(TEXT("Open blocked before release"), IsEndingAvailable(Narrative, EQinglanEnding::Open));
	TestFalse(TEXT("Seal blocked before release"), IsEndingAvailable(Narrative, EQinglanEnding::Seal));
	TestFalse(TEXT("Shared blocked before release"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));

	// Shen released: open and seal are available, shared is not (blueprint section 2.3).
	Narrative.Flags.Add(ShenReleasedFlag());
	TestTrue(TEXT("Open available after release"), IsEndingAvailable(Narrative, EQinglanEnding::Open));
	TestTrue(TEXT("Seal available after release"), IsEndingAvailable(Narrative, EQinglanEnding::Seal));
	TestFalse(TEXT("Shared blocked without agreements"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));

	// Two of three agreements: still blocked.
	Narrative.Flags.Add(FName(TEXT("sq_spillway_verified")));
	Narrative.Flags.Add(FName(TEXT("sq_allocation_agreed")));
	TestFalse(TEXT("Shared blocked with two agreements"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));

	// Third flag present but SQ02 still pending: pending is never effective
	// (code-contracts.md "ending preview reads effective only").
	Narrative.Flags.Add(FName(TEXT("sq_handover_accepted")));
	Narrative.PendingAgreements.Add(FName(TEXT("SQ02")), FName(TEXT("sq02_schedule_b")));
	TestFalse(TEXT("Shared blocked while SQ02 pending"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));

	Narrative.PendingAgreements.Reset();
	TestTrue(TEXT("Shared available with all effective agreements"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));

	// Committed once: after confirmation no ending is offered again (mutual exclusion).
	Narrative.ConfirmedEnding = EQinglanEnding::Open;
	TestFalse(TEXT("No re-offer after confirming Open"), IsEndingAvailable(Narrative, EQinglanEnding::Open));
	TestFalse(TEXT("Seal mutually exclusive after Open"), IsEndingAvailable(Narrative, EQinglanEnding::Seal));
	TestFalse(TEXT("Shared mutually exclusive after Open"), IsEndingAvailable(Narrative, EQinglanEnding::Shared));
	TestEqual(TEXT("Ending enum maps to the ending_open flag"), ToEndingFlagId(EQinglanEnding::Open), FName(TEXT("ending_open")));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Persistence.SaveChecksumRoundtrip - roundtrip + corrupt + future schema
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanSaveRoundtrip, "Qinglan.Persistence.SaveChecksumRoundtrip", QinglanTestFlags)
bool FAutomationTestQinglanSaveRoundtrip::RunTest(const FString& Parameters)
{
	// UE 5.8 enforces ClassWithin: a UGameInstanceSubsystem must live inside a
	// UGameInstance outer, so give the throwaway instance a throwaway owner.
	UGameInstance* OwnerInstance = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanSaveSubsystem* SaveSubsystem = NewObject<UQinglanSaveSubsystem>(OwnerInstance);
	if (!TestNotNull(TEXT("Save subsystem"), SaveSubsystem))
	{
		return false;
	}
	UQinglanSaveGame* Save = MakePopulatedSave();

	// Roundtrip: checksummed container parses back into the same semantic state.
	TArray<uint8> Bytes;
	TestTrue(TEXT("SerializeVerified produces bytes"), SaveSubsystem->SerializeVerified(Save, Bytes));
	TestTrue(TEXT("Container is non-trivial"), Bytes.Num() > 32);
	UQinglanSaveGame* Loaded = nullptr;
	FText Error;
	TestTrue(TEXT("DeserializeVerified accepts the roundtrip"), SaveSubsystem->DeserializeVerified(Bytes, Loaded, Error));
	if (!TestNotNull(TEXT("Loaded save object"), Loaded))
	{
		return false;
	}
	TestTrue(TEXT("Checksum mirrored onto the object"), Loaded->BodyChecksum != 0ull);
	TestEqual(TEXT("Profile survives roundtrip"), Loaded->ProfileId, Save->ProfileId);
	TestEqual(TEXT("Sequence survives roundtrip"), Loaded->SaveSequence, Save->SaveSequence);
	TestEqual(TEXT("Shards survive roundtrip"), Loaded->Inventory.SpiritShards, Save->Inventory.SpiritShards);
	TestEqual(TEXT("Herbs survive roundtrip"), Loaded->Inventory.SpiritHerb, Save->Inventory.SpiritHerb);
	TestTrue(TEXT("Unlocked techniques survive roundtrip"), SameNameArray(Loaded->UnlockedTechniques, Save->UnlockedTechniques));
	TestTrue(TEXT("Campaign flags survive roundtrip"), SameNameArray(Loaded->CampaignFlags, Save->CampaignFlags));
	TestTrue(TEXT("Committed transactions survive roundtrip"), SameNameArray(Loaded->CommittedTransactionIds, Save->CommittedTransactionIds));
	TestTrue(TEXT("Narrative flags survive roundtrip"), SameNameArray(Loaded->Narrative.Flags, Save->Narrative.Flags));
	TestEqual(TEXT("Pending agreements survive roundtrip"), Loaded->Narrative.PendingAgreements.Num(), Save->Narrative.PendingAgreements.Num());
	TestEqual(TEXT("Effective agreements survive roundtrip"), Loaded->Narrative.EffectiveAgreements.Num(), Save->Narrative.EffectiveAgreements.Num());
	TestEqual(TEXT("Checkpoint realm survives roundtrip"), Loaded->Checkpoint.RealmId, Save->Checkpoint.RealmId);
	TestEqual(TEXT("Checkpoint room survives roundtrip"), Loaded->Checkpoint.RoomIndex, Save->Checkpoint.RoomIndex);

	// Corruption: flipping one body byte must break the checksum (docs/05 section 6).
	TArray<uint8> Corrupted = Bytes;
	Corrupted[Corrupted.Num() / 2] = static_cast<uint8>(~Corrupted[Corrupted.Num() / 2]);
	UQinglanSaveGame* CorruptLoaded = nullptr;
	TestFalse(TEXT("Corrupted container rejected"), SaveSubsystem->DeserializeVerified(Corrupted, CorruptLoaded, Error));
	TestNull(TEXT("No object escapes a corrupt container"), CorruptLoaded);

	// Truncation is rejected too.
	TArray<uint8> Truncated = Bytes;
	Truncated.SetNum(Truncated.Num() - 4);
	UQinglanSaveGame* TruncatedLoaded = nullptr;
	TestFalse(TEXT("Truncated container rejected"), SaveSubsystem->DeserializeVerified(Truncated, TruncatedLoaded, Error));
	TestNull(TEXT("No object escapes a truncated container"), TruncatedLoaded);

	// A newer schema is explicitly rejected, never silently overwritten (docs/05 section 6).
	UQinglanSaveGame* Future = MakePopulatedSave();
	Future->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion + 1;
	FText MigrationError;
	TestEqual(TEXT("Migration rejects a future schema"),
		static_cast<int32>(QinglanSave::MigrateToCurrentSchema(Future, MigrationError)),
		static_cast<int32>(EQinglanMigrationResult::RejectedFutureSchema));
	TArray<uint8> FutureBytes;
	TestFalse(TEXT("A future-schema save cannot produce a verified container"), SaveSubsystem->SerializeVerified(Future, FutureBytes));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Persistence.TransactionIdempotency - docs/03 section 6
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanTransactions, "Qinglan.Persistence.TransactionIdempotency", QinglanTestFlags)
bool FAutomationTestQinglanTransactions::RunTest(const FString& Parameters)
{
	UGameInstance* ProfileOwner = NewObject<UGameInstance>(GetTransientPackage()); // UE 5.8 ClassWithin: subsystems need a GameInstance outer
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(ProfileOwner);
	if (!TestNotNull(TEXT("Profile subsystem"), Profile))
	{
		return false;
	}

	// Grant replay: reports success but never re-awards.
	FQinglanInventory Grant;
	Grant.SpiritShards = 80;
	Grant.SpiritHerb = 1;
	TestTrue(TEXT("Grant commits"), Profile->TryGrant(FName(TEXT("tx.grant")), Grant));
	TestTrue(TEXT("Grant replay reports success"), Profile->TryGrant(FName(TEXT("tx.grant")), Grant));
	TestEqual(TEXT("Grant replay does not re-award shards"), Profile->GetInventory().SpiritShards, static_cast<int64>(80));
	TestEqual(TEXT("Grant replay does not re-award herbs"), Profile->GetInventory().SpiritHerb, static_cast<int64>(1));
	TestTrue(TEXT("Transaction is journaled"), Profile->WasTransactionCommitted(FName(TEXT("tx.grant"))));
	TestFalse(TEXT("Unknown transaction is not journaled"), Profile->WasTransactionCommitted(FName(TEXT("tx.other"))));

	// Spend replay: no double deduction.
	FQinglanInventory Cost;
	Cost.SpiritShards = 80;
	Cost.SpiritHerb = 1;
	TestTrue(TEXT("Spend commits"), Profile->TrySpend(FName(TEXT("tx.spend")), Cost));
	TestTrue(TEXT("Spend replay reports success"), Profile->TrySpend(FName(TEXT("tx.spend")), Cost));
	TestEqual(TEXT("Spend replay deducts nothing extra"), Profile->GetInventory().SpiritShards, static_cast<int64>(0));
	TestEqual(TEXT("Spend replay deducts no extra material"), Profile->GetInventory().SpiritHerb, static_cast<int64>(0));

	// Insufficient funds: atomic rejection.
	FQinglanInventory OneShard;
	OneShard.SpiritShards = 1;
	TestFalse(TEXT("Insufficient spend rejected"), Profile->TrySpend(FName(TEXT("tx.broke")), OneShard));
	TestEqual(TEXT("Rejected spend changes nothing"), Profile->GetInventory().SpiritShards, static_cast<int64>(0));
	TestFalse(TEXT("Rejected spend is not journaled"), Profile->WasTransactionCommitted(FName(TEXT("tx.broke"))));

	// Negative and overflowing grants are rejected (docs/03 section 6).
	FQinglanInventory Negative;
	Negative.SpiritShards = -1;
	TestFalse(TEXT("Negative grant rejected"), Profile->TryGrant(FName(TEXT("tx.neg")), Negative));
	FQinglanInventory Overflow;
	Overflow.SpiritShards = MAX_int64;
	TestTrue(TEXT("Max grant accepted once"), Profile->TryGrant(FName(TEXT("tx.max")), Overflow));
	TestFalse(TEXT("Overflow grant rejected"), Profile->TryGrant(FName(TEXT("tx.over")), Overflow));
	TestEqual(TEXT("Overflow left the inventory bounded"), Profile->GetInventory().SpiritShards, MAX_int64);

	// Keyless transactions are refused (stable ids are mandatory).
	TestFalse(TEXT("Keyless grant refused"), Profile->TryGrant(NAME_None, Grant));

	// Technique unlock is idempotent across transaction ids.
	TestTrue(TEXT("Technique unlock commits"), Profile->UnlockTechnique(FName(TEXT("tx.tech1")), FName(TEXT("mist_step"))));
	TestTrue(TEXT("Already-owned unlock stays a no-op success"), Profile->UnlockTechnique(FName(TEXT("tx.tech2")), FName(TEXT("mist_step"))));
	TestTrue(TEXT("Unlocked technique is owned"), Profile->IsTechniqueUnlocked(FName(TEXT("mist_step"))));
	TestFalse(TEXT("Unknown technique id rejected"), Profile->UnlockTechnique(FName(TEXT("tx.tech3")), FName(TEXT("not_a_technique"))));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Narrative.EndingRulesJsonDrift - hardcoded ending constants must
// match the authoritative CH06_S04 choice payloads in data/story/main-late.json
// (blueprint section 6 risk 3 "Unverifiable contract drift"; QinglanEndingRules.h
// header cites this exact file as its data source)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanEndingRulesJsonDrift, "Qinglan.Narrative.EndingRulesJsonDrift", QinglanTestFlags)
bool FAutomationTestQinglanEndingRulesJsonDrift::RunTest(const FString& Parameters)
{
	// The registry JSON drifts fastest; this spec loads the repo file at runtime
	// and asserts Narrative/QinglanEndingRules.h still agrees. Machines without
	// a checkout skip it (a missing file is a packaging fact, not a drift).
	const FString StoryPath = FindMainLateJsonPath();
	if (StoryPath.IsEmpty())
	{
		AddInfo(TEXT("data/story/main-late.json not found beside the project or binaries dir; skipping the JSON drift guard"));
		return true;
	}

	FString FileText;
	if (!TestTrue(TEXT("main-late.json loads"), FFileHelper::LoadFileToString(FileText, *StoryPath)))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileText);
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("main-late.json parses"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Reader->GetErrorMessage().IsEmpty()))
	{
		return false;
	}

	// Locate CH06_S04 (the ending decision platform scene).
	TSharedPtr<FJsonObject> Scene04;
	if (Root->HasField(TEXT("scenes")))
	{
		for (const TSharedPtr<FJsonValue>& Value : Root->GetFieldUntyped(TEXT("scenes"))->AsArray())
		{
			const TSharedPtr<FJsonObject> Scene = Value->AsObject();
			if (Scene.IsValid() && JsonString(Scene.ToSharedRef(), TEXT("id")) == TEXT("CH06_S04"))
			{
				Scene04 = Scene;
				break;
			}
		}
	}
	if (!TestTrue(TEXT("CH06_S04 present"), Scene04.IsValid()))
	{
		return false;
	}
	const TSharedRef<FJsonObject> Scene = Scene04.ToSharedRef();

	// The scene itself gates on Shen's release (QinglanEnding::ShenReleasedFlag;
	// blueprint section 2.3 "All require mq_shen_released").
	TestTrue(TEXT("CH06_S04 prerequisites carry the hardcoded Shen release flag"),
		JsonNameArray(Scene, TEXT("prerequisites")).Contains(FName(QinglanEnding::ShenReleasedFlag)));

	// Split the ending choices into preview_ending / confirm_ending payloads.
	TArray<TSharedRef<FJsonObject>> PreviewChoices;
	TArray<TSharedRef<FJsonObject>> ConfirmChoices;
	if (Scene->HasField(TEXT("choices")))
	{
		for (const TSharedPtr<FJsonValue>& Value : Scene->GetFieldUntyped(TEXT("choices"))->AsArray())
		{
			const TSharedPtr<FJsonObject> Choice = Value->AsObject();
			if (!Choice.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Action = JsonObject(Choice.ToSharedRef(), TEXT("action"));
			if (!Action.IsValid())
			{
				continue;
			}
			const FString Type = JsonString(Action.ToSharedRef(), TEXT("type"));
			if (Type == TEXT("preview_ending"))
			{
				PreviewChoices.Add(Choice.ToSharedRef());
			}
			else if (Type == TEXT("confirm_ending"))
			{
				ConfirmChoices.Add(Choice.ToSharedRef());
			}
		}
	}
	TestEqual(TEXT("Three authored endings to preview"), PreviewChoices.Num(), 3);
	TestEqual(TEXT("Three authored endings to confirm"), ConfirmChoices.Num(), 3);

	// Mutual exclusivity: every confirm payload lists exactly the three
	// hardcoded commit flags (QinglanEnding::Ending*Id).
	TArray<FName> ExclusiveSet;
	ExclusiveSet.Add(FName(QinglanEnding::EndingOpenId));
	ExclusiveSet.Add(FName(QinglanEnding::EndingSealId));
	ExclusiveSet.Add(FName(QinglanEnding::EndingSharedId));
	TArray<FName> SharedAgreementSet;
	for (int32 FlagIndex = 0; FlagIndex < QinglanEnding::SharedAgreementFlagCount; ++FlagIndex)
	{
		SharedAgreementSet.Add(FName(QinglanEnding::SharedAgreementFlags[FlagIndex]));
	}

	for (const TSharedRef<FJsonObject>& Confirm : ConfirmChoices)
	{
		const TSharedRef<FJsonObject> Action = JsonObject(Confirm, TEXT("action")).ToSharedRef();
		const FString EndingToken = JsonString(Action, TEXT("ending_id"));
		const FName EndingFlag = EndingFlagForToken(EndingToken);
		if (!TestTrue(FString::Printf(TEXT("Payload ending_id %s maps to a hardcoded ending flag"), *EndingToken), EndingFlag != NAME_None))
		{
			continue;
		}
		// The choice sets exactly the commit flag the hardcoded rules derive.
		TestTrue(TEXT("Confirm choice sets its hardcoded commit flag"),
			JsonNameArray(Confirm, TEXT("sets_flags")).Contains(EndingFlag));
		TestTrue(TEXT("mutually_exclusive_flags is exactly the three hardcoded ending flags"),
			SameNameArray(JsonNameArray(Action, TEXT("mutually_exclusive_flags")), ExclusiveSet));
		// ConfirmEnding hardcodes the warning gate and the one-time commit.
		TestTrue(TEXT("requires_warning_acknowledgment stays mandatory"), JsonBool(Action, TEXT("requires_warning_acknowledgment")));
		TestTrue(TEXT("commit_once stays mandatory"), JsonBool(Action, TEXT("commit_once")));
		// requires_preview names a preview_ending of the SAME ending in this
		// scene (ConfirmEnding only accepts endings it previewed).
		const FString RequiresPreview = JsonString(Action, TEXT("requires_preview"));
		TestFalse(TEXT("requires_preview is authored"), RequiresPreview.IsEmpty());
		bool bPreviewMatches = false;
		for (const TSharedRef<FJsonObject>& Preview : PreviewChoices)
		{
			const TSharedPtr<FJsonObject> PreviewAction = JsonObject(Preview, TEXT("action"));
			if (JsonString(Preview, TEXT("id")) == RequiresPreview && PreviewAction.IsValid()
				&& JsonString(PreviewAction.ToSharedRef(), TEXT("ending_id")) == EndingToken)
			{
				bPreviewMatches = true;
			}
		}
		TestTrue(TEXT("requires_preview names a same-ending preview choice"), bPreviewMatches);

		const TArray<FName> Prerequisites = JsonNameArray(Confirm, TEXT("prerequisites"));
		TestTrue(TEXT("Every confirm requires the hardcoded Shen release flag"),
			Prerequisites.Contains(FName(QinglanEnding::ShenReleasedFlag)));
		// END_SHARED additionally carries the three hardcoded agreement flags.
		if (EndingFlag == FName(QinglanEnding::EndingSharedId))
		{
			for (const FName AgreementFlag : SharedAgreementSet)
			{
				TestTrue(TEXT("Shared confirm carries every hardcoded agreement flag"), Prerequisites.Contains(AgreementFlag));
			}
			TestEqual(TEXT("Shared confirm adds only the Shen release beyond the agreements"),
				Prerequisites.Num(), QinglanEnding::SharedAgreementFlagCount + 1);
		}
	}

	// The shared PREVIEW is presented only with the three agreements
	// (blueprint section 2.3: journal shows the unlock conditions) and no
	// preview choice ever sets a commit flag (preview commits nothing,
	// docs/story/narrative-implementation.md section 4).
	for (const TSharedRef<FJsonObject>& Preview : PreviewChoices)
	{
		const TSharedPtr<FJsonObject> PreviewAction = JsonObject(Preview, TEXT("action"));
		if (!PreviewAction.IsValid())
		{
			continue;
		}
		const FName EndingFlag = EndingFlagForToken(JsonString(PreviewAction.ToSharedRef(), TEXT("ending_id")));
		if (EndingFlag == FName(QinglanEnding::EndingSharedId))
		{
			TestTrue(TEXT("Shared preview prerequisites are exactly the three hardcoded agreement flags"),
				SameNameArray(JsonNameArray(Preview, TEXT("prerequisites")), SharedAgreementSet));
		}
		const TArray<FName> SetsFlags = JsonNameArray(Preview, TEXT("sets_flags"));
		TestFalse(TEXT("Preview sets no ending_open flag"), SetsFlags.Contains(FName(QinglanEnding::EndingOpenId)));
		TestFalse(TEXT("Preview sets no ending_seal flag"), SetsFlags.Contains(FName(QinglanEnding::EndingSealId)));
		TestFalse(TEXT("Preview sets no ending_shared flag"), SetsFlags.Contains(FName(QinglanEnding::EndingSharedId)));
	}
	return true;
}

// ===========================================================================
// AUD-20 trial specs: catalog contract, deterministic hazard placement, and
// the two-phase survival machine (clock expiry requires the kill; Golden Core
// story beats fire in authored order).
//
// The phase-machine specs drive a full throwaway world: game instance (its
// Init() instantiates the subsystems), a private game world in the file's
// manual-time pattern, and a possessed player pawn so GetPlayerCharacter's
// controller-iterator fallback resolves it. Non-exported runtime classes (the
// pawn, the wave engine, enemies) are reached only through engine-level calls
// (reflection-loaded UClass spawn, virtual Tick, FindComponentByClass), so the
// modular editor build links against exported symbols alone; AQinglanTrialDirector
// and the trial-storm placement function are linkage-exported for these specs.
// ===========================================================================
namespace
{
	/**
	 * Throwaway trial world: game instance + subsystems + player pawn. The
	 * destructor always clears the story-registry injection (its contract).
	 */
	class FQinglanTrialWorld
	{
	public:
		FQinglanTrialWorld()
		{
			Instance = NewObject<UGameInstance>(GetTransientPackage());
			if (Instance == nullptr) { return; }
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (World == nullptr) { return; }
			// Register a world context: without one the world is absent from
			// the engine's world list, controller SpawnActor fails silently,
			// and every enemy is dropped as "controller-less".
			if (GEngine != nullptr)
			{
				FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
				Context.SetCurrentWorld(World);
			}
			World->SetGameInstance(Instance);
			Instance->Init(); // instantiates the game-instance subsystems

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			PlayerController = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(), FTransform::Identity, SpawnParams);
			// AQinglanCharacter is not API-exported; resolve the class by
			// reflection (no symbol linkage) and spawn through the engine type.
			UClass* PawnClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanCharacter"));
			APawn* Pawn = (PlayerController && PawnClass)
				? World->SpawnActor<APawn>(PawnClass, FTransform::Identity, SpawnParams)
				: nullptr;
			if (PlayerController && Pawn)
			{
				PlayerController->Possess(Pawn);
			}
			// Begin play so pawns with PlacedInWorldOrSpawned AI receive
			// their controller at spawn: the encounter director rejects
			// controller-less enemies, which would silently drop every wave
			// and the required boss in this headless world. BeginPlay()
			// dispatches actor callbacks; SetBegunPlay flips the flag
			// PostInitializeComponents consults for at-spawn possession.
			World->BeginPlay();
			World->SetBegunPlay(true);
		}

		~FQinglanTrialWorld()
		{
#if WITH_DEV_AUTOMATION_TESTS
			QinglanStoryRuntime::SetRegistryForTesting(nullptr); // injection contract: always clear
#endif
			if (World != nullptr)
			{
				if (GEngine != nullptr)
				{
					GEngine->WorldDestroyed(World); // drop the fixture's world context
				}
				World->DestroyWorld(false);
			}
		}

		UQinglanCombatComponent* PlayerCombat() const
		{
			// The bare CreateWorld fixture never registers a local player, so
			// GetPlayerCharacter would return null here; the possessed pawn is the
			// authority the director and these specs actually exercise.
			APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
			return Pawn ? Pawn->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
		}

		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		APlayerController* PlayerController = nullptr;
	};

	/**
	 * Minimal story registry: both trial scenes with their authored objective
	 * ids (data/story main-early.json CH03_S02_OBJECTIVE, main-late.json
	 * s02.objective), no prerequisites and no choices, so admission passes and
	 * the objective commit completes the scene headlessly.
	 */
	UQinglanStoryRegistry* MakeTrialStoryRegistry()
	{
		UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
		FQinglanRegistryScene FoundationScene;
		FoundationScene.SceneId = FName(TEXT("CH03_S02"));
		FQinglanRegistryObjective FoundationObjective;
		FoundationObjective.ObjectiveId = FName(TEXT("CH03_S02_OBJECTIVE"));
		FoundationScene.Objectives.Add(FoundationObjective);
		FQinglanRegistryScene GoldenScene;
		GoldenScene.SceneId = FName(TEXT("CH06_S02"));
		FQinglanRegistryObjective GoldenObjective;
		GoldenObjective.ObjectiveId = FName(TEXT("s02.objective"));
		GoldenScene.Objectives.Add(GoldenObjective);
		Registry->MainScenes.Add(FoundationScene);
		Registry->MainScenes.Add(GoldenScene);
		return Registry;
	}

	/** Purchases the current stage's three ranks with journaled test funds. */
	void AdvanceThreeTrialRanks(UGameInstance* Instance, const TCHAR* TxPrefix)
	{
		UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		if (Profile == nullptr) { return; }
		for (int32 Rank = 0; Rank < QinglanProgression::RanksPerStage; ++Rank)
		{
			int64 Shards = 0;
			int64 MaterialCount = 0;
			FName Material(NAME_None);
			if (!UQinglanEconomyLibrary::GetRankCost(Profile->GetCultivationStage(), Rank, Shards, MaterialCount, Material))
			{
				return;
			}
			FQinglanInventory Funds;
			Funds.SpiritShards = Shards;
			if (Material == FName(TEXT("spirit_herb")))
			{
				Funds.SpiritHerb = MaterialCount;
			}
			else
			{
				Funds.VeinCrystal = MaterialCount;
			}
			Profile->TryGrant(FName(*FString::Printf(TEXT("%s.grant.%d"), TxPrefix, Rank)), Funds);
			Profile->TryAdvanceRank(FName(*FString::Printf(TEXT("%s.rank.%d"), TxPrefix, Rank)));
		}
	}

	/** Qi Refining rank 3 + bamboo proof: exactly the Foundation admission. */
	void GrindTrialProfileToFoundationReady(UGameInstance* Instance)
	{
		AdvanceThreeTrialRanks(Instance, TEXT("tx.test.trial.f"));
		if (UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr)
		{
			Profile->SetCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag));
		}
	}

	/** FoundationEstablishment rank 3 + ashen proof: the Golden Core admission. */
	void GrindTrialProfileToGoldenCoreReady(UGameInstance* Instance)
	{
		GrindTrialProfileToFoundationReady(Instance);
		UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		if (Profile == nullptr) { return; }
		Profile->SetCampaignFlag(FName(QinglanProgression::FoundationTrialPassedCampaignFlag));
		Profile->TryBreakthrough(FName(TEXT("tx.test.trial.bt_foundation")));
		AdvanceThreeTrialRanks(Instance, TEXT("tx.test.trial.c"));
		Profile->SetCampaignFlag(FName(QinglanProgression::AshenClearedCampaignFlag));
	}

	AQinglanTrialDirector* SpawnTrialDirector(UWorld* World)
	{
		if (World == nullptr) { return nullptr; }
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		return World->SpawnActor<AQinglanTrialDirector>(
			AQinglanTrialDirector::StaticClass(), FTransform::Identity, SpawnParams);
	}

	/** The wave engine: the one actor the trial director owns. */
	AActor* FindTrialWaveEngine(FQinglanTrialWorld& Fix, AQinglanTrialDirector* Director)
	{
		if (Fix.World == nullptr || Director == nullptr) { return nullptr; }
		for (TActorIterator<AActor> It(Fix.World); It; ++It)
		{
			if (It->GetOwner() == Director)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** The required elite once the director has sighted it alive. The
	 *  director already tracks this in production; reading the same field
	 *  exercises that tracking instead of re-deriving ownership in the test. */
	ACharacter* FindTrialEnginePawn(FQinglanTrialWorld& /*Fix*/, AQinglanTrialDirector* Director)
	{
		return Director ? Cast<ACharacter>(Director->GetRequiredTarget()) : nullptr;
	}

	/**
	 * One manual world step, in dependency order: wave engine (portal trickle,
	 * materialization, encounter completion), then enemies + their controllers
	 * (FSM + portal warning), then the director, then a player top-up - the
	 * specs verify the phase machine, not the health bar.
	 */
	void TickTrialWorld(FQinglanTrialWorld& Fix, AQinglanTrialDirector* Director, float DeltaSeconds)
	{
		if (Fix.World == nullptr || Director == nullptr) { return; }

		if (AActor* Engine = FindTrialWaveEngine(Fix, Director))
		{
			Engine->Tick(DeltaSeconds);
		}

		if (AActor* Engine = FindTrialWaveEngine(Fix, Director))
		{
			TArray<ACharacter*> Enemies;
			const APawn* PlayerPawn = Fix.PlayerController ? Fix.PlayerController->GetPawn() : nullptr;
			for (TActorIterator<ACharacter> It(Fix.World); It; ++It)
			{
				ACharacter* Character = *It;
				// Tick every non-player character in this dedicated test world;
				// enemies and the required elite both live here and possession
				// rewrites Owner, so engine identity is not re-derived here.
				if (Character != nullptr && Character != PlayerPawn)
				{
					Enemies.Add(Character);
				}
			}
			for (ACharacter* Enemy : Enemies)
			{
				Enemy->Tick(DeltaSeconds);
				if (AController* Controller = Enemy->GetController())
				{
					Controller->Tick(DeltaSeconds);
				}
			}
		}

		Director->Tick(DeltaSeconds);

		if (UQinglanCombatComponent* Combat = Fix.PlayerCombat())
		{
			Combat->Heal(1000.f);
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// Qinglan.Trial.TrialCatalogSpecs - AUD-20 catalog contract: elite construct
// boss spec, Broken Guardian strike warning, boss array order
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanTrialCatalogSpecs, "Qinglan.Trial.TrialCatalogSpecs", QinglanTestFlags)
bool FAutomationTestQinglanTrialCatalogSpecs::RunTest(const FString& Parameters)
{
	// --- elite_construct: the Foundation trial's required kill ---
	const FQinglanArchetypeSpec* Elite = QinglanEnemyCatalog::FindSpec(FName(TEXT("elite_construct")));
	if (!TestNotNull(TEXT("elite_construct spec exists"), Elite))
	{
		return false;
	}
	TestTrue(TEXT("elite_construct is a boss"), Elite->bBoss);
	TestEqual(TEXT("elite_construct health"), Elite->Health, 620.f);
	TestEqual(TEXT("elite_construct defense"), Elite->Defense, 12.f);
	TestEqual(TEXT("elite_construct move speed"), Elite->MoveSpeed, 260.f);
	TestEqual(TEXT("elite_construct arena is the 60 m storm platform"), Elite->ArenaDiameter, 6000.f);
	TestEqual(TEXT("elite_construct enrage-lite at half health"), Elite->PhaseTwoAtHealthFraction, 0.5f);
	TestEqual(TEXT("elite_construct carries exactly two patterns"), Elite->Patterns.Num(), 2);

	const FQinglanBossPattern* Sweep = nullptr;
	const FQinglanBossPattern* Strike = nullptr;
	for (const FQinglanBossPattern& Pattern : Elite->Patterns)
	{
		if (Pattern.PatternId == QinglanPatternIds::SweepingBlade()) { Sweep = &Pattern; }
		else if (Pattern.PatternId == QinglanPatternIds::DelayedRoots()) { Strike = &Pattern; }
	}
	if (TestNotNull(TEXT("sweeping blade pattern present"), Sweep))
	{
		TestEqual(TEXT("sweeping blade weight"), Sweep->Weight, 2.f);
		TestTrue(TEXT("sweeping blade telegraph keeps the >= 0.9 s trial warning"), Sweep->TelegraphDuration >= 0.9f);
		TestEqual(TEXT("sweeping blade telegraph"), Sweep->TelegraphDuration, 1.1f);
		TestEqual(TEXT("sweeping blade damage"), Sweep->Damage, 24.f);
		TestEqual(TEXT("sweeping blade range"), Sweep->Range, 420.f);
		TestEqual(TEXT("sweeping blade resolves as a frontal cone"),
			static_cast<int32>(QinglanEnemyCatalog::ShapeOfPattern(*Sweep)),
			static_cast<int32>(EQinglanAttackShape::MeleeCone));
	}
	if (TestNotNull(TEXT("delayed ground strike pattern present"), Strike))
	{
		TestEqual(TEXT("ground strike weight"), Strike->Weight, 3.f);
		TestTrue(TEXT("ground strike telegraph keeps the >= 0.9 s trial warning"), Strike->TelegraphDuration >= 0.9f);
		TestEqual(TEXT("ground strike telegraph"), Strike->TelegraphDuration, 1.0f);
		TestEqual(TEXT("ground strike damage"), Strike->Damage, 20.f);
		TestEqual(TEXT("ground strike range"), Strike->Range, 550.f);
		TestTrue(TEXT("ground strike marks a persistent outline"), Strike->bGroundMarker);
		TestEqual(TEXT("ground strike resolves as a ground ring"),
			static_cast<int32>(QinglanEnemyCatalog::ShapeOfPattern(*Strike)),
			static_cast<int32>(EQinglanAttackShape::GroundRing));
	}

	// --- broken_guardian: the Golden Core required kill keeps its contract ---
	const FQinglanArchetypeSpec* Guardian = QinglanEnemyCatalog::FindSpec(FName(TEXT("broken_guardian")));
	if (!TestNotNull(TEXT("broken_guardian spec exists"), Guardian))
	{
		return false;
	}
	TestEqual(TEXT("broken_guardian enrage-lite at half health"), Guardian->PhaseTwoAtHealthFraction, 0.5f);
	const FQinglanBossPattern* Volley = nullptr;
	for (const FQinglanBossPattern& Pattern : Guardian->Patterns)
	{
		if (Pattern.PatternId == QinglanPatternIds::StormBoltVolley()) { Volley = &Pattern; }
	}
	if (TestNotNull(TEXT("storm bolt volley pattern present"), Volley))
	{
		TestTrue(TEXT("storm volley telegraph keeps the >= 0.9 s trial warning"), Volley->TelegraphDuration >= 0.9f);
		TestTrue(TEXT("storm volley marks ground outlines"), Volley->bGroundMarker);
		TestEqual(TEXT("storm volley resolves as multi ground strikes"),
			static_cast<int32>(QinglanEnemyCatalog::ShapeOfPattern(*Volley)),
			static_cast<int32>(EQinglanAttackShape::MultiGroundStrikes));
	}

	// --- Array order: the expedition fallback reads Bosses[0] ---
	const TArray<FQinglanArchetypeSpec>& Bosses = QinglanEnemyCatalog::GetAllBosses();
	TestEqual(TEXT("Four bosses ship"), Bosses.Num(), 4);
	if (Bosses.Num() >= 4)
	{
		TestEqual(TEXT("bamboo_guardian keeps the Bosses[0] fallback slot"), Bosses[0].EnemyId, FName(TEXT("bamboo_guardian")));
		TestEqual(TEXT("elite_construct appends last"), Bosses[3].EnemyId, FName(TEXT("elite_construct")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Trial.HazardPlacementDeterministic - same seed replays the same
// volley; centers honor the offset band, pairwise separation, and live cap
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanTrialHazardPlacement, "Qinglan.Trial.HazardPlacementDeterministic", QinglanTestFlags)
bool FAutomationTestQinglanTrialHazardPlacement::RunTest(const FString& Parameters)
{
	const FVector Anchor(100.f, -200.f, 0.f);

	// Contract constants (authorized spec; >= 0.9 s warnings, 300 cm radius).
	TestTrue(TEXT("Strike warning keeps the >= 0.9 s contract"), QinglanTrialStorm::StrikeWarnSeconds >= 0.9f);
	TestEqual(TEXT("Strike radius is 300 cm"), QinglanTrialStorm::StrikeRadiusCm, 300.f);
	TestEqual(TEXT("Live outline cap is four"), QinglanTrialStorm::MaxSimultaneousStrikes, 4);

	auto CheckVolley = [this, &Anchor](const TArray<FVector>& Centers, const TCHAR* Label) -> bool
	{
		bool bGeometry = true;
		for (int32 A = 0; A < Centers.Num(); ++A)
		{
			bGeometry = bGeometry && FVector::Dist2D(Centers[A], Anchor) <= QinglanTrialStorm::StrikeMaxOffsetCm + 1.0;
			for (int32 B = A + 1; B < Centers.Num(); ++B)
			{
				bGeometry = bGeometry && FVector::Dist2D(Centers[A], Centers[B]) >= QinglanTrialStorm::StrikeMinSeparationCm;
			}
		}
		return TestTrue(FString::Printf(TEXT("%s: centers within %.0f cm of the anchor, pairwise >= %.0f cm apart"),
			Label, QinglanTrialStorm::StrikeMaxOffsetCm, QinglanTrialStorm::StrikeMinSeparationCm), bGeometry);
	};

	// Determinism + geometry across every authored volley size.
	for (int32 Count = 1; Count <= QinglanTrialStorm::MaxSimultaneousStrikes; ++Count)
	{
		FRandomStream First;
		First.Initialize(9137 + Count);
		FRandomStream Second;
		Second.Initialize(9137 + Count);
		const TArray<FVector> VolleyA = QinglanTrialStorm::PickStrikeVolley(
			First, Anchor, Count, QinglanTrialStorm::StrikeMinSeparationCm, TArray<FVector>());
		const TArray<FVector> VolleyB = QinglanTrialStorm::PickStrikeVolley(
			Second, Anchor, Count, QinglanTrialStorm::StrikeMinSeparationCm, TArray<FVector>());
		TestEqual(FString::Printf(TEXT("Count %d replays the same volley size"), Count), VolleyA.Num(), VolleyB.Num());
		bool bSame = VolleyA.Num() == VolleyB.Num();
		for (int32 Index = 0; bSame && Index < VolleyA.Num(); ++Index)
		{
			bSame = VolleyA[Index].Equals(VolleyB[Index], 0.01f);
		}
		TestTrue(FString::Printf(TEXT("Count %d: same seed, same centers"), Count), bSame);
		CheckVolley(VolleyA, *FString::Printf(TEXT("Count %d"), Count));
	}

	// Live-outline cap: an over-asked volley never exceeds the maximum.
	FRandomStream Cap;
	Cap.Initialize(424242);
	TestTrue(TEXT("An over-asked volley caps at the simultaneous maximum"),
		QinglanTrialStorm::PickStrikeVolley(Cap, Anchor, 9, QinglanTrialStorm::StrikeMinSeparationCm, TArray<FVector>()).Num()
			<= QinglanTrialStorm::MaxSimultaneousStrikes);

	// Occupied field: the avoid set shrinks the budget, never the separation.
	TArray<FVector> Occupied;
	Occupied.Add(FVector(Anchor.X + 700.f, Anchor.Y, Anchor.Z));
	Occupied.Add(FVector(Anchor.X - 700.f, Anchor.Y, Anchor.Z));
	Occupied.Add(FVector(Anchor.X, Anchor.Y + 700.f, Anchor.Z));
	FRandomStream Avoid;
	Avoid.Initialize(777);
	const TArray<FVector> Extra = QinglanTrialStorm::PickStrikeVolley(
		Avoid, Anchor, 4, QinglanTrialStorm::StrikeMinSeparationCm, Occupied);
	TestTrue(TEXT("Three live outlines leave room for at most one more"), Extra.Num() <= 1);
	if (!Extra.IsEmpty())
	{
		bool bClear = true;
		for (const FVector& Live : Occupied)
		{
			bClear = bClear && FVector::Dist2D(Extra[0], Live) >= QinglanTrialStorm::StrikeMinSeparationCm;
		}
		TestTrue(TEXT("The extra strike clears every live outline"), bClear);
	}
	Occupied.Add(FVector(Anchor.X, Anchor.Y - 700.f, Anchor.Z));
	FRandomStream Saturated;
	Saturated.Initialize(778);
	TestEqual(TEXT("A saturated field schedules nothing"),
		QinglanTrialStorm::PickStrikeVolley(Saturated, Anchor, 2, QinglanTrialStorm::StrikeMinSeparationCm, Occupied).Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Trial.TimerExpiryRequiresKill - outliving the Foundation clock
// grants nothing: the phase machine moves to the required kill and proof is
// withheld until the elite construct dies
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanTrialTimerExpiry, "Qinglan.Trial.TimerExpiryRequiresKill", QinglanTestFlags)
bool FAutomationTestQinglanTrialTimerExpiry::RunTest(const FString& Parameters)
{
	FQinglanTrialWorld Fix;
	if (!TestNotNull(TEXT("Trial world"), Fix.World) || !TestNotNull(TEXT("Game instance"), Fix.Instance))
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	QinglanStoryRuntime::SetRegistryForTesting(MakeTrialStoryRegistry());
#endif
	GrindTrialProfileToFoundationReady(Fix.Instance);
	if (!TestNotNull(TEXT("Player controller"), Fix.PlayerController)
		|| !TestNotNull(TEXT("Player pawn possessed"), Fix.PlayerController->GetPawn().Get())
		|| !TestNotNull(TEXT("Player combat component"), Fix.PlayerCombat()))
	{
		return false;
	}
	AQinglanTrialDirector* Director = SpawnTrialDirector(Fix.World);
	if (!TestNotNull(TEXT("Trial director spawned"), Director))
	{
		return false;
	}

	TestTrue(TEXT("Foundation trial admitted"),
		Director->BeginTrial(FName(TEXT("CH03_S02")), FName(TEXT("CH03_S02_OBJECTIVE")), 90.f, 1));
	TestEqual(TEXT("New attempts start in the storm phase"), Director->GetTrialPhase(), EQinglanTrialPhase::Storm);
	TestTrue(TEXT("The clock phase is the timed survival phase"), Director->IsTimedSurvivalActive());

	// Manual 0.25 s steps past the 90 s clock. Enemies materialize and posture
	// but never reach the player (no navmesh, no movement ticking); storm
	// impacts are healed off between steps by the fixture.
	for (float Elapsed = 0.f; Elapsed < 95.f; Elapsed += 0.25f)
	{
		TickTrialWorld(Fix, Director, 0.25f);
		if (Director->GetTrialPhase() == EQinglanTrialPhase::RequiredKill)
		{
			break;
		}
	}
	TestEqual(TEXT("Clock expiry enters the required-kill phase"), Director->GetTrialPhase(), EQinglanTrialPhase::RequiredKill);
	TestFalse(TEXT("The timed-survival query closes with the clock"), Director->IsTimedSurvivalActive());
	TestTrue(TEXT("The trial stays active until the required elite dies"), Director->IsTrialActive());
	TestEqual(TEXT("The countdown reads zero past expiry"), Director->GetRemainingSeconds(), 0.f);

	// The storm actually ran: the three authored Foundation wave beats, in order.
	TArray<FName> Events;
	Director->GetTrialEventLog(Events);
	const FName WaveKeys[3] =
	{
		FName(TEXT("trial.lightning_wave_1")),
		FName(TEXT("trial.lightning_wave_2")),
		FName(TEXT("trial.lightning_wave_3")),
	};
	int32 LastIndex = INDEX_NONE;
	bool bInOrder = true;
	for (int32 Wave = 0; Wave < 3; ++Wave)
	{
		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Events.Num(); ++Index)
		{
			if (Events[Index] == WaveKeys[Wave])
			{
				Found = Index;
				break;
			}
		}
		TestTrue(FString::Printf(TEXT("Foundation lightning wave %d beat fired"), Wave + 1), Found != INDEX_NONE);
		bInOrder = bInOrder && Found != INDEX_NONE && Found > LastIndex;
		LastIndex = Found;
	}
	TestTrue(TEXT("The wave beats fire in authored order"), bInOrder);

	// The clock alone commits no proof.
	UQinglanStorySubsystem* Story = Fix.Instance->GetSubsystem<UQinglanStorySubsystem>();
	TestFalse(TEXT("Objective proof is withheld until the required kill"),
		QinglanStoryRuntime::IsSceneObjectiveCompleted(Story, FName(TEXT("CH03_S02")), FName(TEXT("CH03_S02_OBJECTIVE"))));
	return true;
}

// ---------------------------------------------------------------------------
// Qinglan.Trial.GoldenCoreEventOrder - the 150 s storm + shell kill fires the
// eight authored CH06_S02 beats exactly once, in order, and commits proof
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestQinglanTrialGoldenCoreOrder, "Qinglan.Trial.GoldenCoreEventOrder", QinglanTestFlags)
bool FAutomationTestQinglanTrialGoldenCoreOrder::RunTest(const FString& Parameters)
{
	FQinglanTrialWorld Fix;
	if (!TestNotNull(TEXT("Trial world"), Fix.World) || !TestNotNull(TEXT("Game instance"), Fix.Instance))
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	QinglanStoryRuntime::SetRegistryForTesting(MakeTrialStoryRegistry());
#endif
	GrindTrialProfileToGoldenCoreReady(Fix.Instance);
	if (!TestNotNull(TEXT("Player controller"), Fix.PlayerController)
		|| !TestNotNull(TEXT("Player pawn possessed"), Fix.PlayerController->GetPawn().Get())
		|| !TestNotNull(TEXT("Player combat component"), Fix.PlayerCombat()))
	{
		return false;
	}
	AQinglanTrialDirector* Director = SpawnTrialDirector(Fix.World);
	if (!TestNotNull(TEXT("Trial director spawned"), Director))
	{
		return false;
	}

	TestTrue(TEXT("Golden Core trial admitted"),
		Director->BeginTrial(FName(TEXT("CH06_S02")), FName(TEXT("s02.objective")), 150.f, 2));

	const FName ExpectedOrder[8] =
	{
		FName(TEXT("storm_time_0")),
		FName(TEXT("first_lightning_telegraph")),
		FName(TEXT("storm_time_50")),
		FName(TEXT("storm_time_100")),
		FName(TEXT("storm_survived_150")),
		FName(TEXT("shell_combat_started")),
		FName(TEXT("first_shell_recovery_window")),
		FName(TEXT("shell_defeated")),
	};
	TArray<FName> Events;
	Director->GetTrialEventLog(Events);
	TestEqual(TEXT("BeginTrial fires only the storm's opening beat"), Events.Num(), 1);
	if (Events.Num() >= 1)
	{
		TestEqual(TEXT("The opening beat is storm_time_0"), Events[0], ExpectedOrder[0]);
	}

	// --- Storm: drive the 150 s clock, checking each beat as it lands. ---
	bool bSawTime50 = false;
	bool bSawTime100 = false;
	bool bSawSurvived = false;
	for (float Elapsed = 0.f; Elapsed < 155.f; Elapsed += 0.25f)
	{
		TickTrialWorld(Fix, Director, 0.25f);
		Director->GetTrialEventLog(Events);
		if (!bSawTime50 && Events.Contains(ExpectedOrder[2]))
		{
			bSawTime50 = true;
			TestFalse(TEXT("storm_time_100 has not fired at the 50 s beat"), Events.Contains(ExpectedOrder[3]));
		}
		if (bSawTime50 && !bSawTime100 && Events.Contains(ExpectedOrder[3]))
		{
			bSawTime100 = true;
			TestFalse(TEXT("No shell beat fires before the clock expires"), Events.Contains(ExpectedOrder[5]));
		}
		if (!bSawSurvived && Events.Contains(ExpectedOrder[4]))
		{
			bSawSurvived = true;
			TestEqual(TEXT("storm_survived_150 lands with the required-kill phase"),
				Director->GetTrialPhase(), EQinglanTrialPhase::RequiredKill);
		}
		if (Director->GetTrialPhase() == EQinglanTrialPhase::RequiredKill)
		{
			break;
		}
	}
	TestTrue(TEXT("The 50 s storm boundary beat fired"), bSawTime50);
	TestTrue(TEXT("The 100 s storm boundary beat fired"), bSawTime100);
	TestTrue(TEXT("The survived-150 beat fired"), bSawSurvived);
	TestEqual(TEXT("Clock expiry enters the required-kill phase"), Director->GetTrialPhase(), EQinglanTrialPhase::RequiredKill);

	// --- Required kill: sight the shell, force its recovery window, kill it. ---
	ACharacter* Boss = nullptr;
	for (int32 Step = 0; Step < 40 && Boss == nullptr; ++Step)
	{
		TickTrialWorld(Fix, Director, 0.25f);
		Boss = FindTrialEnginePawn(Fix, Director);
	}
	if (!TestNotNull(TEXT("The Broken Guardian shell spawned"), Boss))
	{
		return false;
	}
	Director->GetTrialEventLog(Events);
	TestTrue(TEXT("Sighting the shell fires shell_combat_started"), Events.Contains(ExpectedOrder[5]));

	// Park the shell inside the player's band so the FSM walks
	// Idle -> Acquire -> Approach -> Windup -> Attack -> Recover.
	Boss->SetActorLocation(FVector(200.f, 0.f, 0.f));
	bool bRecoveryWindow = false;
	for (int32 Step = 0; Step < 300 && !bRecoveryWindow; ++Step)
	{
		TickTrialWorld(Fix, Director, 0.1f);
		Director->GetTrialEventLog(Events);
		bRecoveryWindow = Events.Contains(ExpectedOrder[6]);
	}
	TestTrue(TEXT("The shell's first recovery window fires"), bRecoveryWindow);

	UQinglanCombatComponent* BossCombat = Boss->FindComponentByClass<UQinglanCombatComponent>();
	if (!TestNotNull(TEXT("Shell combat component"), BossCombat))
	{
		return false;
	}
	FQinglanDamageEvent Lethal;
	Lethal.CastId = UQinglanCombatComponent::NextCastId();
	Lethal.HitOrdinal = 0;
	Lethal.RawMagnitude = 99999.f;
	TestTrue(TEXT("The shell dies to the killing blow"), BossCombat->ApplyDamage(Lethal));

	for (int32 Step = 0; Step < 20 && Director->IsTrialActive(); ++Step)
	{
		TickTrialWorld(Fix, Director, 0.25f);
	}
	TestFalse(TEXT("The trial resolves after the required kill"), Director->IsTrialActive());
	TestEqual(TEXT("A resolved trial rests in Idle"), Director->GetTrialPhase(), EQinglanTrialPhase::Idle);

	Director->GetTrialEventLog(Events);
	TestEqual(TEXT("Exactly the eight authored beats fired"), Events.Num(), 8);
	bool bInOrder = Events.Num() == 8;
	for (int32 Index = 0; bInOrder && Index < 8; ++Index)
	{
		bInOrder = Events[Index] == ExpectedOrder[Index];
	}
	TestTrue(TEXT("The beats fire in authored order"), bInOrder);

	UQinglanStorySubsystem* Story = Fix.Instance->GetSubsystem<UQinglanStorySubsystem>();
	TestTrue(TEXT("The Golden Core objective proof commits"),
		QinglanStoryRuntime::IsSceneObjectiveCompleted(Story, FName(TEXT("CH06_S02")), FName(TEXT("s02.objective"))));
	TestEqual(TEXT("The trial's breakthrough landed"),
		Fix.Instance->GetSubsystem<UQinglanProfileSubsystem>()->GetCultivationStage(), EQinglanStage::GoldenCore);
	return true;
}
