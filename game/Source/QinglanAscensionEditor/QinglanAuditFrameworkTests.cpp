// AUD-07/08/09/18: direct helper and profile regression coverage, without a world.
// These do not prove disk durability, save migration, or gameplay entry wiring.
#include "Misc/AutomationTest.h"

#include "Core/QinglanTrialAdmission.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Engine/GameInstance.h"
#include "Narrative/QinglanStoryIdentity.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveMigration.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "UObject/UObjectGlobals.h"

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
#endif // WITH_DEV_AUTOMATION_TESTS
