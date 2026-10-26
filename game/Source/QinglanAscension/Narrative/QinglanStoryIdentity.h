// AUD-09: shared identity contract for importer, runtime, and save migration.
#pragma once

#include "CoreMinimal.h"
#include "Narrative/QinglanStoryDataTypes.h"

namespace QinglanStoryIdentity
{
	/** Reserved separator prevents authored components from aliasing composite keys. */
	inline bool IsValidComponent(FName Id)
	{
		return !Id.IsNone() && !Id.ToString().Contains(TEXT("::"));
	}

	inline FName MakeKey(FName SceneId, const TCHAR* Kind, FName LocalId)
	{
		if (!IsValidComponent(SceneId) || !IsValidComponent(LocalId)) { return NAME_None; }
		return FName(*FString::Printf(TEXT("%s::%s::%s"), *SceneId.ToString(), Kind, *LocalId.ToString()));
	}

	inline FName ObjectiveKey(FName SceneId, FName ObjectiveId)
	{
		return MakeKey(SceneId, TEXT("objective"), ObjectiveId);
	}

	inline FName ChoiceKey(FName SceneId, FName ChoiceId)
	{
		return MakeKey(SceneId, TEXT("choice"), ChoiceId);
	}

	/** Returns a verified key only if exactly one authored member owns the ID.
	 * Passing OwnerSceneId supports local IDs; NAME_None is for legacy bare saves.
	 * Ambiguous or unknown legacy IDs fail closed instead of choosing the first scene.
	 * Existing composite keys are accepted only when they still resolve in this registry.
	 */
	inline FName ResolveKey(const UQinglanStoryRegistry& Registry, FName Id,
		FName OwnerSceneId, bool bObjective)
	{
		if (Id.IsNone()) { return NAME_None; }
		FName Result = NAME_None;
		int32 Matches = 0;
		auto Visit = [&](const FQinglanRegistryScene& Scene)
		{
			if (!OwnerSceneId.IsNone() && Scene.SceneId != OwnerSceneId) { return; }
			auto Match = [&](FName LocalId)
			{
				const FName Key = bObjective ? ObjectiveKey(Scene.SceneId, LocalId) : ChoiceKey(Scene.SceneId, LocalId);
				if (!Key.IsNone() && (Id == LocalId || Id == Key))
				{
					++Matches;
					Result = Key;
				}
			};
			if (bObjective)
			{
				for (const FQinglanRegistryObjective& Objective : Scene.Objectives) { Match(Objective.ObjectiveId); }
			}
			else
			{
				for (const FQinglanRegistryChoice& Choice : Scene.Choices) { Match(Choice.ChoiceId); }
			}
		};
		for (const FQinglanRegistryScene& Scene : Registry.MainScenes) { Visit(Scene); }
		for (const FQinglanRegistrySideQuest& Quest : Registry.SideQuests)
		{
			for (const FQinglanRegistryScene& Stage : Quest.Stages) { Visit(Stage); }
		}
		return Matches == 1 ? Result : NAME_None;
	}

	inline FName ResolveObjectiveKey(const UQinglanStoryRegistry& Registry, FName Id, FName OwnerSceneId = NAME_None)
	{
		return ResolveKey(Registry, Id, OwnerSceneId, true);
	}

	inline FName ResolveChoiceKey(const UQinglanStoryRegistry& Registry, FName Id, FName OwnerSceneId = NAME_None)
	{
		return ResolveKey(Registry, Id, OwnerSceneId, false);
	}

	/** v1 saves may contain bare choice IDs. Only unique ownership is recoverable;
	 * overwritten history cannot be reconstructed and must never be guessed.
	 * Values remain authored local option IDs; only keys provide global identity.
	 */
	inline TMap<FName, FName> NormalizeChoiceDecisions(const UQinglanStoryRegistry& Registry,
		const TMap<FName, FName>& Input, TArray<FName>& RejectedKeys)
	{
		TMap<FName, FName> Result;
		for (const TPair<FName, FName>& Entry : Input)
		{
			const FName Key = ResolveChoiceKey(Registry, Entry.Key);
			FString Owner, Tail, Kind, Local;
			if (!Key.IsNone())
			{
				Key.ToString().Split(TEXT("::"), &Owner, &Tail);
				Tail.Split(TEXT("::"), &Kind, &Local);
			}
			const FName LocalId(*Local);
			if (Key.IsNone() || (Entry.Value != LocalId && Entry.Value != Key))
			{
				RejectedKeys.Add(Entry.Key);
				continue;
			}
			Result.Add(Key, LocalId);
		}
		return Result;
	}
}
