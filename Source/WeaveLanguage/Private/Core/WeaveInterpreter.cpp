#include "Core/WeaveInterpreter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Message.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MathExpression.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Core/DynamicPins/SequenceDynamicPinHandler.h"
#include "Core/DynamicPins/SwitchEnumDynamicPinHandler.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Knot.h"
#include "ScopedTransaction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"

// 通过名称查找 UClass，支持完整路径名和短名称
static UClass* FindClassByShortName(const FString& ClassName)
{
	// 如果是完整路径（如 /Script/ModuleName.ClassName），优先用 LoadObject 加载
	if (ClassName.StartsWith(TEXT("/")))
	{
		UClass* Result = LoadObject<UClass>(nullptr, *ClassName);
		if (Result) return Result;
	}

	// 尝试 TryFindTypeSlow（适用于完整路径名或已加载的短名称）
	UClass* Result = UClass::TryFindTypeSlow<UClass>(*ClassName);
	if (Result) return Result;

	// 尝试加前缀
	Result = UClass::TryFindTypeSlow<UClass>(*(TEXT("A") + ClassName));
	if (Result) return Result;
	Result = UClass::TryFindTypeSlow<UClass>(*(TEXT("U") + ClassName));
	if (Result) return Result;

	// TObjectIterator 兜底：按 GetName() 匹配（仅对已加载的类有效）
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (Candidate->GetName() == ClassName)
		{
			return Candidate;
		}
	}

	return nullptr;
}

namespace
{
	template <typename T>
	T* SpawnEditorNode(UEdGraph* Graph)
	{
		T* Node = Graph->CreateIntermediateNode<T>();
		if (Node)
		{
			Node->CreateNewGuid();
		}
		return Node;
	}
}

bool FWeaveInterpreter::Parse(const FString& WeaveCode, FWeaveAST& OutAST, FString& OutError)
{
	TArray<FString> Tokens = Tokenize(WeaveCode);
	if (Tokens.Num() == 0)
	{
		OutError = TEXT("Empty code");
		return false;
	}

	int32 Index = 0;


	if (Index < Tokens.Num() && Tokens[Index] == TEXT("graphset"))
	{
		Index++;
		if (Index >= Tokens.Num())
		{
			OutError = TEXT("Missing blueprint name after graphset");
			return false;
		}
		Index++;


		FString Path;
		while (Index < Tokens.Num() && Tokens[Index] != TEXT("graph"))
		{
			Path += Tokens[Index++];
		}
		OutAST.BlueprintPath = Path;
	}

	if (!ParseGraph(Tokens, Index, OutAST.GraphName))
	{
		OutError = TEXT("Failed to parse graph declaration");
		return false;
	}

	while (Index < Tokens.Num())
	{
		const FString& Token = Tokens[Index];

		if (Token == TEXT("node"))
		{
			FWeaveNodeDecl Node;
			if (!ParseNode(Tokens, Index, Node))
			{
				OutError = FString::Printf(TEXT("Failed to parse node at token %d"), Index);
				return false;
			}
			OutAST.Nodes.Add(Node);
		}
		else if (Token == TEXT("set"))
		{
			FWeaveSetStmt Set;
			if (!ParseSet(Tokens, Index, Set))
			{
				OutError = FString::Printf(TEXT("Failed to parse set at token %d"), Index);
				return false;
			}
			OutAST.Sets.Add(Set);
		}
		else if (Token == TEXT("link"))
		{
			FWeaveLinkStmt Link;
			if (!ParseLink(Tokens, Index, Link))
			{
				// 输出解析失败位置附近的 token 上下文
				FString Context;
				int32 CtxStart = FMath::Max(0, Index - 3);
				int32 CtxEnd = FMath::Min(Tokens.Num(), Index + 4);
				for (int32 c = CtxStart; c < CtxEnd; ++c)
				{
					if (c == Index) Context += TEXT("[>>]");
					Context += Tokens[c] + TEXT(" ");
				}
				OutError = FString::Printf(TEXT("Failed to parse link at token %d. 附近 token: %s（详细原因见 OutputLog）"), Index, *Context);
				return false;
			}
			if (Link.FromNode == Link.ToNode)
			{
				OutError = FString::Printf(
					TEXT(
						"自连死循环：节点 '%s' 的输出引脚 '%s' 连接到自身的输入引脚 '%s'。执行引脚自连会让蓝图在该节点永远循环，永远无法继续执行后续逻辑。请改为连接到其他节点，或删除此 link 语句。"),
					*Link.FromNode, *Link.FromPin, *Link.ToPin);
				return false;
			}
			OutAST.Links.Add(Link);
		}
		else if (Token == TEXT("var"))
		{
			FWeaveVarDecl Var;
			if (!ParseVar(Tokens, Index, Var))
			{
				OutError = FString::Printf(TEXT("Failed to parse var at token %d"), Index);
				return false;
			}
			OutAST.Vars.Add(Var);
		}
		else
		{
			Index++;
		}
	}

	return true;
}

TArray<FString> FWeaveInterpreter::Tokenize(const FString& Code)
{
	TArray<FString> Tokens;
	FString Current;
	bool InString = false;
	bool InComment = false;

	for (int32 i = 0; i < Code.Len(); i++)
	{
		TCHAR Ch = Code[i];


		if (Ch == TEXT('#') && !InString)
		{
			InComment = true;
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			continue;
		}

		if (InComment)
		{
			if (Ch == TEXT('\n') || Ch == TEXT('\r'))
			{
				InComment = false;
			}
			continue;
		}

		if (InString)
		{
			if (Ch == TEXT('\\') && i + 1 < Code.Len() && Code[i + 1] == TEXT('"'))
			{
				Current.AppendChar(TEXT('"'));
				i++;
			}
			else
			{
				Current.AppendChar(Ch);
				if (Ch == TEXT('"'))
				{
					InString = false;
				}
			}
		}
		else if (Ch == TEXT('"'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Current.AppendChar(Ch);
			InString = true;
		}
		else if (FChar::IsWhitespace(Ch) || Ch == TEXT('\n') || Ch == TEXT('\r'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
		}
		else if (Ch == TEXT(':') || Ch == TEXT('=') || Ch == TEXT('.') || Ch == TEXT('(') || Ch == TEXT(')') || Ch ==
			TEXT(',') || Ch == TEXT('@'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Tokens.Add(FString::Chr(Ch));
		}
		else if (Ch == TEXT('-') && i + 1 < Code.Len() && Code[i + 1] == TEXT('>'))
		{
			if (!Current.IsEmpty())
			{
				Tokens.Add(Current);
				Current.Empty();
			}
			Tokens.Add(TEXT("->"));
			i++;
		}
		else
		{
			Current.AppendChar(Ch);
		}
	}

	if (!Current.IsEmpty())
	{
		Tokens.Add(Current);
	}

	return Tokens;
}

bool FWeaveInterpreter::ParseGraph(const TArray<FString>& Tokens, int32& Index, FString& OutGraphName)
{
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("graph"))
	{
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutGraphName = Tokens[Index];
	Index++;

	return true;
}

bool FWeaveInterpreter::ParseNode(const TArray<FString>& Tokens, int32& Index, FWeaveNodeDecl& OutNode)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutNode.NodeId = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
	{
		return false;
	}
	Index++;


	FString SchemaId;
	while (Index < Tokens.Num() && Tokens[Index] != TEXT("@") && Tokens[Index] != TEXT("node") && Tokens[Index] !=
		TEXT("set") && Tokens[Index] != TEXT("link"))
	{
		SchemaId += Tokens[Index++];
	}
	OutNode.SchemaId = SchemaId.TrimStartAndEnd();

	if (Index < Tokens.Num() && Tokens[Index] == TEXT("@"))
	{
		Index++;

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT("("))
		{
			return false;
		}
		Index++;

		if (Index >= Tokens.Num())
		{
			return false;
		}
		float X = FCString::Atof(*Tokens[Index++]);

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(","))
		{
			return false;
		}
		Index++;

		if (Index >= Tokens.Num())
		{
			return false;
		}
		float Y = FCString::Atof(*Tokens[Index++]);

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(")"))
		{
			return false;
		}
		Index++;

		OutNode.Position = FVector2D(X, Y);
	}
	else
	{
		OutNode.Position = FVector2D::ZeroVector;
	}

	return true;
}

bool FWeaveInterpreter::ParseSet(const TArray<FString>& Tokens, int32& Index, FWeaveSetStmt& OutSet)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	OutSet.NodeId = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}
	OutSet.PinName = Tokens[Index++];
	if (OutSet.PinName.StartsWith(TEXT("\"")) && OutSet.PinName.EndsWith(TEXT("\"")) && OutSet.PinName.Len() >= 2)
	{
		OutSet.PinName = OutSet.PinName.Mid(1, OutSet.PinName.Len() - 2);
	}

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("="))
	{
		return false;
	}
	Index++;

	FString Value;

	if (Index < Tokens.Num() && Tokens[Index].StartsWith(TEXT("\"")))
	{
		// 引号包裹的值，去掉首尾引号
		Value = Tokens[Index++];
		if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) && Value.Len() >= 2)
		{
			Value = Value.Mid(1, Value.Len() - 2);
			// 还原转义的引号
			Value = Value.Replace(TEXT("\\\""), TEXT("\""));
		}
	}
	else
	{
		while (Index < Tokens.Num())
		{
			const FString& Token = Tokens[Index];
			if (Token == TEXT("node") || Token == TEXT("set") || Token == TEXT("link") || Token == TEXT("graph") ||
				Token == TEXT("graphset") || Token == TEXT("var"))
			{
				break;
			}
			Value += Token;
			Index++;
		}
	}

	OutSet.Value = Value.TrimStartAndEnd();
	return true;
}

bool FWeaveInterpreter::ParseLink(const TArray<FString>& Tokens, int32& Index, FWeaveLinkStmt& OutLink)
{
	const int32 StartIndex = Index;
	Index++;

	// 期望格式: link FromNode . FromPin -> ToNode . ToPin
	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: 缺少 FromNode（'link' 后没有更多 token）"), StartIndex);
		return false;
	}

	OutLink.FromNode = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: FromNode='%s' 后期望 '.' 但得到 '%s'"), Index, *OutLink.FromNode, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: FromNode='%s' 后缺少 FromPin"), Index, *OutLink.FromNode);
		return false;
	}
	OutLink.FromPin = Tokens[Index++];
	if (OutLink.FromPin.StartsWith(TEXT("\"")) && OutLink.FromPin.EndsWith(TEXT("\"")) && OutLink.FromPin.Len() >= 2)
	{
		OutLink.FromPin = OutLink.FromPin.Mid(1, OutLink.FromPin.Len() - 2);
	}

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("->"))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: '%s.%s' 后期望 '->' 但得到 '%s'"), Index, *OutLink.FromNode, *OutLink.FromPin, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: '%s.%s ->' 后缺少 ToNode"), Index, *OutLink.FromNode, *OutLink.FromPin);
		return false;
	}
	OutLink.ToNode = Tokens[Index++];

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		FString Got = Index < Tokens.Num() ? Tokens[Index] : TEXT("<EOF>");
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: ToNode='%s' 后期望 '.' 但得到 '%s'"), Index, *OutLink.ToNode, *Got);
		return false;
	}
	Index++;

	if (Index >= Tokens.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] ParseLink failed at token %d: ToNode='%s' 后缺少 ToPin"), Index, *OutLink.ToNode);
		return false;
	}
	OutLink.ToPin = Tokens[Index++];
	if (OutLink.ToPin.StartsWith(TEXT("\"")) && OutLink.ToPin.EndsWith(TEXT("\"")) && OutLink.ToPin.Len() >= 2)
	{
		OutLink.ToPin = OutLink.ToPin.Mid(1, OutLink.ToPin.Len() - 2);
	}

	return true;
}

bool FWeaveInterpreter::ParseVar(const TArray<FString>& Tokens, int32& Index, FWeaveVarDecl& OutVar)
{
	Index++;

	if (Index >= Tokens.Num())
	{
		return false;
	}

	FString VarName;
	while (Index < Tokens.Num() && Tokens[Index] != TEXT(":"))
	{
		if (!VarName.IsEmpty())
		{
			VarName += TEXT(" ");
		}
		VarName += Tokens[Index++];
	}
	OutVar.VarName = VarName.TrimStartAndEnd();

	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
	{
		return false;
	}
	Index++; // 跳过 ":"

	if (Index >= Tokens.Num())
	{
		return false;
	}

	FString TypeToken = Tokens[Index++];

	// 检查是否为容器类型前缀：array、set、map
	if (TypeToken == TEXT("array") || TypeToken == TEXT("set"))
	{
		OutVar.ContainerType = (TypeToken == TEXT("array"))
			? EPinContainerType::Array
			: EPinContainerType::Set;

		// 期望 ":" 和元素类型
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.VarType = Tokens[Index++];
	}
	else if (TypeToken == TEXT("map"))
	{
		OutVar.ContainerType = EPinContainerType::Map;

		// 期望 ":" KeyType ":" ValueType
		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.VarType = Tokens[Index++]; // Key 类型

		if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
		{
			return false;
		}
		Index++; // 跳过 ":"

		if (Index >= Tokens.Num())
		{
			return false;
		}
		OutVar.ValueType = Tokens[Index++]; // Value 类型
	}
	else
	{
		OutVar.ContainerType = EPinContainerType::None;
		OutVar.VarType = TypeToken;
	}

	return true;
}


int32 FWeaveInterpreter::GenerateBlueprint(const FWeaveAST& AST, UEdGraph* Graph, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return 0;
	}

	FScopedTransaction Transaction(NSLOCTEXT("WeaveLanguage", "GenerateBlueprint", "Weave Generate Blueprint"));
	Graph->Modify();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Generating blueprint for graph: %s"), *AST.GraphName);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] Vars: %d, Nodes: %d, Sets: %d, Links: %d"), AST.Vars.Num(), AST.Nodes.Num(),
	       AST.Sets.Num(), AST.Links.Num());


	FString FunctionEventNodeId;
	bool bIsFunctionGraph = (AST.GraphName == TEXT("UserConstructionScript") ||
		Graph->GetName().Contains(TEXT("UserConstructionScript")) ||
		!AST.GraphName.Contains(TEXT("EventGraph")));

	if (bIsFunctionGraph)
	{
		for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
		{
			if (NodeDecl.SchemaId == TEXT("event.Actor.UserConstructionScript"))
			{
				FunctionEventNodeId = NodeDecl.NodeId;
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] 检测到函数图表中的事件节点 %s，将自动转换为 entry 节点"), *NodeDecl.NodeId);
				break;
			}
		}
	}


	UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter());
	if (Blueprint && AST.Vars.Num() > 0)
	{
		for (const FWeaveVarDecl& VarDecl : AST.Vars)
		{
			bool bExists = false;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName.ToString() == VarDecl.VarName)
				{
					bExists = true;
					break;
				}
			}

			if (!bExists)
			{
				FEdGraphPinType PinType;
				bool bTypeResolved = false;


				if (VarDecl.VarType == TEXT("bool"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int64"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("float"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("double"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("string"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_String;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("text"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("name"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
					bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("byte"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					bTypeResolved = true;
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UEnum> It; It; ++It)
					{
						if (It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved)
				{
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (It->GetPrefixCPP() + It->GetName() == VarDecl.VarType)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
							PinType.PinSubCategoryObject = *It;
							bTypeResolved = true;
							break;
						}
					}
				}


				if (!bTypeResolved && VarDecl.VarType.StartsWith(TEXT("class:")))
				{
					FString ClassName = VarDecl.VarType.Mid(6);

					if (ClassName.StartsWith(TEXT("/")))
					{
						if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassName))
						{
							if (BP->GeneratedClass)
							{
								PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
								PinType.PinSubCategoryObject = BP->GeneratedClass;
								bTypeResolved = true;
							}
						}
					}
					else
					{
						for (TObjectIterator<UClass> It; It; ++It)
						{
							if (It->GetPrefixCPP() + It->GetName() == ClassName)
							{
								PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
								PinType.PinSubCategoryObject = *It;
								bTypeResolved = true;
								break;
							}
						}
					}
				}


				if (!bTypeResolved && VarDecl.VarType.StartsWith(TEXT("/")))
				{
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *VarDecl.VarType))
					{
						if (BP->GeneratedClass)
						{
							PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
							PinType.PinSubCategoryObject = BP->GeneratedClass;
							bTypeResolved = true;
						}
					}
				}

				if (!bTypeResolved)
				{
					UE_LOG(LogTemp, Warning,
					       TEXT("[Weaver] Unknown variable type: %s. Use SearchType to find valid names."),
					       *VarDecl.VarType);
					if (!OutError.IsEmpty()) OutError += TEXT("\n");
					OutError += FString::Printf(
						TEXT("var %s : %s 失败：未知类型 '%s'，请先调用 SearchType 查询正确的类型名称。"),
						*VarDecl.VarName, *VarDecl.VarType, *VarDecl.VarType);
					continue;
				}


				// 设置容器类型
				PinType.ContainerType = VarDecl.ContainerType;
				if (VarDecl.ContainerType == EPinContainerType::Map)
				{
					// 解析 Map 的 Value 类型到 PinValueType
					const FString& ValTypeStr = VarDecl.ValueType;
					if (ValTypeStr == TEXT("bool")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Boolean; }
					else if (ValTypeStr == TEXT("int")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int; }
					else if (ValTypeStr == TEXT("int64")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int64; }
					else if (ValTypeStr == TEXT("float")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Real; PinType.PinValueType.TerminalSubCategory = UEdGraphSchema_K2::PC_Float; }
					else if (ValTypeStr == TEXT("double")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Real; PinType.PinValueType.TerminalSubCategory = UEdGraphSchema_K2::PC_Double; }
					else if (ValTypeStr == TEXT("string")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_String; }
					else if (ValTypeStr == TEXT("text")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Text; }
					else if (ValTypeStr == TEXT("name")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Name; }
					else if (ValTypeStr == TEXT("byte")) { PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte; }
					else
					{
						// 尝试 struct / enum / class 解析
						bool bValResolved = false;
						for (TObjectIterator<UScriptStruct> It; It && !bValResolved; ++It)
						{
							if (It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Struct;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						for (TObjectIterator<UEnum> It; It && !bValResolved; ++It)
						{
							if (It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						for (TObjectIterator<UClass> It; It && !bValResolved; ++It)
						{
							if (It->GetPrefixCPP() + It->GetName() == ValTypeStr)
							{
								PinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Object;
								PinType.PinValueType.TerminalSubCategoryObject = *It;
								bValResolved = true;
							}
						}
						if (!bValResolved)
						{
							UE_LOG(LogTemp, Warning, TEXT("[Weaver] Unknown map value type: %s"), *ValTypeStr);
						}
					}
				}

				FName VarName = FName(*VarDecl.VarName);
				FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);

				UE_LOG(LogTemp, Log, TEXT("[Weaver] Created variable: %s (%s, container=%d)"), *VarDecl.VarName, *VarDecl.VarType, (int32)VarDecl.ContainerType);
			}
		}

		// 编译蓝图骨架，确保新变量的 FProperty 可用于后续节点的 AllocateDefaultPins
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}


	TMap<FString, UK2Node*> CreatedNodes;
	int32 NodesCreated = 0;


	for (const FWeaveNodeDecl& NodeDecl : AST.Nodes)
	{
		if (!FunctionEventNodeId.IsEmpty() && NodeDecl.NodeId == FunctionEventNodeId)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] 跳过事件节点 %s，使用 entry 代替"), *NodeDecl.NodeId);
			continue;
		}

		UK2Node* NewNode = nullptr;


		TArray<FString> Parts;
		NodeDecl.SchemaId.ParseIntoArray(Parts, TEXT("."));

		if (Parts.Num() < 2)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Invalid schema ID: %s"), *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT(
					"节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名 / special.类型），该节点未被创建。"),
				*NodeDecl.NodeId, *NodeDecl.SchemaId);
			continue;
		}

		FString NodeKind = Parts[0];

		if (NodeKind == TEXT("special"))
		{
			FString NodeType = Parts[1];
			if (NodeType == TEXT("Branch"))
			{
				NewNode = CreateBranchNode(Graph);
			}
			else if (NodeType == TEXT("Sequence"))
			{
				NewNode = CreateSequenceNode(Graph);
			}
			else if (NodeType == TEXT("MathExpression"))
			{
				NewNode = CreateMathExpressionNode(Graph);
			}
			else if (NodeType == TEXT("Make") && Parts.Num() >= 3)
			{
				FString StructTypeName = Parts[2];
				NewNode = CreateMakeStructNode(Graph, StructTypeName);
			}
			else if (NodeType == TEXT("Break") && Parts.Num() >= 3)
			{
				FString StructTypeName = Parts[2];
				NewNode = CreateBreakStructNode(Graph, StructTypeName);
			}
			else if (NodeType == TEXT("SpawnActorFromClass"))
			{
				NewNode = CreateSpawnActorFromClassNode(Graph);
			}
			else if (NodeType == TEXT("ConstructObjectFromClass"))
			{
				NewNode = CreateConstructObjectFromClassNode(Graph);
			}
			else if (NodeType == TEXT("Cast") && Parts.Num() >= 3)
			{
				// Parts[2:] 可能是完整路径（含 .），需要拼接回来
				FString TargetTypeName = Parts[2];
				for (int32 p = 3; p < Parts.Num(); ++p)
				{
					TargetTypeName += TEXT(".") + Parts[p];
				}
				NewNode = CreateDynamicCastNode(Graph, TargetTypeName);
			}
			else if (NodeType == TEXT("SwitchEnum") && Parts.Num() >= 3)
			{
				FString EnumName = Parts[2];
				NewNode = CreateSwitchEnumNode(Graph, EnumName);
			}
			else if (NodeType == TEXT("GetArrayItem"))
			{
				NewNode = CreateGetArrayItemNode(Graph);
			}
			else if (NodeType == TEXT("Knot"))
			{
				NewNode = CreateKnotNode(Graph);
			}
		}
		else if (Parts.Num() < 3)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Invalid schema ID: %s"), *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT("节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名），该节点未被创建。"),
				*NodeDecl.NodeId, *NodeDecl.SchemaId);
			continue;
		}
		else
		{
			FString ClassName = Parts[1];
			FString FunctionName = Parts[2];

			if (NodeKind == TEXT("event"))
			{
				NewNode = CreateEventNode(Graph, ClassName, FunctionName);
			}
			else if (NodeKind == TEXT("message"))
			{
				NewNode = CreateMessageNode(Graph, ClassName, FunctionName);
			}
			else if (NodeKind == TEXT("call"))
			{
				// 兼容旧脚本：如果脚本引用了 Target 引脚（典型的接口消息节点），优先还原为 UK2Node_Message。
				const bool bWantsTargetPin = AST.Links.ContainsByPredicate([&NodeDecl](const FWeaveLinkStmt& L)
				{
					return (L.ToNode == NodeDecl.NodeId && L.ToPin == TEXT("Target")) || (L.FromNode == NodeDecl.NodeId && L.FromPin == TEXT("Target"));
				}) || AST.Sets.ContainsByPredicate([&NodeDecl](const FWeaveSetStmt& S)
				{
					return (S.NodeId == NodeDecl.NodeId && S.PinName == TEXT("Target"));
				});
				if (bWantsTargetPin)
				{
					NewNode = CreateMessageNode(Graph, ClassName, FunctionName);
				}
				if (!NewNode)
				{
					NewNode = CreateCallNode(Graph, ClassName, FunctionName);
				}
			}
			else if (NodeKind == TEXT("macro"))
			{
				FString MacroPath;
				FString MacroName;

				if (ClassName == TEXT("StandardMacros"))
				{
					MacroPath = FString::Printf(TEXT("/Engine/EditorBlueprintResources/%s.%s"), *ClassName, *ClassName);
					MacroName = FunctionName;
				}
				else
				{
					int32 ColonIndex;
					if (FunctionName.FindChar(TEXT(':'), ColonIndex))
					{
						MacroPath = ClassName + TEXT(".") + FunctionName.Left(ColonIndex);
						MacroName = FunctionName.Mid(ColonIndex + 1);
					}
					else
					{
						MacroPath = ClassName;
						MacroName = FunctionName;
					}
				}

				NewNode = CreateMacroNode(Graph, MacroPath, MacroName);
			}
			else if (NodeKind == TEXT("VariableGet") || NodeKind == TEXT("VariableSet"))
			{
				// 对于外部类，路径可能包含多个 .（如 /Script/Module.ClassName.VarName）
				// 最后一段是变量名，前面的拼接回来是类名
				if (Parts.Num() > 3)
				{
					// 重新构建 ClassName：Parts[1] 到 Parts[Num-2] 拼接
					ClassName = Parts[1];
					for (int32 p = 2; p < Parts.Num() - 1; ++p)
					{
						ClassName += TEXT(".") + Parts[p];
					}
					FunctionName = Parts.Last();
				}

				// 判断是自身变量还是外部类成员变量
				bool bIsSelfVar = false;
				bool bIsExternalVar = false;
				UClass* ExternalClass = nullptr;

				// 检查是否为当前蓝图自身变量
				if (Blueprint)
				{
					FString BPClassName;
					if (Blueprint->GeneratedClass)
					{
						BPClassName = Blueprint->GeneratedClass->GetName();
					}

					if (ClassName == BPClassName || ClassName.IsEmpty())
					{
						// 属于当前蓝图
						UClass* GenClass = Blueprint->GeneratedClass;
						if (GenClass)
						{
							FProperty* Prop = GenClass->FindPropertyByName(FName(*FunctionName));
							bIsSelfVar = (Prop != nullptr);
						}
						if (!bIsSelfVar)
						{
							for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
							{
								if (ExistingVar.VarName.ToString() == FunctionName)
								{
									bIsSelfVar = true;
									break;
								}
							}
						}
						if (!bIsSelfVar)
						{
							for (const FWeaveVarDecl& VarDecl : AST.Vars)
							{
								if (VarDecl.VarName == FunctionName)
								{
									bIsSelfVar = true;
									break;
								}
							}
						}
					}
					else
					{
						// ClassName 不是当前蓝图类名
						// 先检查该变量是否在 AST.Vars 中声明（即由 Weave 代码创建的 Self 变量）
						// 这处理了跨蓝图粘贴时源蓝图类名与目标蓝图类名不同的情况
						bool bDeclaredInAST = false;
						for (const FWeaveVarDecl& VarDecl : AST.Vars)
						{
							if (VarDecl.VarName == FunctionName)
							{
								bDeclaredInAST = true;
								break;
							}
						}

						if (bDeclaredInAST)
						{
							// 变量在 Weave 代码中声明，已由 AddMemberVariable 创建，视为 Self 变量
							bIsSelfVar = true;
							UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' declared in AST, treating as self var (source class: %s)"), *FunctionName, *ClassName);
						}
						else
						{
							// 再检查 Blueprint->NewVariables（可能目标蓝图已有同名变量）
							bool bFoundInNewVars = false;
							for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
							{
								if (ExistingVar.VarName.ToString() == FunctionName)
								{
									bFoundInNewVars = true;
									break;
								}
							}

							if (bFoundInNewVars)
							{
								bIsSelfVar = true;
								UE_LOG(LogTemp, Log, TEXT("[Weaver] Variable '%s' found in Blueprint->NewVariables, treating as self var"), *FunctionName);
							}
							else
							{
								// 尝试作为外部类成员变量
								ExternalClass = FindClassByShortName(ClassName);
								if (ExternalClass)
								{
									bIsExternalVar = true;
								}
								else
								{
									UE_LOG(LogTemp, Warning, TEXT("[Weaver] External class not found: %s, trying as self var"), *ClassName);
									// 最后回退：检查 GeneratedClass 的 FProperty
									if (Blueprint->GeneratedClass)
									{
										FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(FName(*FunctionName));
										bIsSelfVar = (Prop != nullptr);
									}
								}
							}
						}
					}
				}

				if (!bIsSelfVar && !bIsExternalVar)
				{
					OutError = FString::Printf(
						TEXT("变量 '%s' 不存在：%s.%s.%s 引用了未知变量。")
						TEXT("蓝图中的用户变量和组件变量均未找到同名属性，")
						TEXT("本次 Weave 也未声明 'var %s : <类型>'。")
						TEXT("请先用 SearchContextVar 确认变量名称，或添加 var 声明。"),
						*FunctionName, *NodeKind, *ClassName, *FunctionName, *FunctionName);
					return -1;
				}

				if (NodeKind == TEXT("VariableGet"))
				{
					if (bIsExternalVar && ExternalClass)
					{
						NewNode = CreateVariableGetNodeExternal(Graph, ExternalClass, FunctionName);
					}
					else
					{
						NewNode = CreateVariableGetNode(Graph, Blueprint, FunctionName);
					}
				}
				else
				{
					if (bIsExternalVar && ExternalClass)
					{
						NewNode = CreateVariableSetNodeExternal(Graph, ExternalClass, FunctionName);
					}
					else
					{
						NewNode = CreateVariableSetNode(Graph, Blueprint, FunctionName);
					}
				}
			}
		}

		if (NewNode)
		{
			NewNode->NodePosX = NodeDecl.Position.X;
			NewNode->NodePosY = NodeDecl.Position.Y;

			CreatedNodes.Add(NodeDecl.NodeId, NewNode);
			NodesCreated++;

			UE_LOG(LogTemp, Log, TEXT("[Weaver] Created node: %s (%s)"), *NodeDecl.NodeId, *NodeDecl.SchemaId);
		}
	}

	if (NodesCreated == 0)
	{
		OutError = TEXT("No nodes created");
		return 0;
	}

	// 修补 VariableGet/VariableSet 的引脚类型：
	// AllocateDefaultPins 可能在蓝图未编译时找不到 FProperty，导致引脚类型缺失容器信息。
	// 直接从 Blueprint->NewVariables 读取完整 PinType 覆盖到引脚上。
	if (Blueprint)
	{
		for (auto& KV : CreatedNodes)
		{
			UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(KV.Value);
			UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(KV.Value);
			if (!VarGet && !VarSet) continue;

			FName VarName = VarGet ? VarGet->GetVarName() : VarSet->GetVarName();
			for (const FBPVariableDescription& Desc : Blueprint->NewVariables)
			{
				if (Desc.VarName == VarName && Desc.VarType.ContainerType != EPinContainerType::None)
				{
					// 修补输出引脚（Get）或输入引脚（Set）
					UEdGraphPin* ValuePin = KV.Value->FindPin(VarName, VarGet ? EGPD_Output : EGPD_Input);
					if (ValuePin)
					{
						ValuePin->PinType = Desc.VarType;
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Patched pin type for variable '%s' (container=%d)"),
							*VarName.ToString(), (int32)Desc.VarType.ContainerType);
					}
					break;
				}
			}
		}
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());

	for (const FWeaveSetStmt& Set : AST.Sets)
	{
		UK2Node** NodePtr = CreatedNodes.Find(Set.NodeId);
		if (!NodePtr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: node not found (%s)"), *Set.NodeId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(
				TEXT("set %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"), *Set.NodeId, *Set.PinName, *Set.NodeId);
			continue;
		}

		UK2Node* Node = *NodePtr;


		if (Set.PinName == TEXT("Expression"))
		{
			if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Node))
			{
				FString FinalValue = Set.Value;
				if (FinalValue.StartsWith(TEXT("\"")) && FinalValue.EndsWith(TEXT("\"")) && !FinalValue.Contains(
					TEXT("\\\"")))
				{
					FinalValue = FinalValue.Mid(1, FinalValue.Len() - 2);
				}
				MathNode->Expression = FinalValue;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.Expression = %s"), *Set.NodeId, *FinalValue);
				continue;
			}
		}


		if (Set.PinName == TEXT("Class"))
		{
			FString ClassPath = Set.Value;
			if (ClassPath.StartsWith(TEXT("\"")) && ClassPath.EndsWith(TEXT("\"")) && !ClassPath.Contains(TEXT("\\\"")))
			{
				ClassPath = ClassPath.Mid(1, ClassPath.Len() - 2);
			}

			if (ClassPath.StartsWith(TEXT("class:")))
			{
				ClassPath.RemoveFromStart(TEXT("class:"));
			}
			if (UK2Node_SpawnActorFromClass* SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node))
			{
				UClass* ActorClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None,
				                                     nullptr);
				if (!ActorClass)
				{
					FString AssetPath = ClassPath;
					if (AssetPath.EndsWith(TEXT("_C")))
						AssetPath.RemoveFromEnd(TEXT("_C"));
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
						ActorClass = BP->GeneratedClass;
				}
				if (ActorClass)
				{
					UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
					if (ClassPin)
					{
						ClassPin->DefaultObject = ActorClass;
						ClassPin->DefaultValue = ActorClass->GetPathName();
						SpawnNode->PinDefaultValueChanged(ClassPin);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] SpawnActorFromClass: Class set to %s"), *ClassPath);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] SpawnActorFromClass: Class not found: %s"), *ClassPath);
				}
				continue;
			}
			if (UK2Node_ConstructObjectFromClass* ConstructNode = Cast<UK2Node_ConstructObjectFromClass>(Node))
			{
				UClass* ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None,
				                                   nullptr);
				if (!ObjClass)
				{
					FString AssetPath2 = ClassPath;
					if (AssetPath2.EndsWith(TEXT("_C")))
						AssetPath2.RemoveFromEnd(TEXT("_C"));
					if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath2))
						ObjClass = BP->GeneratedClass;
				}
				if (ObjClass)
				{
					UEdGraphPin* ClassPin = ConstructNode->GetClassPin();
					if (ClassPin)
					{
						ClassPin->DefaultObject = ObjClass;
						ClassPin->DefaultValue = ObjClass->GetPathName();
						ConstructNode->PinDefaultValueChanged(ClassPin);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] ConstructObjectFromClass: Class set to %s"), *ClassPath);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] ConstructObjectFromClass: Class not found: %s"),
					       *ClassPath);
				}
				continue;
			}
		}


		UEdGraphPin* Pin = Node->FindPin(*Set.PinName, EGPD_Input);

		// 如果找不到引脚，尝试递归展开结构体引脚
		if (!Pin && Set.PinName.Contains(TEXT("_")) && Schema)
		{
			TArray<FString> Ancestors;
			FString PinName = Set.PinName;
			int32 LastUnderscore;
			while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
			{
				PinName = PinName.Left(LastUnderscore);
				Ancestors.Insert(PinName, 0);
			}

			for (const FString& AncestorName : Ancestors)
			{
				UEdGraphPin* AncestorPin = Node->FindPin(*AncestorName, EGPD_Input);
				if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
					AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					Schema->SplitPin(AncestorPin, false);
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin for set: %s.%s"), *Set.NodeId, *AncestorName);
				}
			}
			Pin = Node->FindPin(*Set.PinName, EGPD_Input);
		}

		if (!Pin)
		{
			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				UFunction* Func = CallNode->GetTargetFunction();
				if (Func)
				{
					for (TFieldIterator<FProperty> It(Func); It; ++It)
					{
						if ((*It)->GetName() == Set.PinName)
						{
							Pin = Node->FindPin(TEXT("self"), EGPD_Input);
							break;
						}
					}
				}
			}
		}

		if (!Pin)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: pin not found (%s.%s)"), *Set.NodeId, *Set.PinName);
			continue;
		}


		FString FinalValue = Set.Value;
		if (FinalValue.StartsWith(TEXT("\"")) && FinalValue.EndsWith(TEXT("\"")) && !FinalValue.Contains(TEXT("\\\"")))
		{
			FinalValue = FinalValue.Mid(1, FinalValue.Len() - 2);
		}


		{
			TArray<FString> ValParts;
			FinalValue.ParseIntoArray(ValParts, TEXT("."));
			if (ValParts.Num() == 2
				&& !FinalValue.Contains(TEXT(" "))
				&& !FinalValue.StartsWith(TEXT("/"))
				&& CreatedNodes.Contains(ValParts[0]))
			{
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += FString::Printf(
					TEXT("set %s.%s = %s 错误：'%s' 是节点引脚引用，不是一个值。")
					TEXT("应改为 link 语句：link %s -> %s.%s"),
					*Set.NodeId, *Set.PinName, *FinalValue,
					*FinalValue, *FinalValue, *Set.NodeId, *Set.PinName);
				continue;
			}
		}


		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
		{
			FString ClassValue = FinalValue;
			if (ClassValue.StartsWith(TEXT("class:")))
				ClassValue.RemoveFromStart(TEXT("class:"));
			UClass* PinClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassValue, nullptr, LOAD_None,
			                                   nullptr);
			if (!PinClass)
			{
				FString AssetPath = ClassValue;
				if (AssetPath.EndsWith(TEXT("_C")))
					AssetPath.RemoveFromEnd(TEXT("_C"));
				if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
					PinClass = BP->GeneratedClass;
			}
			if (PinClass)
			{
				Pin->DefaultObject = PinClass;
				Pin->DefaultValue = PinClass->GetPathName();
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set class pin: %s.%s = %s"), *Set.NodeId, *Set.PinName,
				       *PinClass->GetName());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: class not found %s.%s = %s"), *Set.NodeId,
				       *Set.PinName, *FinalValue);
			}
		}
		else
		{
			const bool bIsObjectPin = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject);
			if (bIsObjectPin)
			{
				FString Lower = FinalValue.ToLower();


				if (Lower == TEXT("nullptr") || Lower == TEXT("none") || Lower.IsEmpty()
					|| (Lower == TEXT("self") && !Pin->bHidden))
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.%s = (skipped for object pin, value='%s')"),
					       *Set.NodeId, *Set.PinName, *FinalValue);
					continue;
				}
			}


			FString UEValue = FinalValue;
			if (UEValue.StartsWith(TEXT("vec(")) && UEValue.EndsWith(TEXT(")")))
			{
				FString Inner = UEValue.Mid(4, UEValue.Len() - 5);
				TArray<FString> Parts;
				Inner.ParseIntoArray(Parts, TEXT(","), true);
				if (Parts.Num() == 3)
				{
					const float X = FCString::Atof(*Parts[0].TrimStartAndEnd());
					const float Y = FCString::Atof(*Parts[1].TrimStartAndEnd());
					const float Z = FCString::Atof(*Parts[2].TrimStartAndEnd());
					UEValue = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
				}
			}

			else if (UEValue.StartsWith(TEXT("rot(")) && UEValue.EndsWith(TEXT(")")))
			{
				FString Inner = UEValue.Mid(4, UEValue.Len() - 5);
				TArray<FString> Parts;
				Inner.ParseIntoArray(Parts, TEXT(","), true);
				if (Parts.Num() == 3)
				{
					const float R = FCString::Atof(*Parts[0].TrimStartAndEnd());
					const float P = FCString::Atof(*Parts[1].TrimStartAndEnd());
					const float Y = FCString::Atof(*Parts[2].TrimStartAndEnd());
					UEValue = FString::Printf(TEXT("(Pitch=%f,Yaw=%f,Roll=%f)"), P, Y, R);
				}
			}
			Pin->DefaultValue = UEValue;
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.%s = %s"), *Set.NodeId, *Set.PinName, *UEValue);
		}
	}


	FSequenceDynamicPinHandler SequenceHandler;
	SequenceHandler.PreScanLinks(AST.Links, CreatedNodes);
	SequenceHandler.AddDynamicPins(CreatedNodes);

	FSwitchEnumDynamicPinHandler SwitchEnumHandler;
	SwitchEnumHandler.PreScanLinks(AST.Links, CreatedNodes);
	SwitchEnumHandler.AddDynamicPins(CreatedNodes);


	if (Schema)
	{
		for (const FWeaveLinkStmt& Link : AST.Links)
		{
			UK2Node* FromNode = nullptr;
			UK2Node* ToNode = nullptr;


			FString FromNodeId = Link.FromNode;
			FString ToNodeId = Link.ToNode;

			if (!FunctionEventNodeId.IsEmpty())
			{
				if (FromNodeId == FunctionEventNodeId)
				{
					FromNodeId = TEXT("entry");
					UE_LOG(LogTemp, Log, TEXT("[Weaver] 连接时将 %s 替换为 entry"), *Link.FromNode);
				}
				if (ToNodeId == FunctionEventNodeId)
				{
					ToNodeId = TEXT("entry");
					UE_LOG(LogTemp, Log, TEXT("[Weaver] 连接时将 %s 替换为 entry"), *Link.ToNode);
				}
			}


			if (FromNodeId == TEXT("entry"))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						FromNode = EntryNode;
						break;
					}
				}
			}
			else
			{
				UK2Node** FromNodePtr = CreatedNodes.Find(FromNodeId);
				if (FromNodePtr) FromNode = *FromNodePtr;
			}

			if (ToNodeId == TEXT("entry"))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						ToNode = EntryNode;
						break;
					}
				}
			}
			else
			{
				UK2Node** ToNodePtr = CreatedNodes.Find(ToNodeId);
				if (ToNodePtr) ToNode = *ToNodePtr;
			}

			if (!FromNode || !ToNode)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: node not found (%s or %s)"), *FromNodeId,
				       *ToNodeId);
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += FString::Printf(TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"),
				                            *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
				                            !FromNode ? *FromNodeId : *ToNodeId);
				continue;
			}


			UEdGraphPin* FromPin = FromNode->FindPin(*Link.FromPin, EGPD_Output);

			// ReturnValue 兜底：仅在引脚名不是 ReturnValue 子引脚时使用
			if (!FromPin && Link.FromPin != TEXT("ReturnValue") && !Link.FromPin.StartsWith(TEXT("ReturnValue_")))
				FromPin = FromNode->FindPin(TEXT("ReturnValue"), EGPD_Output);

			// 如果找不到输出引脚，尝试递归展开结构体引脚（Split Struct Pin）
			// 例如 ReturnValue_Frame_Value 需要先展开 ReturnValue，再展开 ReturnValue_Frame
			if (!FromPin && Link.FromPin.Contains(TEXT("_")))
			{
				// 收集从根到目标的所有可能的父引脚层级
				TArray<FString> Ancestors;
				FString PinName = Link.FromPin;
				int32 LastUnderscore;
				while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
				{
					PinName = PinName.Left(LastUnderscore);
					Ancestors.Insert(PinName, 0); // 从根到叶排列
				}

				// 从最顶层的祖先开始，逐层展开
				for (const FString& AncestorName : Ancestors)
				{
					UEdGraphPin* AncestorPin = FromNode->FindPin(*AncestorName, EGPD_Output);
					if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
						AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						Schema->SplitPin(AncestorPin, false);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin: %s.%s"), *Link.FromNode, *AncestorName);
					}
				}
				FromPin = FromNode->FindPin(*Link.FromPin, EGPD_Output);
			}

			UEdGraphPin* ToPin = ToNode->FindPin(*Link.ToPin, EGPD_Input);

			// 如果找不到输入引脚，尝试递归展开结构体引脚
			if (!ToPin && Link.ToPin.Contains(TEXT("_")))
			{
				TArray<FString> Ancestors;
				FString PinName = Link.ToPin;
				int32 LastUnderscore;
				while (PinName.FindLastChar(TEXT('_'), LastUnderscore))
				{
					PinName = PinName.Left(LastUnderscore);
					Ancestors.Insert(PinName, 0);
				}

				for (const FString& AncestorName : Ancestors)
				{
					UEdGraphPin* AncestorPin = ToNode->FindPin(*AncestorName, EGPD_Input);
					if (AncestorPin && AncestorPin->SubPins.Num() == 0 &&
						AncestorPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						Schema->SplitPin(AncestorPin, false);
						UE_LOG(LogTemp, Log, TEXT("[Weaver] Auto-split struct pin: %s.%s"), *Link.ToNode, *AncestorName);
					}
				}
				ToPin = ToNode->FindPin(*Link.ToPin, EGPD_Input);
			}

			if (!ToPin)
			{
				if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(ToNode))
				{
					UFunction* Func = CallNode->GetTargetFunction();
					if (Func)
					{
						for (TFieldIterator<FProperty> It(Func); It; ++It)
						{
							if ((*It)->GetName() == Link.ToPin)
							{
								UEdGraphPin* SelfPin = ToNode->FindPin(TEXT("self"), EGPD_Input);
								if (SelfPin)
								{
									ToPin = SelfPin;
									UE_LOG(LogTemp, Log, TEXT("[Weaver] Remapped pin '%s' -> 'self' for node '%s'"),
									       *Link.ToPin, *Link.ToNode);
								}
								break;
							}
						}
					}
				}
			}

			if (!FromPin || !ToPin)
			{
				// 收集引脚详细信息的 lambda（名称 + 类型 + SubCategory）
				auto CollectPinDetails = [](UK2Node* Node, EEdGraphPinDirection Dir) -> FString
				{
					TArray<FString> Details;
					for (UEdGraphPin* P : Node->Pins)
					{
						if (P->Direction == Dir)
						{
							FString SubObj = P->PinType.PinSubCategoryObject.IsValid()
								? P->PinType.PinSubCategoryObject->GetName()
								: TEXT("null");
							FString Detail = FString::Printf(TEXT("%s(%s|%s|%s)"),
								*P->PinName.ToString(),
								*P->PinType.PinCategory.ToString(),
								*P->PinType.PinSubCategory.ToString(),
								*SubObj);
							if (P->bHidden) Detail += TEXT("[hidden]");
							if (P->ParentPin) Detail += FString::Printf(TEXT("[parent:%s]"), *P->ParentPin->PinName.ToString());
							Details.Add(Detail);
						}
					}
					return Details.IsEmpty() ? TEXT("(无)") : FString::Join(Details, TEXT(", "));
				};

				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: pin not found (%s.%s or %s.%s)"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);

				if (!FromPin)
				{
					FString AllOutputs = CollectPinDetails(FromNode, EGPD_Output);
					UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromNode '%s' (%s) 全部输出引脚: %s"),
						*Link.FromNode, *FromNode->GetClass()->GetName(), *AllOutputs);
				}
				if (!ToPin)
				{
					FString AllInputs = CollectPinDetails(ToNode, EGPD_Input);
					UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToNode '%s' (%s) 全部输入引脚: %s"),
						*Link.ToNode, *ToNode->GetClass()->GetName(), *AllInputs);
				}

				auto CollectPinNames = [](UK2Node* Node, EEdGraphPinDirection Dir) -> FString
				{
					TArray<FString> Names;
					for (UEdGraphPin* P : Node->Pins)
					{
						if (P->Direction == Dir && !P->bHidden)
							Names.Add(P->PinName.ToString());
					}
					return Names.IsEmpty() ? TEXT("(无)") : FString::Join(Names, TEXT(", "));
				};

				FString LinkError;
				if (!FromPin)
				{
					LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 没有名为 '%s' 的输出引脚。")
						TEXT("该节点实际输出引脚：[%s]"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*Link.FromNode, *Link.FromPin,
						*CollectPinNames(FromNode, EGPD_Output));
				}
				else
				{
					LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 没有名为 '%s' 的输入引脚。")
						TEXT("该节点实际输入引脚：[%s]。")
						TEXT("提示：调用成员函数（如 DestroyComponent、SetActorLocation 等）时，")
						TEXT("组件/对象参数对应的引脚名为 'self'，不是 'Object' 或 'Target'。"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*Link.ToNode, *Link.ToPin,
						*CollectPinNames(ToNode, EGPD_Input));
				}
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}


			FromPin->Modify();
			ToPin->Modify();


			// 输出引脚完整类型信息的 lambda
			auto PinTypeToDebugStr = [](const UEdGraphPin* Pin) -> FString
			{
				FString SubObj = Pin->PinType.PinSubCategoryObject.IsValid()
					? Pin->PinType.PinSubCategoryObject->GetName()
					: TEXT("null");
				FString ContainerStr;
				switch (Pin->PinType.ContainerType)
				{
				case EPinContainerType::Array: ContainerStr = TEXT("Array"); break;
				case EPinContainerType::Set:   ContainerStr = TEXT("Set");   break;
				case EPinContainerType::Map:   ContainerStr = TEXT("Map");   break;
				default:                       ContainerStr = TEXT("None");  break;
				}
				return FString::Printf(TEXT("Cat=%s, Sub=%s, SubObj=%s, Container=%s, Ref=%s"),
					*Pin->PinType.PinCategory.ToString(),
					*Pin->PinType.PinSubCategory.ToString(),
					*SubObj,
					*ContainerStr,
					Pin->PinType.bIsReference ? TEXT("true") : TEXT("false"));
			};

			const FPinConnectionResponse ConnectResponse = Schema->CanCreateConnection(FromPin, ToPin);
			if (ConnectResponse.Response == CONNECT_RESPONSE_DISALLOW)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] CanCreateConnection DISALLOW: %s.%s -> %s.%s"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin type: {%s}"), *PinTypeToDebugStr(FromPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToPin   type: {%s}"), *PinTypeToDebugStr(ToPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   Reason: %s"), *ConnectResponse.Message.ToString());

				// 通配符引脚本质上兼容任何类型，Schema 可能因缺少具体类型信息而拒绝连接
				// 此时用 MakeLinkTo 强制建立连接
				if (FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
					ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					FromPin->MakeLinkTo(ToPin);
					UE_LOG(LogTemp, Log, TEXT("[Weaver]   -> Wildcard fallback: forced MakeLinkTo"));
				}
				else
				{
					FString LinkError = FString::Printf(
						TEXT("link %s.%s -> %s.%s 无法建立：FromPin={%s}, ToPin={%s}, 原因: %s"),
						*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
						*PinTypeToDebugStr(FromPin), *PinTypeToDebugStr(ToPin),
						*ConnectResponse.Message.ToString());
					if (!OutError.IsEmpty()) OutError += TEXT("\n");
					OutError += LinkError;
					continue;
				}
			}

			bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);

			// TryCreateConnection 对通配符引脚可能失败
			if (!bConnected &&
				(FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
				 ToPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard))
			{
				FromPin->MakeLinkTo(ToPin);
				bConnected = true;
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Wildcard fallback MakeLinkTo: %s.%s -> %s.%s"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
			}

			if (!bConnected)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] TryCreateConnection FAILED: %s.%s -> %s.%s"),
				       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin: '%s' {%s}"),
					*FromPin->PinName.ToString(), *PinTypeToDebugStr(FromPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   ToPin:   '%s' {%s}"),
					*ToPin->PinName.ToString(), *PinTypeToDebugStr(ToPin));
				UE_LOG(LogTemp, Warning, TEXT("[Weaver]   FromPin linked=%d, ToPin linked=%d"),
					FromPin->LinkedTo.Num(), ToPin->LinkedTo.Num());

				FString LinkError = FString::Printf(
					TEXT("link %s.%s -> %s.%s 连接失败: FromPin={%s}, ToPin={%s}"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
					*PinTypeToDebugStr(FromPin), *PinTypeToDebugStr(ToPin));
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}

			UE_LOG(LogTemp, Log, TEXT("[Weaver] Linked: %s.%s -> %s.%s"),
			       *Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
		}
	}

	// 所有连线完成后，将通配符引脚类型设置为已连接的具体类型。
	// 遍历图中所有创建的节点（包括已有连线对面的节点引脚也会间接获益）。
	// 多轮迭代处理通配符链。
	{
		bool bChanged = true;
		int32 MaxIterations = 5;
		while (bChanged && MaxIterations-- > 0)
		{
			bChanged = false;
			for (auto& KV : CreatedNodes)
			{
				UK2Node* Node = KV.Value;
				if (!Node) continue;

				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard) continue;
					if (Pin->LinkedTo.Num() == 0) continue;

					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
						{
							Pin->PinType = LinkedPin->PinType;
							bChanged = true;
							break;
						}
					}
				}
			}
		}
	}

	// 类型传播完成后，统一通知所有节点连线变更
	for (auto& KV : CreatedNodes)
	{
		UK2Node* Node = KV.Value;
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				Node->PinConnectionListChanged(Pin);
			}
		}
	}

	for (auto& KV : CreatedNodes)
	{
		UK2Node* Node = KV.Value;
		if (!Node) continue;


		int32 ExecOutTotal = 0;
		int32 ExecOutUnlinked = 0;
		TArray<FString> UnlinkedPinNames;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->bHidden) continue;

			ExecOutTotal++;
			if (Pin->LinkedTo.Num() == 0)
			{
				ExecOutUnlinked++;
				UnlinkedPinNames.Add(Pin->PinName.ToString());
			}
		}


		if (ExecOutTotal >= 2 && ExecOutUnlinked > 0)
		{
			FString PinList = FString::Join(UnlinkedPinNames, TEXT(", "));
			FString Warning = FString::Printf(
				TEXT("警告：节点 '%s'（%s）的执行分支引脚 [%s] 没有连接后继节点，可能遗漏了对应的 link 语句。请检查一下，但如果后面确实不需要链接，则不要告知用户!!!!。"),
				*KV.Key, *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
				*PinList);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += Warning;
		}
	}


	Graph->NotifyGraphChanged();

	UE_LOG(LogTemp, Log, TEXT("[Weaver] Created %d nodes successfully"), NodesCreated);
	return NodesCreated;
}

UK2Node* FWeaveInterpreter::CreateEventNode(UEdGraph* Graph, const FString& ClassName, const FString& EventName)
{
	if (!Graph)
	{
		return nullptr;
	}
	if (EventName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateEventNode: empty EventName (ClassName=%s)"), *ClassName);
		return nullptr;
	}

	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (UK2Node_Event* ExistingEvent = Cast<UK2Node_Event>(ExistingNode))
		{
			if (ExistingEvent->EventReference.GetMemberName().ToString() == EventName ||
				ExistingEvent->GetFunctionName().ToString() == EventName)
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Reusing existing event node: %s"), *EventName);
				return ExistingEvent;
			}
		}
	}

	UBlueprint* Blueprint = Graph->GetTypedOuter<UBlueprint>();

	FString FullClassName = ClassName;
	if (ClassName == TEXT("Actor"))
	{
		FullClassName = TEXT("/Script/Engine.Actor");
	}

	UClass* EventClass = FindClassByShortName(FullClassName);
	if (!EventClass && Blueprint)
	{
		if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetName() == ClassName)
		{
			EventClass = Blueprint->GeneratedClass;
		}
		else if (Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->GetName() == ClassName)
		{
			EventClass = Blueprint->SkeletonGeneratedClass;
		}
	}

	const FName EventFName(*EventName);
	bool bCreateCustomEvent = (EventClass == nullptr);
	if (!bCreateCustomEvent && Cast<UBlueprintGeneratedClass>(EventClass))
	{
		UClass* SuperClass = EventClass->GetSuperClass();
		UFunction* SuperFunc = SuperClass ? SuperClass->FindFunctionByName(EventFName) : nullptr;
		if (!SuperFunc)
		{
			bCreateCustomEvent = true;
		}
	}

	if (bCreateCustomEvent)
	{
		UK2Node_CustomEvent* CustomEventNode = SpawnEditorNode<UK2Node_CustomEvent>(Graph);
		if (CustomEventNode)
		{
			CustomEventNode->CustomFunctionName = EventFName;
			CustomEventNode->AllocateDefaultPins();
			CustomEventNode->ReconstructNode();
		}
		return CustomEventNode;
	}

	UK2Node_Event* EventNode = SpawnEditorNode<UK2Node_Event>(Graph);
	if (EventNode)
	{
		EventNode->EventReference.SetExternalMember(EventFName, EventClass);
		EventNode->bOverrideFunction = true;
		EventNode->AllocateDefaultPins();
		EventNode->ReconstructNode();
	}
	return EventNode;
}
UK2Node* FWeaveInterpreter::CreateCallNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName)
{
	UK2Node_CallFunction* CallNode = SpawnEditorNode<UK2Node_CallFunction>(Graph);
	if (CallNode)
	{


		static const TMap<FString, FString> FuncNameTranslation = {
			{TEXT("Conv_FloatToString"), TEXT("Conv_DoubleToString")},
			{TEXT("Conv_FloatToInt"), TEXT("Conv_DoubleToInt")},
			{TEXT("Conv_FloatToBool"), TEXT("Conv_DoubleToBool")},
			{TEXT("Conv_FloatToVector"), TEXT("Conv_DoubleToVector")},
			{TEXT("Conv_FloatToVector2D"), TEXT("Conv_DoubleToVector2D")},
			{TEXT("Conv_IntToFloat"), TEXT("Conv_IntToDouble")},
			{TEXT("Conv_ByteToFloat"), TEXT("Conv_ByteToDouble")},
			{TEXT("Conv_BoolToFloat"), TEXT("Conv_BoolToDouble")},
			{TEXT("FMin"), TEXT("Min")},
			{TEXT("FMax"), TEXT("Max")},
			{TEXT("FClamp"), TEXT("Clamp_Float")},
		};
		FString ResolvedFunctionName = FunctionName;
		if (const FString* Translated = FuncNameTranslation.Find(FunctionName))
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Translating function name: %s -> %s"), *FunctionName, **Translated);
			ResolvedFunctionName = *Translated;
		}


		FString FullClassName = ClassName;
		if (ClassName == TEXT("KismetSystemLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetSystemLibrary");
		}
		else if (ClassName == TEXT("KismetMathLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetMathLibrary");
		}
		else if (ClassName == TEXT("KismetStringLibrary"))
		{
			FullClassName = TEXT("/Script/Engine.KismetStringLibrary");
		}
		else if (ClassName == TEXT("KismetArrayLibrary")) 		{ 			FullClassName = TEXT("/Script/Engine.KismetArrayLibrary"); 		}
		else if (ClassName == TEXT("GameplayStatics"))
		{
			FullClassName = TEXT("/Script/Engine.GameplayStatics");
		}


		UClass* TargetClass = FindClassByShortName(FullClassName);
		if (TargetClass)
		{
			UFunction* Function = TargetClass->FindFunctionByName(*ResolvedFunctionName);
			if (Function)
			{
				CallNode->SetFromFunction(Function);
				CallNode->AllocateDefaultPins();
				CallNode->ReconstructNode();
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Function not found: %s::%s"), *FullClassName,
				       *ResolvedFunctionName);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class not found: %s"), *FullClassName);
		}
	}
	return CallNode;
}


UK2Node* FWeaveInterpreter::CreateMessageNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName)
{
	UK2Node_Message* MessageNode = SpawnEditorNode<UK2Node_Message>(Graph);
	if (!MessageNode)
	{
		return nullptr;
	}

	static const TMap<FString, FString> FuncNameTranslation = {
		{TEXT("Conv_FloatToString"), TEXT("Conv_DoubleToString")},
		{TEXT("Conv_FloatToInt"), TEXT("Conv_DoubleToInt")},
		{TEXT("Conv_FloatToBool"), TEXT("Conv_DoubleToBool")},
		{TEXT("Conv_FloatToVector"), TEXT("Conv_DoubleToVector")},
		{TEXT("Conv_FloatToVector2D"), TEXT("Conv_DoubleToVector2D")},
		{TEXT("Conv_IntToFloat"), TEXT("Conv_IntToDouble")},
		{TEXT("Conv_ByteToFloat"), TEXT("Conv_ByteToDouble")},
		{TEXT("Conv_BoolToFloat"), TEXT("Conv_BoolToDouble")},
		{TEXT("FMin"), TEXT("Min")},
		{TEXT("FMax"), TEXT("Max")},
		{TEXT("FClamp"), TEXT("Clamp_Float")},
	};

	FString ResolvedFunctionName = FunctionName;
	if (const FString* Translated = FuncNameTranslation.Find(FunctionName))
	{
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Translating function name: %s -> %s"), *FunctionName, **Translated);
		ResolvedFunctionName = *Translated;
	}

	FString FullClassName = ClassName;
	if (ClassName == TEXT("KismetSystemLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetSystemLibrary");
	}
	else if (ClassName == TEXT("KismetMathLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetMathLibrary");
	}
	else if (ClassName == TEXT("KismetStringLibrary"))
	{
		FullClassName = TEXT("/Script/Engine.KismetStringLibrary");
	}
	else if (ClassName == TEXT("GameplayStatics"))
	{
		FullClassName = TEXT("/Script/Engine.GameplayStatics");
	}

	UClass* TargetClass = FindClassByShortName(FullClassName);
	if (!TargetClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class not found: %s"), *FullClassName);
		return nullptr;
	}

	if (!TargetClass->HasAnyClassFlags(CLASS_Interface))
	{
		// 不是接口类就不创建 Message 节点（避免错误还原）。
		return nullptr;
	}

	UFunction* Function = TargetClass->FindFunctionByName(*ResolvedFunctionName);
	if (!Function)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] Function not found: %s::%s"), *FullClassName, *ResolvedFunctionName);
		return nullptr;
	}

	MessageNode->SetFromFunction(Function);
	MessageNode->AllocateDefaultPins();
	MessageNode->ReconstructNode();
	return MessageNode;
}

UK2Node* FWeaveInterpreter::CreateMacroNode(UEdGraph* Graph, const FString& MacroPath, const FString& MacroName)
{
	UK2Node_MacroInstance* MacroNode = SpawnEditorNode<UK2Node_MacroInstance>(Graph);
	if (MacroNode)
	{

		UE_LOG(LogTemp, Log, TEXT("[Weaver] Loading macro: Path=%s, Name=%s"), *MacroPath, *MacroName);


		UBlueprint* MacroBlueprint = LoadObject<UBlueprint>(nullptr, *MacroPath);
		if (MacroBlueprint)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Macro blueprint loaded, searching for macro graph..."));


			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* TempGraph : MacroBlueprint->MacroGraphs)
			{
				if (TempGraph)
				{
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Found macro graph: %s"), *TempGraph->GetName());
					if (TempGraph->GetName() == MacroName)
					{
						MacroGraph = TempGraph;
						break;
					}
				}
			}

			if (MacroGraph)
			{
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Macro graph found, setting up node..."));

				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->AllocateDefaultPins();
				MacroNode->ReconstructNode();
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Macro graph '%s' not found in blueprint"), *MacroName);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Failed to load macro blueprint: %s"), *MacroPath);
		}
	}
	return MacroNode;
}

UK2Node* FWeaveInterpreter::CreateBranchNode(UEdGraph* Graph)
{
	UK2Node_IfThenElse* BranchNode = SpawnEditorNode<UK2Node_IfThenElse>(Graph);
	if (BranchNode)
	{
		BranchNode->AllocateDefaultPins();
		BranchNode->ReconstructNode();
	}
	return BranchNode;
}

UK2Node* FWeaveInterpreter::CreateSequenceNode(UEdGraph* Graph)
{
	UK2Node_ExecutionSequence* SequenceNode = SpawnEditorNode<UK2Node_ExecutionSequence>(Graph);
	if (SequenceNode)
	{
		SequenceNode->AllocateDefaultPins();
		SequenceNode->ReconstructNode();
	}
	return SequenceNode;
}

UK2Node* FWeaveInterpreter::CreateMathExpressionNode(UEdGraph* Graph)
{
	UK2Node_MathExpression* MathNode = SpawnEditorNode<UK2Node_MathExpression>(Graph);
	if (MathNode)
	{
		MathNode->AllocateDefaultPins();
	}
	return MathNode;
}

UK2Node* FWeaveInterpreter::CreateMakeStructNode(UEdGraph* Graph, const FString& StructTypeName)
{
	UScriptStruct* StructType = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetStructCPPName() == StructTypeName)
		{
			StructType = *It;
			break;
		}
	}

	if (StructType)
	{
		UK2Node_MakeStruct* MakeNode = SpawnEditorNode<UK2Node_MakeStruct>(Graph);
		if (MakeNode)
		{
			MakeNode->StructType = StructType;
			MakeNode->AllocateDefaultPins();


			bool bHasInputPins = false;
			for (UEdGraphPin* Pin : MakeNode->Pins)
			{
				if (Pin->Direction == EGPD_Input)
				{
					bHasInputPins = true;
					break;
				}
			}
			if (bHasInputPins)
				return MakeNode;

			MakeNode->DestroyNode();
		}
	}


	FString TypeShortName = StructTypeName;
	if (TypeShortName.StartsWith(TEXT("F")))
		TypeShortName = TypeShortName.RightChop(1);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] MakeStruct fallback to KismetMathLibrary.Make%s"), *TypeShortName);
	return CreateCallNode(Graph, TEXT("KismetMathLibrary"), FString::Printf(TEXT("Make%s"), *TypeShortName));
}

UK2Node* FWeaveInterpreter::CreateBreakStructNode(UEdGraph* Graph, const FString& StructTypeName)
{
	UScriptStruct* StructType = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetStructCPPName() == StructTypeName)
		{
			StructType = *It;
			break;
		}
	}

	if (StructType)
	{
		UK2Node_BreakStruct* BreakNode = SpawnEditorNode<UK2Node_BreakStruct>(Graph);
		if (BreakNode)
		{
			BreakNode->StructType = StructType;
			BreakNode->AllocateDefaultPins();


			bool bHasOutputPins = false;
			for (UEdGraphPin* Pin : BreakNode->Pins)
			{
				if (Pin->Direction == EGPD_Output)
				{
					bHasOutputPins = true;
					break;
				}
			}
			if (bHasOutputPins)
				return BreakNode;

			BreakNode->DestroyNode();
		}
	}


	FString TypeShortName = StructTypeName;
	if (TypeShortName.StartsWith(TEXT("F")))
		TypeShortName = TypeShortName.RightChop(1);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] BreakStruct fallback to KismetMathLibrary.Break%s"), *TypeShortName);
	return CreateCallNode(Graph, TEXT("KismetMathLibrary"), FString::Printf(TEXT("Break%s"), *TypeShortName));
}

UK2Node* FWeaveInterpreter::CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName)
{
	UK2Node_VariableGet* VarGetNode = SpawnEditorNode<UK2Node_VariableGet>(Graph);
	if (VarGetNode)
	{
		FName VarFName = FName(*VarName);
		VarGetNode->VariableReference.SetSelfMember(VarFName);
		VarGetNode->AllocateDefaultPins();
	}
	return VarGetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableGetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName)
{
	UK2Node_VariableGet* VarGetNode = SpawnEditorNode<UK2Node_VariableGet>(Graph);
	if (VarGetNode)
	{
		FName VarFName = FName(*VarName);
		VarGetNode->VariableReference.SetExternalMember(VarFName, OwnerClass);
		VarGetNode->AllocateDefaultPins();
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created external VariableGet: %s.%s"), *OwnerClass->GetName(), *VarName);
	}
	return VarGetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableSetNodeExternal(UEdGraph* Graph, UClass* OwnerClass, const FString& VarName)
{
	UK2Node_VariableSet* VarSetNode = SpawnEditorNode<UK2Node_VariableSet>(Graph);
	if (VarSetNode)
	{
		FName VarFName = FName(*VarName);
		VarSetNode->VariableReference.SetExternalMember(VarFName, OwnerClass);
		VarSetNode->AllocateDefaultPins();
		UE_LOG(LogTemp, Log, TEXT("[Weaver] Created external VariableSet: %s.%s"), *OwnerClass->GetName(), *VarName);
	}
	return VarSetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName)
{
	UK2Node_VariableSet* VarSetNode = SpawnEditorNode<UK2Node_VariableSet>(Graph);
	if (VarSetNode)
	{
		FName VarFName = FName(*VarName);
		VarSetNode->VariableReference.SetSelfMember(VarFName);
		VarSetNode->AllocateDefaultPins();
	}
	return VarSetNode;
}

UK2Node* FWeaveInterpreter::CreateSpawnActorFromClassNode(UEdGraph* Graph)
{
	UK2Node_SpawnActorFromClass* SpawnNode = SpawnEditorNode<UK2Node_SpawnActorFromClass>(Graph);
	if (SpawnNode)
	{
		SpawnNode->AllocateDefaultPins();
	}
	return SpawnNode;
}

UK2Node* FWeaveInterpreter::CreateConstructObjectFromClassNode(UEdGraph* Graph)
{
	UK2Node_ConstructObjectFromClass* ConstructNode = SpawnEditorNode<UK2Node_ConstructObjectFromClass>(Graph);
	if (ConstructNode)
	{
		ConstructNode->AllocateDefaultPins();
	}
	return ConstructNode;
}

UK2Node* FWeaveInterpreter::CreateDynamicCastNode(UEdGraph* Graph, const FString& TargetTypeName)
{
	UK2Node_DynamicCast* CastNode = SpawnEditorNode<UK2Node_DynamicCast>(Graph);
	if (CastNode)
	{


		UClass* TargetClass = FindClassByShortName(TargetTypeName);

		if (TargetClass)
		{
			CastNode->TargetType = TargetClass;
			UE_LOG(LogTemp, Log, TEXT("[Weaver] CreateDynamicCastNode: Found type %s (Class=%s)"), *TargetTypeName, *TargetClass->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateDynamicCastNode: Type not found: %s"), *TargetTypeName);
		}

		CastNode->AllocateDefaultPins();

		// 诊断：列出所有引脚
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			UE_LOG(LogTemp, Log, TEXT("[Weaver] Cast pin: Name=%s Dir=%d Category=%s SubObj=%s"),
				*Pin->PinName.ToString(),
				(int32)Pin->Direction,
				*Pin->PinType.PinCategory.ToString(),
				Pin->PinType.PinSubCategoryObject.IsValid() ? *Pin->PinType.PinSubCategoryObject->GetName() : TEXT("null"));
		}

		// AllocateDefaultPins 在某些情况下会创建 wildcard 引脚而非 PC_Object，
		// 手动修正 Object 引脚和 As 输出引脚类型
		for (UEdGraphPin* Pin : CastNode->Pins)
		{
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				// 修正 Object 输入引脚
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
					Pin->PinType.PinSubCategoryObject = UObject::StaticClass();
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Fixed Cast input pin: %s -> PC_Object"), *Pin->PinName.ToString());
				}
				// 修正 As 输出引脚
				else if (Pin->Direction == EGPD_Output && TargetClass)
				{
					if (TargetClass->IsChildOf(UInterface::StaticClass()))
					{
						Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
					}
					else
					{
						Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
					}
					Pin->PinType.PinSubCategoryObject = TargetClass;
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Fixed Cast output pin: %s -> PC_Object/%s"), *Pin->PinName.ToString(), *TargetClass->GetName());
				}
			}
		}
	}
	return CastNode;
}

UK2Node* FWeaveInterpreter::CreateSwitchEnumNode(UEdGraph* Graph, const FString& EnumName)
{
	UEnum* TargetEnum = nullptr;
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		UEnum* Candidate = *It;
		if (!Candidate->HasAnyFlags(RF_Public))
			continue;


		FString FullName = Candidate->GetName();
		FString ShortName = FullName;
		int32 ColonIdx;
		if (FullName.FindLastChar(TEXT(':'), ColonIdx))
			ShortName = FullName.RightChop(ColonIdx + 1);

		if (ShortName == EnumName || FullName == EnumName)
		{
			TargetEnum = Candidate;
			break;
		}
	}

	if (!TargetEnum)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateSwitchEnumNode: Enum not found: %s"), *EnumName);
		return nullptr;
	}

	UK2Node_SwitchEnum* SwitchNode = SpawnEditorNode<UK2Node_SwitchEnum>(Graph);
	if (SwitchNode)
	{

		SwitchNode->Enum = TargetEnum;
		SwitchNode->EnumEntries.Empty();
		SwitchNode->EnumFriendlyNames.Empty();

		if (TargetEnum)
		{
			if (IsInGameThread() || TargetEnum->IsPostLoadThreadSafe())
			{
				TargetEnum->ConditionalPostLoad();
			}

			for (int32 EnumIndex = 0; EnumIndex < TargetEnum->NumEnums() - 1; ++EnumIndex)
			{
				const bool bShouldBeHidden = TargetEnum->HasMetaData(TEXT("Hidden"), EnumIndex) || TargetEnum->HasMetaData(TEXT("Spacer"), EnumIndex);
				if (bShouldBeHidden)
				{
					continue;
				}

				const FString EnumValueName = TargetEnum->GetNameStringByIndex(EnumIndex);
				SwitchNode->EnumEntries.Add(FName(*EnumValueName));
				SwitchNode->EnumFriendlyNames.Add(TargetEnum->GetDisplayNameTextByIndex(EnumIndex));
			}
		}

		SwitchNode->ReconstructNode();
	}
	return SwitchNode;
}

UK2Node* FWeaveInterpreter::CreateGetArrayItemNode(UEdGraph* Graph)
{
	UK2Node_GetArrayItem* ArrayGetNode = SpawnEditorNode<UK2Node_GetArrayItem>(Graph);
	if (ArrayGetNode)
	{
		ArrayGetNode->AllocateDefaultPins();
	}
	return ArrayGetNode;
}

UK2Node* FWeaveInterpreter::CreateKnotNode(UEdGraph* Graph)
{
	UK2Node_Knot* KnotNode = SpawnEditorNode<UK2Node_Knot>(Graph);
	if (KnotNode)
	{
		KnotNode->AllocateDefaultPins();
	}
	return KnotNode;
}





