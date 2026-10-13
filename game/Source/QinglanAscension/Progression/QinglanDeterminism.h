// Deterministic seeding helpers for the expedition run module.
//
// One master seed derives the three independent FRandomStream instances the
// run subsystem owns (encounters / loot / blessings):
//   - docs/01-game-design.md section 6: "Use separate streams for encounters,
//     loot, and blessings so adding a cosmetic effect does not change rewards."
//   - docs/dev/implementation-blueprint.md section 1.3: "Seeded FRandomStream
//     x3 (encounters / loot / blessings), same-seed reproducibility."
//
// No FMath::RandRange anywhere in gameplay (code-contracts.md); every roll
// goes through these stream-driven helpers so identical seeds reproduce
// identical content selections on every platform.
#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

namespace QinglanDeterminism
{
	// --- Stream derivation salts -------------------------------------------------
	// The master seed is XOR-mixed with each frozen constant (then masked
	// positive) to produce the three stream seeds. The values are arbitrary
	// but FROZEN: changing any one of them changes every downstream encounter,
	// loot, and blessing selection, which is a content-affecting change
	// (treat like a content_version bump, blueprint section 3).
	inline constexpr int32 EncounterStreamSalt = 0x00000000; // master seed, unmodified
	inline constexpr int32 LootStreamSalt      = 0x5EED0002; // "SEED-2" -> loot stream
	inline constexpr int32 BlessingStreamSalt  = 0x5EED0003; // "SEED-3" -> blessing stream

	/**
	 * Maps an arbitrary caller-supplied seed onto a valid FRandomStream seed.
	 * FRandomStream degenerates on seeds <= 0 (the Lehmer generator never
	 * leaves 0), so 0 remaps to 1 and negatives fold positive. INT32_MIN is
	 * folded through int64 to avoid signed-overflow UB. The canonical value is
	 * what gets serialized as FQinglanCheckpoint::RealmSeed so import derives
	 * byte-identical streams.
	 */
	inline int32 CanonicalSeed(int32 Seed)
	{
		if (Seed == 0)
		{
			return 1;
		}
		if (Seed < 0)
		{
			const int64 Folded = -static_cast<int64>(Seed);
			return static_cast<int32>(FMath::Min<int64>(Folded, MAX_int32));
		}
		return Seed;
	}

	/** XOR-mix + positive mask; never returns 0 (degenerate Lehmer state). */
	inline int32 MixSeed(int32 MasterSeed, int32 Salt)
	{
		int32 Mixed = (MasterSeed ^ Salt) & 0x7FFFFFFF;
		if (Mixed == 0)
		{
			Mixed = 1;
		}
		return Mixed;
	}

	inline FRandomStream MakeEncounterStream(int32 MasterSeed)
	{
		return FRandomStream(MixSeed(CanonicalSeed(MasterSeed), EncounterStreamSalt));
	}

	inline FRandomStream MakeLootStream(int32 MasterSeed)
	{
		return FRandomStream(MixSeed(CanonicalSeed(MasterSeed), LootStreamSalt));
	}

	inline FRandomStream MakeBlessingStream(int32 MasterSeed)
	{
		return FRandomStream(MixSeed(CanonicalSeed(MasterSeed), BlessingStreamSalt));
	}

	/**
	 * Restores a stream from a checkpoint-serialized generator state
	 * (FRandomStream::GetCurrentSeed at export time). Initialize() seeds the
	 * generator with the saved state directly, so the next draw equals the
	 * next draw at save time (pre-room snapshot resume, blueprint section 1.6).
	 * States <= 0 are degenerate/non-serializable and fall back to the derived
	 * expedition seed so a hand-corrupted save can never wedge the pipeline.
	 */
	inline FRandomStream RestoreStream(int32 SavedGeneratorState, int32 DerivedSeedFallback)
	{
		const int32 Fallback = DerivedSeedFallback > 0 ? DerivedSeedFallback : 1;
		return FRandomStream(SavedGeneratorState > 0 ? SavedGeneratorState : Fallback);
	}

	/**
	 * Uniform index in [0, Count-1] from a stream fraction. Count <= 1 consumes
	 * nothing and yields 0 (documented: single-candidate "rolls" stay free so
	 * stream traces stay comparable across data revisions).
	 */
	inline int32 RollIndex(FRandomStream& Stream, int32 Count)
	{
		if (Count <= 1)
		{
			return 0;
		}
		const float Fraction = Stream.GetFraction(); // [0,1)
		return FMath::Clamp(FMath::FloorToInt32(Fraction * static_cast<float>(Count)), 0, Count - 1);
	}

	/**
	 * Fisher-Yates shuffle driven solely by the stream (never FMath::RandRange,
	 * code-contracts.md "Deterministic"). Consumes exactly Num()-1 fractions.
	 */
	template <typename T>
	void ShuffleWithStream(TArray<T>& Items, FRandomStream& Stream)
	{
		for (int32 I = Items.Num() - 1; I > 0; --I)
		{
			const int32 J = RollIndex(Stream, I + 1);
			if (J != I)
			{
				Items.Swap(I, J);
			}
		}
	}
} // namespace QinglanDeterminism
