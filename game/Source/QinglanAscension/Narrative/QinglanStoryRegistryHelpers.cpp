// Registry lookup helpers: const accessors over the imported, immutable story
// registry (UQinglanStoryRegistry) and the realm template asset
// (UQinglanRealmDataAsset). Linear scans are deliberate: the registries are
// small (30 main scenes + 24 side stages + 24 records + 40 barks; blueprint
// section 1.9/2.x), immutable at runtime, and lookups happen per safe-boundary
// interaction rather than per frame.
//
// FIXME-CONTRACT: the realm asset accessors are declared in
// Narrative/QinglanStoryDataTypes.h alongside the story registry types; their
// definitions live in Progression/QinglanRealmDataAsset.cpp (the richer,
// warning-logging copy) even though the expedition runtime
// (UQinglanRunSubsystem) is their primary consumer.
#include "Narrative/QinglanStoryDataTypes.h"

const FQinglanRegistryScene* UQinglanStoryRegistry::FindMainScene(FName SceneId) const
{
	if (SceneId == NAME_None)
	{
		return nullptr;
	}
	for (const FQinglanRegistryScene& Scene : MainScenes)
	{
		if (Scene.SceneId == SceneId)
		{
			return &Scene;
		}
	}
	return nullptr;
}

const FQinglanRegistryScene* UQinglanStoryRegistry::FindSideStage(FName StageId, FName& OutQuestId) const
{
	OutQuestId = NAME_None;
	if (StageId == NAME_None)
	{
		return nullptr;
	}
	for (const FQinglanRegistrySideQuest& Quest : SideQuests)
	{
		for (const FQinglanRegistryScene& Stage : Quest.Stages)
		{
			if (Stage.SceneId == StageId)
			{
				OutQuestId = Quest.QuestId;
				return &Stage;
			}
		}
	}
	return nullptr;
}

const FQinglanRegistrySideQuest* UQinglanStoryRegistry::FindQuest(FName QuestId) const
{
	if (QuestId == NAME_None)
	{
		return nullptr;
	}
	for (const FQinglanRegistrySideQuest& Quest : SideQuests)
	{
		if (Quest.QuestId == QuestId)
		{
			return &Quest;
		}
	}
	return nullptr;
}
