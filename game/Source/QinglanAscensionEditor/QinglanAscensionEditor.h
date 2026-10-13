// Qinglan: Path of Ascension - editor-only module (data import commandlet,
// validation, automation specs). Runtime gameplay lives in QinglanAscension;
// nothing here is ever cooked into the game.
#pragma once

#include "CoreMinimal.h"

// Editor-side log category (runtime categories live in QinglanAscension.h).
// Logs asset import/validation results; never logs raw save payloads.
DECLARE_LOG_CATEGORY_EXTERN(LogQinglanEditor, Log, All);
