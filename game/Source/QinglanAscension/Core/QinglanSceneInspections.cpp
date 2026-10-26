#include "Core/QinglanSceneInspections.h"

#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanRoomLayout.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "UI/QinglanHUDStatics.h"
#include <initializer_list>

namespace
{
	bool HasFlags(const UQinglanStorySubsystem* Story, const TArray<FName>& Flags)
	{
		if (!Story) { return false; }
		for (FName Flag : Flags) { if (!Story->HasFlag(Flag)) { return false; } }
		return true;
	}
	bool MatchesMap(const UWorld* World, FName Anchor)
	{
		if (!World) { return false; }
		FString Id = Anchor.ToString();
		Id.RemoveFromStart(TEXT("sq."));
		const FString Map = World->GetMapName();
		return (Id.StartsWith(TEXT("hub.")) && Map.Contains(TEXT("L_Hub")))
			|| (Id.StartsWith(TEXT("bamboo.")) && Map.Contains(TEXT("L_BambooHollow")))
			|| (Id.StartsWith(TEXT("ashen.")) && Map.Contains(TEXT("L_AshenVerge")));
	}
	bool IsSafe(const UWorld* World)
	{
		if (!World) { return false; }
		for (TActorIterator<AQinglanEncounterDirector> It(World); It; ++It)
		{
			if (It->IsEncounterActive()) { return false; }
		}
		return true;
	}
	FName ReadFlag(const QinglanSceneInspections::FDocument& Document)
	{
		return FName(*(TEXT("inspection.read.") + Document.Id.ToString()));
	}
	AQinglanInteractable* SpawnPlate(UWorld* World, FName Id, const FVector& Position)
	{
		for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
		{
			if (It->InteractionId == Id) { return *It; }
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AQinglanInteractable* Actor = World->SpawnActor<AQinglanInteractable>(
			AQinglanInteractable::StaticClass(), Position, FRotator::ZeroRotator, Params);
		if (!Actor) { return nullptr; }
		Actor->InteractionId = Id;
		Actor->DisplayNameKey = FName(*(TEXT("interact.") + Id.ToString()));
		UStaticMeshComponent* Plate = NewObject<UStaticMeshComponent>(Actor, TEXT("InspectionPlate"));
		Actor->AddInstanceComponent(Plate);
		Plate->SetupAttachment(Actor->GetRootComponent());
		Plate->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Plate->SetRelativeScale3D(FVector(0.9f, 0.7f, 0.08f));
		Plate->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Plate->RegisterComponent();
		return Actor;
	}
}

namespace QinglanSceneInspections
{
	const TArray<FDocument>& Catalog()
	{
		static const TArray<FDocument> Documents = []
		{
			TArray<FDocument> Out;
			auto Add = [&Out](const TCHAR* Id, const TCHAR* Alias, const TCHAR* Scene, const TCHAR* Anchor,
				std::initializer_list<const TCHAR*> Keys)
			{
				FDocument D{FName(Id), FName(Alias), FName(Scene), FName(Anchor), {}};
				for (const TCHAR* Key : Keys) { D.BodyKeys.Add(FName(Key)); }
				Out.Add(MoveTemp(D));
			};
			Add(TEXT("route_letter"), TEXT("route_letter"), TEXT("CH00_S03"), TEXT("hub.shrine"), {TEXT("story.ch00.s03.line_01"), TEXT("story.ch00.s03.line_02"), TEXT("story.ch00.s03.line_03"), TEXT("story.ch00.s03.line_04")});
			Add(TEXT("water_marks"), TEXT("water_marks"), TEXT("CH01_S01"), TEXT("bamboo.room_0"), {TEXT("story.ch01.s01.line_01")});
			Add(TEXT("delivery_note"), TEXT("delivery_note"), TEXT("CH01_S01"), TEXT("bamboo.room_0"), {TEXT("story.ch01.s01.line_02")});
			Add(TEXT("pressure_dial"), TEXT("pressure_dial"), TEXT("CH02_S01"), TEXT("bamboo.room_0"), {TEXT("story.ch02.s01.line_01"), TEXT("story.ch02.s01.line_02")});
			Add(TEXT("scale_1"), TEXT("scale"), TEXT("CH04_S01"), TEXT("ashen.approach_1_2"), {TEXT("story.ch04.s01.scale")});
			Add(TEXT("scale_2"), TEXT("scale"), TEXT("CH04_S01"), TEXT("ashen.approach_1_2"), {TEXT("story.ch04.s01.note")});
			Add(TEXT("witness_transcript"), TEXT("witness_transcript"), TEXT("CH04_S02"), TEXT("ashen.archive_3"), {TEXT("story.ch04.s02.reader"), TEXT("story.ch04.s02.compact"), TEXT("story.ch04.s02.compact2"), TEXT("story.ch04.s02.witness"), TEXT("story.ch04.s02.echo"), TEXT("story.ch04.s02.note")});
			for (int32 I = 1; I <= 4; ++I)
			{
				const FString Id = FString::Printf(TEXT("renewal_entry_%d"), I);
				const FString Key = FString::Printf(TEXT("story.ch04.s03.renewal%d"), I);
				Add(*Id, TEXT("renewal_entry"), TEXT("CH04_S03"), TEXT("ashen.archive_3"), {*Key});
			}
			Add(TEXT("handover_summary"), TEXT("handover_summary"), TEXT("CH04_S03"), TEXT("ashen.archive_3"), {TEXT("story.ch04.s03.handover")});
			Add(TEXT("forecast"), TEXT("forecast"), TEXT("CH04_S04"), TEXT("ashen.release_antechamber"), {TEXT("story.ch04.s04.forecast"), TEXT("story.ch04.s04.limit")});
			Add(TEXT("repair_brief"), TEXT("repair_brief"), TEXT("CH04_S04"), TEXT("ashen.release_antechamber"), {TEXT("story.ch04.s04.brief")});
			for (int32 I = 1; I <= 4; ++I)
			{
				const FString Id = FString::Printf(TEXT("sealed_page_%d"), I);
				const FString Key = FString::Printf(TEXT("story.ch05.s03.yun%d"), I);
				Add(*Id, TEXT("sealed_page"), TEXT("CH05_S03"), TEXT("hub.ledger_table"), {TEXT("story.ch05.s03.date"), *Key});
			}
			Add(TEXT("morning_ledger"), TEXT("morning_ledger"), TEXT("CH07_S01"), TEXT("hub.ledger_table"), {TEXT("story.ch07.s01.time"), TEXT("story.ch07.s01.open"), TEXT("story.ch07.s01.seal"), TEXT("story.ch07.s01.shared"), TEXT("story.ch07.s01.ledger")});
			Add(TEXT("thirty_day_card"), TEXT("thirty_day_card"), TEXT("CH07_S02"), TEXT("hub.correspondence_table"), {TEXT("story.ch07.s02.time")});
			Add(TEXT("outcome_report"), TEXT("outcome_report"), TEXT("CH07_S02"), TEXT("hub.correspondence_table"), {TEXT("story.ch07.s02.open"), TEXT("story.ch07.s02.seal"), TEXT("story.ch07.s02.shared"), TEXT("story.ch07.s02.route"), TEXT("story.ch07.s02.route_missing"), TEXT("story.ch07.s02.allocation"), TEXT("story.ch07.s02.handover")});
			Add(TEXT("yun_new_letter"), TEXT("yun_new_letter"), TEXT("CH07_S03"), TEXT("hub.correspondence_table"), {TEXT("story.ch07.s03.date"), TEXT("story.ch07.s03.open"), TEXT("story.ch07.s03.seal"), TEXT("story.ch07.s03.shared"), TEXT("story.ch07.s03.common")});
			// The authored side-quest note deliberately contains several original
			// fields in one TextKey. Each physical field opens that complete source
			// note, preserving its dates/limits instead of inventing new prose.
			for (int32 I = 1; I <= 2; ++I) { const FString Id = FString::Printf(TEXT("request_%d"), I); Add(*Id, TEXT("request"), TEXT("SQ02.S1"), TEXT("sq.hub.courier_board"), {TEXT("story.sq02.s1.note")}); }
			Add(TEXT("archive_term"), TEXT("archive_term"), TEXT("SQ03.S1"), TEXT("sq.ashen.archive_safe"), {TEXT("story.sq03.s1.note")});
			for (int32 I = 1; I <= 4; ++I) { const FString Id = FString::Printf(TEXT("duty_offer_%d"), I); Add(*Id, *Id, TEXT("SQ03.S1"), TEXT("sq.ashen.archive_safe"), {TEXT("story.sq03.s1.note")}); }
			Add(TEXT("bell_ledger"), TEXT("bell_ledger"), TEXT("SQ04.S1"), TEXT("sq.bamboo.old_bell_safe"), {TEXT("story.sq04.s1.lan")});
			Add(TEXT("miao_testimony"), TEXT("miao_testimony"), TEXT("SQ04.S1"), TEXT("sq.bamboo.old_bell_safe"), {TEXT("story.sq04.s1.note")});
			Add(TEXT("crate_list"), TEXT("crate_list"), TEXT("SQ05.S1"), TEXT("sq.hub.stonewright_crate"), {TEXT("story.sq05.s1.lan"), TEXT("story.sq05.s1.note")});
			Add(TEXT("loan_note"), TEXT("loan_note"), TEXT("SQ05.S1"), TEXT("sq.hub.stonewright_crate"), {TEXT("story.sq05.s1.note")});
			Add(TEXT("trial_card"), TEXT("trial_card"), TEXT("SQ06.S1"), TEXT("sq.ashen.orchard_safe"), {TEXT("story.sq06.s1.note")});
			for (int32 I = 1; I <= 3; ++I) { const FString Id = FString::Printf(TEXT("renewal_%d"), I); Add(*Id, TEXT("renewal"), TEXT("SQ07.S1"), TEXT("sq.ashen.archive_safe"), {TEXT("story.sq07.s1.note")}); }
			Add(TEXT("recipe"), TEXT("recipe"), TEXT("SQ08.S1"), TEXT("sq.hub.old_table"), {TEXT("story.sq08.s1.note")});
			return Out;
		}();
		return Documents;
	}

	const FQinglanRegistryScene* FindScene(const UQinglanStorySubsystem* Story, FName SceneId)
	{
		const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
		if (!Registry) { return nullptr; }
		if (const FQinglanRegistryScene* Main = Registry->FindMainScene(SceneId)) { return Main; }
		FName Quest;
		return Registry->FindSideStage(SceneId, Quest);
	}

	FText Body(const UQinglanStorySubsystem* Story, const FDocument& Document)
	{
		const FQinglanRegistryScene* Scene = FindScene(Story, Document.Scene);
		if (!Scene) { return FText::GetEmpty(); }
		TArray<FString> Lines;
		for (FName Key : Document.BodyKeys)
		{
			const FQinglanRegistryLine* Line = Scene->Script.FindByPredicate([Key](const FQinglanRegistryLine& L) { return L.TextKey == Key; });
			if (!Line || !HasFlags(Story, Line->WhenAll)) { continue; }
			bool bForbidden = false;
			for (FName Flag : Line->WhenNone) { bForbidden |= Story->HasFlag(Flag); }
			if (!bForbidden && UQinglanHUDStatics::HasGameText(Key)) { Lines.Add(UQinglanHUDStatics::GameText(Key).ToString()); }
		}
		return FText::FromString(FString::Join(Lines, TEXT("\n\n")));
	}

	bool CanRead(const UQinglanStorySubsystem* Story, const AQinglanCharacter* Player, const FDocument& Document)
	{
		const FQinglanRegistryScene* Scene = FindScene(Story, Document.Scene);
		if (!Scene || !Player || !MatchesMap(Player->GetWorld(), Document.Anchor) || !IsSafe(Player->GetWorld())
			|| !HasFlags(Story, Scene->Prerequisites)) { return false; }
		FName QuestId;
		const UQinglanStoryRegistry* Registry = Story->GetRegistry();
		if (Registry->FindSideStage(Document.Scene, QuestId))
		{
			const FQinglanRegistrySideQuest* Quest = Registry->FindQuest(QuestId);
			if (!Quest || !HasFlags(Story, Quest->Availability)) { return false; }
		}
		return Story->IsSceneCompleted(Document.Scene) || Player->HasSceneScriptExhaustedForInspection(Document.Scene);
	}

	void Spawn(UWorld* World, const TMap<FName, FVector>& AnchorPositions)
	{
		if (!World || !World->IsGameWorld()) { return; }
		TMap<FName, int32> Counts;
		for (const FDocument& Document : Catalog())
		{
			if (!MatchesMap(World, Document.Anchor)) { continue; }
			FName Anchor = Document.Anchor;
			const FVector* Position = AnchorPositions.Find(Anchor);
			FString Plain = Anchor.ToString();
			if (!Position && Plain.RemoveFromStart(TEXT("sq."))) { Position = AnchorPositions.Find(FName(*Plain)); }
			if (!Position) { continue; }
			const int32 Index = Counts.FindOrAdd(Anchor)++;
			// 190cm spacing makes each plate independently aimable; 2 rows keep
			// documents near their scene station without hiding the original E point.
			const FVector Offset(350.f + (Index / 5) * 210.f, (Index % 5 - 2) * 190.f, 35.f);
			SpawnPlate(World, FName(*(TEXT("inspect.") + Document.Id.ToString())), *Position + Offset);
		}
		if (World->GetMapName().Contains(TEXT("L_BambooHollow")))
		{
			AQinglanInteractable* Shutter = SpawnPlate(World, FName(TEXT("device.inspection_shutter")),
				QinglanRoomLayout::RoomCenter(2) + FVector(900.f, -1000.f, 135.f));
			UGameInstance* Instance = World->GetGameInstance();
			UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
			const bool bOpen = Story && Story->HasFlag(TEXT("inspection.shutter.opened")) && !Story->HasFlag(TEXT("inspection.shutter.closed"));
			if (Shutter)
			{
				Shutter->Tags.Remove(FName(TEXT("InspectionShutterOpen")));
				Shutter->Tags.Remove(FName(TEXT("InspectionShutterClosed")));
				Shutter->Tags.AddUnique(FName(bOpen ? TEXT("InspectionShutterOpen") : TEXT("InspectionShutterClosed")));
				Shutter->DisplayNameKey = FName(bOpen ? TEXT("inspection.shutter_open") : TEXT("inspection.shutter_closed"));
				if (UStaticMeshComponent* Plate = Shutter->FindComponentByClass<UStaticMeshComponent>()) { Plate->SetRelativeRotation(FRotator(bOpen ? -70.f : 0.f, 0.f, 0.f)); }
			}
		}
	}

	bool Execute(AQinglanInteractable* Actor, AQinglanCharacter* Player)
	{
		if (!Actor) { return false; }
		const bool bShutter = Actor->InteractionId == FName(TEXT("device.inspection_shutter"));
		const FDocument* Document = Catalog().FindByPredicate([Actor](const FDocument& D) { return Actor->InteractionId == FName(*(TEXT("inspect.") + D.Id.ToString())); });
		if (!bShutter && !Document) { return false; }
		if (!Player || Player->GetWorld() != Actor->GetWorld() || !Player->IsPlayerControlled()
			|| FVector::Dist(Player->GetActorLocation(), Actor->GetActorLocation()) > 300.f) { return true; }
		UGameInstance* Instance = Player->GetGameInstance();
		UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
		if (!Story) { return true; }
		if (bShutter)
		{
			FQinglanRegistryGuard Guard;
			FQinglanRegistryGuardTerm Term;
			Term.Kind = TEXT("event"); Term.Event = TEXT("encounter.room_cleared.tutorial.2"); Guard.AllOf.Add(Term);
			const FDocument Gate{NAME_None, NAME_None, FName(TEXT("CH01_S02")), FName(TEXT("bamboo.room_1_2_guided")), {FName(TEXT("story.ch01.s02.line_03")), FName(TEXT("story.ch01.s02.line_04"))}};
			if (!CanRead(Story, Player, Gate) || QinglanStoryRuntime::EvaluateGuard(Story, Guard).Status != QinglanStoryRuntime::EQinglanGuardStatus::Satisfied)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(TEXT("inspection.locked"))); return true;
			}
			if (Story->HasFlag(TEXT("inspection.shutter.closed")))
			{
				QinglanStoryRuntime::Delegates(Story).OnRecordOpened.Broadcast(NAME_None,
					UQinglanHUDStatics::GameText(TEXT("inspection.shutter_closed")), Body(Story, Gate));
				return true;
			}
			const bool bClosing = Story->HasFlag(TEXT("inspection.shutter.opened"));
			Actor->Tags.Remove(FName(TEXT("InspectionShutterOpen")));
			Actor->Tags.Remove(FName(TEXT("InspectionShutterClosed")));
			Actor->Tags.AddUnique(FName(bClosing ? TEXT("InspectionShutterClosed") : TEXT("InspectionShutterOpen")));
			if (UStaticMeshComponent* Plate = Actor->FindComponentByClass<UStaticMeshComponent>())
			{
				Plate->SetRelativeRotation(FRotator(bClosing ? 0.f : -70.f, 0.f, 0.f));
			}
			Actor->DisplayNameKey = FName(bClosing ? TEXT("inspection.shutter_closed") : TEXT("inspection.shutter_open"));
			QinglanStoryRuntime::Delegates(Story).OnRecordOpened.Broadcast(NAME_None,
				UQinglanHUDStatics::GameText(Actor->DisplayNameKey), Body(Story, Gate));
			if (bClosing) { QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("device.shutter_tested"), {FName(TEXT("inspection.shutter.closed"))}); }
			else { QinglanStoryRuntime::NotifyGameplayEvent(Story, NAME_None, {FName(TEXT("inspection.shutter.opened"))}); }
			QinglanStoryRuntime::SaveStoryBoundary(Story);
			return true;
		}
		if (!CanRead(Story, Player, *Document))
		{
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(TEXT("inspection.locked"))); return true;
		}
		const FText Text = Body(Story, *Document);
		if (Text.IsEmpty()) { return true; }
		const bool bFirstRead = !Story->HasFlag(ReadFlag(*Document));
		if (bFirstRead) { QinglanStoryRuntime::NotifyGameplayEvent(Story, NAME_None, {ReadFlag(*Document)}); }
		// The real Character record UI delegate produces record.opened.<alias>.
		// Reopening an already-read physical entry displays it without farming
		// event_count; no collectible registry/profile read count is touched.
		QinglanStoryRuntime::Delegates(Story).OnRecordOpened.Broadcast(bFirstRead ? Document->Alias : NAME_None,
			UQinglanHUDStatics::GameText(Actor->DisplayNameKey), Text);
		if (bFirstRead) { QinglanStoryRuntime::SaveStoryBoundary(Story); }
		return true;
	}
}
