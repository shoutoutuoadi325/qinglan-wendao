// Pure damage math implementation. See QinglanDamageMath.h for doc citations.
#include "Combat/QinglanDamageMath.h"

float QinglanDamageMath::RoundHalfAwayFromZero(float Value)
{
	// Nearest integer, ties away from zero: 31.5 -> 32, -31.5 -> -32.
	// Never use FMath::RoundToInt here: FPlatformMath may round halves to even.
	return Value >= 0.f ? FMath::FloorToFloat(Value + 0.5f) : FMath::CeilToFloat(Value - 0.5f);
}

float QinglanDamageMath::ComputeRawDamage(float TechniquePower, float Coefficient, float FlatDamage)
{
	// RawDamage = TechniquePower * Coefficient + FlatDamage (docs/02 §5).
	return TechniquePower * Coefficient + FlatDamage;
}

float QinglanDamageMath::ComputeMitigatedDamage(float RawDamage, float Defense)
{
	// MitigatedDamage = RawDamage * 100 / (100 + max(0, Defense)) (docs/02 §5).
	const float EffectiveDefense = FMath::Max(0.f, Defense);
	return RawDamage * 100.f / (100.f + EffectiveDefense);
}

float QinglanDamageMath::ComputeFinalDamage(float MitigatedDamage, float ReactionMultiplier, float DifficultyMultiplier)
{
	// FinalDamage = round(max(1, Mitigated * Reaction * Difficulty)), halves away
	// from zero (docs/02 §5). Accepted hits always deal at least 1 damage.
	const float Scaled = MitigatedDamage * ReactionMultiplier * DifficultyMultiplier;
	return RoundHalfAwayFromZero(FMath::Max(1.f, Scaled));
}

uint64 QinglanDamageMath::MakeHitKey(const FQinglanCastId& CastId, uint32 VictimKeyId, int32 HitOrdinal)
{
	// 64-bit layout: hash(CastId) in the high word, (victim id, ordinal) mixed
	// into the low word. The victim is part of the key per docs/02 §3 so the
	// same cast hitting several actors keeps distinct keys per victim even if
	// the set were ever shared.
	const uint32 CastHash = GetTypeHash(CastId);
	const uint32 LowWord = HashCombine(VictimKeyId, ::GetTypeHash(HitOrdinal));
	return (static_cast<uint64>(CastHash) << 32) | static_cast<uint64>(LowWord);
}
