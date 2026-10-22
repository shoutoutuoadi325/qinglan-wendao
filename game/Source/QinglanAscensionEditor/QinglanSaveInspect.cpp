#include "QinglanSaveInspect.h"

#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

UQinglanSaveInspectCommandlet::UQinglanSaveInspectCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UQinglanSaveInspectCommandlet::Main(const FString& Params)
{
	FString Input, Output;
	if (!FParse::Value(*Params, TEXT("input="), Input) || Input.IsEmpty() ||
		!FParse::Value(*Params, TEXT("output="), Output) || Output.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection requires -input=<save> -output=<new JSON path>"));
		return 1;
	}
	Input = FPaths::ConvertRelativePathToFull(Input);
	Output = FPaths::ConvertRelativePathToFull(Output);
	// Evidence is append-only. This also rejects output aliases of the input
	// (including symlinks), rather than ever replacing the source container.
	if (IFileManager::Get().FileExists(*Output))
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection refuses an existing output: %s"), *Output);
		return 1;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Input))
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection could not read: %s"), *Input);
		return 1;
	}

	// Do not initialize a game session or call LoadProfile: that path may
	// recover/rotate saves. DeserializeVerified only parses, migrates in
	// memory, and checks the production checksum and state invariants.
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanSaveSubsystem* Reader = NewObject<UQinglanSaveSubsystem>(Owner);
	UQinglanSaveGame* Save = nullptr;
	FText Error;
	if (!Reader->DeserializeVerified(Bytes, Save, Error) || !Save)
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection rejected %s: %s"), *Input, *Error.ToString());
		return 1;
	}

	// Preserve 64-bit values exactly. Ordinary JSON numeric values pass
	// through double and can round valid inventory/checksum integers.
	const FJsonObjectConverter::CustomExportCallback ExactIntegers =
		FJsonObjectConverter::CustomExportCallback::CreateLambda(
			[](FProperty* Property, const void* Value) -> TSharedPtr<FJsonValue>
			{
				if (const FInt64Property* Signed = CastField<FInt64Property>(Property))
				{
					return MakeShared<FJsonValueString>(LexToString(Signed->GetPropertyValue(Value)));
				}
				if (const FUInt64Property* Unsigned = CastField<FUInt64Property>(Property))
				{
					return MakeShared<FJsonValueString>(LexToString(Unsigned->GetPropertyValue(Value)));
				}
				return nullptr;
			});
	TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
	if (!FJsonObjectConverter::UStructToJsonObject(UQinglanSaveGame::StaticClass(), Save, State,
		0, 0, &ExactIntegers))
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection could not export reflected state"));
		return 1;
	}
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("scope"), TEXT("Read-only disk-state inspection; does not prove runtime resume or reward behavior"));
	Report->SetStringField(TEXT("source_path"), Input);
	Report->SetNumberField(TEXT("source_bytes"), Bytes.Num());
	Report->SetStringField(TEXT("source_sha1"), FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num()).ToString());
	Report->SetStringField(TEXT("verified_container_checksum_fnv1a64"), LexToString(Save->BodyChecksum));
	Report->SetStringField(TEXT("integer_encoding"), TEXT("64-bit integers are exact decimal strings"));
	Report->SetObjectField(TEXT("state"), State);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report, Writer) ||
		!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true) ||
		!FFileHelper::SaveStringToFile(Json, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("Save inspection could not write report: %s"), *Output);
		return 1;
	}
	UE_LOG(LogTemp, Display, TEXT("Save inspection exported sequence %d to %s"), Save->SaveSequence, *Output);
	return 0;
}
