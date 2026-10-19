// AUD-07/08/09/18: direct helper and profile regression coverage, without a world.
// These do not prove disk durability, save migration, or gameplay entry wiring.
#include "Misc/AutomationTest.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

#include "Abilities/QinglanProjectile.h"
#include "Abilities/QinglanTechniqueExecutors.h"
#include "Abilities/QinglanTechniqueTypes.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Core/QinglanLocalizationService.h"
#include "Core/QinglanTrialAdmission.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "VFX/QinglanVFXCueComponent.h"
#include "UI/QinglanHUDStatics.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Narrative/QinglanStoryIdentity.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveMigration.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/UObjectGlobals.h"

// AUD-45 path-contract helpers live in QinglanAnimBootstrap.cpp (same module).
// Declared here because the commandlet header is intentionally frozen.
namespace QinglanAnimBootstrapPaths
{
	FString MontageDirectory(const FString& Group);
	FString MontagePackagePath(const FString& Directory, const FString& MontageName);
	FString MontageObjectPath(const FString& PackagePath, const FString& MontageName);
}

// AUD-55 texture/material contract helpers live in QinglanContentBootstrap.cpp
// (same module). These are pure string/policy functions; the test does not
// touch the content on disk.
namespace QinglanTextureSetContract
{
	FString TextureFolderPath(const FString& SetId);
	FString TexturePackagePath(const FString& SetId, const FString& Slot);
	FString TextureObjectPath(const FString& SetId, const FString& Slot);
	FString SourceFileToken(const FString& Slot);
	FString MaterialParameterName(const FString& Slot);
	bool IsSRGB(const FString& Slot);
	FString MaterialObjectPath(const FString& MaterialName);
	FString SolidMaterialAssetName(const FString& MaterialName);
	FString SolidMaterialObjectPath(const FString& MaterialName);
	bool ShouldUseSolidFallback(bool bLowQuality, bool bHasBaseColor);
	FString ResolveSurfaceMaterialObjectPath(const FString& MaterialName, bool bLowQuality, bool bHasBaseColor);
}

namespace
{
	constexpr EAutomationTestFlags AuditFrameworkTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter;

	FQinglanRegistryScene MakeIdentityScene(const TCHAR* SceneId)
	{
		FQinglanRegistryScene Scene;
		Scene.SceneId = FName(SceneId);
		Scene.Objectives.AddDefaulted_GetRef().ObjectiveId = FName(TEXT("shared"));
		Scene.Choices.AddDefaulted_GetRef().ChoiceId = FName(TEXT("shared"));
		return Scene;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditStoryIdentity,
	"Qinglan.Narrative.StoryIdentityResolution", AuditFrameworkTestFlags)

bool FQinglanAuditStoryIdentity::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryIdentity;
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	Registry->MainScenes.Add(MakeIdentityScene(TEXT("main_a")));
	Registry->MainScenes.Add(MakeIdentityScene(TEXT("main_b")));
	Registry->MainScenes[0].Objectives.AddDefaulted_GetRef().ObjectiveId = FName(TEXT("unique_objective"));
	Registry->MainScenes[0].Choices.AddDefaulted_GetRef().ChoiceId = FName(TEXT("unique_choice"));
	FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
	Quest.QuestId = FName(TEXT("side_quest"));
	Quest.Stages.Add(MakeIdentityScene(TEXT("side_stage")));

	const FName Shared(TEXT("shared"));
	const FName MainA(TEXT("main_a"));
	const FName MainB(TEXT("main_b"));
	const FName SideStage(TEXT("side_stage"));
	const FName ObjectiveA = ObjectiveKey(MainA, Shared);
	const FName ChoiceA = ChoiceKey(MainA, Shared);
	TestEqual(TEXT("Objective key format is stable"), ObjectiveA, FName(TEXT("main_a::objective::shared")));
	TestEqual(TEXT("Choice key format is stable"), ChoiceA, FName(TEXT("main_a::choice::shared")));
	TestTrue(TEXT("Member kinds have separate namespaces"), ObjectiveA != ChoiceA);
	TestTrue(TEXT("Owners have separate namespaces"), ObjectiveA != ObjectiveKey(MainB, Shared));
	TestEqual(TEXT("Ambiguous legacy objective fails closed"), ResolveObjectiveKey(*Registry, Shared), FName());
	TestEqual(TEXT("Ambiguous legacy choice fails closed"), ResolveChoiceKey(*Registry, Shared), FName());
	TestEqual(TEXT("Unique legacy objective migrates"),
		ResolveObjectiveKey(*Registry, FName(TEXT("unique_objective"))), ObjectiveKey(MainA, FName(TEXT("unique_objective"))));
	TestEqual(TEXT("Unique legacy choice migrates"),
		ResolveChoiceKey(*Registry, FName(TEXT("unique_choice"))), ChoiceKey(MainA, FName(TEXT("unique_choice"))));

	for (const FName Owner : { MainA, MainB, SideStage })
	{
		TestEqual(*FString::Printf(TEXT("Scoped objective resolves in %s"), *Owner.ToString()),
			ResolveObjectiveKey(*Registry, Shared, Owner), ObjectiveKey(Owner, Shared));
		TestEqual(*FString::Printf(TEXT("Scoped choice resolves in %s"), *Owner.ToString()),
			ResolveChoiceKey(*Registry, Shared, Owner), ChoiceKey(Owner, Shared));
		TestEqual(TEXT("Verified objective key remains stable"),
			ResolveObjectiveKey(*Registry, ObjectiveKey(Owner, Shared)), ObjectiveKey(Owner, Shared));
		TestEqual(TEXT("Verified choice key remains stable"),
			ResolveChoiceKey(*Registry, ChoiceKey(Owner, Shared)), ChoiceKey(Owner, Shared));
	}
	TestEqual(TEXT("Composite key cannot escape owner scope"), ResolveObjectiveKey(*Registry, ObjectiveA, MainB), FName());
	TestEqual(TEXT("Choice composite key cannot escape owner scope"), ResolveChoiceKey(*Registry, ChoiceA, SideStage), FName());
	TestEqual(TEXT("Objective resolver rejects choice namespace"), ResolveObjectiveKey(*Registry, ChoiceA), FName());
	TestEqual(TEXT("Choice resolver rejects objective namespace"), ResolveChoiceKey(*Registry, ObjectiveA), FName());
	TestEqual(TEXT("Unknown legacy ID is rejected"), ResolveObjectiveKey(*Registry, FName(TEXT("unknown"))), FName());
	TestEqual(TEXT("Unknown composite owner is rejected"),
		ResolveChoiceKey(*Registry, ChoiceKey(FName(TEXT("unknown")), Shared)), FName());
	TestEqual(TEXT("Empty ID is rejected"), ResolveChoiceKey(*Registry, NAME_None), FName());
	TestEqual(TEXT("Empty owner cannot form a key"), ObjectiveKey(NAME_None, Shared), FName());
	TestEqual(TEXT("Empty member cannot form a key"), ChoiceKey(MainA, NAME_None), FName());
	TestEqual(TEXT("Reserved separator in owner is rejected"), ObjectiveKey(FName(TEXT("a::b")), Shared), FName());
	TestEqual(TEXT("Reserved separator in member is rejected"), ChoiceKey(MainA, FName(TEXT("a::b"))), FName());

	Registry->MainScenes[0].Objectives.AddDefaulted_GetRef().ObjectiveId = Shared;
	Registry->MainScenes[0].Choices.AddDefaulted_GetRef().ChoiceId = Shared;
	TestEqual(TEXT("Duplicate authored objectives fail even with an owner"), ResolveObjectiveKey(*Registry, Shared, MainA), FName());
	TestEqual(TEXT("Duplicate authored choices fail even with an owner"), ResolveChoiceKey(*Registry, ChoiceA, MainA), FName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditBankingFloor,
	"Qinglan.Progression.ExactBankingFloor", AuditFrameworkTestFlags)

bool FQinglanAuditBankingFloor::RunTest(const FString& Parameters)
{
	FQinglanInventory Loot;
	Loot.SpiritShards = 100;
	Loot.SpiritHerb = 10;
	Loot.VeinCrystal = 3;
	FQinglanInventory Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, false, true);
	TestEqual(TEXT("Retreat pays exact 70 percent of shards"), Banked.SpiritShards, int64(70));
	TestEqual(TEXT("Retreat pays exact 70 percent of herbs"), Banked.SpiritHerb, int64(7));
	TestEqual(TEXT("Retreat floors each material stack"), Banked.VeinCrystal, int64(2));
	Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, false, false);
	TestEqual(TEXT("Defeat halves shards"), Banked.SpiritShards, int64(50));
	TestEqual(TEXT("Defeat floors odd material stack"), Banked.VeinCrystal, int64(1));
	Loot.SpiritShards = MAX_int64;
	Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, true, false);
	TestEqual(TEXT("Victory preserves maximum signed inventory exactly"), Banked.SpiritShards, int64(MAX_int64));
	Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(Loot, false, true);
	TestEqual(TEXT("Retreat computes large stacks without intermediate overflow"),
		Banked.SpiritShards, int64(6456360425798343064LL));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditTrialAdmission,
	"Qinglan.Progression.TrialAdmissionPolicy", AuditFrameworkTestFlags)

bool FQinglanAuditTrialAdmission::RunTest(const FString& Parameters)
{
	using namespace QinglanTrialAdmission;
	ETrial Resolved = ETrial::GoldenCore;
	TestTrue(TEXT("Foundation scene resolves"), ResolveScene(FName(TEXT("CH03_S02")), Resolved));
	TestTrue(TEXT("Foundation scene selects Foundation"), Resolved == ETrial::Foundation);
	TestTrue(TEXT("Golden Core scene resolves"), ResolveScene(FName(TEXT("CH06_S02")), Resolved));
	TestTrue(TEXT("Golden Core scene selects Golden Core"), Resolved == ETrial::GoldenCore);
	TestFalse(TEXT("Ordinary scene has no trial"), ResolveScene(FName(TEXT("CH03_S01")), Resolved));
	TestFalse(TEXT("Empty scene has no trial"), ResolveScene(NAME_None, Resolved));

	for (const ETrial Trial : { ETrial::Foundation, ETrial::GoldenCore })
	{
		const EQinglanStage RequiredStage = Trial == ETrial::Foundation
			? EQinglanStage::QiRefining : EQinglanStage::FoundationEstablishment;
		FPrerequisites State;
		State.Stage = RequiredStage;
		State.Rank = QinglanProgression::RanksPerStage;
		State.bBambooCleared = true;
		State.bAshenCleared = true;
		FResult Result = Evaluate(Trial, State);
		TestTrue(TEXT("Boss proof never substitutes for a profile"), Result.Rejection == ERejection::MissingProfile);
		TestFalse(TEXT("Missing profile is denied"), Result.IsAllowed());
		State.bHasProfile = true;

		for (const EQinglanStage Stage : { EQinglanStage::QiRefining,
			EQinglanStage::FoundationEstablishment, EQinglanStage::GoldenCore })
		{
			State.Stage = Stage;
			Result = Evaluate(Trial, State);
			TestTrue(TEXT("Admission requires the exact cultivation stage"),
				Stage == RequiredStage ? Result.IsAllowed() : Result.Rejection == ERejection::WrongStage);
		}
		State.Stage = RequiredStage;
		for (int32 Rank = 0; Rank < QinglanProgression::RanksPerStage; ++Rank)
		{
			State.Rank = Rank;
			Result = Evaluate(Trial, State);
			TestTrue(*FString::Printf(TEXT("Rank %d remains incomplete despite boss proofs"), Rank),
				Result.Rejection == ERejection::RanksIncomplete && !Result.IsAllowed());
			TestEqual(TEXT("Incomplete ranks use the localized reason"), Result.ReasonKey,
				FName(TEXT("cultivation.blocked.ranks_incomplete")));
		}
		State.Rank = QinglanProgression::RanksPerStage;
		for (int32 ProofMask = 0; ProofMask < 4; ++ProofMask)
		{
			State.bBambooCleared = (ProofMask & 1) != 0;
			State.bAshenCleared = (ProofMask & 2) != 0;
			const bool bExpectedAllowed = Trial == ETrial::Foundation ? State.bBambooCleared : State.bAshenCleared;
			Result = Evaluate(Trial, State);
			TestEqual(*FString::Printf(TEXT("Trial %d proof mask %d uses only its own boss"), int32(Trial), ProofMask),
				Result.IsAllowed(), bExpectedAllowed);
			TestTrue(TEXT("Rejection matches boss-proof status"),
				Result.Rejection == (bExpectedAllowed ? ERejection::None : ERejection::BossProofMissing));
			TestEqual(TEXT("Allowed trials clear the reason; missing proof has a localized reason"),
				Result.ReasonKey, bExpectedAllowed ? FName() : FName(TEXT("cultivation.blocked.boss_required")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditExpeditionBanking,
	"Qinglan.Persistence.ExpeditionBankingAtomicity", AuditFrameworkTestFlags)

bool FQinglanAuditExpeditionBanking::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Owner);
	if (!TestNotNull(TEXT("Profile subsystem"), Profile)) { return false; }
	const FName BankA(TEXT("tx.bank.a"));
	const FName BankB(TEXT("tx.bank.b"));
	const FName FirstClear(TEXT("tx.firstclear.realm"));
	FQinglanInventory Base;
	Base.SpiritShards = 10;
	Base.SpiritHerb = 2;
	FQinglanInventory Bonus;
	Bonus.SpiritShards = 20;
	Bonus.VeinCrystal = 3;
	TestTrue(TEXT("First bank commits both grants"), Profile->TryBankExpedition(BankA, Base, FirstClear, Bonus));
	TestEqual(TEXT("First bank includes base and bonus shards"), Profile->GetInventory().SpiritShards, int64(30));
	TestEqual(TEXT("First bank includes base materials"), Profile->GetInventory().SpiritHerb, int64(2));
	TestEqual(TEXT("First bank includes bonus materials"), Profile->GetInventory().VeinCrystal, int64(3));
	TestTrue(TEXT("Both first-bank IDs commit"),
		Profile->WasTransactionCommitted(BankA) && Profile->WasTransactionCommitted(FirstClear));
	TestTrue(TEXT("Same bank ID replays successfully"), Profile->TryBankExpedition(BankA, Base, FirstClear, Bonus));
	TestEqual(TEXT("Replay grants no shards"), Profile->GetInventory().SpiritShards, int64(30));
	TestEqual(TEXT("Replay grants no base materials"), Profile->GetInventory().SpiritHerb, int64(2));
	TestEqual(TEXT("Replay grants no bonus materials"), Profile->GetInventory().VeinCrystal, int64(3));
	TestTrue(TEXT("Fresh run banks with old first-clear ID"), Profile->TryBankExpedition(BankB, Base, FirstClear, Bonus));
	TestEqual(TEXT("Fresh run grants only base shards"), Profile->GetInventory().SpiritShards, int64(40));
	TestEqual(TEXT("Fresh run grants base materials"), Profile->GetInventory().SpiritHerb, int64(4));
	TestEqual(TEXT("First-clear bonus stays once only"), Profile->GetInventory().VeinCrystal, int64(3));
	TestTrue(TEXT("Fresh bank ID commits"), Profile->WasTransactionCommitted(BankB));

	FQinglanProfileSnapshot Snapshot;
	Snapshot.Inventory.SpiritShards = MAX_int64 - 1;
	Profile->ImportStateFromSave(Snapshot);
	Base.SpiritShards = 1;
	Bonus.SpiritShards = 1;
	TestFalse(TEXT("Bonus overflow rejects the entire bank"), Profile->TryBankExpedition(BankA, Base, FirstClear, Bonus));
	TestEqual(TEXT("Bonus overflow preserves original shards"), Profile->GetInventory().SpiritShards, int64(MAX_int64 - 1));
	TestEqual(TEXT("Bonus overflow preserves base materials"), Profile->GetInventory().SpiritHerb, int64(0));
	TestEqual(TEXT("Bonus overflow preserves bonus materials"), Profile->GetInventory().VeinCrystal, int64(0));
	TestFalse(TEXT("Failed bank ID stays uncommitted"), Profile->WasTransactionCommitted(BankA));
	TestFalse(TEXT("Failed bonus ID stays uncommitted"), Profile->WasTransactionCommitted(FirstClear));
	FQinglanProfileSnapshot After;
	Profile->ExportStateForSave(After);
	TestEqual(TEXT("Overflow inserts no journal entries"), After.CommittedTransactionIds.Num(), 0);

	Snapshot.Inventory.SpiritShards = 0;
	for (int32 Index = 0; Index < 8191; ++Index)
	{
		Snapshot.CommittedTransactionIds.Add(FName(*FString::Printf(TEXT("tx.filled.%d"), Index)));
	}
	Profile->ImportStateFromSave(Snapshot);
	TestFalse(TEXT("One free journal slot cannot fit two IDs"), Profile->TryBankExpedition(BankA, Base, FirstClear, Bonus));
	TestEqual(TEXT("Capacity rejection grants no shards"), Profile->GetInventory().SpiritShards, int64(0));
	TestFalse(TEXT("Capacity rejection commits neither ID"),
		Profile->WasTransactionCommitted(BankA) || Profile->WasTransactionCommitted(FirstClear));
	Profile->ExportStateForSave(After);
	TestEqual(TEXT("Capacity rejection preserves journal size"), After.CommittedTransactionIds.Num(), 8191);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// The override must not survive this test, including future early returns.
	class FScopedAuditStoryRegistry
	{
	public:
		explicit FScopedAuditStoryRegistry(UQinglanStoryRegistry* Registry)
		{
			QinglanStoryRuntime::SetRegistryForTesting(Registry);
		}
		~FScopedAuditStoryRegistry() { QinglanStoryRuntime::SetRegistryForTesting(nullptr); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditScopedObjectiveRuntime,
	"Qinglan.Narrative.ScopedObjectiveRuntime", AuditFrameworkTestFlags)

bool FQinglanAuditScopedObjectiveRuntime::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	for (const TCHAR* SceneId : { TEXT("audit_main_a"), TEXT("audit_main_b") })
	{
		FQinglanRegistryScene Scene = MakeIdentityScene(SceneId);
		Scene.Choices.Reset();
		Registry->MainScenes.Add(Scene);
	}
	FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
	Quest.QuestId = FName(TEXT("audit_quest"));
	Quest.Stages.Add(MakeIdentityScene(TEXT("audit_side_stage")));
	Quest.Stages[0].Choices.Reset();
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	const FName MainA(TEXT("audit_main_a"));
	const FName MainB(TEXT("audit_main_b"));
	const FName SideStage(TEXT("audit_side_stage"));
	const FName Shared(TEXT("shared"));
	Story->NotifyObjectiveCompleted(Shared);
	TestFalse(TEXT("Ambiguous legacy event cannot credit first owner"), IsSceneObjectiveCompleted(Story, MainA, Shared));
	TestFalse(TEXT("Ambiguous legacy event cannot credit second owner"), IsSceneObjectiveCompleted(Story, MainB, Shared));
	Story->NotifySceneObjectiveCompleted(MainA, QinglanStoryIdentity::ObjectiveKey(MainB, Shared));
	TestFalse(TEXT("Composite ID cannot credit a mismatched owner"), Story->IsSceneCompleted(MainA));
	Story->NotifySceneObjectiveCompleted(MainA, Shared);
	TestTrue(TEXT("Scoped event credits its objective"), IsSceneObjectiveCompleted(Story, MainA, Shared));
	TestTrue(TEXT("Scoped event completes its no-choice scene"), Story->IsSceneCompleted(MainA));
	TestFalse(TEXT("Scoped event leaves other main scene incomplete"), Story->IsSceneCompleted(MainB));
	TestFalse(TEXT("Scoped event leaves side stage incomplete"), IsSceneObjectiveCompleted(Story, SideStage, Shared));
	FQinglanNarrativeState Saved;
	ExportNarrativeState(Story, Saved);
	UQinglanStorySubsystem* Reloaded = NewObject<UQinglanStorySubsystem>(Owner);
	ImportNarrativeState(Reloaded, Saved);
	TestTrue(TEXT("Reload derives objective only for completed owner"), IsSceneObjectiveCompleted(Reloaded, MainA, Shared));
	TestFalse(TEXT("Reload cannot credit another owner's same local ID"), IsSceneObjectiveCompleted(Reloaded, MainB, Shared));
	Reloaded->NotifySceneObjectiveCompleted(MainB, Shared);
	Reloaded->NotifySceneObjectiveCompleted(SideStage, Shared);
	Reloaded->NotifySceneObjectiveCompleted(MainA, Shared);
	ExportNarrativeState(Reloaded, Saved);
	TestTrue(TEXT("Second main owner completes independently"), Reloaded->IsSceneCompleted(MainB));
	TestTrue(TEXT("Side owner completes independently"), IsSceneObjectiveCompleted(Reloaded, SideStage, Shared));
	TestEqual(TEXT("Repeated event does not duplicate main completion"), Saved.CompletedScenes.Num(), 2);
	TestEqual(TEXT("Side completion is stored separately"), Saved.CompletedSideStages.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditChoiceSaveRuntime,
	"Qinglan.Narrative.ChoiceSaveScopeMigration", AuditFrameworkTestFlags)

bool FQinglanAuditChoiceSaveRuntime::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryIdentity;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	Registry->MainScenes.Add(MakeIdentityScene(TEXT("audit_choice_a")));
	Registry->MainScenes.Add(MakeIdentityScene(TEXT("audit_choice_b")));
	Registry->MainScenes[0].Choices.AddDefaulted_GetRef().ChoiceId = FName(TEXT("unique_choice"));
	Registry->MainScenes[0].Choices.AddDefaulted_GetRef().ChoiceId = FName(TEXT("malformed_value"));
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	const FName MainA(TEXT("audit_choice_a"));
	const FName MainB(TEXT("audit_choice_b"));
	const FName Shared(TEXT("shared"));
	const FName Unique(TEXT("unique_choice"));
	FQinglanNarrativeState Saved;
	Saved.ChoiceDecisions.Add(ChoiceKey(MainA, Shared), Shared);
	Saved.ChoiceDecisions.Add(ChoiceKey(MainB, Shared), ChoiceKey(MainB, Shared));
	Saved.ChoiceDecisions.Add(Unique, Unique);
	Saved.ChoiceDecisions.Add(Shared, Shared); // Ambiguous legacy history is unrecoverable.
	Saved.ChoiceDecisions.Add(FName(TEXT("unknown")), FName(TEXT("unknown")));
	Saved.ChoiceDecisions.Add(FName(TEXT("malformed_value")), Shared);
	TArray<FName> Rejected;
	const TMap<FName, FName> Normalized = NormalizeChoiceDecisions(*Registry, Saved.ChoiceDecisions, Rejected);
	TestEqual(TEXT("Only valid scoped or uniquely owned legacy choices survive"), Normalized.Num(), 3);
	TestEqual(TEXT("Ambiguous, unknown and mismatched values are rejected"), Rejected.Num(), 3);
	QinglanStoryRuntime::ImportNarrativeState(Story, Saved);
	TestEqual(TEXT("First scoped owner restores local option value"), Story->GetSceneChoiceDecision(MainA, Shared), Shared);
	TestEqual(TEXT("Second scoped owner normalizes composite option value"), Story->GetSceneChoiceDecision(MainB, Shared), Shared);
	TestEqual(TEXT("Legacy ambiguous query refuses either owner"), Story->GetChoiceDecision(Shared), FName());
	TestEqual(TEXT("Unique legacy query resolves after migration"), Story->GetChoiceDecision(Unique), Unique);
	FQinglanNarrativeState Exported;
	QinglanStoryRuntime::ExportNarrativeState(Story, Exported);
	TestEqual(TEXT("Export persists three verified entries"), Exported.ChoiceDecisions.Num(), 3);
	TestTrue(TEXT("Export includes migrated scoped legacy key"), Exported.ChoiceDecisions.Contains(ChoiceKey(MainA, Unique)));
	TestFalse(TEXT("Export drops bare legacy keys"), Exported.ChoiceDecisions.Contains(Unique));
	TestFalse(TEXT("Export drops ambiguous bare history"), Exported.ChoiceDecisions.Contains(Shared));
	TestFalse(TEXT("Export drops invalid option value"), Exported.ChoiceDecisions.Contains(ChoiceKey(MainA, FName(TEXT("malformed_value")))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditSkipChoiceResume,
	"Qinglan.Narrative.SkipChoiceResume", AuditFrameworkTestFlags)

bool FQinglanAuditSkipChoiceResume::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("audit_skip_choice"));
	Scene.Script.SetNum(2); // Empty text keys keep this state regression independent of localization assets.
	Scene.SetsFlags = { FName(TEXT("audit_scene_committed")) };
	Scene.Objectives.AddDefaulted_GetRef().ObjectiveId = FName(TEXT("audit_decision_made"));
	FQinglanRegistryChoice& Choice = Scene.Choices.AddDefaulted_GetRef();
	Choice.ChoiceId = FName(TEXT("audit_select"));
	Choice.SetsFlags = { FName(TEXT("audit_choice_selected")) };
	Choice.ResponseKeys.Add(NAME_None); // One actual response-delivery step before the remaining script.
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	TestTrue(TEXT("Choice scene begins"), Story->BeginScene(Scene.SceneId));
	Story->SkipScene();
	TestTrue(TEXT("Skip pauses at the unresolved choice"), Story->IsScenePlaying());
	TestFalse(TEXT("Skip cannot select an option"), Story->HasFlag(Choice.SetsFlags[0]));
	TestFalse(TEXT("Skip cannot commit the scene"), Story->HasFlag(Scene.SetsFlags[0]));
	TestEqual(TEXT("Skip creates no decision history"), Story->GetSceneChoiceDecision(Scene.SceneId, Choice.ChoiceId), FName());
	Story->AdvanceLine();
	TestFalse(TEXT("Advance while choice is pending cannot complete scene"), Story->IsSceneCompleted(Scene.SceneId));
	TestTrue(TEXT("Presented option remains selectable after skip"), Story->SelectChoice(Choice.ChoiceId));
	TestTrue(TEXT("Selection applies its authored choice flag"), Story->HasFlag(Choice.SetsFlags[0]));
	TestEqual(TEXT("Selection records its scoped decision"), Story->GetSceneChoiceDecision(Scene.SceneId, Choice.ChoiceId), Choice.ChoiceId);
	TestFalse(TEXT("Selection waits for response and remaining script"), Story->IsSceneCompleted(Scene.SceneId));
	Story->AdvanceLine(); // Selected response.
	TestFalse(TEXT("Response delivery does not skip the remaining script"), Story->IsSceneCompleted(Scene.SceneId));
	Story->AdvanceLine(); // Second authored line.
	Story->AdvanceLine(); // Script exhausted: commit scene transaction.
	TestTrue(TEXT("Advance resumes after selection and completes scene"), Story->IsSceneCompleted(Scene.SceneId));
	TestFalse(TEXT("Completed scene stops playback"), Story->IsScenePlaying());
	TestTrue(TEXT("Completion applies its authored scene flag"), Story->HasFlag(Scene.SetsFlags[0]));
	TestTrue(TEXT("Completion credits the owning scene objective"),
		QinglanStoryRuntime::IsSceneObjectiveCompleted(Story, Scene.SceneId, Scene.Objectives[0].ObjectiveId));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditGroupedChoices,
	"Qinglan.Narrative.GroupedChoiceAcknowledgment", AuditFrameworkTestFlags)

bool FQinglanAuditGroupedChoices::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("audit_grouped_choice"));
	Scene.SetsFlags = { FName(TEXT("audit_consent_committed")) };
	Scene.Script.SetNum(3);
	// Use real authored ST_Game keys for pins; assertions concern state, not translated text.
	Scene.Script[0].TextKey = FName(TEXT("story.ch05.s04.lan"));
	Scene.Script[1].TextKey = FName(TEXT("story.ch05.s04.conditions"));
	Scene.Script[1].WhenAll = { FName(TEXT("audit_question_chosen")) };
	Scene.Choices.SetNum(2);
	Scene.Choices[0].ChoiceId = FName(TEXT("audit_question"));
	Scene.Choices[0].PresentationGroup = FName(TEXT("live_question"));
	Scene.Choices[0].ShowAfterTextKey = Scene.Script[0].TextKey;
	Scene.Choices[0].SetsFlags = Scene.Script[1].WhenAll;
	Scene.Choices[1].ChoiceId = FName(TEXT("audit_ack"));
	Scene.Choices[1].PresentationGroup = FName(TEXT("acknowledgment"));
	Scene.Choices[1].ShowAfterTextKey = Scene.Script[1].TextKey;
	Scene.Choices[1].Prerequisites = Scene.Script[1].WhenAll;
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	for (const bool bSkipToAcknowledgment : { false, true })
	{
		UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
		TestTrue(TEXT("Grouped scene begins at first authored pin"), Story->BeginScene(Scene.SceneId));
		TestFalse(TEXT("Hidden later acknowledgment cannot be selected early"), Story->SelectChoice(Scene.Choices[1].ChoiceId));
		TestTrue(TEXT("First pinned group accepts its own option"), Story->SelectChoice(Scene.Choices[0].ChoiceId));
		TestFalse(TEXT("First group cannot complete the scene"), Story->IsSceneCompleted(Scene.SceneId));
		TestFalse(TEXT("First group cannot set final consent flag"), Story->HasFlag(Scene.SetsFlags[0]));
		TestFalse(TEXT("Later acknowledgment stays hidden until its presentation"), Story->SelectChoice(Scene.Choices[1].ChoiceId));
		if (bSkipToAcknowledgment) { Story->SkipScene(); }
		else { Story->AdvanceLine(); } // Choice flag admits the conditional second pin.
		TestTrue(TEXT("Unresolved second group keeps scene open"), Story->IsScenePlaying());
		TestFalse(TEXT("Presenting second group cannot commit consent"), Story->IsSceneCompleted(Scene.SceneId));
		TestFalse(TEXT("Resolved earlier group cannot be selected through second group"), Story->SelectChoice(Scene.Choices[0].ChoiceId));
		TestTrue(TEXT("Presented acknowledgment can now be selected"), Story->SelectChoice(Scene.Choices[1].ChoiceId));
		for (int32 Advance = 0; Advance < 3; ++Advance) { Story->AdvanceLine(); }
		TestTrue(TEXT("Acknowledgment permits scene completion"), Story->IsSceneCompleted(Scene.SceneId));
		TestTrue(TEXT("Final scene consent flag is committed"), Story->HasFlag(Scene.SetsFlags[0]));
		for (const FQinglanRegistryChoice& Choice : Scene.Choices)
		{
			TestEqual(TEXT("Each group retains its own scoped decision"), Story->GetSceneChoiceDecision(Scene.SceneId, Choice.ChoiceId), Choice.ChoiceId);
		}
		FQinglanNarrativeState Saved;
		QinglanStoryRuntime::ExportNarrativeState(Story, Saved);
		TestEqual(TEXT("Both independently resolved group decisions survive"), Saved.ChoiceDecisions.Num(), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditAuthoredTrialAdmission,
	"Qinglan.Progression.AuthoredTrialAdmission", AuditFrameworkTestFlags)

bool FQinglanAuditAuthoredTrialAdmission::RunTest(const FString& Parameters)
{
	using namespace QinglanTrialAdmission;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Owner);
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	for (const ETrial Trial : { ETrial::Foundation, ETrial::GoldenCore })
	{
		FQinglanRegistryScene Preparation;
		Preparation.SceneId = FName(Trial == ETrial::Foundation ? TEXT("CH03_S01") : TEXT("CH06_S01"));
		Preparation.Prerequisites = { FName(TEXT("audit_entry_ready")) };
		Registry->MainScenes.Add(Preparation);
		FQinglanRegistryScene TrialScene;
		TrialScene.SceneId = SceneForTrial(Trial);
		TrialScene.Prerequisites = { FName(TEXT("audit_trial_ready")) };
		TrialScene.Objectives.AddDefaulted_GetRef().ObjectiveId = FName(TEXT("audit_trial_objective"));
		Registry->MainScenes.Add(TrialScene);
	}
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	for (const ETrial Trial : { ETrial::Foundation, ETrial::GoldenCore })
	{
		FQinglanProfileSnapshot ProfileState;
		ProfileState.CultivationStage = Trial == ETrial::Foundation ? EQinglanStage::QiRefining : EQinglanStage::FoundationEstablishment;
		ProfileState.CultivationRank = QinglanProgression::RanksPerStage;
		ProfileState.CampaignFlags = { FName(Trial == ETrial::Foundation
			? QinglanProgression::BambooClearedCampaignFlag : QinglanProgression::AshenClearedCampaignFlag) };
		Profile->ImportStateFromSave(ProfileState);
		FQinglanNarrativeState State;
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
		TestTrue(TEXT("Imported rank and campaign boss proof satisfy progression"), Evaluate(Trial, Profile, Story).IsAllowed());
		TestTrue(TEXT("Missing story cannot authorize trial entry"), EvaluateEntry(Trial, Profile, nullptr).Rejection == ERejection::MissingStory);
		TestTrue(TEXT("Entry independently requires authored preparation flags"),
			EvaluateEntry(Trial, Profile, Story).Rejection == ERejection::StoryPrerequisitesMissing);
		State.Flags.Add(FName(TEXT("audit_entry_ready")));
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
		TestTrue(TEXT("Preparation readiness admits arena travel"), EvaluateEntry(Trial, Profile, Story).IsAllowed());
		TestTrue(TEXT("Entry readiness cannot start combat before trial readiness"),
			EvaluateForTrial(Trial, Profile, Story).Rejection == ERejection::StoryPrerequisitesMissing);
		State.Flags.Add(FName(TEXT("audit_trial_ready")));
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
		TestTrue(TEXT("Authored trial readiness admits trial"), EvaluateForTrial(Trial, Profile, Story).IsAllowed());
		TestTrue(TEXT("Exact authored scene and objective admit combat"),
			EvaluateScene(SceneForTrial(Trial), FName(TEXT("audit_trial_objective")), Profile, Story).IsAllowed());
		TestTrue(TEXT("Unknown objective cannot authorize combat"),
			EvaluateScene(SceneForTrial(Trial), FName(TEXT("unknown")), Profile, Story).Rejection == ERejection::InvalidObjective);
		TestTrue(TEXT("Unknown scene cannot authorize combat"),
			EvaluateScene(FName(TEXT("CH00_S01")), FName(TEXT("audit_trial_objective")), Profile, Story).Rejection == ERejection::InvalidScene);
		State.CompletedScenes.Add(SceneForTrial(Trial));
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
		TestTrue(TEXT("Completed trial cannot replay through campaign admission"),
			EvaluateForTrial(Trial, Profile, Story).Rejection == ERejection::AlreadyCompleted);
		ProfileState.CampaignFlags.Reset();
		Profile->ImportStateFromSave(ProfileState);
		State.Flags.Add(FName(Trial == ETrial::Foundation
			? QinglanProgression::BambooClearedStoryFlag : QinglanProgression::AshenClearedStoryFlag));
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
		TestTrue(TEXT("Actual story boss alias independently satisfies progression"), Evaluate(Trial, Profile, Story).IsAllowed());
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditEndingConfirmation,
	"Qinglan.Narrative.EndingConfirmationTransaction", AuditFrameworkTestFlags)

bool FQinglanAuditEndingConfirmation::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("CH06_S04"));
	Scene.SetsFlags = { FName(TEXT("audit_ending_scene_commit")) };
	Scene.Objectives.AddDefaulted_GetRef().ObjectiveId = FName(TEXT("audit_ending_decided"));
	Scene.Choices.SetNum(3);
	Scene.Choices[0].ChoiceId = FName(TEXT("s04.preview_shared"));
	Scene.Choices[0].Action = FName(TEXT("preview_ending"));
	Scene.Choices[1].ChoiceId = FName(TEXT("s04.confirm_shared"));
	Scene.Choices[1].Action = FName(TEXT("confirm_ending"));
	Scene.Choices[1].SetsFlags = { FName(TEXT("audit_confirm_payload_commit")) };
	Scene.Choices[2].ChoiceId = FName(TEXT("s04.cancel"));
	Scene.Choices[2].Action = FName(TEXT("cancel_confirmation"));
	const FScopedAuditStoryRegistry RegistryOverride(Registry);
	FQinglanNarrativeState Eligible;
	Eligible.Flags.Add(FName(QinglanEnding::ShenReleasedFlag));
	for (const TCHAR* Agreement : QinglanEnding::SharedAgreementFlags) { Eligible.Flags.Add(FName(Agreement)); }
	QinglanStoryRuntime::ImportNarrativeState(Story, Eligible);
	const FName Shared(QinglanEnding::EndingSharedId);
	FText Error;
	TestFalse(TEXT("Acknowledgment alone cannot replace preview"), Story->ConfirmEnding(Shared, true, Error));
	TestTrue(TEXT("Ending choice scene starts"), Story->BeginScene(Scene.SceneId));
	TestTrue(TEXT("Authored preview choice is accepted"), Story->SelectChoice(Scene.Choices[0].ChoiceId));
	TestFalse(TEXT("Preview without warning acknowledgment cannot commit"), Story->ConfirmEnding(Shared, false, Error));
	TestTrue(TEXT("Authored confirmation requests an atomic commit"), Story->SelectChoice(Scene.Choices[1].ChoiceId));
	TestFalse(TEXT("Confirmation choice alone cannot apply its payload"), Story->HasFlag(Scene.Choices[1].SetsFlags[0]));
	TestTrue(TEXT("Cancel remains available after confirmation request"), Story->SelectChoice(Scene.Choices[2].ChoiceId));
	TestFalse(TEXT("Cancel clears the preview authorization"), Story->ConfirmEnding(Shared, true, Error));
	FQinglanNarrativeState AfterCancel;
	QinglanStoryRuntime::ExportNarrativeState(Story, AfterCancel);
	TestEqual(TEXT("Preview/request/cancel add no permanent flags"), AfterCancel.Flags.Num(), Eligible.Flags.Num());
	for (const FName Flag : Eligible.Flags)
	{
		TestTrue(TEXT("Preview/request/cancel retain every eligible flag"), AfterCancel.Flags.Contains(Flag));
	}
	TestTrue(TEXT("Cancel leaves ending unconfirmed"), Story->GetConfirmedEnding() == EQinglanEnding::None);
	TestFalse(TEXT("Cancel cannot complete ending scene"), Story->IsSceneCompleted(Scene.SceneId));
	TestFalse(TEXT("Cancel cannot apply scene commit flags"), Story->HasFlag(Scene.SetsFlags[0]));

	TestTrue(TEXT("Player can preview again after cancel"), Story->SelectChoice(Scene.Choices[0].ChoiceId));
	TestTrue(TEXT("Player can request confirmation again"), Story->SelectChoice(Scene.Choices[1].ChoiceId));
	TestFalse(TEXT("Repeated request still requires warning acknowledgment"), Story->ConfirmEnding(Shared, false, Error));
	TestTrue(TEXT("Preview plus acknowledged confirmation commits"), Story->ConfirmEnding(Shared, true, Error));
	TestTrue(TEXT("Exact ending enum is committed"), Story->GetConfirmedEnding() == EQinglanEnding::Shared);
	TestTrue(TEXT("Exact authored ending scene completes"), Story->IsSceneCompleted(Scene.SceneId));
	TestFalse(TEXT("Ending commit stops scene playback"), Story->IsScenePlaying());
	TestTrue(TEXT("Scene commit flags apply atomically"), Story->HasFlag(Scene.SetsFlags[0]));
	TestTrue(TEXT("Confirmation payload applies atomically"), Story->HasFlag(Scene.Choices[1].SetsFlags[0]));
	TestTrue(TEXT("Chapter closure flag applies atomically"), Story->HasFlag(FName(TEXT("mq_ch06_complete"))));
	TestTrue(TEXT("Selected ending flag is present"), Story->HasFlag(Shared));
	TestFalse(TEXT("Open ending flag is absent"), Story->HasFlag(FName(QinglanEnding::EndingOpenId)));
	TestFalse(TEXT("Seal ending flag is absent"), Story->HasFlag(FName(QinglanEnding::EndingSealId)));
	TestFalse(TEXT("Same ending cannot commit a second time"), Story->ConfirmEnding(Shared, true, Error));
	TestFalse(TEXT("Different ending cannot replace a committed ending"), Story->ConfirmEnding(FName(QinglanEnding::EndingOpenId), true, Error));
	FQinglanNarrativeState Committed;
	QinglanStoryRuntime::ExportNarrativeState(Story, Committed);
	TestEqual(TEXT("Rejected repeat commits cannot duplicate scene completion"), Committed.CompletedScenes.Num(), 1);
	return true;
}
namespace
{
	// ---------------------------------------------------------------------------
	// AUD-15/16/17: courier clock, delivery persistence, agreement revision.
	// ---------------------------------------------------------------------------

	/** Minimal SQ02-shaped protocol quest: S2 selects a proposal, S3 accepts the
	 *  verified reply (retry line for undelivered answers). Quest id "SQ02"
	 *  borrows the authored 18 h courier round trip (blueprint section 1.9). */
	UQinglanStoryRegistry* MakeAuditProtocolRegistry()
	{
		UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
		FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
		Quest.QuestId = FName(TEXT("SQ02"));
		Quest.Availability = { FName(TEXT("audit_quest_available")) };
		Quest.CompletionFlags = { FName(TEXT("sq_allocation_agreed")) }; // shared-ending agreement flag
		FQinglanRegistryScene& Select = Quest.Stages.AddDefaulted_GetRef();
		Select.SceneId = FName(TEXT("SQ02.S2"));
		Select.Prerequisites = { FName(TEXT("audit_stage_one_complete")) };
		for (const TCHAR* Option : { TEXT("a"), TEXT("b") })
		{
			FQinglanRegistryChoice& Choice = Select.Choices.AddDefaulted_GetRef();
			Choice.ChoiceId = FName(Option);
			Choice.Action = FName(TEXT("select_proposal"));
			Choice.SetsFlags = { FName(*FString::Printf(TEXT("audit_schedule_%s"), Option)) };
		}
		FQinglanRegistryScene& Accept = Quest.Stages.AddDefaulted_GetRef();
		Accept.SceneId = FName(TEXT("SQ02.S3"));
		Accept.Prerequisites = { FName(TEXT("audit_stage_two_complete")) };
		Accept.RetryKey = FName(TEXT("story.sq02.s3.retry"));
		FQinglanRegistryChoice& AcceptChoice = Accept.Choices.AddDefaulted_GetRef();
		AcceptChoice.ChoiceId = FName(TEXT("accept"));
		AcceptChoice.Action = FName(TEXT("accept_verified_proposal"));
		return Registry;
	}

	FQinglanNarrativeState MakeAuditProtocolStart()
	{
		FQinglanNarrativeState Start;
		Start.Flags.Add(FName(TEXT("audit_quest_available")));
		Start.Flags.Add(FName(TEXT("audit_stage_one_complete")));
		Start.Flags.Add(FName(TEXT("audit_stage_two_complete")));
		return Start;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditCourierDeliveryFlow,
	"Qinglan.Narrative.CourierDeliveryFlow", AuditFrameworkTestFlags)

bool FQinglanAuditCourierDeliveryFlow::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	const FScopedAuditStoryRegistry RegistryOverride(MakeAuditProtocolRegistry());
	ImportNarrativeState(Story, MakeAuditProtocolStart());
	const FName QuestId(TEXT("SQ02"));

	// Dispatch: the selection registers the pending slot and the courier clock.
	TestTrue(TEXT("Select stage begins"), Story->BeginScene(FName(TEXT("SQ02.S2"))));
	TestTrue(TEXT("Proposal option is accepted"), Story->SelectChoice(FName(TEXT("a"))));
	TestEqual(TEXT("Selection stores the pending version"), GetPendingAgreement(Story, QuestId), FName(TEXT("a")));
	TestEqual(TEXT("Effective agreement stays unset"), Story->GetEffectiveAgreement(QuestId), FName());
	const FQinglanDeliveryStatus Dispatched = QueryPendingDeliveries(Story);
	TestTrue(TEXT("Dispatch leaves a reply in flight"), Dispatched.bHasPending);
	TestEqual(TEXT("SQ02 round trip is 18 fiction hours"), Dispatched.RemainingHours, 18.0);
	TestEqual(TEXT("Pending list names one quest"), Dispatched.Quests.Num(), 1);
	TestEqual(TEXT("Pending quest is the selecting quest"), Dispatched.Quests[0], QuestId);
	TestTrue(TEXT("Select stage commits from its selection"), Story->IsSceneCompleted(FName(TEXT("SQ02.S2"))));

	// Premature confirmation: retry line queued, nothing effective, no flags.
	// The still-playing exchange is the observable for the response-key
	// override: the authored accept choice carries no responses, so only the
	// retry override keeps a line pending for the next AdvanceLine.
	TestTrue(TEXT("Accept stage begins"), Story->BeginScene(FName(TEXT("SQ02.S3"))));
	TestTrue(TEXT("Premature accept choice is selectable"), Story->SelectChoice(FName(TEXT("accept"))));
	TestTrue(TEXT("Failed verification keeps the exchange open for the retry line"), Story->IsScenePlaying());
	TestFalse(TEXT("Failed verification applies no completion flag"), Story->HasFlag(FName(TEXT("sq_allocation_agreed"))));
	TestFalse(TEXT("Failed verification completes no stage"), Story->IsSceneCompleted(FName(TEXT("SQ02.S3"))));
	TestEqual(TEXT("Failed verification keeps the effective slot empty"), Story->GetEffectiveAgreement(QuestId), FName());
	Story->AdvanceLine(); // delivers the retry response
	Story->AdvanceLine(); // non-committing selection stops the replayable stage
	TestFalse(TEXT("Retry exchange ends without completing the stage"), Story->IsScenePlaying());

	// Arrival boundary: 17.9 h falls short, the final 0.1 h lands the reply.
	TestFalse(TEXT("Early advance does not deliver"), AdvanceHubTime(Story, 17.9));
	TestTrue(TEXT("Reply is still in flight"), QueryPendingDeliveries(Story).bHasPending);
	TestTrue(TEXT("Final fraction of the round trip delivers"), AdvanceHubTime(Story, 0.1));
	TestFalse(TEXT("Delivered quest leaves the pending list"), QueryPendingDeliveries(Story).bHasPending);

	// Confirmation after arrival: flags + effective + accept stage complete.
	TestTrue(TEXT("Accept stage replays after the reply arrived"), Story->BeginScene(FName(TEXT("SQ02.S3"))));
	TestTrue(TEXT("Delivered accept choice is selectable"), Story->SelectChoice(FName(TEXT("accept"))));
	TestEqual(TEXT("Delivered accept commits the effective version"), Story->GetEffectiveAgreement(QuestId), FName(TEXT("a")));
	TestTrue(TEXT("Effective commit applies the quest completion flag"), Story->HasFlag(FName(TEXT("sq_allocation_agreed"))));
	TestTrue(TEXT("Effective commit completes the accept stage"), Story->IsSceneCompleted(FName(TEXT("SQ02.S3"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditDeliverySaveRoundtrip,
	"Qinglan.Persistence.DeliverySaveRoundtrip", AuditFrameworkTestFlags)

bool FQinglanAuditDeliverySaveRoundtrip::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	const FScopedAuditStoryRegistry RegistryOverride(MakeAuditProtocolRegistry());
	const FName QuestId(TEXT("SQ02"));
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	ImportNarrativeState(Story, MakeAuditProtocolStart());

	// Export -> Import -> Export at every checkpoint; the second export is the
	// reloaded profile's next save, so field-equality is the persistence proof.
	int32 Checkpoint = 0;
	auto VerifyRoundtrip = [this, &Story, &Checkpoint, QuestId]()
	{
		++Checkpoint;
		FQinglanNarrativeState Saved;
		ExportNarrativeState(Story, Saved);
		UQinglanStorySubsystem* Reloaded = NewObject<UQinglanStorySubsystem>(Story->GetGameInstance());
		ImportNarrativeState(Reloaded, Saved);
		FQinglanNarrativeState Resaved;
		ExportNarrativeState(Reloaded, Resaved);
		const FString Tag(*FString::Printf(TEXT("Checkpoint %d"), Checkpoint));
		TestEqual(FString::Printf(TEXT("%s: clock is faithful"), *Tag), Resaved.StoryClockSeconds, Saved.StoryClockSeconds);
		TestEqual(FString::Printf(TEXT("%s: pending slot is faithful"), *Tag),
			GetPendingAgreement(Reloaded, QuestId), GetPendingAgreement(Story, QuestId));
		TestEqual(FString::Printf(TEXT("%s: effective slot is faithful"), *Tag),
			Reloaded->GetEffectiveAgreement(QuestId), Story->GetEffectiveAgreement(QuestId));
		TestEqual(FString::Printf(TEXT("%s: effective slots never regress"), *Tag),
			Resaved.EffectiveAgreements.Num(), Saved.EffectiveAgreements.Num());
		TestEqual(FString::Printf(TEXT("%s: delivery entries are faithful"), *Tag),
			Resaved.Deliveries.Num(), Saved.Deliveries.Num());
		for (const TPair<FName, FQinglanNarrativeDelivery>& Entry : Saved.Deliveries)
		{
			const FQinglanNarrativeDelivery* Restored = Resaved.Deliveries.Find(Entry.Key);
			if (!TestNotNull(FString::Printf(TEXT("%s: delivery for %s restores"), *Tag, *Entry.Key.ToString()), Restored))
			{
				continue;
			}
			TestEqual(FString::Printf(TEXT("%s: version restores"), *Tag), Restored->Version, Entry.Value.Version);
			TestEqual(FString::Printf(TEXT("%s: dispatch stamp restores"), *Tag), Restored->DispatchedAtSeconds, Entry.Value.DispatchedAtSeconds);
			TestEqual(FString::Printf(TEXT("%s: ready stamp restores"), *Tag), Restored->ReadyAtStorySeconds, Entry.Value.ReadyAtStorySeconds);
			TestEqual(FString::Printf(TEXT("%s: arrival flag restores"), *Tag),
				Restored->bDelivered ? 1 : 0, Entry.Value.bDelivered ? 1 : 0);
		}
	};

	VerifyRoundtrip();                                                    // 1: pre-dispatch
	TestTrue(TEXT("Proposal dispatch commits"), Story->SelectProposal(QuestId, FName(TEXT("a"))));
	VerifyRoundtrip();                                                    // 2: post-dispatch (clock 0, ready 18 h)
	TestFalse(TEXT("Partial advance moves only the clock (nothing arrives yet)"), AdvanceHubTime(Story, 5.0));
	VerifyRoundtrip();                                                    // 3: pre-arrival (clock 5 h)
	TestTrue(TEXT("Round-trip advance delivers"), AdvanceHubTime(Story, 13.0));
	VerifyRoundtrip();                                                    // 4: post-arrival (clock 18 h)
	TestFalse(TEXT("Late advance moves only the clock again"), AdvanceHubTime(Story, 2.0));
	VerifyRoundtrip();                                                    // 5: pre-verify (clock 20 h)
	TestTrue(TEXT("Delivered reply verifies"), Story->DeliverAgreement(QuestId));
	VerifyRoundtrip();                                                    // 6: post-verify (effective a)
	TestEqual(TEXT("Verification journals the pending slot alongside the effective one"),
		GetPendingAgreement(Story, QuestId), FName(TEXT("a")));
	TestTrue(TEXT("Completion flag survives the verified roundtrip"),
		Story->HasFlag(FName(TEXT("sq_allocation_agreed"))));

	// Import sanitization: unrestorable entries drop instead of restoring.
	FQinglanNarrativeState Unrestorable;
	Unrestorable.StoryClockSeconds = 100.0;
	Unrestorable.PendingAgreements.Add(QuestId, FName(TEXT("a")));
	FQinglanNarrativeDelivery Premature;
	Premature.Version = FName(TEXT("a"));
	Premature.DispatchedAtSeconds = 0.0;
	Premature.ReadyAtStorySeconds = 3600.0;
	Premature.bDelivered = true; // arrival claims to predate the restored clock
	Unrestorable.Deliveries.Add(QuestId, Premature);
	Unrestorable.Deliveries.Add(FName(TEXT("SQ03")), FQinglanNarrativeDelivery()); // no live version
	UQinglanStorySubsystem* Sanitized = NewObject<UQinglanStorySubsystem>(Owner);
	ImportNarrativeState(Sanitized, Unrestorable);
	FQinglanNarrativeState SanitizedExport;
	ExportNarrativeState(Sanitized, SanitizedExport);
	TestEqual(TEXT("Import drops unrestorable deliveries"), SanitizedExport.Deliveries.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditSaveV1DeliveryMigration,
	"Qinglan.Persistence.SaveV1DeliveryMigration", AuditFrameworkTestFlags)

bool FQinglanAuditSaveV1DeliveryMigration::RunTest(const FString& Parameters)
{
	auto MakeV1Save = []()
	{
		UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>(GetTransientPackage());
		Save->SchemaVersion = 1;
		Save->Narrative.PendingAgreements.Add(FName(TEXT("SQ02")), FName(TEXT("a")));
		Save->Narrative.PendingAgreements.Add(FName(TEXT("SQ03")), FName(TEXT("b")));
		Save->Narrative.PendingAgreements.Add(FName(TEXT("SQ04")), FName()); // empty version slot
		return Save;
	};
	FText Error;
	UQinglanSaveGame* Migrated = MakeV1Save();
	TestEqual(TEXT("v1 save migrates"), int32(QinglanSave::MigrateToCurrentSchema(Migrated, Error)),
		int32(EQinglanMigrationResult::Migrated));
	TestEqual(TEXT("Migration lands on schema v2"), Migrated->SchemaVersion, 2);
	TestEqual(TEXT("v1 carried no fiction clock; migration starts one at zero"), Migrated->Narrative.StoryClockSeconds, 0.0);
	TestEqual(TEXT("Every answerable pending version gains a delivery"), Migrated->Narrative.Deliveries.Num(), 2);
	const FQinglanNarrativeDelivery* SQ02Delivery = Migrated->Narrative.Deliveries.Find(FName(TEXT("SQ02")));
	if (TestNotNull(TEXT("SQ02 delivery synthesized"), SQ02Delivery))
	{
		TestEqual(TEXT("Synthesized delivery carries the pending version"), SQ02Delivery->Version, FName(TEXT("a")));
		TestEqual(TEXT("SQ02 round trip is 18 h"), SQ02Delivery->ReadyAtStorySeconds, 18.0 * 3600.0);
		TestFalse(TEXT("Synthesized delivery has not arrived"), SQ02Delivery->bDelivered);
	}
	const FQinglanNarrativeDelivery* SQ03Delivery = Migrated->Narrative.Deliveries.Find(FName(TEXT("SQ03")));
	if (TestNotNull(TEXT("SQ03 delivery synthesized"), SQ03Delivery))
	{
		TestEqual(TEXT("SQ03 round trip is 12 h"), SQ03Delivery->ReadyAtStorySeconds, 12.0 * 3600.0);
	}
	TestFalse(TEXT("Empty pending slot is dropped, not delivered"), Migrated->Narrative.PendingAgreements.Contains(FName(TEXT("SQ04"))));
	TestTrue(TEXT("Migrated save satisfies v2 invariants"), QinglanSave::ValidateSaveInvariants(Migrated, Error));

	// Corrupt variants: each mutation of a valid v2 delivery must fail validation.
	// Their rejection logs are the validator working as intended; authorize the
	// invariant error pattern for this test so the framework does not count the
	// expected logs as failures.
	AddExpectedError(TEXT("Save invariant"), EAutomationExpectedErrorFlags::Contains, 0);
	auto RequireCorrupt = [this](const TCHAR* Label, UQinglanSaveGame* Save)
	{
		FText MutationError;
		TestFalse(FString(Label), QinglanSave::ValidateSaveInvariants(Save, MutationError));
	};
	UQinglanSaveGame* NoVersion = MakeV1Save();
	QinglanSave::MigrateToCurrentSchema(NoVersion, Error);
	NoVersion->Narrative.Deliveries[FName(TEXT("SQ02"))].Version = NAME_None;
	RequireCorrupt(TEXT("Delivery without a version is corrupt"), NoVersion);

	UQinglanSaveGame* Inverted = MakeV1Save();
	QinglanSave::MigrateToCurrentSchema(Inverted, Error);
	Inverted->Narrative.Deliveries[FName(TEXT("SQ02"))].DispatchedAtSeconds = 3600.0;
	Inverted->Narrative.Deliveries[FName(TEXT("SQ02"))].ReadyAtStorySeconds = 0.0;
	RequireCorrupt(TEXT("Reply dated before its dispatch is corrupt"), Inverted);

	UQinglanSaveGame* LateArrival = MakeV1Save();
	QinglanSave::MigrateToCurrentSchema(LateArrival, Error);
	LateArrival->Narrative.Deliveries[FName(TEXT("SQ02"))].bDelivered = true; // ready at 18 h, clock 0
	RequireCorrupt(TEXT("Arrival past the clock is corrupt"), LateArrival);

	UQinglanSaveGame* Mismatch = MakeV1Save();
	QinglanSave::MigrateToCurrentSchema(Mismatch, Error);
	Mismatch->Narrative.PendingAgreements[FName(TEXT("SQ02"))] = FName(TEXT("b"));
	RequireCorrupt(TEXT("Delivery answering a different version is corrupt"), Mismatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditAgreementRevisionFlow,
	"Qinglan.Narrative.AgreementRevisionFlow", AuditFrameworkTestFlags)

bool FQinglanAuditAgreementRevisionFlow::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	const FScopedAuditStoryRegistry RegistryOverride(MakeAuditProtocolRegistry());
	const FName QuestId(TEXT("SQ02"));
	FQinglanNarrativeState Start = MakeAuditProtocolStart();
	Start.Flags.Add(FName(QinglanEnding::ShenReleasedFlag));
	Start.Flags.Add(FName(TEXT("sq_spillway_verified")));   // SQ01 effective agreement
	Start.Flags.Add(FName(TEXT("sq_handover_accepted")));   // SQ03 effective agreement
	ImportNarrativeState(Story, Start);

	// First round trip: a becomes effective (scene-driven, so S2 completes).
	TestTrue(TEXT("Select stage begins"), Story->BeginScene(FName(TEXT("SQ02.S2"))));
	TestTrue(TEXT("First proposal is selected"), Story->SelectChoice(FName(TEXT("a"))));
	TestTrue(TEXT("Round trip delivers"), AdvanceHubTime(Story, 18.0));
	TestTrue(TEXT("First draft verifies"), Story->DeliverAgreement(QuestId));
	TestEqual(TEXT("Effective agreement is the first draft"), Story->GetEffectiveAgreement(QuestId), FName(TEXT("a")));
	FText Requirement;
	TestTrue(TEXT("Shared ending is available under agreement a"),
		Story->IsEndingAvailable(FName(QinglanEnding::EndingSharedId), Requirement));

	// Revision: the completed negotiation stage replays with live choices.
	TestTrue(TEXT("Completed negotiation stage replays as a revision"), BeginRevisionScene(Story, FName(TEXT("SQ02.S2"))));
	TestTrue(TEXT("Revision presents live choices"), Story->IsScenePlaying());
	TestTrue(TEXT("Rival proposal is selectable in the revision"), Story->SelectChoice(FName(TEXT("b"))));
	TestEqual(TEXT("Revision stores the new pending draft"), GetPendingAgreement(Story, QuestId), FName(TEXT("b")));
	TestEqual(TEXT("Effective agreement survives the reselection"), Story->GetEffectiveAgreement(QuestId), FName(TEXT("a")));
	TestEqual(TEXT("Courier clock restarts for the new draft"), QueryPendingDeliveries(Story).RemainingHours, 18.0);
	TestTrue(TEXT("Shared ending availability is unchanged by a pending draft"),
		Story->IsEndingAvailable(FName(QinglanEnding::EndingSharedId), Requirement));

	// Second round trip: b replaces a; replays commit nothing new. (The
	// sq.<quest>.reward journal is owned by the profile subsystem, which a
	// bare NewObject game instance cannot host; exactly-once is proxied by the
	// completed-stage and effective-slot counts staying put.)
	TestTrue(TEXT("Revised round trip delivers"), AdvanceHubTime(Story, 18.0));
	TestTrue(TEXT("Revised draft verifies"), Story->DeliverAgreement(QuestId));
	TestEqual(TEXT("Effective agreement is replaced by the revision"), Story->GetEffectiveAgreement(QuestId), FName(TEXT("b")));
	TestTrue(TEXT("DeliverAgreement replays as a committed no-op"), Story->DeliverAgreement(QuestId));
	FQinglanNarrativeState Settled;
	ExportNarrativeState(Story, Settled);
	TestEqual(TEXT("Replays add no duplicate stage completions"), Settled.CompletedSideStages.Num(), 2);
	TestEqual(TEXT("Replays keep exactly one effective slot"), Settled.EffectiveAgreements.Num(), 1);
	TestEqual(TEXT("Effective slot holds the revision"), Settled.EffectiveAgreements.FindChecked(QuestId), FName(TEXT("b")));

	// Guards: a confirmed ending, unknown scenes, an uncompleted stage, and an
	// unavailable quest all refuse the revision.
	FQinglanNarrativeState Decided;
	ExportNarrativeState(Story, Decided);
	Decided.ConfirmedEnding = EQinglanEnding::Shared;
	ImportNarrativeState(Story, Decided);
	TestFalse(TEXT("Confirmed ending refuses further revisions"), BeginRevisionScene(Story, FName(TEXT("SQ02.S2"))));
	TestFalse(TEXT("Unknown scene cannot revise"), BeginRevisionScene(Story, FName(TEXT("SQ02.S9"))));
	TestFalse(TEXT("Main scenes never revise"), BeginRevisionScene(Story, FName(TEXT("CH06_S04"))));
	UQinglanStorySubsystem* FreshStory = NewObject<UQinglanStorySubsystem>(Owner);
	FQinglanNarrativeState Fresh = MakeAuditProtocolStart();
	Fresh.Flags.Remove(FName(TEXT("audit_quest_available")));
	ImportNarrativeState(FreshStory, Fresh);
	TestFalse(TEXT("Unavailable quest refuses the revision"), BeginRevisionScene(FreshStory, FName(TEXT("SQ02.S2"))));
	UQinglanStorySubsystem* UnplayedStory = NewObject<UQinglanStorySubsystem>(Owner);
	ImportNarrativeState(UnplayedStory, MakeAuditProtocolStart());
	TestFalse(TEXT("Uncompleted stage refuses the revision"), BeginRevisionScene(UnplayedStory, FName(TEXT("SQ02.S2"))));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-34: consecutive dodges re-anchor the immunity clock at their own start.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditConsecutiveDodgeImmunity,
	"Qinglan.Combat.ConsecutiveDodgeImmunity", AuditFrameworkTestFlags)

bool FQinglanAuditConsecutiveDodgeImmunity::RunTest(const FString& Parameters)
{
	AActor* Dummy = NewObject<AActor>(GetTransientPackage());
	UQinglanCombatComponent* Combat = NewObject<UQinglanCombatComponent>(Dummy);
	if (!TestNotNull(TEXT("Combat component"), Combat))
	{
		return false;
	}

	// First dodge starts at t=10.0: immunity only inside [10.10, 10.28].
	Combat->NotifyDodgeStarted(10.f);
	TestEqual(TEXT("First dodge start is recorded exactly"), Combat->GetDodgeStartTime(), 10.f);
	TestFalse(TEXT("Start of dodge is not immune"), Combat->IsInvulnerableAtTime(10.05f));
	TestTrue(TEXT("Window opens at +0.10 s"), Combat->IsInvulnerableAtTime(10.10f));
	TestTrue(TEXT("Window closes at +0.28 s"), Combat->IsInvulnerableAtTime(10.28f));
	TestFalse(TEXT("Window is closed at +0.29 s"), Combat->IsInvulnerableAtTime(10.29f));

	// A second dodge starts at t=10.45, before the old 0.5 s re-arm guard
	// would have fired. Its window must be measured from 10.45, not 10.0.
	Combat->NotifyDodgeStarted(10.45f);
	TestEqual(TEXT("Consecutive dodge re-anchors the clock"), Combat->GetDodgeStartTime(), 10.45f);
	TestFalse(TEXT("Second dodge start is not immune"), Combat->IsInvulnerableAtTime(10.50f));
	TestTrue(TEXT("Second window opens at its own +0.10 s"), Combat->IsInvulnerableAtTime(10.55f));
	TestTrue(TEXT("Second window closes at its own +0.28 s"), Combat->IsInvulnerableAtTime(10.73f));
	TestFalse(TEXT("Second window is closed at its own +0.29 s"), Combat->IsInvulnerableAtTime(10.74f));
	TestFalse(TEXT("Old clock cannot grant immunity to the second dodge"), Combat->IsInvulnerableAtTime(10.29f));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-36: movement multiplier contract (root 0, boss slow 0.9, expiry 1).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditStatusMoveMultiplier,
	"Qinglan.Combat.StatusMoveMultiplier", AuditFrameworkTestFlags)

bool FQinglanAuditStatusMoveMultiplier::RunTest(const FString& Parameters)
{
	AActor* Dummy = NewObject<AActor>(GetTransientPackage());
	UQinglanStatusComponent* Status = NewObject<UQinglanStatusComponent>(Dummy);
	if (!TestNotNull(TEXT("Status component"), Status))
	{
		return false;
	}

	TestEqual(TEXT("No status: full speed"), Status->GetMoveSpeedMultiplier(), 1.f);
	TestFalse(TEXT("Not rooted initially"), Status->IsRooted());

	// Ordinary target: hard root, multiplier 0, immediate and while active.
	Status->ApplyRootMovement(1.f, /*bBossOwner=*/false);
	TestTrue(TEXT("Ordinary target is rooted"), Status->IsRooted());
	TestEqual(TEXT("Root stops movement (multiplier 0)"), Status->GetMoveSpeedMultiplier(), 0.f);
	Status->AdvanceTime(0.5f);
	TestEqual(TEXT("Root persists at half duration"), Status->GetMoveSpeedMultiplier(), 0.f);

	// Boss: downgraded to the authored 10% slow instead of a hard root.
	Status->ApplyRootMovement(1.f, /*bBossOwner=*/true);
	TestFalse(TEXT("Boss is never rooted"), Status->IsRooted());
	TestTrue(TEXT("Boss is slowed"), Status->IsSlowed());
	TestNearlyEqual(TEXT("Boss slow multiplier is 0.9"), Status->GetMoveSpeedMultiplier(), 0.9f, 1e-6f);
	Status->AdvanceTime(0.5f);
	TestNearlyEqual(TEXT("Boss slow persists at half duration"), Status->GetMoveSpeedMultiplier(), 0.9f, 1e-6f);

	// Expiry restores normal locomotion.
	Status->AdvanceTime(0.6f);
	TestFalse(TEXT("Slow expired"), Status->IsSlowed());
	TestEqual(TEXT("Expiry restores multiplier 1.0"), Status->GetMoveSpeedMultiplier(), 1.f);

	// Explicit removal (Cleansing Talisman/pool reset) also restores.
	Status->ApplyRootMovement(5.f, /*bBossOwner=*/false);
	TestEqual(TEXT("Second root applies"), Status->GetMoveSpeedMultiplier(), 0.f);
	Status->ClearRootMovement();
	TestEqual(TEXT("Explicit clear restores multiplier 1.0"), Status->GetMoveSpeedMultiplier(), 1.f);
	return true;
}

// ---------------------------------------------------------------------------
// AUD-37: tier-2 purchase state, idempotency, and the 1.15 multiplier.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditTechniqueTierBonus,
	"Qinglan.Progression.TechniqueTierBonus", AuditFrameworkTestFlags)

bool FQinglanAuditTechniqueTierBonus::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Owner);
	if (!TestNotNull(TEXT("Profile subsystem"), Profile))
	{
		return false;
	}

	FQinglanProfileSnapshot Snapshot;
	Snapshot.CultivationStage = EQinglanStage::FoundationEstablishment;
	Snapshot.CultivationRank = 0;
	Snapshot.Inventory.SpiritShards = 250;
	Snapshot.Inventory.VeinCrystal = 2;
	Snapshot.UnlockedTechniques = { FName(TEXT("jade_sword_arc")) };
	Snapshot.EquippedTechniques = { FName(TEXT("jade_sword_arc")) };
	Profile->ImportStateFromSave(Snapshot);

	const FName TechniqueId(TEXT("jade_sword_arc"));
	TestEqual(TEXT("Techniques start at tier 1"), Profile->GetTechniqueTier(TechniqueId), 1);
	TestEqual(TEXT("Tier 1 multiplier is 1.0"), Profile->GetTechniqueTierMultiplier(TechniqueId), 1.f);

	const FName UpgradeTx(TEXT("tx.tier2.jade_sword_arc"));
	TestTrue(TEXT("Tier 2 purchase commits at Foundation"), Profile->TryPurchaseTechniqueTier(UpgradeTx, TechniqueId));
	TestEqual(TEXT("Tier is now 2"), Profile->GetTechniqueTier(TechniqueId), 2);
	TestEqual(TEXT("Tier 2 multiplier is 1.15"), Profile->GetTechniqueTierMultiplier(TechniqueId),
		UQinglanEconomyLibrary::Tier2MagnitudeMultiplier);
	TestEqual(TEXT("Tier 2 charges exactly 100 shards"), Profile->GetInventory().SpiritShards, int64(150));
	TestEqual(TEXT("Tier 2 charges exactly 1 crystal"), Profile->GetInventory().VeinCrystal, int64(1));

	// Replaying the same transaction is a no-op that still reports success.
	TestTrue(TEXT("Replayed transaction succeeds"), Profile->TryPurchaseTechniqueTier(UpgradeTx, TechniqueId));
	TestEqual(TEXT("Replay does not charge shards"), Profile->GetInventory().SpiritShards, int64(150));
	TestEqual(TEXT("Replay does not charge crystals"), Profile->GetInventory().VeinCrystal, int64(1));
	TestEqual(TEXT("Replay keeps tier 2"), Profile->GetTechniqueTier(TechniqueId), 2);

	// A different transaction on an already-upgraded technique commits without
	// double-charging, matching the other purchase APIs' idempotency style.
	TestTrue(TEXT("Second transaction on upgraded technique is a no-op success"),
		Profile->TryPurchaseTechniqueTier(FName(TEXT("tx.tier2.jade_sword_arc.again")), TechniqueId));
	TestEqual(TEXT("No second charge on shards"), Profile->GetInventory().SpiritShards, int64(150));
	TestEqual(TEXT("No second charge on crystals"), Profile->GetInventory().VeinCrystal, int64(1));

	TestFalse(TEXT("Unknown technique is rejected"), Profile->TryPurchaseTechniqueTier(
		FName(TEXT("tx.tier2.unknown")), FName(TEXT("unknown_technique"))));
	TestFalse(TEXT("Empty transaction id is rejected"), Profile->TryPurchaseTechniqueTier(
		NAME_None, TechniqueId));

	// Tier-2 is unavailable before Foundation Establishment.
	UGameInstance* QiOwner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* QiProfile = NewObject<UQinglanProfileSubsystem>(QiOwner);
	FQinglanProfileSnapshot QiSnapshot = Snapshot;
	QiSnapshot.CultivationStage = EQinglanStage::QiRefining;
	QiProfile->ImportStateFromSave(QiSnapshot);
	TestFalse(TEXT("Qi Refining cannot buy tier 2"),
		QiProfile->TryPurchaseTechniqueTier(FName(TEXT("tx.tier2.qi")), TechniqueId));
	TestEqual(TEXT("Rejected Qi purchase keeps tier 1"), QiProfile->GetTechniqueTier(TechniqueId), 1);
	TestEqual(TEXT("Rejected Qi purchase charges nothing"), QiProfile->GetInventory().SpiritShards, int64(250));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-43: every cue light is tracked; decay and pool reset return to baseline.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditCueLightBookkeeping,
	"Qinglan.VFX.CueLightBookkeeping", AuditFrameworkTestFlags)

bool FQinglanAuditCueLightBookkeeping::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World))
	{
		return false;
	}
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	AActor* OwnerActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!TestNotNull(TEXT("Cue owner"), OwnerActor))
	{
		World->DestroyWorld(false);
		return false;
	}
	UQinglanVFXCueComponent* Cues = NewObject<UQinglanVFXCueComponent>(OwnerActor);
	OwnerActor->AddOwnedComponent(Cues);
	Cues->RegisterComponent();

	const FName CueId(TEXT("vfx.sword_impact"));
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Cues->PlayCue(CueId, FTransform::Identity, FVector::ZeroVector, EQinglanElement::Fire, 1.f);
	}
	TestEqual(TEXT("100 cues are tracked"), Cues->GetActiveCueCount(), 100);
	TestTrue(TEXT("At least one cue light is tracked"), Cues->GetActiveLightCount() > 0);
	TestTrue(TEXT("Cue lights respect the bounded budget"), Cues->GetActiveLightCount() <= 16);

	// Decay: all cue components and every stored light are destroyed together.
	Cues->TickComponent(1.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Decay releases all cue instances"), Cues->GetActiveCueCount(), 0);
	TestEqual(TEXT("Decay destroys all tracked lights"), Cues->GetActiveLightCount(), 0);

	// Pool reset also returns to baseline after another burst.
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Cues->PlayCue(CueId, FTransform::Identity, FVector::ZeroVector, EQinglanElement::Fire, 1.f);
	}
	Cues->ResetForPool();
	TestEqual(TEXT("Pool reset clears cue instances"), Cues->GetActiveCueCount(), 0);
	TestEqual(TEXT("Pool reset clears cue lights"), Cues->GetActiveLightCount(), 0);

	World->DestroyWorld(false);
	return true;
}

// ---------------------------------------------------------------------------
// AUD-11: gameplay-proof guard evaluation and confirmation gating.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditGuardBlocksDialogueOnlyProgress,
	"Qinglan.Narrative.GuardBlocksDialogueOnlyProgress", AuditFrameworkTestFlags)

bool FQinglanAuditGuardBlocksDialogueOnlyProgress::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("audit_guard_scene"));
	FQinglanRegistryObjective& Objective = Scene.Objectives.AddDefaulted_GetRef();
	Objective.ObjectiveId = FName(TEXT("audit_guard_objective"));
	const FName EventA(TEXT("audit.proof.a"));
	const FName EventB(TEXT("audit.proof.b"));
	const FName EventC(TEXT("audit.proof.c"));
	for (const FName Event : { EventA, EventB, EventC })
	{
		FQinglanRegistryGuardTerm& Term = Objective.Guard.AllOf.AddDefaulted_GetRef();
		Term.Kind = FName(TEXT("event"));
		Term.Event = Event;
	}
	const FScopedAuditStoryRegistry RegistryOverride(Registry);

	// Dialogue playback alone never advances an authored proof: with no
	// gameplay events the guard is Missing and the objective stays open.
	FQinglanGuardReport Report = EvaluateObjectiveGuard(Story, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Unauthored progress is missing"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Missing));
	TestEqual(TEXT("Every authored term is missing"), Report.Missing.Num(), 3);
	TestEqual(TEXT("No term is satisfied yet"), Report.Progress, 0);
	TestEqual(TEXT("Report counts the authored terms"), Report.Total, 3);
	TestFalse(TEXT("Dialogue alone does not credit the objective"),
		IsSceneObjectiveCompleted(Story, Scene.SceneId, Objective.ObjectiveId));

	NotifyGameplayEvent(Story, EventA, {});
	Report = EvaluateObjectiveGuard(Story, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("First proof event advances progress"), Report.Progress, 1);
	TestEqual(TEXT("Two terms remain"), Report.Missing.Num(), 2);
	TestFalse(TEXT("Partial proof cannot credit the objective"),
		IsSceneObjectiveCompleted(Story, Scene.SceneId, Objective.ObjectiveId));

	NotifyGameplayEvent(Story, EventB, {});
	Report = EvaluateObjectiveGuard(Story, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Second proof event advances progress"), Report.Progress, 2);
	TestEqual(TEXT("One term remains"), Report.Missing.Num(), 1);

	NotifyGameplayEvent(Story, EventC, {});
	Report = EvaluateObjectiveGuard(Story, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("All proof events satisfy the guard"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Satisfied));
	TestEqual(TEXT("Every term is satisfied"), Report.Progress, 3);
	TestTrue(TEXT("Satisfied guard auto-completes the objective"),
		IsSceneObjectiveCompleted(Story, Scene.SceneId, Objective.ObjectiveId));
	TestTrue(TEXT("Choice-free scene commits through its objective"), Story->IsSceneCompleted(Scene.SceneId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditOrderedGuardReset,
	"Qinglan.Narrative.OrderedGuardReset", AuditFrameworkTestFlags)

bool FQinglanAuditOrderedGuardReset::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("audit_ordered_scene"));
	FQinglanRegistryObjective& Objective = Scene.Objectives.AddDefaulted_GetRef();
	Objective.ObjectiveId = FName(TEXT("audit_ordered_objective"));
	FQinglanRegistryGuardTerm& Ordered = Objective.Guard.AllOf.AddDefaulted_GetRef();
	Ordered.Kind = FName(TEXT("ordered"));
	const FName StepA(TEXT("audit.step.a"));
	const FName StepB(TEXT("audit.step.b"));
	const FName StepC(TEXT("audit.step.c"));
	Ordered.OrderedSteps = { StepA, StepB, StepC };
	const FScopedAuditStoryRegistry RegistryOverride(Registry);

	UQinglanStorySubsystem* InOrder = NewObject<UQinglanStorySubsystem>(Owner);
	for (const FName Step : Ordered.OrderedSteps)
	{
		NotifyGameplayEvent(InOrder, Step, {});
	}
	FQinglanGuardReport Report = EvaluateObjectiveGuard(InOrder, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Authored order satisfies the guard"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Satisfied));
	TestEqual(TEXT("No step is missing"), Report.Missing.Num(), 0);
	TestTrue(TEXT("Ordered proof auto-completes the objective"),
		IsSceneObjectiveCompleted(InOrder, Scene.SceneId, Objective.ObjectiveId));

	UQinglanStorySubsystem* OutOfOrder = NewObject<UQinglanStorySubsystem>(Owner);
	NotifyGameplayEvent(OutOfOrder, StepC, {});
	NotifyGameplayEvent(OutOfOrder, StepA, {});
	NotifyGameplayEvent(OutOfOrder, StepB, {});
	Report = EvaluateObjectiveGuard(OutOfOrder, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Out-of-order step resets the sequence"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::OrderReset));
	TestEqual(TEXT("Only the out-of-order step remains missing"), Report.Missing.Num(), 1);
	TestEqual(TEXT("The missing step is the one fired early"), Report.Missing[0], StepC);
	TestFalse(TEXT("Out-of-order proof cannot credit the objective"),
		IsSceneObjectiveCompleted(OutOfOrder, Scene.SceneId, Objective.ObjectiveId));

	ResetOrderedProgress(OutOfOrder, Scene.SceneId);
	Report = EvaluateObjectiveGuard(OutOfOrder, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Reset returns the guard to its initial state"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Missing));
	TestEqual(TEXT("Reset clears every sequence step"), Report.Missing.Num(), 3);
	TestTrue(TEXT("Reset restores the first expected step"), Report.Missing.Contains(StepA));

	// A retried in-order sequence satisfies the guard after the reset.
	for (const FName Step : Ordered.OrderedSteps)
	{
		NotifyGameplayEvent(OutOfOrder, Step, {});
	}
	Report = EvaluateObjectiveGuard(OutOfOrder, Scene.SceneId, Objective.ObjectiveId);
	TestEqual(TEXT("Retried order satisfies the guard"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Satisfied));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditConfirmationGuardGate,
	"Qinglan.Narrative.ConfirmationGuardGate", AuditFrameworkTestFlags)

bool FQinglanAuditConfirmationGuardGate::RunTest(const FString& Parameters)
{
	using namespace QinglanStoryRuntime;
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>(GetTransientPackage());
	FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
	Quest.QuestId = FName(TEXT("audit_gate_quest"));
	FQinglanRegistryScene& Stage = Quest.Stages.AddDefaulted_GetRef();
	Stage.SceneId = FName(TEXT("audit_gate_s3"));
	Stage.ScriptTrigger = FName(TEXT("confirmation_checks_ready"));
	const FName RequiredFlag(TEXT("audit_gate_flag"));
	const FName RequiredEvent(TEXT("audit_gate_event"));
	FQinglanRegistryGuardTerm& FlagTerm = Stage.ConfirmationGuardTerms.AllOf.AddDefaulted_GetRef();
	FlagTerm.Kind = FName(TEXT("flag"));
	FlagTerm.Flag = RequiredFlag;
	FQinglanRegistryGuardTerm& EventTerm = Stage.ConfirmationGuardTerms.AllOf.AddDefaulted_GetRef();
	EventTerm.Kind = FName(TEXT("event"));
	EventTerm.Event = RequiredEvent;
	const FScopedAuditStoryRegistry RegistryOverride(Registry);

	TestTrue(TEXT("Structured confirmation guard is detected"), SceneHasCommitGuard(Story, Stage.SceneId));
	FQinglanGuardReport Report = EvaluateSceneCommitGuard(Story, Stage.SceneId);
	TestEqual(TEXT("Confirmation checks start unmet"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Missing));
	TestFalse(TEXT("Confirmation scene refuses to begin before its checks"),
		Story->BeginScene(Stage.SceneId));

	// One satisfied term is not enough.
	FQinglanNarrativeState Partial;
	Partial.Flags.Add(RequiredFlag);
	ImportNarrativeState(Story, Partial);
	TestFalse(TEXT("Flag alone leaves the confirmation gate closed"), Story->BeginScene(Stage.SceneId));

	NotifyGameplayEvent(Story, RequiredEvent, {});
	Report = EvaluateSceneCommitGuard(Story, Stage.SceneId);
	TestEqual(TEXT("Both authored checks satisfy the confirmation guard"), static_cast<int32>(Report.Status), static_cast<int32>(EQinglanGuardStatus::Satisfied));
	TestTrue(TEXT("Confirmation scene begins once its checks are ready"),
		Story->BeginScene(Stage.SceneId));
	return true;
}

// AUD-48: both shipped locales resolve from the runtime service; a key missing
// from zh-Hans falls back to en-US, and a key missing everywhere falls back to
// the key itself. Tables are injected, so this stays headless and disk-free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditLocalizationResolve,
	"Qinglan.Localization.BilingualKeyParityAndResolve", AuditFrameworkTestFlags)

bool FQinglanAuditLocalizationResolve::RunTest(const FString& Parameters)
{
	TMap<FString, FString> English;
	TMap<FString, FString> Chinese;
	English.Add(TEXT("game.title"), TEXT("Qinglan: Path of Ascension"));
	Chinese.Add(TEXT("game.title"), TEXT("青岚问道"));
	English.Add(TEXT("hud.clock"), TEXT("{minutes}:{seconds}"));
	Chinese.Add(TEXT("hud.clock"), TEXT("{minutes}:{seconds}"));
	English.Add(TEXT("only.english"), TEXT("English only"));
	Chinese.Add(TEXT("only.english"), TEXT("English only"));

	// Bilingual key parity: same authored key set before testing fallback.
	TArray<FString> EnglishKeys;
	TArray<FString> ChineseKeys;
	English.GetKeys(EnglishKeys);
	Chinese.GetKeys(ChineseKeys);
	EnglishKeys.Sort();
	ChineseKeys.Sort();
	TestEqual(TEXT("Bilingual tables declare the same key count"), EnglishKeys.Num(), ChineseKeys.Num());
	for (int32 KeyIndex = 0; KeyIndex < EnglishKeys.Num() && KeyIndex < ChineseKeys.Num(); ++KeyIndex)
	{
		TestEqual(*FString::Printf(TEXT("Bilingual key %d is shared"), KeyIndex), ChineseKeys[KeyIndex], EnglishKeys[KeyIndex]);
	}

	// Exercise the en fallback with a deliberately zh-missing key.
	Chinese.Remove(TEXT("only.english"));
	FQinglanLocalizationService::SetTranslationTablesForTesting(English, Chinese);

	TestTrue(TEXT("Injected bilingual resources are reported loaded"), FQinglanLocalizationService::AreResourcesLoaded());
	TestEqual(TEXT("en-US resolves the source string"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("game.title")), TEXT("en-US")).ToString(),
		FString(TEXT("Qinglan: Path of Ascension")));
	TestEqual(TEXT("zh-Hans resolves the translated string"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("game.title")), TEXT("zh-Hans")).ToString(),
		FString(TEXT("青岚问道")));
	TestEqual(TEXT("A regional Chinese culture normalizes to zh-Hans"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("game.title")), TEXT("zh-Hans-CN")).ToString(),
		FString(TEXT("青岚问道")));
	TestEqual(TEXT("A key missing from zh-Hans falls back to en-US"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("only.english")), TEXT("zh-Hans")).ToString(),
		FString(TEXT("English only")));
	TestEqual(TEXT("A key missing everywhere falls back to the key"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("missing.key")), TEXT("zh-Hans")).ToString(),
		FString(TEXT("missing.key")));
	TestEqual(TEXT("Named placeholders survive translation"),
		FQinglanLocalizationService::ResolveForCulture(FName(TEXT("hud.clock")), TEXT("zh-Hans")).ToString(),
		FString(TEXT("{minutes}:{seconds}")));
	TestTrue(TEXT("HasKey sees the English fallback"), FQinglanLocalizationService::HasKey(FName(TEXT("only.english"))));
	TestFalse(TEXT("HasKey rejects an absent key"), FQinglanLocalizationService::HasKey(FName(TEXT("missing.key"))));

	FQinglanLocalizationService::ResetTranslationTablesForTesting();
	return true;
}

// AUD-49: the startup decision must be driven by the first-launch flag sampled
// before profile 0 exists, and an explicit persisted preference must win.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditFirstLaunchLocaleOrdering,
	"Qinglan.Localization.FirstLaunchOrdering", AuditFrameworkTestFlags)

bool FQinglanAuditFirstLaunchLocaleOrdering::RunTest(const FString& Parameters)
{
	// A truly new profile (captured before CreateNewProfile) on a Chinese OS.
	bool bShouldPersistDetected = false;
	FString Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ true, FString(), TEXT("zh-Hans-CN"), bShouldPersistDetected);
	TestEqual(TEXT("Chinese OS on first launch selects zh-Hans"), Culture, FString(TEXT("zh-Hans")));
	TestTrue(TEXT("First-launch detection is persisted as the later preference"), bShouldPersistDetected);

	Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ true, FString(), TEXT("en-US"), bShouldPersistDetected);
	TestEqual(TEXT("English OS on first launch selects en-US"), Culture, FString(TEXT("en-US")));

	// The pre-fix ordering sampled first-launch after profile creation; this is
	// exactly that scenario and must fall through to the stable default.
	Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ false, FString(), TEXT("zh-Hans-CN"), bShouldPersistDetected);
	TestEqual(TEXT("A returning launch without a preference defaults to en-US"), Culture, FString(TEXT("en-US")));
	TestFalse(TEXT("A returning launch does not clear or overwrite a preference"), bShouldPersistDetected);

	// Explicit player preference outranks first-launch OS detection.
	Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ true, TEXT("en-US"), TEXT("zh-Hans-CN"), bShouldPersistDetected);
	TestEqual(TEXT("Explicit preference wins over first-launch OS detection"), Culture, FString(TEXT("en-US")));
	TestFalse(TEXT("Explicit preference is never overwritten by detection"), bShouldPersistDetected);

	Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ false, TEXT("zh-Hans"), TEXT("en-US"), bShouldPersistDetected);
	TestEqual(TEXT("Persisted preference is re-applied on later launches"), Culture, FString(TEXT("zh-Hans")));

	Culture = FQinglanLocalizationService::ResolveStartupCulture(
		/*bFirstLaunch*/ false, TEXT("fr-FR"), TEXT("en-US"), bShouldPersistDetected);
	TestEqual(TEXT("An unsupported stored value falls back to en-US"), Culture, FString(TEXT("en-US")));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-45: every bootstrap montage owns an independent package whose object
// name matches the file name, and the runtime sword fallback paths use the
// same contract. Pure path assertions, so the test does not require bootstrap
// to run (and does not depend on the currently stale on-disk assets). The
// skeleton half of the contract is enforced at bootstrap run time by
// VerifyMontageContract (loadable + non-null skeleton + skeleton == source).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditAnimMontagePathContract,
	"Qinglan.Anim.MontagePathContract", AuditFrameworkTestFlags)

bool FQinglanAuditAnimMontagePathContract::RunTest(const FString& Parameters)
{
	using namespace QinglanAnimBootstrapPaths;

	const FString HumanDir = MontageDirectory(TEXT("Human"));
	TestEqual(TEXT("Human montage directory follows the mesh layout"),
		HumanDir, FString(TEXT("/Game/Qinglan/Characters/Human/Montages")));
	const FString MonstersDir = MontageDirectory(TEXT("Monsters"));
	TestEqual(TEXT("Monster montage directory follows the mesh layout"),
		MonstersDir, FString(TEXT("/Game/Qinglan/Characters/Monsters/Montages")));

	const FString SwordCut1(TEXT("AM_Human_SwordCut_1"));
	const FString CutPackage = MontagePackagePath(HumanDir, SwordCut1);
	TestEqual(TEXT("Montage package path is directory/name"),
		CutPackage, HumanDir / SwordCut1);
	TestTrue(TEXT("Package path is not the shared directory (AUD-45 regression)"),
		CutPackage != HumanDir);
	TestEqual(TEXT("Package short name matches the montage name"),
		CutPackage.RightChop(CutPackage.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1),
		SwordCut1);

	const FString CutObject = MontageObjectPath(CutPackage, SwordCut1);
	TestEqual(TEXT("Object path is package.object"),
		CutObject, FString::Printf(TEXT("%s.%s"), *CutPackage, *SwordCut1));
	TestEqual(TEXT("Object name suffix equals the montage name"),
		CutObject.RightChop(CutObject.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1),
		SwordCut1);

	// Each cut must be its own loadable package, never one shared package.
	TSet<FString> DistinctPackages;
	for (int32 CutIndex = 1; CutIndex <= 3; ++CutIndex)
	{
		const FString Name = FString::Printf(TEXT("AM_Human_SwordCut_%d"), CutIndex);
		DistinctPackages.Add(MontagePackagePath(HumanDir, Name));
	}
	TestEqual(TEXT("Three cuts produce three distinct packages"), DistinctPackages.Num(), 3);

	// Monster groups can carry different rigs; the contract is per-montage, so
	// same-suffix montages across groups never share a package or object name.
	const FString MonsterAttack = MontageObjectPath(MontagePackagePath(MonstersDir, TEXT("AM_Monsters_Attack")), TEXT("AM_Monsters_Attack"));
	const FString HumanCut = MontageObjectPath(CutPackage, SwordCut1);
	TestTrue(TEXT("Human and Monster montage objects are distinct"), MonsterAttack != HumanCut);
	TestTrue(TEXT("Monster package lives in its own group directory"),
		MontagePackagePath(MonstersDir, TEXT("AM_Monsters_Attack")).StartsWith(MonstersDir + TEXT("/")));
	TestEqual(TEXT("Monster montage name keeps the group prefix"),
		MontagePackagePath(MonstersDir, TEXT("AM_Monsters_Attack")),
		FString(TEXT("/Game/Qinglan/Characters/Monsters/Montages/AM_Monsters_Attack")));

	// Runtime contract: the sword component's fallback soft paths must point at
	// exactly the object paths the bootstrap authors.
	UQinglanSwordComponent* Sword = NewObject<UQinglanSwordComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("Sword component"), Sword))
	{
		return false;
	}
	TestEqual(TEXT("Sword has one fallback montage per cut"), Sword->SwingMontageSoftPaths.Num(), 3);
	for (int32 CutIndex = 0; CutIndex < Sword->SwingMontageSoftPaths.Num(); ++CutIndex)
	{
		const FString Name = FString::Printf(TEXT("AM_Human_SwordCut_%d"), CutIndex + 1);
		TestEqual(*FString::Printf(TEXT("Cut %d fallback path matches the bootstrap contract"), CutIndex + 1),
			Sword->SwingMontageSoftPaths[CutIndex].ToString(),
			MontageObjectPath(MontagePackagePath(HumanDir, Name), Name));
	}
	return true;
}

// ---------------------------------------------------------------------------
// AUD-42: pooled impact cues must retain the technique identity instead of
// publishing an empty cast context. Constructs the cue without a world.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditImpactContextCarriesTechnique,
	"Qinglan.VFX.ImpactContextCarriesTechnique", AuditFrameworkTestFlags)

bool FQinglanAuditImpactContextCarriesTechnique::RunTest(const FString& Parameters)
{
	AQinglanProjectile* Projectile = NewObject<AQinglanProjectile>(GetTransientPackage());
	if (!TestNotNull(TEXT("Pooled projectile"), Projectile))
	{
		return false;
	}

	const FQinglanCastId CastId{ 42, 7 };
	const FName TechniqueId(TEXT("ember_lance"));
	Projectile->SetupLinear(CastId, TechniqueId, EQinglanElement::Fire,
		/*InOwner*/ nullptr, FVector::ForwardVector, 100.f, 100.f, 10.f,
		/*bInAppliesBurn*/ true, /*bInPiercing*/ false, 1.f, 3.f);

	UQinglanTechniqueDataAsset* Technique = NewObject<UQinglanTechniqueDataAsset>(GetTransientPackage());
	Technique->TechniqueId = TechniqueId;
	Technique->Element = EQinglanElement::Fire;
	FQinglanTechniqueCastContext Context;
	Context.CastId = CastId;
	Context.Technique = Technique;
	Projectile->SetCueContext(Context);

	const FVector ImpactPoint(120.f, -30.f, 40.f);
	const FQinglanCueEvent Cue = Projectile->BuildImpactCueEvent(ImpactPoint);

	TestEqual(TEXT("Impact cue carries the technique id"), Cue.TechniqueId, TechniqueId);
	TestEqual(TEXT("Impact cue carries the cast serial"), Cue.CastId.Serial, 42);
	TestEqual(TEXT("Impact cue carries the cast frame"), Cue.CastId.FrameNumber, 7);
	TestEqual(TEXT("Impact cue carries the element"), static_cast<int32>(Cue.Element), static_cast<int32>(EQinglanElement::Fire));
	TestEqual(TEXT("Impact cue is the docs/04 impact stage"), Cue.StageId, FName(TEXT("impact")));
	TestEqual(TEXT("Impact cue source is the impact point"), Cue.SourceTransform.GetLocation(), ImpactPoint);
	TestEqual(TEXT("Impact cue feature id targets the spell impact gate"),
		Cue.FeatureId, FName(TEXT("vfx.spell.impact")));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-22: exit/victory requires the realm boss room AND the exit room; boss
// proof is a victory-only settlement outcome (defeat/retreat never write it).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditExitRequiresBossClear,
	"Qinglan.Progression.ExitRequiresBossClear", AuditFrameworkTestFlags)

bool FQinglanAuditExitRequiresBossClear::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	if (!TestNotNull(TEXT("Run subsystem"), Run))
	{
		return false;
	}

	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = FName(UQinglanEconomyLibrary::BambooRealmIdText);
	Checkpoint.RealmSeed = 1;
	Checkpoint.RoomIndex = 6; // exit room
	Checkpoint.CommittedRoomIds = { 0, 1, 2, 3, 4 }; // boss (5) not cleared
	Run->ImportCheckpoint(Checkpoint);

	TestTrue(TEXT("Checkpoint resumes the expedition"), Run->IsInExpedition());
	TestEqual(TEXT("Realm boss room index is 5"), Run->GetBossRoomIndex(), 5);
	TestFalse(TEXT("Boss room is not completed"), Run->IsBossRoomCompleted());
	TestFalse(TEXT("Victory is not claimable without the boss clear"), Run->CanClaimVictory());
	TestFalse(TEXT("Exit request is rejected before the boss clear"), Run->RequestExit());
	TestTrue(TEXT("A rejected exit leaves the run open"), Run->IsInExpedition());

	// Standing in the boss room (even with its reward committed) is not an exit.
	Checkpoint.CommittedRoomIds.Add(5);
	Checkpoint.RoomIndex = 5;
	Run->ImportCheckpoint(Checkpoint);
	TestTrue(TEXT("Boss room completion is recorded"), Run->IsBossRoomCompleted());
	TestFalse(TEXT("Boss room is not the exit room"), Run->CanClaimVictory());

	// Reaching the exit after the boss clear is the authoritative victory.
	Checkpoint.RoomIndex = 6;
	Run->ImportCheckpoint(Checkpoint);
	TestTrue(TEXT("Exit plus boss clear claims victory"), Run->CanClaimVictory());
	TestFalse(TEXT("Defeat settlement never writes boss proof"), Run->ShouldWriteBossProof(false));
	TestFalse(TEXT("Retreat settlement never writes boss proof"), Run->ShouldWriteBossProof(false));
	TestTrue(TEXT("Victory settlement writes boss proof"), Run->ShouldWriteBossProof(true));
	TestTrue(TEXT("Exit request is accepted after the boss clear"), Run->RequestExit());
	return true;
}

// ---------------------------------------------------------------------------
// AUD-32: one action-priority gate for sword/dodge/cast/lock/move.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditActionStateGate,
	"Qinglan.Combat.ActionStateGate", AuditFrameworkTestFlags)

bool FQinglanAuditActionStateGate::RunTest(const FString& Parameters)
{
	// Priority: dead/reaction > menu/scene freeze > committed action > buffer.
	FQinglanActionGateState State;
	TestTrue(TEXT("Idle sword is allowed"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestTrue(TEXT("Idle technique is allowed"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));
	TestTrue(TEXT("Idle dodge starts immediately"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Start);

	// Cast release/recovery: no normal attack and no second cast; dodge is
	// blocked now but may be buffered for the recovery tail.
	State.bCasting = true;
	State.bCastRecovery = true;
	TestFalse(TEXT("Sword cannot cancel cast recovery"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestFalse(TEXT("Technique cannot overlap cast recovery"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));
	TestTrue(TEXT("Movement remains legal during cast recovery"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Move, State));
	TestTrue(TEXT("Dodge during recovery is buffered"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Buffer);

	// Anticipation is cancellable by a dodge and still blocks a normal attack.
	State = FQinglanActionGateState();
	State.bCasting = true;
	State.bCastAnticipation = true;
	TestFalse(TEXT("Sword cannot cancel cast anticipation"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestFalse(TEXT("A second cast cannot overlap anticipation"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));
	TestTrue(TEXT("Dodge cancels the zero-cost anticipation window"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Start);

	// Scene choices freeze movement, sword, dodge, techniques, and lock-on.
	State = FQinglanActionGateState();
	State.bSceneChoicePending = true;
	TestFalse(TEXT("Choice pending blocks movement"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Move, State));
	TestFalse(TEXT("Choice pending blocks sword"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestFalse(TEXT("Choice pending blocks dodge"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Dodge, State));
	TestFalse(TEXT("Choice pending blocks techniques"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));
	TestFalse(TEXT("Choice pending blocks lock-on"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::LockOn, State));
	TestTrue(TEXT("Choice pending rejects (never buffers) dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Reject);

	// Menus use the same freeze.
	State = FQinglanActionGateState();
	State.bMenuOpen = true;
	TestFalse(TEXT("Open menu blocks sword"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestTrue(TEXT("Open menu rejects dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Reject);

	// Forced hit reaction outranks every action and never buffers.
	State = FQinglanActionGateState();
	State.bHitReaction = true;
	for (const EQinglanActionKind Kind : { EQinglanActionKind::Move, EQinglanActionKind::Sword,
		EQinglanActionKind::Dodge, EQinglanActionKind::Technique, EQinglanActionKind::LockOn })
	{
		TestFalse(*FString::Printf(TEXT("Hit reaction blocks action kind %d"), int32(Kind)),
			AQinglanCharacter::IsActionAllowed(Kind, State));
	}
	TestTrue(TEXT("Hit reaction rejects dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Reject);

	// A committed sword active window blocks dodge/technique but the sword keeps
	// owning its own 0.15 s attack buffer.
	State = FQinglanActionGateState();
	State.bSwordSwinging = true;
	State.bSwordWindupOrActive = true;
	State.bSwordActive = true;
	State.bSwordCanDodgeCancel = false;
	TestTrue(TEXT("Sword input remains legal while a swing is committed"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, State));
	TestTrue(TEXT("Active hit window buffers an early dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Buffer);
	TestFalse(TEXT("Technique cannot start inside sword active"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));

	// Windup and recovery after the cancel point may be dodge-canceled.
	State.bSwordCanDodgeCancel = true;
	TestTrue(TEXT("Windup/cancelable recovery starts a dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Start);

	// Attack recovery permits a cast (docs/02 section 3 priority).
	State = FQinglanActionGateState();
	State.bSwordSwinging = true;
	State.bSwordCanDodgeCancel = true;
	TestTrue(TEXT("Cast may start from attack recovery"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Technique, State));

	// Root and missing stamina reject outright.
	State = FQinglanActionGateState();
	State.bRooted = true;
	TestFalse(TEXT("Root blocks movement"), AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Move, State));
	TestTrue(TEXT("Root rejects dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(State, true) == EQinglanDodgeRequest::Reject);
	TestTrue(TEXT("Missing stamina rejects dodge"),
		AQinglanCharacter::EvaluateDodgeRequest(FQinglanActionGateState(), false) == EQinglanDodgeRequest::Reject);

	// 0.15 s buffer window: inclusive at the boundary, expired after it.
	TestTrue(TEXT("Buffer is fresh at the 0.15 s boundary"),
		AQinglanCharacter::IsDodgeBufferFresh(10.f, 10.15f, 0.15f));
	TestFalse(TEXT("Buffer expires past 0.15 s"),
		AQinglanCharacter::IsDodgeBufferFresh(10.f, 10.151f, 0.15f));
	TestFalse(TEXT("A press stamped in the future is not fresh"),
		AQinglanCharacter::IsDodgeBufferFresh(10.f, 9.99f, 0.15f));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-27: profile EquippedTechniques are imported into runtime slots 0-3.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditLoadoutSync,
	"Qinglan.Combat.LoadoutSync", AuditFrameworkTestFlags)

bool FQinglanAuditLoadoutSync::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Owner);
	if (!TestNotNull(TEXT("Profile subsystem"), Profile))
	{
		return false;
	}

	const FName JadeId(TEXT("jade_sword_arc"));
	const FName EmberId(TEXT("ember_lance"));
	const FName MissingId(TEXT("missing_technique"));
	FQinglanProfileSnapshot Snapshot;
	Snapshot.UnlockedTechniques = { JadeId, EmberId, MissingId };
	Snapshot.EquippedTechniques = { JadeId, EmberId };
	Profile->ImportStateFromSave(Snapshot);

	UQinglanTechniqueDataAsset* Jade = NewObject<UQinglanTechniqueDataAsset>(GetTransientPackage());
	Jade->TechniqueId = JadeId;
	UQinglanTechniqueDataAsset* Ember = NewObject<UQinglanTechniqueDataAsset>(GetTransientPackage());
	Ember->TechniqueId = EmberId;
	TArray<UQinglanTechniqueDataAsset*> Catalog;
	Catalog.Add(Jade);
	Catalog.Add(Ember);
	UQinglanTechniqueComponent::SetTechniqueCatalogForTesting(Catalog);

	AActor* Pawn = NewObject<AActor>(GetTransientPackage());
	UQinglanTechniqueComponent* Techniques = NewObject<UQinglanTechniqueComponent>(Pawn);
	if (!TestNotNull(TEXT("Technique component"), Techniques))
	{
		UQinglanTechniqueComponent::SetTechniqueCatalogForTesting(TArray<UQinglanTechniqueDataAsset*>());
		return false;
	}

	TestTrue(TEXT("Profile loadout sync succeeds"), Techniques->SyncLoadoutFromProfile(Profile));
	TArray<UQinglanTechniqueDataAsset*> Loadout = Techniques->GetLoadout();
	TestEqual(TEXT("Two equipped techniques occupy two slots"), Loadout.Num(), 2);
	if (Loadout.Num() == 2)
	{
		TestEqual(TEXT("Slot 0 follows the profile order"), Loadout[0] ? Loadout[0]->TechniqueId : NAME_None, JadeId);
		TestEqual(TEXT("Slot 1 follows the profile order"), Loadout[1] ? Loadout[1]->TechniqueId : NAME_None, EmberId);
	}

	// An id without a DA_Tech asset logs and keeps an explicit empty slot, so
	// later equipped techniques never shift position.
	Snapshot.EquippedTechniques = { JadeId, MissingId, EmberId };
	Profile->ImportStateFromSave(Snapshot);
	TestTrue(TEXT("Sync preserves unresolved slots"), Techniques->SyncLoadoutFromProfile(Profile));
	Loadout = Techniques->GetLoadout();
	TestEqual(TEXT("Three equipped ids occupy three slots"), Loadout.Num(), 3);
	if (Loadout.Num() == 3)
	{
		TestNotNull(TEXT("Resolved slot 0"), Loadout[0]);
		TestNull(TEXT("Missing asset leaves slot 1 empty"), Loadout[1]);
		TestEqual(TEXT("Later techniques keep their slot"), Loadout[2] ? Loadout[2]->TechniqueId : NAME_None, EmberId);
	}

	// Locked techniques are also refused at sync time and leave the slot empty.
	Snapshot.UnlockedTechniques = { JadeId };
	Snapshot.EquippedTechniques = { JadeId, EmberId };
	Profile->ImportStateFromSave(Snapshot);
	TestTrue(TEXT("Sync still commits resolved slots"), Techniques->SyncLoadoutFromProfile(Profile));
	Loadout = Techniques->GetLoadout();
	TestEqual(TEXT("Locked technique keeps the slot count"), Loadout.Num(), 2);
	if (Loadout.Num() == 2)
	{
		TestNotNull(TEXT("Unlocked technique resolves"), Loadout[0]);
		TestNull(TEXT("Locked technique leaves an empty slot"), Loadout[1]);
	}

	UQinglanTechniqueComponent::SetTechniqueCatalogForTesting(TArray<UQinglanTechniqueDataAsset*>());
	return true;
}

// ---------------------------------------------------------------------------
// AUD-52: journal open -> record -> close -> reopen state machine. Pure state
// only, so no rendering world (or Slate) is required.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditJournalStateMachine,
	"Qinglan.UI.JournalOpenCloseReopen", AuditFrameworkTestFlags)

bool FQinglanAuditJournalStateMachine::RunTest(const FString& Parameters)
{
	FQinglanJournalState Journal;
	const FName RecordA(TEXT("L01"));
	const FName RecordB(TEXT("L02"));
	TestFalse(TEXT("Journal starts closed"), Journal.IsOpen());
	TestFalse(TEXT("No record starts open"), Journal.IsRecordOpen());

	Journal.OpenJournal();
	TestTrue(TEXT("Journal opens to the record list"), Journal.IsOpen());
	TestFalse(TEXT("Opening the journal does not open a record"), Journal.IsRecordOpen());
	TestTrue(TEXT("First record opens"), Journal.OpenRecord(RecordA));
	TestTrue(TEXT("Record body is active"), Journal.IsRecordOpen());
	TestEqual(TEXT("Current record is the opened record"), Journal.GetCurrentRecordId(), RecordA);
	TestEqual(TEXT("Most recent record is remembered"), Journal.LastRecordId, RecordA);

	TestTrue(TEXT("Closing the record succeeds"), Journal.CloseRecord());
	TestFalse(TEXT("Record body closes"), Journal.IsRecordOpen());
	TestTrue(TEXT("Record list/journal stays open"), Journal.IsOpen());
	TestEqual(TEXT("Closing a record clears the current id"), Journal.GetCurrentRecordId(), NAME_None);
	TestEqual(TEXT("Closing a record keeps the reopen target"), Journal.LastRecordId, RecordA);

	TestTrue(TEXT("Reopen restores the last record"), Journal.ReopenLastRecord());
	TestEqual(TEXT("Reopened record is the last record"), Journal.GetCurrentRecordId(), RecordA);

	TestTrue(TEXT("Second record replaces the current body"), Journal.OpenRecord(RecordB));
	TestEqual(TEXT("Current record follows the second open"), Journal.GetCurrentRecordId(), RecordB);
	TestEqual(TEXT("Reopen target follows the second open"), Journal.LastRecordId, RecordB);
	TestTrue(TEXT("Whole journal closes"), Journal.CloseJournal());
	TestFalse(TEXT("Journal is closed"), Journal.IsOpen());
	TestFalse(TEXT("No record remains open after journal close"), Journal.IsRecordOpen());
	TestEqual(TEXT("Reopen target survives journal close"), Journal.LastRecordId, RecordB);

	Journal.OpenJournal();
	TestTrue(TEXT("A closed journal can be reopened"), Journal.IsOpen());
	TestTrue(TEXT("The last record reopens after journal close"), Journal.ReopenLastRecord());
	TestEqual(TEXT("Reopen after journal close returns the same record"),
		Journal.GetCurrentRecordId(), RecordB);

	// Source classification: the registry has no source field, so the journal
	// infers it from the stable authored id prefix (documented in the header).
	TestEqual(TEXT("Lore L-ids classify as paper"),
		static_cast<int32>(FQinglanJournalState::InferRecordSourceKind(RecordA)),
		static_cast<int32>(EQinglanSourceKind::Paper));
	TestEqual(TEXT("Memory M-ids classify as memory"),
		static_cast<int32>(FQinglanJournalState::InferRecordSourceKind(FName(TEXT("M03")))),
		static_cast<int32>(EQinglanSourceKind::Memory));
	TestEqual(TEXT("Live transcript ids classify as live"),
		static_cast<int32>(FQinglanJournalState::InferRecordSourceKind(FName(TEXT("LIVE_02")))),
		static_cast<int32>(EQinglanSourceKind::Live));
	TestEqual(TEXT("Paper records expose the paper source label"),
		FQinglanJournalState::SourceKindLabelKey(EQinglanSourceKind::Paper),
		FName(TEXT("story.speaker.source.paper")));
	return true;
}

// ---------------------------------------------------------------------------
// AUD-51: pause -> settings -> back, gameplay input freeze, numeric capture
// clearing on close.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditMenuStateMachine,
	"Qinglan.UI.MenuStateMachine", AuditFrameworkTestFlags)

bool FQinglanAuditMenuStateMachine::RunTest(const FString& Parameters)
{
	FQinglanMenuState Menu;
	TestFalse(TEXT("Menu starts closed"), Menu.IsOpen());
	TestFalse(TEXT("Closed menu does not block gameplay"), Menu.IsGameplayInputBlocked());

	Menu.Open(EQinglanMenuScreen::Pause);
	TestTrue(TEXT("Pause opens the menu"), Menu.IsOpen());
	TestTrue(TEXT("Open menu blocks gameplay input"), Menu.IsGameplayInputBlocked());
	TestEqual(TEXT("Pause is the initial screen"),
		static_cast<int32>(Menu.Screen), static_cast<int32>(EQinglanMenuScreen::Pause));

	TestTrue(TEXT("Pause navigates to Settings"),
		Menu.NavigateTo(EQinglanMenuScreen::Settings));
	TestEqual(TEXT("Settings screen is active"),
		static_cast<int32>(Menu.Screen), static_cast<int32>(EQinglanMenuScreen::Settings));
	TestTrue(TEXT("Gameplay stays blocked while navigating"), Menu.IsGameplayInputBlocked());

	Menu.SetOptionCount(3);
	Menu.MoveSelection(1);
	TestEqual(TEXT("Down changes the selected row"), Menu.SelectionIndex, 1);
	Menu.MoveSelection(-1);
	TestEqual(TEXT("Up returns to the first row"), Menu.SelectionIndex, 0);

	Menu.CaptureNumberKey(2);
	TestTrue(TEXT("Numeric selection is captured while open"), Menu.WasNumberKeyCaptured(2));
	TestTrue(TEXT("Back returns Settings to Pause"), Menu.Back());
	TestEqual(TEXT("Pause is active after back"),
		static_cast<int32>(Menu.Screen), static_cast<int32>(EQinglanMenuScreen::Pause));
	TestTrue(TEXT("Numeric capture persists across sub-screens until close"),
		Menu.WasNumberKeyCaptured(2));
	TestFalse(TEXT("Back from Pause closes the menu"), Menu.Back());
	TestFalse(TEXT("Menu is closed"), Menu.IsOpen());
	TestFalse(TEXT("Closed menu restores gameplay input"), Menu.IsGameplayInputBlocked());
	TestFalse(TEXT("Closing clears all numeric capture"), Menu.WasNumberKeyCaptured(2));
	TestEqual(TEXT("Closing empties the capture list"), Menu.CapturedNumberKeys.Num(), 0);

	// Explicit Close has the same cleanup contract.
	Menu.Open(EQinglanMenuScreen::Profile);
	Menu.CaptureNumberKey(1);
	TestTrue(TEXT("Capture before explicit close"), Menu.WasNumberKeyCaptured(1));
	Menu.Close();
	TestFalse(TEXT("Explicit close clears capture"), Menu.WasNumberKeyCaptured(1));
	TestFalse(TEXT("Explicit close restores gameplay"), Menu.IsGameplayInputBlocked());

	// The AUD-32 gate consumes the same flag on the character snapshot.
	FQinglanActionGateState Gate;
	Gate.bMenuOpen = true;
	TestFalse(TEXT("Open menu blocks the sword gate"),
		AQinglanCharacter::IsActionAllowed(EQinglanActionKind::Sword, Gate));
	TestTrue(TEXT("Open menu rejects dodge outright"),
		AQinglanCharacter::EvaluateDodgeRequest(Gate, true) == EQinglanDodgeRequest::Reject);
	return true;
}

// ---------------------------------------------------------------------------
// AUD-51: offers persist until an explicit selection or dismissal.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditOfferPersistence,
	"Qinglan.UI.OfferPersistence", AuditFrameworkTestFlags)

bool FQinglanAuditOfferPersistence::RunTest(const FString& Parameters)
{
	TArray<FQinglanOfferChoice> Entries;
	FQinglanOfferChoice First;
	First.OfferId = FName(TEXT("relic_a"));
	First.Title = FText::FromString(TEXT("Relic A"));
	Entries.Add(First);
	FQinglanOfferChoice Second;
	Second.OfferId = FName(TEXT("relic_b"));
	Second.Title = FText::FromString(TEXT("Relic B"));
	Entries.Add(Second);

	FQinglanOfferState Offers;
	TestTrue(TEXT("Offer set becomes active"), Offers.BeginOffer(Entries));
	TestTrue(TEXT("Offer set is active"), Offers.IsActive());
	TestEqual(TEXT("Both offers are retained"), Offers.Entries.Num(), 2);
	TestNotNull(TEXT("First offer is selected initially"), Offers.GetSelected());

	// Screen changes and navigation do not clear an offer.
	FQinglanMenuState Menu;
	Menu.Open(EQinglanMenuScreen::Offer);
	Menu.NavigateTo(EQinglanMenuScreen::Settings);
	Menu.NavigateTo(EQinglanMenuScreen::Offer);
	TestTrue(TEXT("Offer survives menu screen changes"), Offers.IsActive());
	TestTrue(TEXT("Offer selection can move"), Offers.MoveSelection(1));
	TestEqual(TEXT("Second offer is selected"), Offers.SelectedIndex, 1);
	TestTrue(TEXT("Offer remains active after selection movement"), Offers.IsActive());

	// Unknown selection is rejected without consuming the offer.
	TestFalse(TEXT("Unknown offer id is rejected"), Offers.Select(FName(TEXT("missing"))));
	TestTrue(TEXT("Rejected selection keeps the offer"), Offers.IsActive());
	TestEqual(TEXT("Rejected selection keeps every entry"), Offers.Entries.Num(), 2);

	// A valid selection consumes the whole set.
	TestTrue(TEXT("Valid selection succeeds"), Offers.Select(FName(TEXT("relic_a"))));
	TestFalse(TEXT("Selection clears the offer"), Offers.IsActive());
	TestEqual(TEXT("Selection clears every entry"), Offers.Entries.Num(), 0);

	// Dismiss is the other explicit exit.
	TestTrue(TEXT("Offer can begin again"), Offers.BeginOffer(Entries));
	TestTrue(TEXT("Dismiss clears the active offer"), Offers.Dismiss());
	TestFalse(TEXT("Dismissed offer is inactive"), Offers.IsActive());
	TestEqual(TEXT("Dismissed offer retains no entries"), Offers.Entries.Num(), 0);
	TestFalse(TEXT("Second dismiss is a no-op"), Offers.Dismiss());

	// An empty offer is never active, so dismiss/select cannot leak state.
	TestFalse(TEXT("Empty offer does not activate"), Offers.BeginOffer(TArray<FQinglanOfferChoice>()));
	TestFalse(TEXT("Empty offer is inactive"), Offers.IsActive());
	return true;
}

// ---------------------------------------------------------------------------
// AUD-55: texture-set contract. Asserts generated asset paths, Poly Haven
// suffix/parameter mapping, sRGB policy, and the solid-colour fallback path
// without importing or loading any content.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanContentTextureSetContract,
	"Qinglan.Content.TextureSetContract", AuditFrameworkTestFlags)

bool FQinglanContentTextureSetContract::RunTest(const FString& Parameters)
{
	using namespace QinglanTextureSetContract;

	TestEqual(TEXT("Texture folder is per surface set"),
		TextureFolderPath(TEXT("bamboo_wall")),
		FString(TEXT("/Game/Qinglan/Textures/bamboo_wall")));
	TestEqual(TEXT("Texture package is <Set>/<Type>"),
		TexturePackagePath(TEXT("bamboo_wall"), TEXT("BaseColor")),
		FString(TEXT("/Game/Qinglan/Textures/bamboo_wall/BaseColor")));
	TestEqual(TEXT("Texture object path is package.Type"),
		TextureObjectPath(TEXT("bamboo_wall"), TEXT("BaseColor")),
		FString(TEXT("/Game/Qinglan/Textures/bamboo_wall/BaseColor.BaseColor")));

	TestEqual(TEXT("BaseColor comes from the _diff_ file"),
		SourceFileToken(TEXT("BaseColor")), FString(TEXT("diff")));
	TestEqual(TEXT("Roughness comes from the _rough_ file"),
		SourceFileToken(TEXT("Roughness")), FString(TEXT("rough")));
	TestEqual(TEXT("AO comes from the _ao_ file"),
		SourceFileToken(TEXT("AmbientOcclusion")), FString(TEXT("ao")));
	TestEqual(TEXT("Packed ARM comes from the _arm_ file"),
		SourceFileToken(TEXT("Arm")), FString(TEXT("arm")));
	TestEqual(TEXT("Normal would come from the _nor_ file"),
		SourceFileToken(TEXT("Normal")), FString(TEXT("nor")));

	TestEqual(TEXT("BaseColor binds the BaseColor parameter"),
		MaterialParameterName(TEXT("BaseColor")), FString(TEXT("BaseColor")));
	TestEqual(TEXT("Roughness binds the Roughness parameter"),
		MaterialParameterName(TEXT("Roughness")), FString(TEXT("Roughness")));
	TestEqual(TEXT("AO binds the AmbientOcclusion parameter"),
		MaterialParameterName(TEXT("AmbientOcclusion")), FString(TEXT("AmbientOcclusion")));
	TestEqual(TEXT("Normal binds the Normal parameter"),
		MaterialParameterName(TEXT("Normal")), FString(TEXT("Normal")));
	TestEqual(TEXT("ARM is channel-packed, not a wholesale parameter"),
		MaterialParameterName(TEXT("Arm")), FString());

	TestTrue(TEXT("BaseColor is sRGB"),
		IsSRGB(TEXT("BaseColor")));
	TestFalse(TEXT("Roughness is linear"),
		IsSRGB(TEXT("Roughness")));
	TestFalse(TEXT("AO is linear"),
		IsSRGB(TEXT("AmbientOcclusion")));
	TestFalse(TEXT("ARM is linear"),
		IsSRGB(TEXT("Arm")));
	TestFalse(TEXT("Normal is linear (normal-map sampler)"),
		IsSRGB(TEXT("Normal")));

	TestEqual(TEXT("Textured material path"),
		ResolveSurfaceMaterialObjectPath(TEXT("M_BambooWall"), false, true),
		FString(TEXT("/Game/Qinglan/Materials/M_BambooWall.M_BambooWall")));
	TestEqual(TEXT("Low quality resolves the solid companion"),
		ResolveSurfaceMaterialObjectPath(TEXT("M_BambooWall"), true, true),
		FString(TEXT("/Game/Qinglan/Materials/M_BambooWall_Solid.M_BambooWall_Solid")));
	TestEqual(TEXT("Missing BaseColor resolves the solid companion"),
		ResolveSurfaceMaterialObjectPath(TEXT("M_FineWood"), false, false),
		FString(TEXT("/Game/Qinglan/Materials/M_FineWood_Solid.M_FineWood_Solid")));
	TestEqual(TEXT("Solid asset name is stable"),
		SolidMaterialAssetName(TEXT("M_FineWood")), FString(TEXT("M_FineWood_Solid")));

	TestFalse(TEXT("Textured set and standard quality keep textures"),
		ShouldUseSolidFallback(false, true));
	TestTrue(TEXT("Low quality falls back to solid"),
		ShouldUseSolidFallback(true, true));
	TestTrue(TEXT("Missing BaseColor falls back to solid"),
		ShouldUseSolidFallback(false, false));

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// AUD-50: the committed string table must contain every runtime key and the
// FromStringTable lookup path used by UI code must resolve it. This spec caught
// a stale table surviving an incremental import (the importer reported the
// full key count while the saved asset still held the previous key set).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAuditStringTableRuntimeKeys,
	"Qinglan.Localization.StringTableRuntimeKeys", AuditFrameworkTestFlags)

bool FQinglanAuditStringTableRuntimeKeys::RunTest(const FString& Parameters)
{
	UStringTable* Table = LoadObject<UStringTable>(nullptr, TEXT("/Game/Qinglan/Data/ST_Game"));
	if (!TestNotNull(TEXT("ST_Game loads"), Table))
	{
		return false;
	}
	const TSharedRef<FStringTable> Root = Table->GetMutableStringTable();
	int32 KeyCount = 0;
	Root->EnumerateKeysAndSourceStrings([&KeyCount](const FTextKey&, const FString&) { ++KeyCount; return true; });
	UE_LOG(LogTemp, Display, TEXT("ST_Game entry count: %d"), KeyCount);
	TestTrue(TEXT("Table carries the full imported key set (>900)"), KeyCount > 900);

	const TCHAR* Keys[] = {
		TEXT("hud.health"), TEXT("hud.objective"), TEXT("hud.objective.none"),
		TEXT("hud.skill_slot"), TEXT("hud.talisman"), TEXT("trial.spawn_failed"),
		TEXT("technique.jade_sword_arc.name"), TEXT("interact.hub.rest_station"),
	};
	for (const TCHAR* Key : Keys)
	{
		const bool bPresent = Root->FindEntry(FTextKey(Key)).IsValid();
		TestTrue(*FString::Printf(TEXT("Table entry present: %s"), Key), bPresent);
		// The registered table id is the asset OBJECT path (see HUDStatics).
		const FText Resolved = FText::FromStringTable(
			UQinglanHUDStatics::GameStringTableId(), Key);
		TestFalse(*FString::Printf(TEXT("FromStringTable resolves %s"), Key),
			Resolved.ToString().Contains(TEXT("MISSING STRING TABLE ENTRY")));
	}
	return true;
}
