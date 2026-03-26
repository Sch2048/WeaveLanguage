#include "Core/WeaveOperator.h"
#include "Core/WeaveGenerator.h"
#include "Core/WeaveInterpreter.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"


TArray<TSharedPtr<FJsonObject>> UWeaveOperator::NodeCatalog;
bool UWeaveOperator::bCatalogInitialized = false;
TArray<TSharedPtr<FJsonObject>> UWeaveOperator::TypeCatalog;
bool UWeaveOperator::bTypeCatalogInitialized = false;
FString UWeaveOperator::CachedBlueprintPath;
FString UWeaveOperator::CachedGraphName;
FString UWeaveOperator::CachedResultWeave;
TMap<FString, TArray<int32>> UWeaveOperator::KeywordIndex;
TMap<FString, int32> UWeaveOperator::IdIndex;

TArray<FString> UWeaveOperator::GenerateKeywords(const FString& Title)
{
	TArray<FString> Keywords;
	TArray<FString> Words;
	Title.ParseIntoArray(Words, TEXT(" "));


	Keywords.Add(Title.ToLower());


	FString NoSpaceTitle = Title.Replace(TEXT(" "), TEXT("")).ToLower();
	if (!NoSpaceTitle.IsEmpty())
	{
		Keywords.AddUnique(NoSpaceTitle);
	}


	for (int32 i = 0; i < Words.Num(); i++)
	{
		for (int32 j = i + 1; j <= Words.Num(); j++)
		{
			FString Combined;
			for (int32 k = i; k < j; k++)
			{
				if (k > i) Combined += TEXT(" ");
				Combined += Words[k];
			}
			if (!Combined.IsEmpty())
			{
				Keywords.AddUnique(Combined.ToLower());
			}
		}
	}

	return Keywords;
}

void UWeaveOperator::BuildSearchIndex()
{
	KeywordIndex.Empty();
	IdIndex.Empty();

	UE_LOG(LogTemp, Log, TEXT("Building search index for %d nodes..."), NodeCatalog.Num());

	for (int32 i = 0; i < NodeCatalog.Num(); i++)
	{
		const TSharedPtr<FJsonObject>& Node = NodeCatalog[i];


		FString Id = Node->GetStringField(TEXT("id")).ToLower();
		IdIndex.Add(Id, i);


		FString Title = Node->GetStringField(TEXT("title")).ToLower();
		TArray<FString> TitleWords;
		Title.ParseIntoArray(TitleWords, TEXT(" "));
		for (const FString& Word : TitleWords)
		{
			if (!Word.IsEmpty())
			{
				KeywordIndex.FindOrAdd(Word).AddUnique(i);
			}
		}


		if (Node->HasField(TEXT("search")))
		{
			TSharedPtr<FJsonObject> Search = Node->GetObjectField(TEXT("search"));
			const TArray<TSharedPtr<FJsonValue>>* Keywords;
			if (Search->TryGetArrayField(TEXT("keywords"), Keywords))
			{
				for (const TSharedPtr<FJsonValue>& Keyword : *Keywords)
				{
					FString KeywordStr = Keyword->AsString();
					if (!KeywordStr.IsEmpty())
					{
						KeywordIndex.FindOrAdd(KeywordStr).AddUnique(i);
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Search index built: %d keywords, %d IDs"), KeywordIndex.Num(), IdIndex.Num());
}

TSharedPtr<FJsonObject> UWeaveOperator::NodeToJson(UEdGraphNode* Node)
{
	if (!Node) return nullptr;

	UK2Node* K2Node = Cast<UK2Node>(Node);
	if (!K2Node)
	{
		UE_LOG(LogTemp, Verbose, TEXT("Node '%s' is not a K2Node"), *Node->GetName());
		return nullptr;
	}


	FString NodeClassName = K2Node->GetClass()->GetName();
	if (NodeClassName.Contains(TEXT("Variable")))
	{
		UE_LOG(LogTemp, Verbose, TEXT("Filtered out Variable node: %s"), *NodeClassName);
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Processing node: %s"), *NodeClassName);

	TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);


	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(K2Node))
	{
		FString EventName = EventNode->EventReference.GetMemberName().ToString();
		UClass* OwnerClass = EventNode->EventReference.GetMemberParentClass();
		FString ClassName = OwnerClass ? OwnerClass->GetName() : TEXT("Unknown");
		ClassName.RemoveFromEnd(TEXT("_C"));


		JsonNode->SetStringField(TEXT("id"), FString::Printf(TEXT("event.%s.%s"), *ClassName, *EventName));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_Event"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("event"));
		JsonNode->SetStringField(TEXT("title"), EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());


		TSharedPtr<FJsonObject> Source = MakeShareable(new FJsonObject);
		Source->SetStringField(TEXT("owner_class"), ClassName);
		Source->SetStringField(TEXT("event_name"), EventName);
		JsonNode->SetObjectField(TEXT("source"), Source);


		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());

		TArray<TSharedPtr<FJsonValue>> ExecOut;
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);

		Pins->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		JsonNode->SetObjectField(TEXT("pins"), Pins);


		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);


		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;


		FString Title = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		TArray<FString> Keywords = GenerateKeywords(Title);


		Keywords.AddUnique(EventName.ToLower());
		FString NoSpaceEventName = EventName.Replace(TEXT(" "), TEXT("")).ToLower();
		Keywords.AddUnique(NoSpaceEventName);

		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShareable(new FJsonValueString(Keyword)));
		}
		Search->SetArrayField(TEXT("keywords"), KeywordsArray);

		TArray<TSharedPtr<FJsonValue>> CategoryArray;
		CategoryArray.Add(MakeShareable(new FJsonValueString(ClassName)));
		Search->SetArrayField(TEXT("category"), CategoryArray);
		JsonNode->SetObjectField(TEXT("search"), Search);

		UE_LOG(LogTemp, Log, TEXT("Successfully converted Event node: %s"), *EventName);
		return JsonNode;
	}


	else if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(K2Node))
	{
		UFunction* Function = FuncNode->GetTargetFunction();
		if (!Function)
		{
			UE_LOG(LogTemp, Warning, TEXT("CallFunction node has no target function"));
			return nullptr;
		}

		FString MemberName = FuncNode->FunctionReference.GetMemberName().ToString();
		UClass* OwnerClass = Function->GetOwnerClass();
		FString ClassName = OwnerClass ? OwnerClass->GetName() : TEXT("Unknown");
		ClassName.RemoveFromEnd(TEXT("_C"));


		JsonNode->SetStringField(TEXT("id"), FString::Printf(TEXT("call.%s.%s"), *ClassName, *MemberName));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_CallFunction"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("call"));
		JsonNode->SetStringField(TEXT("title"), FuncNode->GetNodeTitle(ENodeTitleType::ListView).ToString());


		TSharedPtr<FJsonObject> Source = MakeShareable(new FJsonObject);
		Source->SetStringField(TEXT("member_parent"), ClassName);
		Source->SetStringField(TEXT("member_name"), MemberName);
		JsonNode->SetObjectField(TEXT("source"), Source);


		bool bIsStatic = Function->HasAnyFunctionFlags(FUNC_Static);
		if (!bIsStatic)
		{
			TSharedPtr<FJsonObject> Target = MakeShareable(new FJsonObject);
			Target->SetBoolField(TEXT("required"), true);
			Target->SetStringField(TEXT("type"), ClassName);
			Target->SetStringField(TEXT("pin_name"), TEXT("self"));
			JsonNode->SetObjectField(TEXT("target"), Target);
		}


		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> InputsArray;
		TArray<TSharedPtr<FJsonValue>> OutputsArray;

		bool bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);


		if (!bIsPure)
		{
			TArray<TSharedPtr<FJsonValue>> ExecIn;
			ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
			Pins->SetArrayField(TEXT("exec_in"), ExecIn);

			TArray<TSharedPtr<FJsonValue>> ExecOut;
			ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
			Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		}
		else
		{
			Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());
			Pins->SetArrayField(TEXT("exec_out"), TArray<TSharedPtr<FJsonValue>>());
		}


		bool bFirstInputParam = true;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Prop = *It;
			FString PropType = Prop->GetCPPType();
			FString PropName = Prop->GetName();

			TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);

			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				PinObj->SetStringField(TEXT("name"), PropName);
				PinObj->SetStringField(TEXT("type"), PropType);
				OutputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
			}
			else if (Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm))
			{
				PinObj->SetStringField(TEXT("name"), PropName);
				PinObj->SetStringField(TEXT("type"), PropType);
				OutputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
			}
			else
			{
				FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
				if (!bIsStatic && bFirstInputParam && ObjProp)
				{
					PinObj->SetStringField(TEXT("name"), TEXT("self"));
					PinObj->SetStringField(TEXT("type"), PropType);
					PinObj->SetBoolField(TEXT("required"), true);
					PinObj->SetStringField(TEXT("pin_name"), TEXT("self"));
				}
				else
				{
					PinObj->SetStringField(TEXT("name"), PropName);
					PinObj->SetStringField(TEXT("type"), PropType);
					PinObj->SetBoolField(TEXT("required"), true);
					if (Prop->HasAnyPropertyFlags(CPF_ReferenceParm))
						PinObj->SetBoolField(TEXT("ref"), true);
				}
				bFirstInputParam = false;
				InputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
			}
		}

		Pins->SetArrayField(TEXT("inputs"), InputsArray);
		Pins->SetArrayField(TEXT("outputs"), OutputsArray);
		JsonNode->SetObjectField(TEXT("pins"), Pins);


		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), bIsPure);
		Flags->SetBoolField(TEXT("latent"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);


		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;
		TArray<FString> Keywords = GenerateKeywords(FuncNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShareable(new FJsonValueString(Keyword)));
		}
		Search->SetArrayField(TEXT("keywords"), KeywordsArray);

		TArray<TSharedPtr<FJsonValue>> CategoryArray;
		CategoryArray.Add(MakeShareable(new FJsonValueString(ClassName)));
		Search->SetArrayField(TEXT("category"), CategoryArray);
		JsonNode->SetObjectField(TEXT("search"), Search);

		UE_LOG(LogTemp, Log, TEXT("Successfully converted CallFunction node: %s"), *MemberName);
		return JsonNode;
	}

	UE_LOG(LogTemp, Warning, TEXT("Unhandled node type: %s"), *NodeClassName);
	return nullptr;
}

void UWeaveOperator::GenerateWeaveLanguage()
{
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	int32 ProcessedFunctions = 0;
	int32 ProcessedEvents = 0;


	TSet<FString> ProcessedFunctionIds;
	TSet<FString> ProcessedEventIds;


	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;


		if (Class->HasAnyClassFlags(CLASS_Deprecated))
		{
			continue;
		}


		for (TFieldIterator<UFunction> FuncIt(Class); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;


			if (Function->HasMetaData(TEXT("DeprecatedFunction")))
			{
				continue;
			}

			FString MemberName = Function->GetName();
			FString ClassName = Class->GetName();
			ClassName.RemoveFromEnd(TEXT("_C"));


			if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				UClass* OwnerClass = Function->GetOwnerClass();


				if (MemberName == TEXT("ReceiveBeginPlay"))
				{
					UE_LOG(LogTemp, Warning, TEXT("[Scan] Found ReceiveBeginPlay in class %s, OwnerClass=%s, Match=%d"),
					       *ClassName, *OwnerClass->GetName(), (OwnerClass == Class));
				}

				if (OwnerClass != Class)
				{
					continue;
				}

				FString EventId = FString::Printf(TEXT("event.%s.%s"), *ClassName, *MemberName);


				if (MemberName == TEXT("ReceiveBeginPlay"))
				{
					UE_LOG(LogTemp, Warning,
					       TEXT("[Scan] ReceiveBeginPlay passed OwnerClass check, EventId=%s, AlreadyProcessed=%d"),
					       *EventId, ProcessedEventIds.Contains(EventId));
				}


				if (ProcessedEventIds.Contains(EventId))
				{
					continue;
				}
				ProcessedEventIds.Add(EventId);
				TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);


				JsonNode->SetStringField(TEXT("id"), EventId);
				JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_Event"));
				JsonNode->SetStringField(TEXT("kind"), TEXT("event"));


				FString DisplayName = Function->GetMetaData(TEXT("DisplayName"));
				if (DisplayName.IsEmpty())
				{
					DisplayName = FName::NameToDisplayString(MemberName, false);
				}
				JsonNode->SetStringField(TEXT("title"), DisplayName);


				TSharedPtr<FJsonObject> Source = MakeShareable(new FJsonObject);
				Source->SetStringField(TEXT("owner_class"), ClassName);
				Source->SetStringField(TEXT("event_name"), MemberName);
				JsonNode->SetObjectField(TEXT("source"), Source);


				TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
				Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());

				TArray<TSharedPtr<FJsonValue>> ExecOut;
				ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
				Pins->SetArrayField(TEXT("exec_out"), ExecOut);

				Pins->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
				Pins->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
				JsonNode->SetObjectField(TEXT("pins"), Pins);


				TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
				Flags->SetBoolField(TEXT("pure"), false);
				JsonNode->SetObjectField(TEXT("flags"), Flags);


				TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
				TArray<TSharedPtr<FJsonValue>> KeywordsArray;

				TArray<FString> Keywords = GenerateKeywords(DisplayName);
				Keywords.AddUnique(MemberName.ToLower());
				FString NoSpaceEventName = MemberName.Replace(TEXT(" "), TEXT("")).ToLower();
				Keywords.AddUnique(NoSpaceEventName);

				for (const FString& Keyword : Keywords)
				{
					KeywordsArray.Add(MakeShareable(new FJsonValueString(Keyword)));
				}
				Search->SetArrayField(TEXT("keywords"), KeywordsArray);

				TArray<TSharedPtr<FJsonValue>> CategoryArray;
				CategoryArray.Add(MakeShareable(new FJsonValueString(ClassName)));
				Search->SetArrayField(TEXT("category"), CategoryArray);
				JsonNode->SetObjectField(TEXT("search"), Search);


				if (MemberName == TEXT("ReceiveBeginPlay"))
				{
					UE_LOG(LogTemp, Warning, TEXT("[Scan] ReceiveBeginPlay about to add to NodesArray, EventId=%s"),
					       *EventId);
				}

				NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
				ProcessedEvents++;


				if (MemberName == TEXT("ReceiveBeginPlay"))
				{
					UE_LOG(LogTemp, Warning,
					       TEXT("[Scan] ReceiveBeginPlay JSON added to array, total events so far: %d"),
					       ProcessedEvents);
				}
				continue;
			}


			if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
			{
				continue;
			}


			UClass* OwnerClass = Function->GetOwnerClass();
			if (OwnerClass != Class)
			{
				continue;
			}

			bool bIsStatic = Function->HasAnyFunctionFlags(FUNC_Static);
			bool bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);

			FString FunctionId = FString::Printf(TEXT("call.%s.%s"), *ClassName, *MemberName);


			if (ProcessedFunctionIds.Contains(FunctionId))
			{
				continue;
			}
			ProcessedFunctionIds.Add(FunctionId);


			TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);


			JsonNode->SetStringField(TEXT("id"), FunctionId);
			JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_CallFunction"));
			JsonNode->SetStringField(TEXT("kind"), TEXT("call"));


			FString DisplayName = Function->GetMetaData(TEXT("DisplayName"));
			if (DisplayName.IsEmpty())
			{
				DisplayName = FName::NameToDisplayString(MemberName, false);
			}
			JsonNode->SetStringField(TEXT("title"), DisplayName);


			TSharedPtr<FJsonObject> Source = MakeShareable(new FJsonObject);
			Source->SetStringField(TEXT("member_parent"), ClassName);
			Source->SetStringField(TEXT("member_name"), MemberName);
			JsonNode->SetObjectField(TEXT("source"), Source);


			if (!bIsStatic)
			{
				TSharedPtr<FJsonObject> Target = MakeShareable(new FJsonObject);
				Target->SetBoolField(TEXT("required"), true);
				Target->SetStringField(TEXT("type"), ClassName);
				Target->SetStringField(TEXT("pin_name"), TEXT("self"));
				JsonNode->SetObjectField(TEXT("target"), Target);
			}


			TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
			TArray<TSharedPtr<FJsonValue>> InputsArray;
			TArray<TSharedPtr<FJsonValue>> OutputsArray;


			if (!bIsPure)
			{
				TArray<TSharedPtr<FJsonValue>> ExecIn;
				ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
				Pins->SetArrayField(TEXT("exec_in"), ExecIn);

				TArray<TSharedPtr<FJsonValue>> ExecOut;
				ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
				Pins->SetArrayField(TEXT("exec_out"), ExecOut);
			}
			else
			{
				Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());
				Pins->SetArrayField(TEXT("exec_out"), TArray<TSharedPtr<FJsonValue>>());
			}


			bool bFirstInputParam = true;
			for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				FString PropType = Prop->GetCPPType();
				FString PropName = Prop->GetName();

				TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);

				if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					PinObj->SetStringField(TEXT("name"), PropName);
					PinObj->SetStringField(TEXT("type"), PropType);
					OutputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
				}
				else if (Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm))
				{
					PinObj->SetStringField(TEXT("name"), PropName);
					PinObj->SetStringField(TEXT("type"), PropType);
					OutputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
				}
				else
				{
					FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
					if (!bIsStatic && bFirstInputParam && ObjProp)
					{
						PinObj->SetStringField(TEXT("name"), TEXT("self"));
						PinObj->SetStringField(TEXT("type"), PropType);
						PinObj->SetBoolField(TEXT("required"), true);
						PinObj->SetStringField(TEXT("pin_name"), TEXT("self"));
					}
					else
					{
						PinObj->SetStringField(TEXT("name"), PropName);
						PinObj->SetStringField(TEXT("type"), PropType);
						PinObj->SetBoolField(TEXT("required"), true);
						if (Prop->HasAnyPropertyFlags(CPF_ReferenceParm))
							PinObj->SetBoolField(TEXT("ref"), true);
					}
					bFirstInputParam = false;
					InputsArray.Add(MakeShareable(new FJsonValueObject(PinObj)));
				}
			}

			Pins->SetArrayField(TEXT("inputs"), InputsArray);
			Pins->SetArrayField(TEXT("outputs"), OutputsArray);
			JsonNode->SetObjectField(TEXT("pins"), Pins);


			TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
			Flags->SetBoolField(TEXT("pure"), bIsPure);
			Flags->SetBoolField(TEXT("latent"), false);
			JsonNode->SetObjectField(TEXT("flags"), Flags);


			TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
			TArray<TSharedPtr<FJsonValue>> KeywordsArray;
			TArray<FString> Keywords = GenerateKeywords(DisplayName);
			for (const FString& Keyword : Keywords)
			{
				KeywordsArray.Add(MakeShareable(new FJsonValueString(Keyword)));
			}
			Search->SetArrayField(TEXT("keywords"), KeywordsArray);

			TArray<TSharedPtr<FJsonValue>> CategoryArray;
			CategoryArray.Add(MakeShareable(new FJsonValueString(ClassName)));
			Search->SetArrayField(TEXT("category"), CategoryArray);
			JsonNode->SetObjectField(TEXT("search"), Search);

			NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
			ProcessedFunctions++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Processed %d events and %d functions"), ProcessedEvents, ProcessedFunctions);


	UE_LOG(LogTemp, Log, TEXT("Scanning standard macros..."));
	int32 ProcessedMacros = 0;


	UBlueprint* StandardMacros = LoadObject<UBlueprint>(
		nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	if (StandardMacros)
	{
		for (UEdGraph* Graph : StandardMacros->MacroGraphs)
		{
			if (!Graph) continue;

			FString MacroName = Graph->GetName();
			FString MacroId = FString::Printf(TEXT("macro.StandardMacros.%s"), *MacroName);

			TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
			JsonNode->SetStringField(TEXT("id"), MacroId);
			JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_MacroInstance"));
			JsonNode->SetStringField(TEXT("kind"), TEXT("macro"));
			JsonNode->SetStringField(TEXT("title"), FName::NameToDisplayString(MacroName, false));


			TSharedPtr<FJsonObject> Source = MakeShareable(new FJsonObject);
			Source->SetStringField(
				TEXT("macro_graph"),
				FString::Printf(TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:%s"), *MacroName));
			Source->SetStringField(TEXT("macro_name"), MacroName);
			JsonNode->SetObjectField(TEXT("source"), Source);


			TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
			TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs, Outputs;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;


				if (Node->GetClass()->GetName().Contains(TEXT("Tunnel")) && Node->GetName().Contains(TEXT("Entry")))
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin->Direction == EGPD_Output)
						{
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
							{
								ExecIn.Add(MakeShareable(new FJsonValueString(Pin->PinName.ToString())));
							}
							else
							{
								TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
								PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
								PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
								PinObj->SetBoolField(TEXT("required"), !Pin->bDefaultValueIsIgnored);
								Inputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
							}
						}
					}
				}


				if (Node->GetClass()->GetName().Contains(TEXT("Tunnel")) && Node->GetName().Contains(TEXT("Result")))
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin->Direction == EGPD_Input)
						{
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
							{
								ExecOut.Add(MakeShareable(new FJsonValueString(Pin->PinName.ToString())));
							}
							else
							{
								TSharedPtr<FJsonObject> PinObj = MakeShareable(new FJsonObject);
								PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
								PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
								PinObj->SetBoolField(TEXT("required"), !Pin->bDefaultValueIsIgnored);
								Outputs.Add(MakeShareable(new FJsonValueObject(PinObj)));
							}
						}
					}
				}
			}

			Pins->SetArrayField(TEXT("exec_in"), ExecIn);
			Pins->SetArrayField(TEXT("exec_out"), ExecOut);
			Pins->SetArrayField(TEXT("inputs"), Inputs);
			Pins->SetArrayField(TEXT("outputs"), Outputs);
			JsonNode->SetObjectField(TEXT("pins"), Pins);


			TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
			Flags->SetBoolField(TEXT("pure"), false);
			JsonNode->SetObjectField(TEXT("flags"), Flags);


			TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
			TArray<TSharedPtr<FJsonValue>> Keywords;
			for (const FString& Keyword : GenerateKeywords(MacroName))
			{
				Keywords.Add(MakeShareable(new FJsonValueString(Keyword)));
			}
			Search->SetArrayField(TEXT("keywords"), Keywords);
			TArray<TSharedPtr<FJsonValue>> Category;
			Category.Add(MakeShareable(new FJsonValueString(TEXT("StandardMacros"))));
			Search->SetArrayField(TEXT("category"), Category);
			JsonNode->SetObjectField(TEXT("search"), Search);

			NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
			ProcessedMacros++;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Processed %d macros"), ProcessedMacros);


	UE_LOG(LogTemp, Log, TEXT("Adding special built-in nodes..."));
	int32 ProcessedSpecialNodes = 0;


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.Branch"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_IfThenElse"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Branch"));
		JsonNode->SetStringField(TEXT("AgentNeedKnow"), TEXT("这个是真正的蓝图 Branch"));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("else"))));

		TSharedPtr<FJsonObject> ConditionPin = MakeShareable(new FJsonObject);
		ConditionPin->SetStringField(TEXT("name"), TEXT("Condition"));
		ConditionPin->SetStringField(TEXT("type"), TEXT("bool"));
		ConditionPin->SetBoolField(TEXT("required"), true);
		Inputs.Add(MakeShareable(new FJsonValueObject(ConditionPin)));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		Pins->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("branch"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("if"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("condition"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("FlowControl"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.Sequence"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_ExecutionSequence"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Sequence"));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then_0"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then_1"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("提示：你可以通过直接使用 then_2 then_3 ... then_xxx 等作为引脚!"))));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("sequence"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("parallel"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("fork"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("FlowControl"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.MathExpression"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_MathExpression"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Math Expression"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"Example: \n node x : special.MathExpression @ (0, 0) \n set x.Expression = \"(A + B) * 2\" \n Input pins (A, B, etc.) are automatically created based on the expression"));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("exec_out"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());

		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeShareable(new FJsonValueString(TEXT("ReturnValue"))));
		Pins->SetArrayField(TEXT("outputs"), Outputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), true);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("math"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("MathExpression"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("expression"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("formula"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Math"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.Make"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_MakeStruct"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Make Struct"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"用于用户自定义 Struct（游戏代码定义或蓝图定义）的 Make 节点。用法：node c : special.Make.FMyStruct @ (0, 0)，输出 pin 名为去掉 F 前缀的短名（如 c.MyStruct），成员 pin 直接用成员名（如 set c.X = 10）。注意：FTransform、FVector、FRotator 等 UE 内置数学类型不支持此节点，须用 call.KismetMathLibrary.Make* 代替（见规则 6.5）。"));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("exec_out"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());

		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeShareable(new FJsonValueString(TEXT("Output"))));
		Pins->SetArrayField(TEXT("outputs"), Outputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), true);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("make"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("construct"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("struct"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Struct"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.Break"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_BreakStruct"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Break Struct"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"用于用户自定义 Struct（游戏代码定义或蓝图定义）的 Break 节点。用法：node c : special.Break.FMyStruct @ (0, 0)，各成员作为输出 pin 直接用成员名连线（如 link c.X -> target.input）。注意：FTransform、FVector、FRotator 等 UE 内置数学类型不支持此节点，须用 call.KismetMathLibrary.Break* 代替（见规则 6.5）。"));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		Pins->SetArrayField(TEXT("exec_in"), TArray<TSharedPtr<FJsonValue>>());
		Pins->SetArrayField(TEXT("exec_out"), TArray<TSharedPtr<FJsonValue>>());

		TArray<TSharedPtr<FJsonValue>> Inputs;
		Inputs.Add(MakeShareable(new FJsonValueString(TEXT("Input"))));
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		Pins->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), true);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("break"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("split"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("struct"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Struct"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.SpawnActorFromClass"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_SpawnActorFromClass"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Spawn Actor from Class"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"Spawn an Actor into the world. Use set to specify the Class pin (e.g. /Game/BP_MyActor.BP_MyActor_C). SpawnTransform is a ref pin — it CANNOT be left empty, you MUST connect it. FTransform is a UE built-in math type, use KismetMathLibrary (NOT special.Make):\n  node t : call.KismetMathLibrary.MakeTransform @ (x, y)\n  link t.ReturnValue -> <thisNode>.SpawnTransform\nAfter Class is set, ExposeOnSpawn properties of the selected class appear as additional input pins."));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs, Outputs;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));

		TSharedPtr<FJsonObject> ClassPin = MakeShareable(new FJsonObject);
		ClassPin->SetStringField(TEXT("name"), TEXT("Class"));
		ClassPin->SetStringField(TEXT("type"), TEXT("class<Actor>"));
		Inputs.Add(MakeShareable(new FJsonValueObject(ClassPin)));

		TSharedPtr<FJsonObject> TransformPin = MakeShareable(new FJsonObject);
		TransformPin->SetStringField(TEXT("name"), TEXT("SpawnTransform"));
		TransformPin->SetStringField(TEXT("type"), TEXT("Transform"));
		TransformPin->SetBoolField(TEXT("ref"), true);
		Inputs.Add(MakeShareable(new FJsonValueObject(TransformPin)));

		TSharedPtr<FJsonObject> CollisionPin = MakeShareable(new FJsonObject);
		CollisionPin->SetStringField(TEXT("name"), TEXT("CollisionHandlingOverride"));
		CollisionPin->SetStringField(TEXT("type"), TEXT("ESpawnActorCollisionHandlingMethod"));
		Inputs.Add(MakeShareable(new FJsonValueObject(CollisionPin)));

		TSharedPtr<FJsonObject> OwnerPin = MakeShareable(new FJsonObject);
		OwnerPin->SetStringField(TEXT("name"), TEXT("Owner"));
		OwnerPin->SetStringField(TEXT("type"), TEXT("Actor"));
		Inputs.Add(MakeShareable(new FJsonValueObject(OwnerPin)));

		TSharedPtr<FJsonObject> RetPin = MakeShareable(new FJsonObject);
		RetPin->SetStringField(TEXT("name"), TEXT("ReturnValue"));
		RetPin->SetStringField(TEXT("type"), TEXT("Actor"));
		Outputs.Add(MakeShareable(new FJsonValueObject(RetPin)));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		Pins->SetArrayField(TEXT("outputs"), Outputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("spawn actor"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("spawn"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("spawnactor"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("create actor"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("instantiate"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpawnActorFromClass"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Actor"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.ConstructObjectFromClass"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_ConstructObjectFromClass"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Construct Object from Class"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"Construct a UObject instance from a class. Set Class pin via set statement. After Class is set, ExposeOnSpawn properties appear as additional pins automatically."));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs, Outputs;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));

		TSharedPtr<FJsonObject> ClassPin2 = MakeShareable(new FJsonObject);
		ClassPin2->SetStringField(TEXT("name"), TEXT("Class"));
		ClassPin2->SetStringField(TEXT("type"), TEXT("class<Object>"));
		Inputs.Add(MakeShareable(new FJsonValueObject(ClassPin2)));

		TSharedPtr<FJsonObject> OuterPin = MakeShareable(new FJsonObject);
		OuterPin->SetStringField(TEXT("name"), TEXT("Outer"));
		OuterPin->SetStringField(TEXT("type"), TEXT("Object"));
		Inputs.Add(MakeShareable(new FJsonValueObject(OuterPin)));

		TSharedPtr<FJsonObject> RetPin2 = MakeShareable(new FJsonObject);
		RetPin2->SetStringField(TEXT("name"), TEXT("ReturnValue"));
		RetPin2->SetStringField(TEXT("type"), TEXT("Object"));
		Outputs.Add(MakeShareable(new FJsonValueObject(RetPin2)));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		Pins->SetArrayField(TEXT("outputs"), Outputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("construct object"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("construct"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("create object"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("new object"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("ConstructObjectFromClass"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Object"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.Cast"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_DynamicCast"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Cast To"));
		JsonNode->SetStringField(
			TEXT("AgentNeedKnow"),
			TEXT(
				"Cast an object to a specific type. Type is encoded in schema ID like Make/Break (e.g. special.Cast.MyActor). Output pin is named As<TypeName> (e.g. AsMyActor). CastFailed exec fires on failure."));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs, Outputs;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("then"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("CastFailed"))));

		TSharedPtr<FJsonObject> ObjPin = MakeShareable(new FJsonObject);
		ObjPin->SetStringField(TEXT("name"), TEXT("Object"));
		ObjPin->SetStringField(TEXT("type"), TEXT("Object"));
		Inputs.Add(MakeShareable(new FJsonValueObject(ObjPin)));

		TSharedPtr<FJsonObject> OutPin = MakeShareable(new FJsonObject);
		OutPin->SetStringField(TEXT("name"), TEXT("As<TypeName>"));
		OutPin->SetStringField(TEXT("type"), TEXT("TargetType"));
		Outputs.Add(MakeShareable(new FJsonValueObject(OutPin)));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		Pins->SetArrayField(TEXT("outputs"), Outputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("cast"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("cast to"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("dynamic cast"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("type cast"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Cast"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}


	{
		TSharedPtr<FJsonObject> JsonNode = MakeShareable(new FJsonObject);
		JsonNode->SetStringField(TEXT("id"), TEXT("special.SwitchEnum"));
		JsonNode->SetStringField(TEXT("node_class"), TEXT("K2Node_SwitchEnum"));
		JsonNode->SetStringField(TEXT("kind"), TEXT("special"));
		JsonNode->SetStringField(TEXT("title"), TEXT("Switch on Enum"));
		JsonNode->SetStringField(TEXT("AgentNeedKnow"), TEXT(
			                         "Switch on an enum value. Encode the enum type in the schema ID: special.SwitchEnum.<EnumName> (e.g. special.SwitchEnum.EMyEnum). "
			                         "The exec output pins are named after each enum value exactly as returned by SearchType (e.g. EMyEnum::ValueA → exec out pin name is 'ValueA'). "
			                         "The Selection input pin receives the enum variable. "
			                         "Use SearchType to look up the enum values before writing the node."
		                         ));

		TSharedPtr<FJsonObject> Pins = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> ExecIn, ExecOut, Inputs;
		ExecIn.Add(MakeShareable(new FJsonValueString(TEXT("execute"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("<EnumValue0>"))));
		ExecOut.Add(MakeShareable(new FJsonValueString(TEXT("<EnumValue1> ..."))));

		TSharedPtr<FJsonObject> SelPin = MakeShareable(new FJsonObject);
		SelPin->SetStringField(TEXT("name"), TEXT("Selection"));
		SelPin->SetStringField(TEXT("type"), TEXT("Enum (EnumName)"));
		Inputs.Add(MakeShareable(new FJsonValueObject(SelPin)));

		Pins->SetArrayField(TEXT("exec_in"), ExecIn);
		Pins->SetArrayField(TEXT("exec_out"), ExecOut);
		Pins->SetArrayField(TEXT("inputs"), Inputs);
		JsonNode->SetObjectField(TEXT("pins"), Pins);

		TSharedPtr<FJsonObject> Flags = MakeShareable(new FJsonObject);
		Flags->SetBoolField(TEXT("pure"), false);
		JsonNode->SetObjectField(TEXT("flags"), Flags);

		TSharedPtr<FJsonObject> Search = MakeShareable(new FJsonObject);
		TArray<TSharedPtr<FJsonValue>> Keywords;
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("switch"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("switch on enum"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("enum branch"))));
		Keywords.Add(MakeShareable(new FJsonValueString(TEXT("SpecialCategory"))));
		Search->SetArrayField(TEXT("keywords"), Keywords);
		TArray<TSharedPtr<FJsonValue>> Category;
		Category.Add(MakeShareable(new FJsonValueString(TEXT("Switch"))));
		Search->SetArrayField(TEXT("category"), Category);
		JsonNode->SetObjectField(TEXT("search"), Search);

		NodesArray.Add(MakeShareable(new FJsonValueObject(JsonNode)));
		ProcessedSpecialNodes++;
	}

	UE_LOG(LogTemp, Log, TEXT("Processed %d special nodes"), ProcessedSpecialNodes);


	NodeCatalog.Empty();
	for (const TSharedPtr<FJsonValue>& NodeValue : NodesArray)
	{
		NodeCatalog.Add(NodeValue->AsObject());
	}
	bCatalogInitialized = true;


	BuildSearchIndex();

	UE_LOG(LogTemp, Log, TEXT("Saved %d nodes to memory catalog"), NodeCatalog.Num());


	TSharedPtr<FJsonObject> RootObject = MakeShareable(new FJsonObject);
	RootObject->SetStringField(TEXT("version"), TEXT("bp-node-catalog/0.1"));
	RootObject->SetArrayField(TEXT("nodes"), NodesArray);


	TArray<TSharedPtr<FJsonValue>> TypesArray;
	TSet<FString> ProcessedTypes;
	int32 ProcessedStructs = 0;
	int32 ProcessedClasses = 0;


	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		FString StructName = Struct->GetStructCPPName();

		if (ProcessedTypes.Contains(StructName))
			continue;

		ProcessedTypes.Add(StructName);

		TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject);
		TypeObj->SetStringField(TEXT("name"), StructName);
		TypeObj->SetStringField(TEXT("kind"), TEXT("struct"));

		TArray<TSharedPtr<FJsonValue>> Props;

		for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;

			TSharedPtr<FJsonObject> PropObj = MakeShareable(new FJsonObject);
			PropObj->SetStringField(TEXT("name"), Property->GetName());

			FString TypeName = Property->GetCPPType();
			PropObj->SetStringField(TEXT("type"), TypeName);

			Props.Add(MakeShareable(new FJsonValueObject(PropObj)));
		}

		TypeObj->SetArrayField(TEXT("properties"), Props);
		TypesArray.Add(MakeShareable(new FJsonValueObject(TypeObj)));
		ProcessedStructs++;
	}


	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		FString ClassName = Class->GetPrefixCPP() + Class->GetName();

		if (ProcessedTypes.Contains(ClassName))
			continue;


		if (Class->HasAnyClassFlags(CLASS_Deprecated))
			continue;

		ProcessedTypes.Add(ClassName);

		TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject);
		TypeObj->SetStringField(TEXT("name"), ClassName);
		TypeObj->SetStringField(TEXT("kind"), TEXT("object"));

		TArray<TSharedPtr<FJsonValue>> Props;

		for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;


			if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
				continue;

			TSharedPtr<FJsonObject> PropObj = MakeShareable(new FJsonObject);
			PropObj->SetStringField(TEXT("name"), Property->GetName());

			FString TypeName = Property->GetCPPType();
			PropObj->SetStringField(TEXT("type"), TypeName);

			Props.Add(MakeShareable(new FJsonValueObject(PropObj)));
		}

		TypeObj->SetArrayField(TEXT("properties"), Props);
		TypesArray.Add(MakeShareable(new FJsonValueObject(TypeObj)));
		ProcessedClasses++;
	}


	int32 ProcessedEnums = 0;
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		UEnum* Enum = *It;
		if (!Enum) continue;


		if (!Enum->HasAnyFlags(RF_Public)) continue;

		FString EnumName = Enum->GetName();
		if (ProcessedTypes.Contains(EnumName)) continue;
		ProcessedTypes.Add(EnumName);

		TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject);
		TypeObj->SetStringField(TEXT("name"), EnumName);
		TypeObj->SetStringField(TEXT("kind"), TEXT("enum"));


		TArray<TSharedPtr<FJsonValue>> ValuesArray;
		int32 NumEnums = Enum->NumEnums();
		for (int32 i = 0; i < NumEnums; ++i)
		{
			FString ValueName = Enum->GetNameStringByIndex(i);

			int32 ScopeIdx;
			if (ValueName.FindLastChar(':', ScopeIdx))
				ValueName = ValueName.Mid(ScopeIdx + 1);

			if (ValueName.EndsWith(TEXT("_MAX")) || ValueName == TEXT("MAX"))
				continue;
			ValuesArray.Add(MakeShareable(new FJsonValueString(ValueName)));
		}
		TypeObj->SetArrayField(TEXT("values"), ValuesArray);

		TypesArray.Add(MakeShareable(new FJsonValueObject(TypeObj)));
		ProcessedEnums++;
	}

	RootObject->SetArrayField(TEXT("types"), TypesArray);


	TypeCatalog.Empty();
	for (const TSharedPtr<FJsonValue>& TypeValue : TypesArray)
	{
		TypeCatalog.Add(TypeValue->AsObject());
	}
	bTypeCatalogInitialized = true;

	UE_LOG(LogTemp, Log, TEXT("Processed %d structs, %d classes, %d enums"), ProcessedStructs, ProcessedClasses,
	       ProcessedEnums);


	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);


	FString FilePath = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("Desktop"), TEXT("WeaveLanguage.json"));
	if (FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		UE_LOG(LogTemp, Log, TEXT("Weave Language saved to: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save Weave Language"));
	}
}

TArray<FString> UWeaveOperator::SearchNode(const FString& Query)
{
	TArray<FString> Results;
	FString LowerQuery = Query.ToLower();
	TMap<int32, int32> NodeScores;


	if (IdIndex.Contains(LowerQuery))
	{
		NodeScores.Add(IdIndex[LowerQuery], 100);
	}


	for (const auto& Pair : KeywordIndex)
	{
		if (Pair.Key.Contains(LowerQuery))
		{
			int32 Score = 20;

			if (Pair.Key == LowerQuery)
			{
				Score = 60;
			}
			else if (Pair.Key.StartsWith(LowerQuery))
			{
				Score = 50;
			}
			else if (Pair.Key.EndsWith(LowerQuery))
			{
				Score = 40;
			}

			for (int32 Index : Pair.Value)
			{
				int32& CurrentScore = NodeScores.FindOrAdd(Index, 0);
				CurrentScore = FMath::Max(CurrentScore, Score);
			}
		}
	}


	TArray<TPair<int32, int32>> SortedNodes;
	for (const auto& Pair : NodeScores)
	{
		int32 Index = Pair.Key;
		int32 Score = Pair.Value;


		if (NodeCatalog.IsValidIndex(Index))
		{
			FString Title;
			if (NodeCatalog[Index]->TryGetStringField(TEXT("title"), Title))
			{
				FString LowerTitle = Title.ToLower();
				if (LowerTitle == LowerQuery)
				{
					Score += 1000;
				}
			}
		}

		SortedNodes.Add(TPair<int32, int32>(Index, Score));
	}


	SortedNodes.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
	{
		return A.Value > B.Value;
	});


	int32 MaxResults = FMath::Min(10, SortedNodes.Num());
	for (int32 i = 0; i < MaxResults; i++)
	{
		int32 Index = SortedNodes[i].Key;
		if (NodeCatalog.IsValidIndex(Index))
		{
			FString OriginalJson;
			TSharedRef<TJsonWriter<>> OriginalWriter = TJsonWriterFactory<>::Create(&OriginalJson);
			FJsonSerializer::Serialize(NodeCatalog[Index].ToSharedRef(), OriginalWriter);


			TSharedPtr<FJsonObject> NodeCopy;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OriginalJson);
			FJsonSerializer::Deserialize(Reader, NodeCopy);


			if (NodeCopy.IsValid() && NodeCopy->HasField(TEXT("search")))
			{
				TSharedPtr<FJsonObject> SearchObj = NodeCopy->GetObjectField(TEXT("search"));
				if (SearchObj.IsValid())
				{
					SearchObj->RemoveField(TEXT("keywords"));
				}


				FString JsonString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
				FJsonSerializer::Serialize(NodeCopy.ToSharedRef(), Writer);
				Results.Add(JsonString);
			}
		}
	}

	return Results;
}

TArray<FString> UWeaveOperator::SearchType(const FString& Query)
{
	TArray<FString> Results;

	if (!bTypeCatalogInitialized)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Type catalog not initialized"));
		return Results;
	}

	FString LowerQuery = Query.ToLower();


	for (const TSharedPtr<FJsonObject>& TypeObj : TypeCatalog)
	{
		if (!TypeObj.IsValid())
			continue;

		FString TypeName;
		if (TypeObj->TryGetStringField(TEXT("name"), TypeName))
		{
			FString LowerTypeName = TypeName.ToLower();


			if (LowerTypeName.Contains(LowerQuery))
			{
				FString JsonString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
				FJsonSerializer::Serialize(TypeObj.ToSharedRef(), Writer);
				Results.Add(JsonString);


				if (Results.Num() >= 10)
					break;
			}
		}
	}

	return Results;
}

TArray<FString> UWeaveOperator::SearchBlueprintVariables(const FString& BlueprintPath, const FString& Query)
{
	TArray<FString> Results;


	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to load blueprint: %s"), *BlueprintPath);
		return Results;
	}

	FString LowerQuery = Query.ToLower();


	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		FString VarName = Variable.VarName.ToString();
		FString LowerVarName = VarName.ToLower();


		if (LowerQuery.IsEmpty() || LowerVarName.Contains(LowerQuery))
		{
			TSharedPtr<FJsonObject> VarObj = MakeShareable(new FJsonObject);
			VarObj->SetStringField(TEXT("name"), VarName);
			VarObj->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());

			FString JsonString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(VarObj.ToSharedRef(), Writer);
			Results.Add(JsonString);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Found %d variables in blueprint %s"), Results.Num(), *BlueprintPath);
	return Results;
}

TArray<FString> UWeaveOperator::SearchContextVar(const FString& BlueprintPath, const FString& Query)
{
	TArray<FString> Results;

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint) return Results;

	FString LowerQuery = Query.ToLower();
	FString BPShortName = Blueprint->GetName();


	TSet<FString> UserVarNames;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		UserVarNames.Add(Var.VarName.ToString());


	auto GetFriendlyType = [](FProperty* Prop, bool& bOutIsComponent) -> FString
	{
		bOutIsComponent = false;
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UClass* PropClass = ObjProp->PropertyClass;
			if (PropClass && PropClass->IsChildOf(UActorComponent::StaticClass()))
				bOutIsComponent = true;
			return PropClass ? PropClass->GetName() : TEXT("Object");
		}
		if (CastField<FIntProperty>(Prop)) return TEXT("int");
		if (CastField<FDoubleProperty>(Prop)) return TEXT("float");
		if (CastField<FFloatProperty>(Prop)) return TEXT("float");
		if (CastField<FBoolProperty>(Prop)) return TEXT("bool");
		if (CastField<FStrProperty>(Prop)) return TEXT("string");
		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			return StructProp->Struct ? StructProp->Struct->GetName() : TEXT("struct");
		return Prop->GetCPPType();
	};

	auto MakeEntry = [&](FProperty* Prop, bool bDeletable, bool bFromParent) -> FString
	{
		bool bIsComponent = false;
		FString TypeName = GetFriendlyType(Prop, bIsComponent);
		FString PropName = Prop->GetName();

		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
		Obj->SetStringField(TEXT("name"), PropName);
		Obj->SetStringField(TEXT("type"), TypeName);
		Obj->SetBoolField(TEXT("deletable"), bDeletable);
		if (bIsComponent)
			Obj->SetStringField(TEXT("kind"), TEXT("component"));
		else if (bFromParent)
			Obj->SetStringField(TEXT("kind"), TEXT("inherited"));

		Obj->SetStringField(TEXT("weave_get"),
		                    FString::Printf(TEXT("node x : VariableGet.%s.%s"), *BPShortName, *PropName));
		Obj->SetStringField(TEXT("weave_set"),
		                    FString::Printf(TEXT("node x : VariableSet.%s.%s"), *BPShortName, *PropName));

		FString Json;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Json;
	};


	UClass* GenClass = Blueprint->GeneratedClass;
	if (GenClass)
	{
		for (TFieldIterator<FProperty> It(GenClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
			FString PropName = Prop->GetName();
			if (!LowerQuery.IsEmpty() && !PropName.ToLower().Contains(LowerQuery)) continue;
			Results.Add(MakeEntry(Prop, UserVarNames.Contains(PropName), false));
		}

		for (TFieldIterator<FProperty> It(GenClass->GetSuperClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
			FString PropName = Prop->GetName();
			if (!LowerQuery.IsEmpty() && !PropName.ToLower().Contains(LowerQuery)) continue;
			Results.Add(MakeEntry(Prop, false, true));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] SearchContextVar: GeneratedClass is null for %s, using fallback"),
		       *BlueprintPath);
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			FString VarName = Var.VarName.ToString();
			if (!LowerQuery.IsEmpty() && !VarName.ToLower().Contains(LowerQuery)) continue;
			TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
			Obj->SetStringField(TEXT("name"), VarName);
			Obj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
			Obj->SetBoolField(TEXT("deletable"), true);
			Obj->SetStringField(
				TEXT("weave_get"), FString::Printf(TEXT("node x : VariableGet.%s.%s"), *BPShortName, *VarName));
			Obj->SetStringField(
				TEXT("weave_set"), FString::Printf(TEXT("node x : VariableSet.%s.%s"), *BPShortName, *VarName));
			FString Json;
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Json);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
			Results.Add(Json);
		}
	}

	return Results;
}

bool UWeaveOperator::ModifyVar(const FString& BlueprintPath, const FString& VarName, const FString& NewValue,
                               FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		OutError = TEXT("Failed to load blueprint");
		return false;
	}


	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (Variable.VarName.ToString() == VarName)
		{
			Variable.DefaultValue = NewValue;
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Variable not found: %s"), *VarName);
	return false;
}

bool UWeaveOperator::DeleteVar(const FString& BlueprintPath, const FString& VarName, FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		OutError = TEXT("Failed to load blueprint");
		return false;
	}


	FName VarFName = FName(*VarName);
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarFName);
	return true;
}

TArray<FString> UWeaveOperator::SearchContextFunctions(const FString& BlueprintPath, const FString& Query)
{
	TArray<FString> Results;

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		return Results;
	}

	FString LowerQuery = Query.ToLower();


	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == TEXT("UserConstructionScript"))
		{
			if (LowerQuery.IsEmpty() || FString("UserConstructionScript").ToLower().Contains(LowerQuery))
			{
				TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);
				FuncObj->SetStringField(TEXT("name"), TEXT("UserConstructionScript"));
				FuncObj->SetStringField(TEXT("type"), TEXT("construction"));
				FuncObj->SetArrayField(TEXT("inputs"), TArray<TSharedPtr<FJsonValue>>());
				FuncObj->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());

				FString JsonString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
				FJsonSerializer::Serialize(FuncObj.ToSharedRef(), Writer);
				Results.Add(JsonString);
			}
			break;
		}
	}


	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph) continue;

		FString FuncName = Graph->GetName();
		if (LowerQuery.IsEmpty() || FuncName.ToLower().Contains(LowerQuery))
		{
			TSharedPtr<FJsonObject> FuncObj = MakeShareable(new FJsonObject);
			FuncObj->SetStringField(TEXT("name"), FuncName);
			FuncObj->SetStringField(TEXT("type"), TEXT("function"));

			TArray<TSharedPtr<FJsonValue>> Inputs;
			TArray<TSharedPtr<FJsonValue>> Outputs;


			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
				{
					for (UEdGraphPin* Pin : EntryNode->Pins)
					{
						if (Pin->Direction == EGPD_Output && Pin->PinName != TEXT("then"))
						{
							TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject);
							ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
							ParamObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
							Inputs.Add(MakeShareable(new FJsonValueObject(ParamObj)));
						}
					}
				}
				else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
				{
					for (UEdGraphPin* Pin : ResultNode->Pins)
					{
						if (Pin->Direction == EGPD_Input && Pin->PinName != TEXT("execute"))
						{
							TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject);
							ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
							ParamObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
							Outputs.Add(MakeShareable(new FJsonValueObject(ParamObj)));
						}
					}
				}
			}

			FuncObj->SetArrayField(TEXT("inputs"), Inputs);
			FuncObj->SetArrayField(TEXT("outputs"), Outputs);

			FString JsonString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(FuncObj.ToSharedRef(), Writer);
			Results.Add(JsonString);
		}
	}

	return Results;
}

TArray<FString> UWeaveOperator::SearchAsset(const FString& Query, int32 MaxResults)
{
	TArray<FString> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<
		FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAllAssets(AssetDataList);

	FString LowerQuery = Query.ToLower();

	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetPath = AssetData.GetObjectPathString();
		if (AssetPath.ToLower().Contains(LowerQuery))
		{
			Results.Add(AssetPath);
			if (Results.Num() >= MaxResults)
			{
				break;
			}
		}
	}

	return Results;
}

TArray<FString> UWeaveOperator::GetAssetReferences(const FString& AssetPath, int32 MaxResults)
{
	TArray<FString> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<
		FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FAssetIdentifier AssetId = FAssetIdentifier(FName(*AssetPath));
	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(AssetId, Referencers);

	for (const FAssetIdentifier& Referencer : Referencers)
	{
		Results.Add(Referencer.ToString());
		if (Results.Num() >= MaxResults)
		{
			break;
		}
	}

	return Results;
}

void UWeaveOperator::SearchNodeAsync(const FString& Query, TFunction<void(const TArray<FString>&)> OnComplete)
{
	Async(EAsyncExecution::ThreadPool, [Query, OnComplete]()
	{
		TArray<FString> Results = SearchNode(Query);


		AsyncTask(ENamedThreads::GameThread, [Results, OnComplete]()
		{
			OnComplete(Results);
		});
	});
}

FString UWeaveOperator::GetNodeById(const FString& Id)
{
	for (const TSharedPtr<FJsonObject>& Node : NodeCatalog)
	{
		if (Node->GetStringField(TEXT("id")) == Id)
		{
			FString JsonString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(Node.ToSharedRef(), Writer);
			return JsonString;
		}
	}
	return TEXT("");
}

TArray<FString> UWeaveOperator::GetNodesByCategory(const FString& Category)
{
	TArray<FString> Results;

	for (const TSharedPtr<FJsonObject>& Node : NodeCatalog)
	{
		if (Node->HasField(TEXT("search")))
		{
			TSharedPtr<FJsonObject> Search = Node->GetObjectField(TEXT("search"));
			const TArray<TSharedPtr<FJsonValue>>* Categories;
			if (Search->TryGetArrayField(TEXT("category"), Categories))
			{
				for (const TSharedPtr<FJsonValue>& Cat : *Categories)
				{
					if (Cat->AsString() == Category)
					{
						FString JsonString;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
						FJsonSerializer::Serialize(Node.ToSharedRef(), Writer);
						Results.Add(JsonString);
						break;
					}
				}
			}
		}
	}

	return Results;
}

void UWeaveOperator::AddNodeFromJson(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		NodeCatalog.Add(JsonObject);
	}
}

bool UWeaveOperator::RemoveNode(const FString& Id)
{
	for (int32 i = 0; i < NodeCatalog.Num(); i++)
	{
		if (NodeCatalog[i]->GetStringField(TEXT("id")) == Id)
		{
			NodeCatalog.RemoveAt(i);
			return true;
		}
	}
	return false;
}

TArray<FString> UWeaveOperator::GetAllNodesAsJson()
{
	TArray<FString> Results;

	for (const TSharedPtr<FJsonObject>& Node : NodeCatalog)
	{
		FString JsonString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(Node.ToSharedRef(), Writer);
		Results.Add(JsonString);
	}

	return Results;
}

void UWeaveOperator::ClearNodes()
{
	NodeCatalog.Empty();
	bCatalogInitialized = false;
}

int32 UWeaveOperator::GetNodeCount()
{
	return NodeCatalog.Num();
}

FString UWeaveOperator::GetBlueprintWeave(const FString& BlueprintPath, const FString& GraphName,
                                          const FString& EntryNode)
{
	UBlueprint* BP = nullptr;


	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
			for (UObject* Asset : EditedAssets)
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
				{
					FString AssetPath = Blueprint->GetPathName();

					int32 ColonIndex;
					if (AssetPath.FindChar(':', ColonIndex))
					{
						AssetPath = AssetPath.Left(ColonIndex);
					}

					if (AssetPath == BlueprintPath)
					{
						BP = Blueprint;
						break;
					}
				}
			}
		}
	}


	if (!BP)
	{
		BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	}

	if (!BP)
	{
		UE_LOG(LogTemp, Error, TEXT("[Weaver] Failed to load blueprint: %s"), *BlueprintPath);
		return TEXT("");
	}


	UEdGraph* TargetGraph = nullptr;


	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Contains(GraphName))
		{
			TargetGraph = Graph;
			break;
		}
	}


	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Contains(GraphName))
			{
				TargetGraph = Graph;
				break;
			}
		}
	}

	if (!TargetGraph)
	{
		UE_LOG(LogTemp, Error, TEXT("[Weaver] Graph not found: %s"), *GraphName);
		return TEXT("");
	}


	TArray<UEdGraphNode*> AllNodes;

	if (!EntryNode.IsEmpty())
	{
		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> NodesToVisit;


		UEdGraphNode* StartNode = nullptr;
		for (UEdGraphNode* Node : TargetGraph->Nodes)
		{
			if (Node && Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(EntryNode))
			{
				StartNode = Node;
				break;
			}
		}

		if (StartNode)
		{
			NodesToVisit.Add(StartNode);


			while (NodesToVisit.Num() > 0)
			{
				UEdGraphNode* CurrentNode = NodesToVisit[0];
				NodesToVisit.RemoveAt(0);

				if (VisitedNodes.Contains(CurrentNode))
				{
					continue;
				}

				VisitedNodes.Add(CurrentNode);
				AllNodes.Add(CurrentNode);


				for (UEdGraphPin* Pin : CurrentNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output)
					{
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode())
							{
								UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
								if (!VisitedNodes.Contains(LinkedNode))
								{
									NodesToVisit.Add(LinkedNode);
								}
							}
						}
					}
				}
			}
		}
	}
	else
	{
		for (UEdGraphNode* Node : TargetGraph->Nodes)
		{
			if (Node)
			{
				AllNodes.Add(Node);
			}
		}
	}


	FString WeaveCode;
	if (FWeaveGenerator::Generate(AllNodes, TargetGraph, WeaveCode))
	{
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Generated Weave code for %s"), *GraphName);
		return WeaveCode;
	}

	return TEXT("");
}

FString UWeaveOperator::ApplyWeaveDiff(const FString& OriginalWeave, const FString& DiffCode, FString& OutError)
{
	TArray<FString> OriginalLines;
	OriginalWeave.ParseIntoArrayLines(OriginalLines);

	TArray<FString> DiffLines;
	DiffCode.ParseIntoArrayLines(DiffLines);

	TArray<FString> ResultLines;
	int32 OriginalIndex = 0;
	int32 DiffIndex = 0;

	while (DiffIndex < DiffLines.Num())
	{
		FString Line = DiffLines[DiffIndex];


		if (Line.StartsWith(TEXT("@@")))
		{
			int32 OldStart = 0;
			int32 Pos1 = Line.Find(TEXT("-"));
			int32 Pos2 = Line.Find(TEXT(","), ESearchCase::IgnoreCase, ESearchDir::FromStart, Pos1);
			if (Pos1 != INDEX_NONE && Pos2 != INDEX_NONE)
			{
				FString StartStr = Line.Mid(Pos1 + 1, Pos2 - Pos1 - 1);
				OldStart = FCString::Atoi(*StartStr);
			}


			while (OriginalIndex < OldStart - 1 && OriginalIndex < OriginalLines.Num())
			{
				ResultLines.Add(OriginalLines[OriginalIndex]);
				OriginalIndex++;
			}


			if (OriginalIndex > OldStart - 1 && OldStart > 0)
			{
				int32 Excess = OriginalIndex - (OldStart - 1);
				int32 NewSize = ResultLines.Num() - Excess;
				if (NewSize >= 0)
					ResultLines.SetNum(NewSize);
				OriginalIndex = OldStart - 1;
			}

			DiffIndex++;


			while (DiffIndex < DiffLines.Num())
			{
				FString HunkLine = DiffLines[DiffIndex];

				if (HunkLine.StartsWith(TEXT("@@")))
				{
					break;
				}
				else if (HunkLine.StartsWith(TEXT(" ")))
				{
					ResultLines.Add(HunkLine.RightChop(1));
					OriginalIndex++;
				}
				else if (HunkLine.StartsWith(TEXT("-")))
				{
					OriginalIndex++;
				}
				else if (HunkLine.StartsWith(TEXT("+")))
				{
					ResultLines.Add(HunkLine.RightChop(1));
				}
				else if (!HunkLine.IsEmpty())
				{
					ResultLines.Add(HunkLine);
					OriginalIndex++;
				}

				DiffIndex++;
			}
		}
		else
		{
			DiffIndex++;
		}
	}


	while (OriginalIndex < OriginalLines.Num())
	{
		ResultLines.Add(OriginalLines[OriginalIndex]);
		OriginalIndex++;
	}


	FString Result;
	for (const FString& Line : ResultLines)
	{
		Result += Line + TEXT("\n");
	}

	return Result;
}

FString UWeaveOperator::DiffCheck(const FString& BlueprintPath, const FString& GraphName, const FString& DiffCode,
                                  FString& OutError)
{
	TArray<FString> DiffLines;
	DiffCode.ParseIntoArrayLines(DiffLines);

	bool bHasDiffHeader = false;
	bool bHasDiffPrefix = false;

	for (const FString& Line : DiffLines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		if (TrimmedLine.StartsWith(TEXT("@@")))
		{
			bHasDiffHeader = true;
		}
		if (TrimmedLine.StartsWith(TEXT("+")) || TrimmedLine.StartsWith(TEXT("-")))
		{
			bHasDiffPrefix = true;
		}
	}


	if (!bHasDiffHeader && !bHasDiffPrefix)
	{
		OutError = TEXT("Diff 格式错误：必须使用标准 Unified Diff 格式\n\n")
			TEXT("正确格式示例（空蓝图）：\n")
			TEXT("@@ -0,0 +1,5 @@\n")
			TEXT("+graphset MyActor /Game/MyActor.MyActor\n")
			TEXT("+graph EventGraph\n")
			TEXT("+\n")
			TEXT("+node a : event.Actor.ReceiveBeginPlay @ (0, 0)\n")
			TEXT("+node b : call.KismetSystemLibrary.PrintString @ (200, 0)\n\n")
			TEXT("正确格式示例（修改现有蓝图）：\n")
			TEXT("@@ -3,1 +3,1 @@\n")
			TEXT("-node a : event.Actor.ReceiveBeginPlay @ (0, 0)\n")
			TEXT("+node a : event.Actor.ReceiveBeginPlay @ (100, 0)\n");
		return TEXT("");
	}


	FString ProcessedDiffCode = DiffCode;
	if (!bHasDiffHeader && bHasDiffPrefix)
	{
		int32 AddCount = 0;
		for (const FString& Line : DiffLines)
		{
			if (Line.TrimStartAndEnd().StartsWith(TEXT("+")))
			{
				AddCount++;
			}
		}


		ProcessedDiffCode = FString::Printf(TEXT("@@ -0,0 +1,%d @@\n%s"), AddCount, *DiffCode);
	}


	FString OriginalWeave = GetBlueprintWeave(BlueprintPath, GraphName);


	FString ResultWeave = ApplyWeaveDiff(OriginalWeave, ProcessedDiffCode, OutError);

	if (!OutError.IsEmpty())
	{
		return TEXT("");
	}


	{
		FWeaveAST AST;
		FString ParseError;
		if (!FWeaveInterpreter::Parse(ResultWeave, AST, ParseError))
		{
			OutError = FString::Printf(TEXT("DiffCheck 语义校验失败：%s"), *ParseError);
			return TEXT("");
		}


		{
			TMap<FString, FString> SeenIds;
			for (const FWeaveNodeDecl& Node : AST.Nodes)
			{
				if (const FString* PrevSchema = SeenIds.Find(Node.NodeId))
				{
					OutError = FString::Printf(
						TEXT("DiffCheck 失败：节点 ID '%s' 重复定义（'%s' 和 '%s'）。\n")
						TEXT("可能原因：Diff 的 @@ 行号与蓝图实际行数不匹配，导致新节点被插入到旧节点之前而未删除旧节点。\n")
						TEXT("解决方法：先调用 GetBlueprintWeave 获取最新 Weave 代码，基于其内容重新生成正确的 Diff；\n")
						TEXT("或为新节点使用不与现有节点冲突的 ID（如 d, e, f...），并用 '-' 行删除不需要的旧节点。"),
						*Node.NodeId, **PrevSchema, *Node.SchemaId);
					return TEXT("");
				}
				SeenIds.Add(Node.NodeId, Node.SchemaId);
			}
		}


		{
			TMap<FString, FString> IdToSchema;
			for (const FWeaveNodeDecl& Node : AST.Nodes)
			{
				IdToSchema.Add(Node.NodeId, Node.SchemaId);
			}


			auto FindCatalogNode = [&](const FString& SchemaId) -> TSharedPtr<FJsonObject>
			{
				for (const TSharedPtr<FJsonObject>& CatNode : NodeCatalog)
				{
					if (CatNode->GetStringField(TEXT("id")) == SchemaId)
						return CatNode;
				}
				return nullptr;
			};


			auto CollectPins = [](const TSharedPtr<FJsonObject>& CatNode, bool bOutput) -> TSet<FString>
			{
				TSet<FString> PinNames;
				const TSharedPtr<FJsonObject>* PinsPtr;
				if (!CatNode->TryGetObjectField(TEXT("pins"), PinsPtr) || !PinsPtr || !(*PinsPtr).IsValid())
					return PinNames;
				const TSharedPtr<FJsonObject>& Pins = *PinsPtr;


				const FString ExecKey = bOutput ? TEXT("exec_out") : TEXT("exec_in");
				const TArray<TSharedPtr<FJsonValue>>* ExecArr;
				if (Pins->TryGetArrayField(ExecKey, ExecArr))
				{
					for (const TSharedPtr<FJsonValue>& V : *ExecArr)
						PinNames.Add(V->AsString());
				}


				const FString DataKey = bOutput ? TEXT("outputs") : TEXT("inputs");
				const TArray<TSharedPtr<FJsonValue>>* DataArr;
				if (Pins->TryGetArrayField(DataKey, DataArr))
				{
					for (const TSharedPtr<FJsonValue>& V : *DataArr)
					{
						const TSharedPtr<FJsonObject>* PinObj;
						if (V->TryGetObject(PinObj))
						{
							FString PinName;
							if ((*PinObj)->TryGetStringField(TEXT("name"), PinName))
								PinNames.Add(PinName);
						}
					}
				}
				return PinNames;
			};

			TArray<FString> PinWarnings;
			for (const FWeaveLinkStmt& Link : AST.Links)
			{
				if (const FString* FromSchema = IdToSchema.Find(Link.FromNode))
				{
					if (FromSchema->StartsWith(TEXT("call.")) || FromSchema->StartsWith(TEXT("event.")))
					{
						TSharedPtr<FJsonObject> CatNode = FindCatalogNode(*FromSchema);
						if (CatNode.IsValid())
						{
							TSet<FString> ValidPins = CollectPins(CatNode, /*bOutput=*/true);

							ValidPins.Add(TEXT("ReturnValue"));
							if (!ValidPins.IsEmpty() && !ValidPins.Contains(Link.FromPin))
							{
								PinWarnings.Add(FString::Printf(
									TEXT("link %s.%s -> %s.%s：节点 '%s'（%s）没有名为 '%s' 的输出引脚，可用输出引脚：[%s]"),
									*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
									*Link.FromNode, **FromSchema, *Link.FromPin,
									*FString::Join(ValidPins.Array(), TEXT(", "))));
							}
						}
					}
				}


				if (const FString* ToSchema = IdToSchema.Find(Link.ToNode))
				{
					if (ToSchema->StartsWith(TEXT("call.")) || ToSchema->StartsWith(TEXT("event.")))
					{
						TSharedPtr<FJsonObject> CatNode = FindCatalogNode(*ToSchema);
						if (CatNode.IsValid())
						{
							TSet<FString> ValidPins = CollectPins(CatNode, /*bOutput=*/false);

							ValidPins.Add(TEXT("self"));
							if (!ValidPins.IsEmpty() && !ValidPins.Contains(Link.ToPin))
							{
								PinWarnings.Add(FString::Printf(
									TEXT("link %s.%s -> %s.%s：节点 '%s'（%s）没有名为 '%s' 的输入引脚，可用输入引脚：[%s]"),
									*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
									*Link.ToNode, **ToSchema, *Link.ToPin,
									*FString::Join(ValidPins.Array(), TEXT(", "))));
							}
						}
					}
				}
			}

			if (!PinWarnings.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("DiffCheck 失败：以下 link 语句的引脚名无效（在 NodeCatalog 中未找到），请先调用 SearchNode 查询正确的引脚名后重新生成 Diff：\n\n%s"),
					*FString::Join(PinWarnings, TEXT("\n")));
				return TEXT("");
			}
		}
	}


	CachedBlueprintPath = BlueprintPath;
	CachedGraphName = GraphName;
	CachedResultWeave = ResultWeave;


	return ResultWeave;
}

bool UWeaveOperator::ApplyDiff(FString& OutError)
{
	if (CachedBlueprintPath.IsEmpty() || CachedGraphName.IsEmpty() || CachedResultWeave.IsEmpty())
	{
		OutError = TEXT("错误：必须先调用 DiffCheck 预览结果");
		return false;
	}


	bool bSuccess = ApplyWeaveToBlueprintWithUndo(CachedResultWeave, CachedBlueprintPath, CachedGraphName, OutError);


	CachedBlueprintPath.Empty();
	CachedGraphName.Empty();
	CachedResultWeave.Empty();

	return bSuccess;
}

bool UWeaveOperator::ApplyWeaveToBlueprintWithUndo(const FString& WeaveCode, const FString& BlueprintPath,
                                                   const FString& GraphName, FString& OutError)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
	{
		OutError = TEXT("Failed to load blueprint");
		return false;
	}


	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Contains(GraphName))
		{
			TargetGraph = Graph;
			break;
		}
	}

	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Contains(GraphName))
			{
				TargetGraph = Graph;
				break;
			}
		}
	}

	if (!TargetGraph)
	{
		OutError = TEXT("Graph not found");
		return false;
	}


	FWeaveAST AST;
	if (!FWeaveInterpreter::Parse(WeaveCode, AST, OutError))
	{
		return false;
	}


	const FScopedTransaction Transaction(FText::FromString(TEXT("Apply Weave Code")));
	TargetGraph->Modify();
	BP->Modify();


	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (Node && !Node->IsA<UK2Node_FunctionEntry>())
		{
			Node->BreakAllNodeLinks();
			NodesToRemove.Add(Node);
		}
	}

	for (UEdGraphNode* Node : NodesToRemove)
	{
		TargetGraph->Nodes.Remove(Node);
	}


	int32 NodesCreated = FWeaveInterpreter::GenerateBlueprint(AST, TargetGraph, OutError);


	if (NodesCreated >= 0)
	{
		BP->MarkPackageDirty();
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);


		FCompilerResultsLog CompileLog;
		CompileLog.bAnnotateMentionedNodes = false;
		CompileLog.bLogInfoOnly = false;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &CompileLog);
		if (CompileLog.NumErrors > 0)
		{
			OutError += FString::Printf(TEXT("\n蓝图编译失败（%d 个错误）：\n"), CompileLog.NumErrors);
			for (const TSharedRef<FTokenizedMessage>& Msg : CompileLog.Messages)
			{
				if (Msg->GetSeverity() == EMessageSeverity::Error)
				{
					OutError += TEXT("  - ") + Msg->ToText().ToString() + TEXT("\n");
				}
			}
		}


		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(BP, false);
				if (EditorInstance)
				{
					FBlueprintEditorUtils::RefreshAllNodes(BP);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Refreshed and recompiled blueprint"));
				}
			}
		}

		return true;
	}

	return false;
}
