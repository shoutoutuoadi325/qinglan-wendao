// Talisman component implementation (see QinglanTalismanComponent.h).
// Effect semantics come from docs/03-progression-and-economy.md section 5 and
// the combat rules of docs/02 sections 5-7; every effect resolves through the
// owner's UQinglanCombatComponent so barrier/health/status changes stay inside
// the single authoritative combat pipeline.
#include "Combat/QinglanTalismanComponent.h"

#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h" // QinglanCombatTunables boss tag
#include "Core/QinglanInteractable.h" // QinglanInteraction::TravelToMap/HubMapPath
#include "Engine/DamageEvents.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"

UQinglanTalismanComponent::UQinglanTalismanComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // offline single-process game
}

void UQinglanTalismanComponent::BeginPlay()
{
	Super::BeginPlay();

	// Authored default: no sanctuary UI exists yet, so the slots seed themselves
	// from the charged talismans in catalog order once per pawn. The bindings of
	// docs/02 section 1 (slots Q/R) therefore work right after the sanctuary
	// refill; SetEquippedTalismans remains the explicit loadout path.
	if (EquippedTalismans.Num() > 0)
	{
		return;
	}
	UQinglanProfileSubsystem* Profile = GetProfile();
	if (!Profile)
	{
		return; // headless/test worlds: slots stay empty until set explicitly
	}
	for (const FQinglanTalismanCatalogEntry& Entry : UQinglanEconomyLibrary::GetTalismanCatalog())
	{
		if (EquippedTalismans.Num() >= QinglanProgression::EquippedTalismanLimit)
		{
			break;
		}
		if (Profile->GetTalismanChargeCount(Entry.TalismanId) > 0)
		{
			EquippedTalismans.Add(Entry.TalismanId);
		}
	}
}

UQinglanProfileSubsystem* UQinglanTalismanComponent::GetProfile() const
{
	UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
}

bool UQinglanTalismanComponent::SetEquippedTalismans(const TArray<FName>& TalismanIds)
{
	if (TalismanIds.Num() > QinglanProgression::EquippedTalismanLimit)
	{
		return false; // at most two equipped talismans (docs/03 section 5)
	}
	UQinglanProfileSubsystem* Profile = GetProfile();
	TSet<FName> Seen;
	Seen.Reserve(TalismanIds.Num());
	for (const FName TalismanId : TalismanIds)
	{
		FQinglanTalismanCatalogEntry Entry;
		if (!UQinglanEconomyLibrary::FindTalisman(TalismanId, Entry))
		{
			return false; // unknown ids cannot be equipped
		}
		if (Seen.Contains(TalismanId))
		{
			return false; // no duplicate slots
		}
		Seen.Add(TalismanId);
		// Charges are the availability record (docs/03 section 5: expedition
		// charges, not inventory): a talisman with no charged entry is not
		// equipable. Without a profile (headless tests) catalog validity alone
		// decides, so slots stay drivable.
		if (Profile && Profile->GetTalismanChargeCount(TalismanId) <= 0)
		{
			return false;
		}
	}

	EquippedTalismans = TalismanIds;
	return true;
}

bool UQinglanTalismanComponent::UseSlot(int32 Slot)
{
	if (!EquippedTalismans.IsValidIndex(Slot))
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Talisman slot %d has no talisman equipped"), Slot);
		return false;
	}
	AActor* Owner = GetOwner();
	UQinglanCombatComponent* Combat = Owner ? Owner->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
	if (!Owner || !Combat || !Combat->IsAlive())
	{
		return false; // dead players use nothing (docs/02 section 3 priority)
	}
	const FName TalismanId = EquippedTalismans[Slot];
	FQinglanTalismanCatalogEntry Entry;
	if (!UQinglanEconomyLibrary::FindTalisman(TalismanId, Entry))
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("UseSlot: unknown talisman id '%s'"), *TalismanId.ToString());
		return false;
	}
	UQinglanProfileSubsystem* Profile = GetProfile();
	if (!Profile || Profile->GetTalismanChargeCount(TalismanId) <= 0)
	{
		return false; // out of charges (or no profile): nothing consumed, nothing fired
	}

	// Execute first, journal second: a rejected effect never spends a charge,
	// and the charge check above already fenced the free-use path.
	if (!ExecuteEffect(Entry, Owner, *Combat))
	{
		return false;
	}
	if (!Profile->TryConsumeTalismanCharge(TalismanId))
	{
		// Unreachable on the single game thread (the check above passed); kept
		// as the atomic guard so a future async caller can never double-fire.
		UE_LOG(LogQinglanCombat, Warning, TEXT("UseSlot: charge for '%s' vanished before consumption"), *TalismanId.ToString());
	}
	OnTalismanUsed.Broadcast(TalismanId, Profile->GetTalismanChargeCount(TalismanId));
	return true;
}

bool UQinglanTalismanComponent::ExecuteEffect(const FQinglanTalismanCatalogEntry& Entry, AActor* Owner, UQinglanCombatComponent& Combat)
{
	if (Entry.EffectKind == FName(TEXT("heal_25")))
	{
		// Healing Talisman: heal the catalog magnitude, bounded by maximum
		// health (docs/03 section 5; docs/02 section 6 healing bound) and
		// scaled by the permanent healing percent (Herbal Knowledge, Verdant
		// Seed) through the authoritative Heal entry point.
		float UnusedEnergyRegen = 0.f;
		float UnusedStaminaRegen = 0.f;
		float HealingPercent = 0.f;
		UQinglanEconomyLibrary::AppendPermanentRegen(this, UnusedEnergyRegen, UnusedStaminaRegen, HealingPercent);
		Combat.Heal(Entry.Magnitude * (1.f + HealingPercent / 100.f));
		return true;
	}
	if (Entry.EffectKind == FName(TEXT("cleanse")))
	{
		// Cleansing Talisman: remove Burn/Root (docs/03 section 5; docs/02
		// section 7). The combat component's RemoveStatus is the single removal
		// entry point and also clears Root's movement mirror in the sibling
		// status component, so no QINGLAN_HAS_STATUS_COMPONENT branch is needed.
		Combat.RemoveStatus(EQinglanStatus::Burn);
		Combat.RemoveStatus(EQinglanStatus::Root);
		return true;
	}
	if (Entry.EffectKind == FName(TEXT("barrier_20")))
	{
		// Protective Talisman: barrier points that absorb before health; the
		// combat component applies the stronger-replaces-weaker rule
		// (docs/03 section 5; docs/02 section 6). Talismans are elementless.
		Combat.ApplyBarrier(EQinglanElement::None, Entry.Magnitude, BarrierDurationSeconds);
		return true;
	}
	if (Entry.EffectKind == FName(TEXT("focus_25")))
	{
		// Focus Talisman: restore energy, bounded by the maximum (docs/03
		// section 5); GrantEnergy is the bounded restore entry point.
		Combat.GrantEnergy(Entry.Magnitude);
		return true;
	}
	if (Entry.EffectKind == FName(TEXT("shock_interrupt")))
	{
		// Shock Talisman: interrupt ordinary enemies within the catalog range
		// (docs/03 section 5). MECHANISM NOTE: the stagger state is protected
		// on AQinglanEnemyCharacter and has no public setter, so the shock rides
		// the existing TakeDamage/stagger entry point (docs/02 section 8): its
		// gate AI->CanBeStaggeredBy requires the authored minimum damage,
		// respects stagger immunity, and never staggers bosses. Bosses are
		// filtered here too so "ordinary enemies" also scopes the damage.
		UWorld* World = GetWorld();
		if (!World)
		{
			return false;
		}
		const FVector Origin = Owner->GetActorLocation();
		const float ShockRange = Entry.Range > 0.f ? Entry.Range : 300.f; // docs/03 section 5: 300 cm
		int32 Interrupted = 0;
		for (TActorIterator<AQinglanEnemyCharacter> It(World); It; ++It)
		{
			AQinglanEnemyCharacter* Enemy = *It;
			if (!Enemy || !Enemy->IsAlive())
			{
				continue;
			}
			if (Enemy->ActorHasTag(FName(QinglanCombatTunables::QinglanBossTag)))
			{
				continue; // ordinary enemies only (docs/03 section 5)
			}
			if (FVector::Dist(Origin, Enemy->GetActorLocation()) > ShockRange)
			{
				continue;
			}
			Enemy->TakeDamage(ShockDamage, FDamageEvent(), nullptr, Owner);
			++Interrupted;
		}
		// Target-dependent like recall: an empty radius is a rejected use, so
		// the charge survives (self-affecting kinds always fire and consume).
		return Interrupted > 0;
	}
	if (Entry.EffectKind == FName(TEXT("recall")))
	{
		// Recall Talisman: docs/03 section 5 "request retreat from a cleared
		// room". Refused requests (no expedition, a live encounter, or a
		// banking rejection) never consume the charge; an accepted retreat
		// banks at the voluntary-retreat rate through the run subsystem's
		// authoritative EndExpedition and returns to the hub through the same
		// travel path the realm exits use (Core/QinglanInteractable.cpp).
		// EndExpedition closes the run, so a duplicate press lands in the
		// refusal branch and spends nothing.
		UWorld* World = GetWorld();
		if (!World)
		{
			return false;
		}
		AQinglanCharacter* Player = Cast<AQinglanCharacter>(Owner);
		UGameInstance* GameInstance = World->GetGameInstance();
		UQinglanRunSubsystem* Run = GameInstance ? GameInstance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
		if (!Run || !Run->IsInExpedition())
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("Recall rejected: no active expedition"));
			if (Player)
			{
				// FIXME-CONTRACT: no string-table key for the recall refusal.
				Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Recall.NoExpedition", "Recall works only inside an expedition."));
			}
			return false;
		}
		// Cleared-room gate: the encounter director is the room-clear authority
		// the doors and exits already use ("doors open on clear").
		for (TActorIterator<AQinglanEncounterDirector> It(World); It; ++It)
		{
			if (It->IsEncounterActive())
			{
				UE_LOG(LogQinglanCombat, Verbose, TEXT("Recall rejected: current room is not cleared"));
				if (Player)
				{
					// FIXME-CONTRACT: no string-table key for the recall refusal.
					Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Recall.RoomNotCleared", "Clear the room before recalling."));
				}
				return false;
			}
		}
		// Voluntary retreat: banks 70% floored per stack (docs/03 section 6).
		Run->EndExpedition(/*bVictory=*/false, /*bVoluntaryRetreat=*/true);
		if (Run->IsInExpedition())
		{
			// EndExpedition kept the run open (banking overflow rejection):
			// the retreat was not granted, so the charge survives.
			UE_LOG(LogQinglanCombat, Warning, TEXT("Recall rejected: retreat banking failed; expedition kept open"));
			return false;
		}
		// Do not consume the charge when save-before-travel rejected departure.
		return QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
	}

	UE_LOG(LogQinglanCombat, Warning, TEXT("UseSlot: talisman '%s' carries unknown effect kind '%s'"),
		*Entry.TalismanId.ToString(), *Entry.EffectKind.ToString());
	return false;
}
