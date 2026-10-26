// Constant enemy specs: six archetypes (docs/02 section 8) + four bosses
// (docs/02 section 9; blueprint section 2.4; plus the Foundation trial's
// elite construct, AUD-20). Numbers with a doc citation are contract; numbers
// marked "authored" have no doc value and are the current design pass. The
// editor importer may later mirror these into data assets; the C++ constants
// stay the reference.
#include "AI/QinglanEnemyCatalog.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

namespace QinglanPatternIds
{
	FName SweepingBlade()   { static const FName V(TEXT("sweeping_blade"));   return V; }
	FName DelayedRoots()    { static const FName V(TEXT("delayed_roots"));    return V; }
	FName SpiritBolts()     { static const FName V(TEXT("spirit_bolts"));     return V; }
	FName FireLance()       { static const FName V(TEXT("fire_lance"));       return V; }
	FName RingShockwave()   { static const FName V(TEXT("ring_shockwave"));   return V; }
	FName ChargingStrike()  { static const FName V(TEXT("charging_strike"));  return V; }
	FName StormBoltVolley() { static const FName V(TEXT("storm_bolt_volley")); return V; }
	FName SpiritRend()      { static const FName V(TEXT("spirit_rend"));      return V; }
}

namespace
{
	// Bounded cache for transient catalog assets (AUD-46): the spec id space is
	// fixed (six archetypes + four bosses), so keying by EnemyId caps the
	// instance count at that size; repeated rooms, expeditions, and trials
	// reuse the same assets instead of rooting a new one per encounter.
	// Entries stay rooted while cached because several holders (wave queues,
	// weak data mirrors, the director's TU-local registry) are not
	// GC-visible; the size cap evicts oldest-first with RemoveFromRoot so the
	// rooted set stays bounded even if the spec list ever grows.
	constexpr int32 MaxCachedCatalogAssets = 16;
	TMap<FName, TObjectPtr<UQinglanEnemyDataAsset>> GCatalogAssetCache;
	TArray<FName> GCatalogAssetCacheOrder;

	void TouchCatalogAsset(FName EnemyId)
	{
		GCatalogAssetCacheOrder.Remove(EnemyId);
		GCatalogAssetCacheOrder.Add(EnemyId);
	}

	void EvictOverflowCatalogAssets()
	{
		while (GCatalogAssetCache.Num() > MaxCachedCatalogAssets)
		{
			const FName Oldest = GCatalogAssetCacheOrder[0];
			GCatalogAssetCacheOrder.RemoveAt(0);
			if (TObjectPtr<UQinglanEnemyDataAsset>* Found = GCatalogAssetCache.Find(Oldest))
			{
				if (*Found)
				{
					(*Found)->RemoveFromRoot();
				}
				GCatalogAssetCache.Remove(Oldest);
			}
		}
	}

	FQinglanBossPattern MakePattern(FName PatternId, float Weight, float TelegraphSeconds,
		float Damage, float RangeCm, bool bGroundMarker, FName ChargeCue, FName ReleaseCue)
	{
		FQinglanBossPattern P;
		P.PatternId = PatternId;
		P.Weight = Weight;
		P.TelegraphDuration = TelegraphSeconds;
		P.Damage = Damage;
		P.Range = RangeCm;
		P.bGroundMarker = bGroundMarker;
		P.CueIds.Add(ChargeCue);
		P.CueIds.Add(ReleaseCue);
		return P;
	}

	FQinglanArchetypeSpec MakeSpiritWolf()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("spirit_wolf"));
		S.Health = 45.f;   // docs/02 section 8
		S.Defense = 5.f;   // docs/02 section 8
		S.MoveSpeed = 480.f;        // authored (fast skirmisher)
		S.AttackRange = 220.f;      // authored lunge reach
		S.AttackDamage = 10.f;      // authored
		S.AttackCoefficient = 0.5f; // authored
		S.AttackWindup = 0.65f;     // "0.65 s shoulder dip" docs/02 section 8
		S.AttackActive = 0.25f;     // authored
		S.AttackRecover = 0.55f;    // authored
		S.TelegraphDuration = 0.65f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.spirit_wolf.dip")); // authored cue id
		S.bRanged = false;
		S.AggroRadius = 1500.f;     // authored sensing radius
		S.AttackShape = EQinglanAttackShape::MeleeArc;
		S.bCircleBeforeAttack = true; // "Circles, then lunges" docs/02 section 8
		S.OrbitRadius = 440.f;        // authored orbit ring
		S.ComboCount = 1;
		S.LungeSpeed = 900.f;         // authored lunge velocity (cm/s)
		return S;
	}

	FQinglanArchetypeSpec MakeCorruptedDisciple()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("corrupted_disciple"));
		S.Health = 70.f;  // docs/02 section 8
		S.Defense = 10.f; // docs/02 section 8
		S.MoveSpeed = 340.f;      // authored
		S.AttackRange = 180.f;    // authored sword reach
		S.AttackDamage = 12.f;    // authored
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 0.5f;    // authored ("raised weapon" has no doc number)
		S.AttackActive = 0.2f;
		S.AttackRecover = 0.6f;   // authored punish window
		S.TelegraphDuration = 0.5f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.corrupted_disciple.raise")); // authored cue id
		S.AttackShape = EQinglanAttackShape::MeleeArc;
		S.ComboCount = 2;             // "Two sword hits" docs/02 section 8
		S.bRetreatAfterCombo = true;  // "...then retreats"
		S.RetreatSeconds = 0.9f;      // authored
		return S;
	}

	FQinglanArchetypeSpec MakeHollowArcher()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("hollow_archer"));
		S.Health = 40.f; // docs/02 section 8
		S.Defense = 5.f; // docs/02 section 8
		S.MoveSpeed = 320.f;      // authored
		S.AttackRange = 2600.f;   // authored aimed-arrow range
		S.AttackDamage = 9.f;     // authored
		S.AttackCoefficient = 0.4f;
		S.AttackWindup = 0.9f;    // "0.9 s bow glow" docs/02 section 8
		S.AttackActive = 0.1f;
		S.AttackRecover = 0.9f;   // authored
		S.TelegraphDuration = 0.9f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.hollow_archer.glow")); // authored cue id
		S.bRanged = true;
		S.AggroRadius = 1800.f;    // authored (sees farther than melee)
		S.AttackShape = EQinglanAttackShape::AimedProjectile;
		S.PreferredRange = 1100.f; // authored comfort band
		S.MinComfortRange = 650.f; // authored: reposition when rushed
		S.ProjectileSpeed = 3600.f; // authored arrow speed (cm/s)
		return S;
	}

	FQinglanArchetypeSpec MakeStoneSentinel()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("stone_sentinel"));
		S.Health = 120.f; // docs/02 section 8
		S.Defense = 25.f; // docs/02 section 8
		S.MoveSpeed = 200.f;    // authored ("slow")
		S.AttackRange = 280.f;  // authored slam reach
		S.AttackDamage = 22.f;  // authored heavy hit
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 1.2f;  // "1.2 s ground wedge" docs/02 section 8
		S.AttackActive = 0.3f;
		S.AttackRecover = 1.1f; // authored
		S.TelegraphDuration = 1.2f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.stone_sentinel.wedge")); // authored cue id
		S.AttackShape = EQinglanAttackShape::MeleeCone;
		S.bRootImmune = true;   // authored: stone body ignores vine roots
		return S;
	}

	FQinglanArchetypeSpec MakeEmberWisp()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("ember_wisp"));
		S.Health = 35.f; // docs/02 section 8
		S.Defense = 0.f; // docs/02 section 8
		S.MoveSpeed = 0.f;       // "from fixed perches": never moves, docs/02 section 8
		S.AttackRange = 2400.f;  // authored fire bolt range
		S.AttackDamage = 8.f;    // authored
		S.AttackCoefficient = 0.4f;
		S.AttackWindup = 0.8f;   // "0.8 s bright core" docs/02 section 8
		S.AttackActive = 0.1f;
		S.AttackRecover = 1.2f;  // authored
		S.TelegraphDuration = 0.8f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.ember_wisp.core")); // authored cue id
		S.bRanged = true;
		S.AttackShape = EQinglanAttackShape::AimedProjectile;
		S.bFixedPerch = true;        // docs/02 section 8
		S.bRootImmune = true;        // authored: hovering perch
		S.ProjectileSpeed = 2400.f;  // authored fire bolt speed (cm/s)
		S.Element = EQinglanElement::Fire;
		// Authored: fire bolts apply Burn (docs/01 section 5 "Introduces Burn";
		// tick rules in docs/02 section 7).
		S.StatusOnHit = EQinglanStatus::Burn;
		return S;
	}

	FQinglanArchetypeSpec MakeRootboundKeeper()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("rootbound_keeper"));
		S.Health = 90.f;  // docs/02 section 8
		S.Defense = 15.f; // docs/02 section 8
		S.MoveSpeed = 300.f;    // authored
		S.AttackRange = 1500.f; // authored cast range
		S.AttackDamage = 14.f;  // authored
		S.AttackCoefficient = 0.4f;
		S.AttackWindup = 1.0f;  // "1.0 s vine ring" docs/02 section 8
		S.AttackActive = 0.2f;
		S.AttackRecover = 1.3f; // authored
		S.TelegraphDuration = 1.0f;
		S.TelegraphCueId = FName(TEXT("cue.enemy.rootbound_keeper.ring")); // authored cue id
		S.AttackShape = EQinglanAttackShape::GroundRing;
		S.GroundRingRadius = 500.f; // authored marked-area radius ("leave marked area")
		S.bRootImmune = true;       // authored: it is itself roots
		// Authored: caught-in-the-ring hits apply Root (1 s per docs/02 section 7).
		S.StatusOnHit = EQinglanStatus::Root;
		return S;
	}

	FQinglanArchetypeSpec MakeBambooGuardian()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("bamboo_guardian"));
		S.Health = 900.f;  // docs/02 section 9
		S.Defense = 15.f;  // docs/02 section 9
		S.MoveSpeed = 300.f;       // authored
		S.AttackRange = 450.f;     // authored sweep reach
		S.AttackDamage = 30.f;     // authored heavy baseline
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 1.0f;     // authored default; per-pattern telegraphs override
		S.AttackActive = 0.3f;
		S.AttackRecover = 1.4f;    // authored idle break carrier
		S.TelegraphDuration = 1.0f;
		S.TelegraphCueId = FName(TEXT("cue.boss.bamboo_guardian.default")); // authored cue id
		S.AggroRadius = 6000.f;    // authored: whole-arena boss aggro
		S.AttackShape = EQinglanAttackShape::MeleeCone;
		S.ProjectileSpeed = 2800.f; // authored spirit-bolt speed (cm/s)
		S.bBoss = true;
		S.PhaseTwoAtHealthFraction = 0.5f; // "At 50% HP, shorten idle breaks" docs/02 section 9
		S.ArenaDiameter = 4500.f;          // 45 m arena, docs/02 section 9
		S.GroundRingRadius = 650.f;        // authored root ring radius
		S.Patterns.Add(MakePattern(QinglanPatternIds::SweepingBlade(), /*Weight*/ 3.f, /*Telegraph*/ 1.0f,
			/*Damage*/ 30.f, /*Range*/ 450.f, /*bGroundMarker*/ false,
			FName(TEXT("cue.boss.bamboo.sweep.charge")), FName(TEXT("cue.boss.bamboo.sweep.release"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::DelayedRoots(), 2.f, 1.0f,
			22.f, 650.f, true,
			FName(TEXT("cue.boss.bamboo.roots.mark")), FName(TEXT("cue.boss.bamboo.roots.erupt"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::SpiritBolts(), 2.f, 0.9f,
			16.f, 2600.f, false,
			FName(TEXT("cue.boss.bamboo.bolt.aim")), FName(TEXT("cue.boss.bamboo.bolt.release"))));
		return S;
	}

	FQinglanArchetypeSpec MakeAshenGuardian()
	{
		FQinglanArchetypeSpec S;
		S.EnemyId = FName(TEXT("ashen_guardian"));
		S.Health = 1400.f; // docs/02 section 9
		S.Defense = 20.f;  // docs/02 section 9
		S.MoveSpeed = 330.f;    // authored
		S.AttackRange = 900.f;  // authored shockwave baseline
		S.AttackDamage = 30.f;  // authored
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 1.0f;
		S.AttackActive = 0.3f;
		S.AttackRecover = 1.3f;
		S.TelegraphDuration = 1.0f;
		S.TelegraphCueId = FName(TEXT("cue.boss.ashen_guardian.default")); // authored cue id
		S.AggroRadius = 6000.f; // authored
		S.Element = EQinglanElement::Fire;
		S.AttackShape = EQinglanAttackShape::AimedProjectile;
		S.ProjectileSpeed = 4200.f; // authored fire-lance speed (cm/s)
		S.bBoss = true;
		S.PhaseTwoAtHealthFraction = 0.5f; // "At 50%, add intermittent embers outside marked safe lanes" docs/02 section 9
		S.ArenaDiameter = 4500.f;          // authored (doc gives no Ashen arena size)
		S.MaxImmunityPhaseSeconds = 8.f;   // "No immunity phases longer than eight seconds" docs/02 section 9
		S.GroundRingRadius = 900.f;        // authored shockwave radius with safe gaps
		S.Patterns.Add(MakePattern(QinglanPatternIds::FireLance(), 3.f, 1.0f,
			26.f, 2800.f, false,
			FName(TEXT("cue.boss.ashen.lance.gather")), FName(TEXT("cue.boss.ashen.lance.release"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::RingShockwave(), 2.f, 1.2f,
			30.f, 900.f, true,
			FName(TEXT("cue.boss.ashen.ring.expand")), FName(TEXT("cue.boss.ashen.ring.safe_gap"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::ChargingStrike(), 2.f, 1.1f,
			34.f, 1500.f, false,
			FName(TEXT("cue.boss.ashen.charge.windup")), FName(TEXT("cue.boss.ashen.charge.impact"))));
		return S;
	}

	FQinglanArchetypeSpec MakeBrokenGuardian()
	{
		FQinglanArchetypeSpec S;
		// Docs list the Broken Guardian only as the Golden Core trial's final
		// opponent (docs/02 section 9; blueprint section 2.4); all numbers here
		// are authored, scaled from ashen_guardian for the Golden Core stage.
		S.EnemyId = FName(TEXT("broken_guardian"));
		S.Health = 1600.f;
		S.Defense = 22.f;
		S.MoveSpeed = 310.f;
		S.AttackRange = 500.f;
		S.AttackDamage = 28.f;
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 0.9f;    // trial strikes need >= 0.9 s warning, docs/02 section 9
		S.AttackActive = 0.3f;
		S.AttackRecover = 1.2f;
		S.TelegraphDuration = 0.9f;
		S.TelegraphCueId = FName(TEXT("cue.boss.broken_guardian.default")); // authored cue id
		S.AggroRadius = 6000.f;  // authored
		S.AttackShape = EQinglanAttackShape::MultiGroundStrikes;
		S.ProjectileSpeed = 3000.f; // authored (unused by ground strikes)
		S.bBoss = true;
		S.PhaseTwoAtHealthFraction = 0.5f; // authored: same enrage-lite rule as the other bosses
		S.ArenaDiameter = 4500.f;          // authored
		S.MaxImmunityPhaseSeconds = 8.f;   // authored: reuses the Ashen immunity cap
		S.GroundRingRadius = 300.f;        // authored per-strike outline radius
		S.Patterns.Add(MakePattern(QinglanPatternIds::StormBoltVolley(), 3.f, 0.9f,
			20.f, 300.f, true,
			FName(TEXT("cue.boss.broken.storm.outline")), FName(TEXT("cue.boss.broken.storm.strike"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::SpiritRend(), 2.f, 1.0f,
			32.f, 500.f, false,
			FName(TEXT("cue.boss.broken.rend.windup")), FName(TEXT("cue.boss.broken.rend.release"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::DelayedRoots(), 2.f, 1.0f,
			24.f, 600.f, true,
			FName(TEXT("cue.boss.broken.roots.mark")), FName(TEXT("cue.boss.broken.roots.erupt"))));
		return S;
	}

	FQinglanArchetypeSpec MakeEliteConstruct()
	{
		FQinglanArchetypeSpec S;
		// AUD-20: the Foundation trial's final elite construct (CH03_S02).
		// docs/story/CH03.md S02 authorizes the required kill but gives no stat
		// block; every number is authored between stone_sentinel and
		// bamboo_guardian so a tier-1 player can finish the kill on the 60 m
		// storm platform (both pattern telegraphs stay >= 0.9 s like every
		// other trial hazard).
		S.EnemyId = FName(TEXT("elite_construct"));
		S.Health = 620.f;
		S.Defense = 12.f;
		S.MoveSpeed = 260.f;
		S.AttackRange = 420.f;     // sweeping-blade reach (pattern range below)
		S.AttackDamage = 24.f;
		S.AttackCoefficient = 0.5f;
		S.AttackWindup = 1.0f;     // authored default; per-pattern telegraphs override
		S.AttackActive = 0.3f;
		S.AttackRecover = 1.2f;    // authored punish window
		S.TelegraphDuration = 1.0f;
		S.TelegraphCueId = FName(TEXT("cue.boss.elite_construct.default")); // authored cue id
		S.AggroRadius = 6000.f;    // authored: whole-platform boss aggro
		S.AttackShape = EQinglanAttackShape::MeleeCone;
		S.bBoss = true;
		S.PhaseTwoAtHealthFraction = 0.5f; // authored: same enrage-lite rule as the other bosses
		S.ArenaDiameter = 6000.f;          // the 60 m storm platform, docs/dev/level-layout.md
		S.GroundRingRadius = 550.f;        // strike outline radius = delayed-strike pattern range
		S.Patterns.Add(MakePattern(QinglanPatternIds::SweepingBlade(), /*Weight*/ 2.f, /*Telegraph*/ 1.1f,
			/*Damage*/ 24.f, /*Range*/ 420.f, /*bGroundMarker*/ false,
			FName(TEXT("cue.boss.elite_construct.sweep.charge")), FName(TEXT("cue.boss.elite_construct.sweep.release"))));
		S.Patterns.Add(MakePattern(QinglanPatternIds::DelayedRoots(), 3.f, 1.0f,
			20.f, 550.f, true,
			FName(TEXT("cue.boss.elite_construct.strike.mark")), FName(TEXT("cue.boss.elite_construct.strike.erupt"))));
		return S;
	}
} // namespace

namespace QinglanEnemyCatalog
{
	const TArray<FQinglanArchetypeSpec>& GetAllArchetypes()
	{
		static const TArray<FQinglanArchetypeSpec> Archetypes =
		{
			MakeSpiritWolf(),
			MakeCorruptedDisciple(),
			MakeHollowArcher(),
			MakeStoneSentinel(),
			MakeEmberWisp(),
			MakeRootboundKeeper()
		};
		return Archetypes;
	}

	const TArray<FQinglanArchetypeSpec>& GetAllBosses()
	{
		// Order matters: BuildRoomWave falls back to Bosses[0] and
		// GetBossIdForRealm scans by id, so the expedition bosses keep their
		// authored slots and the trial-only construct appends last (AUD-20).
		static const TArray<FQinglanArchetypeSpec> Bosses =
		{
			MakeBambooGuardian(),
			MakeAshenGuardian(),
			MakeBrokenGuardian(),
			MakeEliteConstruct()
		};
		return Bosses;
	}

	const FQinglanArchetypeSpec* FindSpec(FName EnemyId)
	{
		if (EnemyId.IsNone())
		{
			return nullptr;
		}
		for (const FQinglanArchetypeSpec& Spec : GetAllArchetypes())
		{
			if (Spec.EnemyId == EnemyId)
			{
				return &Spec;
			}
		}
		for (const FQinglanArchetypeSpec& Spec : GetAllBosses())
		{
			if (Spec.EnemyId == EnemyId)
			{
				return &Spec;
			}
		}
		return nullptr;
	}

	UQinglanEnemyDataAsset* CreateDataAssetFromSpec(const FQinglanArchetypeSpec& Spec, UObject* Outer)
	{
		if (!Outer || Spec.EnemyId.IsNone())
		{
			return nullptr;
		}

		// Cached reuse first (AUD-46): spec assets are read-only configuration,
		// so every consumer may share one instance per archetype.
		if (const TObjectPtr<UQinglanEnemyDataAsset>* Cached = GCatalogAssetCache.Find(Spec.EnemyId))
		{
			if (*Cached)
			{
				TouchCatalogAsset(Spec.EnemyId);
				return *Cached;
			}
			GCatalogAssetCache.Remove(Spec.EnemyId);
		}

		UClass* AssetClass = Spec.bBoss
			? UQinglanBossDataAsset::StaticClass()
			: UQinglanEnemyDataAsset::StaticClass();

		const FName AssetName = MakeUniqueObjectName(Outer, AssetClass,
			*FString::Printf(TEXT("DA_QinglanEnemy_%s"), *Spec.EnemyId.ToString()));
		UQinglanEnemyDataAsset* Asset = Spec.bBoss
			? NewObject<UQinglanEnemyDataAsset>(Outer, AssetClass, AssetName)
			: NewObject<UQinglanEnemyDataAsset>(Outer, AssetClass, AssetName);
		if (!Asset)
		{
			return nullptr;
		}

		Asset->EnemyId = Spec.EnemyId;
		Asset->Health = Spec.Health;
		Asset->Defense = Spec.Defense;
		Asset->MoveSpeed = Spec.MoveSpeed;
		Asset->AttackRange = Spec.AttackRange;
		Asset->AttackDamage = Spec.AttackDamage;
		Asset->AttackCoefficient = Spec.AttackCoefficient;
		Asset->AttackWindup = Spec.AttackWindup;
		Asset->AttackActive = Spec.AttackActive;
		Asset->AttackRecover = Spec.AttackRecover;
		Asset->Telegraph.Duration = Spec.TelegraphDuration;
		Asset->Telegraph.CueId = Spec.TelegraphCueId;
		Asset->bRanged = Spec.bRanged;
		Asset->bRootImmune = Spec.bRootImmune;
		Asset->AggroRadius = Spec.AggroRadius;

		// Presentation convention (mesh import + anim bootstrap paths), with
		// the real imported idle sequence names (Quaternius take names differ
		// per pack: Bat/Dragon idle via their Flying take, Skeleton/Slime have
		// explicit Idles). Every reference null-safe: absent assets degrade to
		// the bind pose.
		struct FPresent { const TCHAR* Mesh; const TCHAR* Idle; };
		static const TMap<FName, FPresent> MeshGroups = {
			{ FName(TEXT("spirit_wolf")),         { TEXT("Monsters/SK_Skeleton"), TEXT("Monsters/SK_SkeletonSkeletonArmature_Skeleton_Idle") } },
			{ FName(TEXT("corrupted_disciple")),  { TEXT("Human/SK_Human"),       TEXT("Human/SK_HumanHuman_Armature_Idle") } },
			// The Bat/Dragon FBX imports keep the pack's internal object names
			// (Bat/Dragon); the SK_-prefixed assets are only the anim takes.
			{ FName(TEXT("hollow_archer")),       { TEXT("Monsters/Bat"),         TEXT("Monsters/SK_BatBatArmature_Bat_Flying") } },
			{ FName(TEXT("stone_sentinel")),      { TEXT("Knight/SK_Knight"),     TEXT("Knight/SK_KnightHumanArmature_Idle") } },
			{ FName(TEXT("ember_wisp")),          { TEXT("Monsters/SK_Slime"),    TEXT("Monsters/SK_SlimeArmature_Slime_Idle") } },
			{ FName(TEXT("rootbound_keeper")),    { TEXT("Monsters/SK_Slime"),    TEXT("Monsters/SK_SlimeArmature_Slime_Idle") } },
			{ FName(TEXT("bamboo_guardian")),     { TEXT("Knight/SK_Knight"),     TEXT("Knight/SK_KnightHumanArmature_Idle") } },
			{ FName(TEXT("ashen_guardian")),      { TEXT("Knight/SK_Knight"),     TEXT("Knight/SK_KnightHumanArmature_Idle") } },
			{ FName(TEXT("broken_guardian")),     { TEXT("Monsters/Dragon"),      TEXT("Monsters/SK_DragonDragonArmature_Dragon_Flying") } },
			{ FName(TEXT("elite_construct")),     { TEXT("Knight/SK_Knight"),     TEXT("Knight/SK_KnightHumanArmature_Idle") } },
		};
		if (const FPresent* Group = MeshGroups.Find(Spec.EnemyId))
		{
			const FString GroupPath = FString::Printf(TEXT("/Game/Qinglan/Characters/%s"), Group->Mesh);
			// Group folder is the first path segment after Characters (works for
			// both SK_-prefixed and pack-named meshes like Monsters/Bat).
			const FString RelativeMeshPath(Group->Mesh);
			const FString GroupName = RelativeMeshPath.Left(
				RelativeMeshPath.Contains(TEXT("/")) ? RelativeMeshPath.Find(TEXT("/")) : RelativeMeshPath.Len());
			const FString GroupDir = FString::Printf(TEXT("/Game/Qinglan/Characters/%s"), *GroupName);
			// FSoftObjectPath needs "Package.Object": a package-only path loads
			// the package but resolves no asset, so the mesh/montage stayed null
			// and every enemy spawned without presentation.
			auto MakeObjectPath = [](const FString& PackagePath)
			{
				return FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetShortName(PackagePath));
			};
			Asset->Mesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(MakeObjectPath(GroupPath)));
			Asset->IdleAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(MakeObjectPath(
				FString::Printf(TEXT("/Game/Qinglan/Characters/%s"), Group->Idle))));
			// The anim bootstrap names montages AM_<GroupFolder>_<Suffix>; the
			// Human folder holds the player combo cuts, so humanoid enemies
			// reuse the first cut as their attack swing.
			const TCHAR* AttackSuffix = GroupName == TEXT("Human") ? TEXT("SwordCut_1") : TEXT("Attack");
			Asset->AttackMontage = TSoftObjectPtr<UAnimMontage>(FSoftObjectPath(MakeObjectPath(
				FString::Printf(TEXT("%s/Montages/AM_%s_%s"), *GroupDir, *GroupName, AttackSuffix))));
			Asset->HitReactMontage = TSoftObjectPtr<UAnimMontage>(FSoftObjectPath(MakeObjectPath(
				FString::Printf(TEXT("%s/Montages/AM_%s_HitReact"), *GroupDir, *GroupName))));
		}

		if (UQinglanBossDataAsset* BossAsset = Cast<UQinglanBossDataAsset>(Asset))
		{
			BossAsset->Patterns = Spec.Patterns;
			BossAsset->PhaseTwoAtHealthFraction = Spec.PhaseTwoAtHealthFraction;
			BossAsset->ArenaDiameter = Spec.ArenaDiameter;
		}

		// Cached catalog assets stay rooted while resident (see the cache
		// comment above); the size cap is the release path (AUD-46), so the
		// rooted set is bounded instead of growing per encounter.
		Asset->AddToRoot();
		GCatalogAssetCache.Add(Spec.EnemyId, Asset);
		TouchCatalogAsset(Spec.EnemyId);
		EvictOverflowCatalogAssets();
		return Asset;
	}

	EQinglanAttackShape ShapeOfPattern(const FQinglanBossPattern& Pattern)
	{
		if (Pattern.PatternId == QinglanPatternIds::SweepingBlade() ||
			Pattern.PatternId == QinglanPatternIds::SpiritRend())
		{
			return EQinglanAttackShape::MeleeCone;
		}
		if (Pattern.PatternId == QinglanPatternIds::DelayedRoots())
		{
			return EQinglanAttackShape::GroundRing;
		}
		if (Pattern.PatternId == QinglanPatternIds::SpiritBolts() ||
			Pattern.PatternId == QinglanPatternIds::FireLance())
		{
			return EQinglanAttackShape::AimedProjectile;
		}
		if (Pattern.PatternId == QinglanPatternIds::RingShockwave())
		{
			return EQinglanAttackShape::GroundRingGaps;
		}
		if (Pattern.PatternId == QinglanPatternIds::ChargingStrike())
		{
			return EQinglanAttackShape::ChargeDash;
		}
		if (Pattern.PatternId == QinglanPatternIds::StormBoltVolley())
		{
			return EQinglanAttackShape::MultiGroundStrikes;
		}
		return EQinglanAttackShape::MeleeArc;
	}
} // namespace QinglanEnemyCatalog
