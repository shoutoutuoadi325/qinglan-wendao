// Blessing library implementation.
//
// FIXME-CONTRACT: docs/03 §5 and implementation-blueprint §2.6 specify the
// 12-blessing structure (4 groups x 3, one-stack cap, expire at expedition
// end) but NO authored names/effects exist anywhere (blueprint §5 "Genuine
// gaps: 12 blessings: no names/effects/stack-cap list exists anywhere"). The
// twelve definitions below are authored defaults consistent with the group
// themes and the stacking rules of docs/03 §4; every magnitude is a named,
// tunable definition here so data can replace it without code changes.
#include "Abilities/QinglanBlessingLibrary.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Persistence/QinglanSaveGame.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h" // LogQinglanCombat

namespace QinglanBlessingGroups
{
	const FName SwordOffense("sword_offense");
	const FName CastingEconomy("casting_economy");
	const FName Protection("protection");
	const FName ElementalInteractions("elemental_interactions");
}

namespace
{
	// String table id for /Game/Qinglan/Data/ST_Game (code-contracts:
	// "Localization keys"; the table name is ST_Game).
	const FName GameStringTableId(TEXT("ST_Game"));

	// Authored defaults (see file-header FIXME-CONTRACT), built once on first
	// use so no FName lives in static-initialization order limbo.
	const TArray<FQinglanBlessingDef>& BlessingTable()
	{
		static const TArray<FQinglanBlessingDef> Table = []()
		{
			TArray<FQinglanBlessingDef> T;
			T.Reserve(12); // docs/03 §5: 12 = 4 groups x 3.

			auto Make = [&T](FName Id, FName Group, float SwordDamagePct, float SwordEnergyBonus, float SwordEnergyCapBonus,
				float TechCostPct, float TechCooldownPct, float TechRawDamagePct, float BarrierBonus, float DamageTakenPct,
				float MaxHealthBonus, float ConductPct, float BurnDurBonus, float WetDurBonus)
			{
				FQinglanBlessingDef Def;
				Def.BlessingId = Id;
				Def.GroupId = Group;
				Def.NameTextKey = FName(*FString::Printf(TEXT("blessing.%s.name"), *Id.ToString()));
				Def.DescriptionTextKey = FName(*FString::Printf(TEXT("blessing.%s.desc"), *Id.ToString()));
				Def.bOneStackCap = true; // docs/03 §5: one-stack cap, never re-offered
				Def.SwordDamagePercent = SwordDamagePct;
				Def.SwordEnergyPerHitBonus = SwordEnergyBonus;
				Def.SwordEnergyCapBonus = SwordEnergyCapBonus;
				Def.TechniqueCostPercent = TechCostPct;
				Def.TechniqueCooldownPercent = TechCooldownPct;
				Def.TechniqueRawDamagePercent = TechRawDamagePct;
				Def.BarrierPointsBonus = BarrierBonus;
				Def.DamageTakenPercent = DamageTakenPct;
				Def.MaxHealthBonus = MaxHealthBonus;
				Def.ConductDamagePercent = ConductPct;
				Def.BurnDurationBonus = BurnDurBonus;
				Def.WetDurationBonus = WetDurBonus;
				T.Add(MoveTemp(Def));
			};

			// --- Sword offense (group theme per docs/03 §5; magnitudes align with docs/02 §4) ---
			Make("b_sharp_edge", QinglanBlessingGroups::SwordOffense, 12.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_heavy_hand", QinglanBlessingGroups::SwordOffense, 20.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_draining_edge", QinglanBlessingGroups::SwordOffense, 0.f, 2.f, 2.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			// --- Casting economy (docs/03 §4: min energy cost 1 rounded up) ---
			Make("b_quiet_current", QinglanBlessingGroups::CastingEconomy, 0.f, 0.f, 0.f, -15.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_still_water", QinglanBlessingGroups::CastingEconomy, 0.f, 0.f, 0.f, 0.f, -20.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_focused_will", QinglanBlessingGroups::CastingEconomy, 0.f, 0.f, 0.f, 0.f, 0.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			// --- Protection (docs/02 §6 barrier rules; docs/03 §5 expiry) ---
			Make("b_jade_shell", QinglanBlessingGroups::Protection, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_rooted_stance", QinglanBlessingGroups::Protection, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, -10.f, 0.f, 0.f, 0.f, 0.f, 0.f);
			Make("b_second_wind", QinglanBlessingGroups::Protection, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 15.f, 0.f, 0.f, 0.f, 0.f);
			// --- Elemental interactions (docs/03 §4: reaction mods affect Conduct only; docs/02 §7 durations) ---
			Make("b_thunder_mandate", QinglanBlessingGroups::ElementalInteractions, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 25.f, 0.f, 0.f);
			Make("b_lingering_ash", QinglanBlessingGroups::ElementalInteractions, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f);
			Make("b_cold_snap", QinglanBlessingGroups::ElementalInteractions, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 2.f);
			return T;
		}();
		return Table;
	}
}

void UQinglanBlessingLibrary::GetAllBlessingIds(TArray<FName>& OutIds)
{
	const TArray<FQinglanBlessingDef>& Table = BlessingTable();
	OutIds.Reset(Table.Num());
	for (const FQinglanBlessingDef& Def : Table)
	{
		OutIds.Add(Def.BlessingId);
	}
}

void UQinglanBlessingLibrary::GetBlessingsInGroup(FName GroupId, TArray<FName>& OutIds)
{
	const TArray<FQinglanBlessingDef>& Table = BlessingTable();
	OutIds.Reset(3); // docs/03 §5: three per group.
	for (const FQinglanBlessingDef& Def : Table)
	{
		if (Def.GroupId == GroupId)
		{
			OutIds.Add(Def.BlessingId);
		}
	}
}

bool UQinglanBlessingLibrary::FindBlessingDef(FName BlessingId, FQinglanBlessingDef& OutDef)
{
	for (const FQinglanBlessingDef& Def : BlessingTable())
	{
		if (Def.BlessingId == BlessingId)
		{
			OutDef = Def;
			return true;
		}
	}
	return false;
}

void UQinglanBlessingLibrary::GetBlessingText(const FQinglanBlessingDef& Def, FText& OutName, FText& OutDescription)
{
	// FText from string table only (code-contracts "Conventions"); a missing
	// key yields the key text via the engine's string-table fallback, never a
	// hardcoded literal.
	OutName = FText::FromStringTable(GameStringTableId, Def.NameTextKey.ToString());
	OutDescription = FText::FromStringTable(GameStringTableId, Def.DescriptionTextKey.ToString());
}

void UQinglanBlessingLibrary::GetOwnedBlessings(const UObject* WorldContext, TArray<FName>& OutOwned)
{
	OutOwned.Reset();
	const UWorld* World = WorldContext ? UGameplayStatics::GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		return;
	}
	UQinglanRunSubsystem* Run = GameInstance->GetSubsystem<UQinglanRunSubsystem>();
	if (!Run || !Run->IsInExpedition())
	{
		return; // Blessings exist only inside an expedition (docs/03 §5).
	}
	// Read-only bridge: the checkpoint payload owns "acquired blessings"
	// (save.json contract, blueprint §3). No mutation happens here.
	FQinglanCheckpoint Checkpoint;
	Run->ExportCheckpoint(Checkpoint);
	OutOwned = Checkpoint.AcquiredBlessings;
}

bool UQinglanBlessingLibrary::IsBlessingOfferable(FName BlessingId, const TArray<FName>& OwnedBlessingIds)
{
	FQinglanBlessingDef Def;
	if (!FindBlessingDef(BlessingId, Def))
	{
		return false;
	}
	if (Def.bOneStackCap && OwnedBlessingIds.Contains(BlessingId))
	{
		return false; // one-stack cap: never re-offer (docs/03 §5).
	}
	return true;
}

void UQinglanBlessingLibrary::ComputeModifiers(const TArray<FName>& OwnedBlessingIds, FQinglanBlessingModifiers& OutModifiers)
{
	// Reset then sum within category; consumers apply each category total
	// exactly once (docs/03 §4 "add within category, apply once").
	OutModifiers = FQinglanBlessingModifiers();
	for (const FName& Id : OwnedBlessingIds)
	{
		FQinglanBlessingDef Def;
		if (!FindBlessingDef(Id, Def))
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("ComputeModifiers: unknown blessing id '%s' ignored"), *Id.ToString());
			continue;
		}
		OutModifiers.SwordDamagePercent += Def.SwordDamagePercent;
		OutModifiers.SwordEnergyPerHitBonus += Def.SwordEnergyPerHitBonus;
		OutModifiers.SwordEnergyCapBonus += Def.SwordEnergyCapBonus;
		OutModifiers.TechniqueCostPercent += Def.TechniqueCostPercent;
		OutModifiers.TechniqueCooldownPercent += Def.TechniqueCooldownPercent;
		OutModifiers.TechniqueRawDamagePercent += Def.TechniqueRawDamagePercent;
		OutModifiers.BarrierPointsBonus += Def.BarrierPointsBonus;
		OutModifiers.DamageTakenPercent += Def.DamageTakenPercent;
		OutModifiers.MaxHealthBonus += Def.MaxHealthBonus;
		OutModifiers.ConductDamagePercent += Def.ConductDamagePercent;
		OutModifiers.BurnDurationBonus += Def.BurnDurationBonus;
		OutModifiers.WetDurationBonus += Def.WetDurationBonus;
	}
}

bool UQinglanBlessingLibrary::MakeBlessingEntry(FName BlessingId, FQinglanBlessing& OutEntry)
{
	FQinglanBlessingDef Def;
	if (!FindBlessingDef(BlessingId, Def))
	{
		return false;
	}
	FText Name;
	FText Description;
	GetBlessingText(Def, Name, Description);
	OutEntry.BlessingId = Def.BlessingId;
	OutEntry.DisplayName = MoveTemp(Name);
	OutEntry.Description = MoveTemp(Description);
	OutEntry.GroupId = Def.GroupId;
	return true;
}
