#include "Core/QinglanObjectiveGuide.h"

#include "AI/QinglanEnemyCharacter.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanTrainingTarget.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanRoomLayout.h"
#include "Core/QinglanSceneInspections.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanRunSubsystem.h"

FQinglanObjectiveGuide QinglanObjectiveGuide::Resolve(AQinglanCharacter* Player)
{
	FQinglanObjectiveGuide Guide;
	UGameInstance* Instance = Player ? Player->GetGameInstance() : nullptr;
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (!Story || !Player->GetWorld()) { return Guide; }
	Guide.SceneId = Story->GetTrackedMainSceneId();
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (Registry && Run && Run->IsInExpedition() && !Story->IsScenePlaying())
	{
		const FString Map = Player->GetWorld()->GetMapName();
		const FString Prefix = Map.Contains(TEXT("Bamboo")) ? TEXT("bamboo.")
			: Map.Contains(TEXT("Ashen")) ? TEXT("ashen.") : TEXT("");
		const FQinglanRegistryScene* Tracked = Registry->FindMainScene(Guide.SceneId);
		const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
		if (Realm && !Prefix.IsEmpty() && (!Tracked || !Tracked->Location.ToString().StartsWith(Prefix)))
		{
			// Older profiles could depart before the sanctuary chain finished.
			// Guide the active expedition's real pending work, rather than send
			// its player toward an unreachable hub objective. No proof is granted.
			const TArray<FName> Chapters = QinglanInteraction::ChaptersForRealm(Realm->RealmId);
			for (FName Id : Story->GetPlayableMainScenes())
			{
				const FQinglanRegistryScene* Candidate = Registry->FindMainScene(Id);
				if (Candidate && Chapters.Contains(Candidate->ChapterId) && Candidate->Location.ToString().StartsWith(Prefix))
				{
					Guide.SceneId = Id;
					break;
				}
			}
		}
	}
	const FQinglanRegistryScene* Scene = Registry ? Registry->FindMainScene(Guide.SceneId) : nullptr;
	if (!Scene) { return Guide; }
	auto FindAnchor = [Player](FName Id) -> AActor*
	{
		for (TActorIterator<AQinglanInteractable> It(Player->GetWorld()); It; ++It)
		{
			if (It->InteractionId == Id) { return *It; }
		}
		return nullptr;
	};
	Guide.Target = FindAnchor(Scene->Location);
	Guide.HintKey = TEXT("guide.interact");
	if (Story->IsScenePlaying())
	{
		Guide.HintKey = TEXT("guide.dialogue");
		return Guide;
	}
	const auto Report = Scene->Objectives.IsEmpty() ? QinglanStoryRuntime::FQinglanGuardReport()
		: QinglanStoryRuntime::EvaluateObjectiveGuard(Story, Scene->SceneId, Scene->Objectives[0].ObjectiveId);
	auto Missing = [&Report](const TCHAR* Event) { return Report.Missing.Contains(FName(Event)); };
	const bool bScriptSeen = Player->HasSceneScriptExhaustedForInspection(Scene->SceneId);
	if (bScriptSeen && Scene->SceneId == FName(TEXT("CH00_S02")))
	{
		if (Missing(TEXT("combat.sword_hit")))
		{
			Guide.HintKey = TEXT("guide.training_hits");
			TActorIterator<AQinglanTrainingTarget> Target(Player->GetWorld());
			if (Target) { Guide.Target = *Target; }
		}
		else if (Missing(TEXT("combat.dodge"))) { Guide.HintKey = TEXT("guide.training_dodge"); }
		else if (Missing(TEXT("cast.jade_sword_arc"))) { Guide.HintKey = TEXT("guide.training_arc"); }
		else if (Missing(TEXT("cast.flowing_ward"))) { Guide.HintKey = TEXT("guide.training_ward"); }
		else if (Missing(TEXT("shrine.recovery_used")))
		{
			Guide.HintKey = TEXT("guide.recover"); Guide.Target = FindAnchor(TEXT("hub.shrine"));
		}
	}
	// Physical inspection plates are separate interactions from their dialogue
	// station. Point at the unread source rather than replaying the exchange.
	if (bScriptSeen)
	{
		for (const QinglanSceneInspections::FDocument& Document : QinglanSceneInspections::Catalog())
		{
			const FName Event(*(TEXT("record.opened.") + Document.Alias.ToString()));
			const FName ReadFlag(*(TEXT("inspection.read.") + Document.Id.ToString()));
			if (Document.Scene == Scene->SceneId && Report.Missing.Contains(Event) && !Story->HasFlag(ReadFlag))
			{
				Guide.HintKey = TEXT("guide.read");
				Guide.Target = FindAnchor(FName(*(TEXT("inspect.") + Document.Id.ToString())));
				break;
			}
		}
	}
	// Combat takes priority over a scene station or a destination in a later
	// room. Defeated enemies are excluded so their lingering bodies never win.
	if (Run && Run->IsInExpedition())
	{
		for (TActorIterator<AQinglanEnemyCharacter> It(Player->GetWorld()); It; ++It)
		{
			if (It->IsAlive()) { Guide.HintKey = TEXT("guide.fight"); Guide.Target = *It; return Guide; }
		}
		TArray<FQinglanBlessing> Offer;
		if (Run->GetBlessingOffer(Offer)) { Guide.HintKey = TEXT("guide.blessing"); }
		else if (bScriptSeen && Guide.HintKey == FName(TEXT("guide.interact"))
			&& Report.Missing.ContainsByPredicate([](FName Id) { return Id.ToString().StartsWith(TEXT("encounter.")); }))
		{
			const int32 Room = Run->GetCurrentRoomIndex();
			Guide.Target = FindAnchor(FName(*FString::Printf(TEXT("door.%d_%d"), Room, Room + 1)));
			Guide.HintKey = TEXT("guide.exit");
		}
	}
	if (Run && Run->IsInExpedition() && Scene->SceneId == FName(TEXT("CH01_S02")) && bScriptSeen)
	{
		const int32 Room = Run->GetCurrentRoomIndex();
		Guide.HintKey = TEXT("guide.exit");
		Guide.Target = FindAnchor(FName(*FString::Printf(TEXT("door.%d_%d"), Room, Room + 1)));
		for (TActorIterator<AQinglanEnemyCharacter> It(Player->GetWorld()); It; ++It)
		{
			if (It->IsAlive())
			{
				Guide.HintKey = TEXT("guide.fight"); Guide.Target = *It; return Guide;
			}
		}
		TArray<FQinglanBlessing> Offer;
		if (Run->GetBlessingOffer(Offer)) { Guide.HintKey = TEXT("guide.blessing"); }
		else if (Room >= 2 && Missing(TEXT("device.shutter_tested")))
		{
			Guide.HintKey = TEXT("guide.shutter"); Guide.Target = FindAnchor(TEXT("device.inspection_shutter"));
		}
		else if (Room == 3)
		{
			Guide.HintKey = TEXT("guide.return"); Guide.Target = FindAnchor(TEXT("bamboo.tutorial_return"));
		}
	}
	if (!Guide.Target)
	{
		// The next scene can be in another map; show the actual departure/return
		// station in this world instead of a marker at an unrelated coordinate.
		if (Player->GetWorld()->GetMapName().Contains(TEXT("Hub")))
		{
			const FString Location = Scene->Location.ToString();
			Guide.Target = FindAnchor(Location.StartsWith(TEXT("ashen")) ? FName(TEXT("hub.ashen_gate"))
				: Location.StartsWith(TEXT("trial")) ? FName(TEXT("hub.trial_gate")) : FName(TEXT("hub.bamboo_gate")));
		}
		else if (Run && Run->IsInExpedition())
		{
			const int32 Room = Run->GetCurrentRoomIndex();
			Guide.Target = FindAnchor(FName(*FString::Printf(TEXT("door.%d_%d"), Room, Room + 1)));
			Guide.HintKey = TEXT("guide.exit");
			if (!Guide.Target)
			{
				const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
				const FName ExitId = Realm && Realm->RealmId == FName(UQinglanEconomyLibrary::TutorialRealmIdText)
					? FName(TEXT("bamboo.tutorial_return"))
					: Player->GetWorld()->GetMapName().Contains(TEXT("Ashen"))
						? FName(TEXT("ashen.boss_5")) : FName(TEXT("bamboo.room_5_safe"));
				Guide.Target = FindAnchor(ExitId);
				Guide.HintKey = TEXT("guide.return");
			}
		}
	}
	if (Run && Run->IsInExpedition() && Guide.Target && !Story->IsScenePlaying())
	{
		const int32 Room = Run->GetCurrentRoomIndex();
		if (Guide.Target->GetActorLocation().X > QinglanRoomLayout::RoomCenter(Room).X + QinglanRoomLayout::RoomHalfExtentCm)
		{
			// Distant scene anchors may lie in a room whose encounter has not
			// started. The current exit is the next actionable destination.
			if (AActor* Door = FindAnchor(FName(*FString::Printf(TEXT("door.%d_%d"), Room, Room + 1))))
			{
				Guide.Target = Door; Guide.HintKey = TEXT("guide.exit");
			}
		}
	}
	return Guide;
}
