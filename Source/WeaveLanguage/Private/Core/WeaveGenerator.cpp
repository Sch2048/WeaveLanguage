#include "Core/WeaveGenerator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Message.h"
#include "K2Node_MathExpression.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"

namespace
{
	FString StripUEClassPrefix(const FString& ClassName)
	{
		FString Result = ClassName;
		// 去掉 SKEL_ 前缀（蓝图骨架类）
		if (Result.StartsWith(TEXT("SKEL_")))
		{
			Result = Result.Mid(5);
		}
		if (Result.Len() > 1)
		{
			const TCHAR First = Result[0];
			const TCHAR Second = Result[1];
			if ((First == TEXT('U') || First == TEXT('A')) && FChar::IsUpper(Second))
			{
				return Result.RightChop(1);
			}
		}
		return Result;
	}
}

bool FWeaveGenerator::Generate(const TArray<UEdGraphNode*>& SelectedNodes, UEdGraph* Graph, FString& OutWeaveCode)
{
	if (!Graph || SelectedNodes.Num() == 0)
	{
		return false;
	}


	TSet<UEdGraphNode*> AllNodes;
	for (UEdGraphNode* Node : SelectedNodes)
	{
		CollectDependencies(Node, AllNodes);
	}


	FString Code;


	if (UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter()))
	{
		FString BPPath = Blueprint->GetPathName();
		Code += FString::Printf(TEXT("graphset %s %s\n"), *Blueprint->GetName(), *BPPath);
	}


	Code += FString::Printf(TEXT("graph %s\n\n"), *Graph->GetName());


	auto ClassToWeaveName = [](UClass* C) -> FString
	{
		if (const UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(C))
		{
			if (const UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy))
				return BP->GetPathName();
		}
		return C->GetPrefixCPP() + C->GetName();
	};

	// 基础类型名解析（不含容器前缀）
	auto PinTypeToWeaveNameBase = [&ClassToWeaveName](const FEdGraphPinType& PinType) -> FString
	{
		FString Cat = PinType.PinCategory.ToString();
		if (Cat == TEXT("bool")) return TEXT("bool");
		if (Cat == TEXT("int")) return TEXT("int");
		if (Cat == TEXT("int64")) return TEXT("int64");
		if (Cat == TEXT("real"))
		{
			return (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
				       ? TEXT("double")
				       : TEXT("float");
		}
		if (Cat == TEXT("string")) return TEXT("string");
		if (Cat == TEXT("text")) return TEXT("text");
		if (Cat == TEXT("name")) return TEXT("name");
		if (Cat == TEXT("struct") && PinType.PinSubCategoryObject.IsValid())
		{
			if (const UScriptStruct* S = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
				return S->GetName();
		}
		if (Cat == TEXT("byte") && PinType.PinSubCategoryObject.IsValid())
		{
			if (const UEnum* E = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
				return E->GetName();
		}
		if (Cat == TEXT("object") && PinType.PinSubCategoryObject.IsValid())
		{
			if (UClass* C = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
				return ClassToWeaveName(C);
		}
		if (Cat == TEXT("class") && PinType.PinSubCategoryObject.IsValid())
		{
			if (UClass* C = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
				return TEXT("class:") + ClassToWeaveName(C);
		}
		return Cat;
	};

	// 完整类型名解析（含容器前缀 array:/set:/map:）
	auto PinTypeToWeaveName = [&PinTypeToWeaveNameBase](const UEdGraphPin* Pin) -> FString
	{
		const FEdGraphPinType& PinType = Pin->PinType;
		FString ElementType = PinTypeToWeaveNameBase(PinType);

		if (PinType.ContainerType == EPinContainerType::Array)
		{
			return TEXT("array:") + ElementType;
		}
		if (PinType.ContainerType == EPinContainerType::Set)
		{
			return TEXT("set:") + ElementType;
		}
		if (PinType.ContainerType == EPinContainerType::Map)
		{
			FEdGraphPinType ValType;
			ValType.PinCategory = PinType.PinValueType.TerminalCategory;
			ValType.PinSubCategory = PinType.PinValueType.TerminalSubCategory;
			ValType.PinSubCategoryObject = PinType.PinValueType.TerminalSubCategoryObject;
			FString ValueTypeName = PinTypeToWeaveNameBase(ValType);
			return TEXT("map:") + ElementType + TEXT(":") + ValueTypeName;
		}
		return ElementType;
	};

	TMap<FString, FString> Variables;
	for (UEdGraphNode* Node : AllNodes)
	{
		if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
		{
			// 只为 Self 变量生成 var 声明，外部类变量不需要
			if (!VarGetNode->VariableReference.IsSelfContext()) continue;
			FName VarName = VarGetNode->GetVarName();
			if (UEdGraphPin* ValuePin = VarGetNode->FindPin(VarName, EGPD_Output); ValuePin && !Variables.Contains(
				VarName.ToString()))
				Variables.Add(VarName.ToString(), PinTypeToWeaveName(ValuePin));
		}
		else if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
		{
			if (!VarSetNode->VariableReference.IsSelfContext()) continue;
			FName VarName = VarSetNode->GetVarName();
			if (UEdGraphPin* ValuePin = VarSetNode->FindPin(VarName, EGPD_Input); ValuePin && !Variables.Contains(
				VarName.ToString()))
				Variables.Add(VarName.ToString(), PinTypeToWeaveName(ValuePin));
		}
	}


	if (Variables.Num() > 0)
	{
		for (const auto& Var : Variables)
		{
			Code += FString::Printf(TEXT("var %s : %s\n"), *Var.Key, *Var.Value);
		}
		Code += TEXT("\n");
	}


	TMap<UEdGraphNode*, FString> NodeIdMap;
	int32 NodeCounter = 0;

	for (UEdGraphNode* Node : AllNodes)
	{
		// 注释节点单独处理，跳过普通 node 输出
		if (Node->IsA<UEdGraphNode_Comment>())
		{
			continue;
		}

		FString NodeId;
		int32 Index = NodeCounter++;
		int32 RepeatCount = (Index / 26) + 1;
		TCHAR Letter = 'a' + (Index % 26);

		for (int32 i = 0; i < RepeatCount; i++)
		{
			NodeId.AppendChar(Letter);
		}

		NodeIdMap.Add(Node, NodeId);


		if (Node->IsA<UK2Node_FunctionEntry>())
		{
			NodeIdMap[Node] = TEXT("entry");
			continue;
		}

		FString SchemaId = GetNodeSchemaId(Node);


		if (SchemaId.Contains(TEXT(" ")))
		{
			SchemaId = FString::Printf(TEXT("\"%s\""), *SchemaId);
		}


		int32 PosX = Node->NodePosX;
		int32 PosY = Node->NodePosY;

		Code += FString::Printf(TEXT("node %s : %s @ (%d, %d)\n"), *NodeId, *SchemaId, PosX, PosY);
	}

	Code += TEXT("\n");


	TSet<FString> GeneratedLinks;
	for (UEdGraphNode* Node : AllNodes)
	{
		if (Node->IsA<UEdGraphNode_Comment>()) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
					if (AllNodes.Contains(LinkedNode))
					{
						FString FromNodeId = NodeIdMap[Node];
						FString ToNodeId = NodeIdMap[LinkedNode];

						FString FromPinName = Pin->PinName.ToString();
						FString ToPinName = LinkedPin->PinName.ToString();


						if (FromPinName.Contains(TEXT(" ")))
						{
							FromPinName = FString::Printf(TEXT("\"%s\""), *FromPinName);
						}
						if (ToPinName.Contains(TEXT(" ")))
						{
							ToPinName = FString::Printf(TEXT("\"%s\""), *ToPinName);
						}

						FString LinkCode = FString::Printf(TEXT("link %s.%s -> %s.%s"),
						                                   *FromNodeId, *FromPinName,
						                                   *ToNodeId, *ToPinName);

						if (!GeneratedLinks.Contains(LinkCode))
						{
							Code += LinkCode + TEXT("\n");
							GeneratedLinks.Add(LinkCode);
						}
					}
				}
			}
		}
	}

	Code += TEXT("\n");


	for (UEdGraphNode* Node : AllNodes)
	{
		if (Node->IsA<UEdGraphNode_Comment>()) continue;
		FString NodeId = NodeIdMap[Node];


		if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Node))
		{
			if (!MathNode->Expression.IsEmpty())
			{
				FString EscapedExpr = MathNode->Expression.Replace(TEXT("\""), TEXT("\\\""));
				Code += FString::Printf(TEXT("set %s.Expression = \"%s\"\n"), *NodeId, *EscapedExpr);
			}
		}


		TSet<FName> EmittedPins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 0)
			{
				if (Pin->ParentPin != nullptr) continue;
				FString PinName = Pin->PinName.ToString();
				if (PinName == TEXT("self") || PinName == TEXT("execute"))
				{
					continue;
				}

				if (EmittedPins.Contains(Pin->PinName)) continue;

				FString DefaultValue;


				if (!Pin->DefaultValue.IsEmpty())
				{
					DefaultValue = Pin->DefaultValue;
				}
				else if (Pin->DefaultObject != nullptr)
				{
					DefaultValue = Pin->DefaultObject->GetPathName();
				}
				else if (!Pin->DefaultTextValue.IsEmpty())
				{
					DefaultValue = Pin->DefaultTextValue.ToString();
				}


				if (!DefaultValue.IsEmpty() &&
					DefaultValue != TEXT("0") &&
					DefaultValue != TEXT("0.0") &&
					DefaultValue != TEXT("0.000000") &&
					DefaultValue != TEXT("false") &&
					DefaultValue != TEXT("None"))
				{
						if (PinName.Contains(TEXT(" "))) 						{ 							PinName = FString::Printf(TEXT("\"%s\""), *PinName); 						}

					// DefaultValue 中如果包含空格、关键字或特殊字符，需要用引号包裹
					// 避免 Tokenizer 将其拆分导致 ParseSet 提前终止
					FString SafeValue = DefaultValue;
					static const TArray<FString> Keywords = {
						TEXT("node"), TEXT("set"), TEXT("link"), TEXT("graph"), TEXT("graphset"), TEXT("var"), TEXT("comment")
					};
					bool bNeedsQuote = DefaultValue.Contains(TEXT(" ")) || DefaultValue.Contains(TEXT("\t"))
						|| DefaultValue.Contains(TEXT(".")) || DefaultValue.Contains(TEXT("="))
						|| DefaultValue.Contains(TEXT("(")) || DefaultValue.Contains(TEXT(")"));
					if (!bNeedsQuote)
					{
						for (const FString& Kw : Keywords)
						{
							if (DefaultValue == Kw)
							{
								bNeedsQuote = true;
								break;
							}
						}
					}
					if (bNeedsQuote && !DefaultValue.StartsWith(TEXT("\"")))
					{
						SafeValue = FString::Printf(TEXT("\"%s\""), *DefaultValue.Replace(TEXT("\""), TEXT("\\\"")));
					}

					Code += FString::Printf(TEXT("set %s.%s = %s\n"), *NodeId, *PinName, *SafeValue);
					EmittedPins.Add(Pin->PinName);
				}
			}
		}
	}

	// 输出注释节点
	for (UEdGraphNode* Node : AllNodes)
	{
		if (const UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
		{
			FString CommentText = CommentNode->NodeComment;
			// 转义引号和换行
			CommentText = CommentText.Replace(TEXT("\\"), TEXT("\\\\"));
			CommentText = CommentText.Replace(TEXT("\""), TEXT("\\\""));
			CommentText = CommentText.Replace(TEXT("\n"), TEXT("\\n"));
			CommentText = CommentText.Replace(TEXT("\r"), TEXT(""));

			int32 PosX = CommentNode->NodePosX;
			int32 PosY = CommentNode->NodePosY;
			int32 SizeX = CommentNode->NodeWidth;
			int32 SizeY = CommentNode->NodeHeight;

			// comment "文本" @ (X, Y) size (W, H) color (R, G, B, A) fontsize N
			// 颜色用 0-255 整数避免浮点数中的 . 被 Tokenizer 拆分
			FLinearColor Color = CommentNode->CommentColor;
			int32 R = FMath::RoundToInt(FMath::Clamp(Color.R, 0.f, 1.f) * 255);
			int32 G = FMath::RoundToInt(FMath::Clamp(Color.G, 0.f, 1.f) * 255);
			int32 B = FMath::RoundToInt(FMath::Clamp(Color.B, 0.f, 1.f) * 255);
			int32 A = FMath::RoundToInt(FMath::Clamp(Color.A, 0.f, 1.f) * 255);
			Code += FString::Printf(TEXT("comment \"%s\" @ (%d, %d) size (%d, %d) color (%d, %d, %d, %d) fontsize %d\n"),
				*CommentText, PosX, PosY, SizeX, SizeY,
				R, G, B, A,
				CommentNode->FontSize);
		}
	}

	OutWeaveCode = Code;
	return true;
}

void FWeaveGenerator::CollectDependencies(UEdGraphNode* Node, TSet<UEdGraphNode*>& OutNodes)
{
	if (!Node || OutNodes.Contains(Node))
	{
		return;
	}

	OutNodes.Add(Node);


	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == EGPD_Input)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				CollectDependencies(LinkedPin->GetOwningNode(), OutNodes);
			}
		}
	}
}

FString FWeaveGenerator::GetNodeSchemaId(UEdGraphNode* Node)
{
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		UClass* OwnerClass = EventNode->EventReference.GetMemberParentClass();
		if (!OwnerClass)
		{
			if (const UBlueprint* BP = Node->GetTypedOuter<UBlueprint>())
			{
				OwnerClass = BP->GeneratedClass ? BP->GeneratedClass : BP->SkeletonGeneratedClass;
			}
		}
		FString ClassName = OwnerClass ? StripUEClassPrefix(OwnerClass->GetName()) : TEXT("Unknown");

		FString EventName = EventNode->EventReference.GetMemberName().ToString();
		if (EventName.IsEmpty() || EventName == TEXT("None"))
		{
			EventName = EventNode->GetFunctionName().ToString();
		}

		return FString::Printf(TEXT("event.%s.%s"), *ClassName, *EventName);
	}
	else if (const UK2Node_Message* MessageNode = Cast<UK2Node_Message>(Node))
	{
		if (const UFunction* Function = MessageNode->GetTargetFunction())
		{
			const UClass* OwnerClass = Function->GetOwnerClass();
			FString ClassName = OwnerClass ? StripUEClassPrefix(OwnerClass->GetName()) : TEXT("Unknown");

			return FString::Printf(TEXT("message.%s.%s"), *ClassName, *Function->GetName());
		}
	}
	else if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		if (const UFunction* Function = CallNode->GetTargetFunction())
		{
			const UClass* OwnerClass = Function->GetOwnerClass();
			FString ClassName = OwnerClass ? StripUEClassPrefix(OwnerClass->GetName()) : TEXT("Unknown");

			return FString::Printf(TEXT("call.%s.%s"), *ClassName, *Function->GetName());
		}
	}
	else if (const UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
	{
		const FName VarName = VarGetNode->GetVarName();
		const UClass* OwnerClass = VarGetNode->VariableReference.GetMemberParentClass();
		bool bIsSelfMember = VarGetNode->VariableReference.IsSelfContext();

		if (!OwnerClass)
		{
			if (const UBlueprint* BP = Cast<UBlueprint>(Node->GetGraph()->GetOuter()))
			{
				OwnerClass = BP->GeneratedClass;
			}
		}

		if (OwnerClass)
		{
			// 外部类使用完整路径以确保 Interpreter 能可靠加载
			FString ClassName;
			if (bIsSelfMember)
			{
				ClassName = OwnerClass->GetName();
			}
			else
			{
				ClassName = OwnerClass->GetPathName();
			}
			return FString::Printf(TEXT("VariableGet.%s.%s"),
			                       *ClassName, *VarName.ToString());
		}
		return FString::Printf(TEXT("VariableGet.%s"), *VarName.ToString());
	}
	else if (const UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
	{
		const FName VarName = VarSetNode->GetVarName();
		const UClass* OwnerClass = VarSetNode->VariableReference.GetMemberParentClass();
		bool bIsSelfMember = VarSetNode->VariableReference.IsSelfContext();

		if (!OwnerClass)
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Node->GetGraph()->GetOuter()))
			{
				OwnerClass = BP->GeneratedClass;
			}
		}

		if (OwnerClass)
		{
			FString ClassName;
			if (bIsSelfMember)
			{
				ClassName = OwnerClass->GetName();
			}
			else
			{
				ClassName = OwnerClass->GetPathName();
			}
			return FString::Printf(TEXT("VariableSet.%s.%s"),
			                       *ClassName, *VarName.ToString());
		}
		return FString::Printf(TEXT("VariableSet.%s"), *VarName.ToString());
	}


	FString ClassName = Node->GetClass()->GetName();

	if (ClassName == TEXT("K2Node_ExecutionSequence"))
	{
		return TEXT("special.Sequence");
	}
	else if (ClassName == TEXT("K2Node_IfThenElse"))
	{
		return TEXT("special.Branch");
	}
	else if (ClassName == TEXT("K2Node_MathExpression"))
	{
		return TEXT("special.MathExpression");
	}
	else if (ClassName == TEXT("K2Node_MakeStruct"))
	{
		if (const UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
		{
			if (MakeNode->StructType)
			{
				const FString StructName = MakeNode->StructType->GetStructCPPName();
				return FString::Printf(TEXT("special.Make.%s"), *StructName);
			}
		}
		return TEXT("special.Make");
	}
	else if (ClassName == TEXT("K2Node_BreakStruct"))
	{
		if (const UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
		{
			if (BreakNode->StructType)
			{
				const FString StructName = BreakNode->StructType->GetStructCPPName();
				return FString::Printf(TEXT("special.Break.%s"), *StructName);
			}
		}
		return TEXT("special.Break");
	}

	else if (ClassName == TEXT("K2Node_SpawnActorFromClass"))
	{
		return TEXT("special.SpawnActorFromClass");
	}
	else if (ClassName == TEXT("K2Node_ConstructObjectFromClass"))
	{
		return TEXT("special.ConstructObjectFromClass");
	}
	else if (ClassName == TEXT("K2Node_DynamicCast"))
	{
		if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			if (CastNode->TargetType)
			{
				// 使用完整路径名以确保 Interpreter 能可靠加载类
				FString TypePath = CastNode->TargetType->GetPathName();
				return FString::Printf(TEXT("special.Cast.%s"), *TypePath);
			}
		}
		return TEXT("special.Cast");
	}
	else if (ClassName == TEXT("K2Node_MacroInstance"))
	{
		if (const UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
		{
			if (const UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
			{
				const FString MacroName = MacroGraph->GetName();

				if (const UBlueprint* MacroBlueprint = Cast<UBlueprint>(MacroGraph->GetOuter()))
				{
					if (const FString BlueprintPath = MacroBlueprint->GetPathName(); BlueprintPath.Contains(
						TEXT("/Engine/EditorBlueprintResources/StandardMacros")))
					{
						return FString::Printf(TEXT("macro.StandardMacros.%s"), *MacroName);
					}
					else
					{
						return FString::Printf(TEXT("macro.%s:%s"), *BlueprintPath, *MacroName);
					}
				}
			}
		}
	}

	else if (ClassName == TEXT("K2Node_GetArrayItem"))
	{
		return TEXT("special.GetArrayItem");
	}
	else if (ClassName == TEXT("K2Node_Knot"))
	{
		return TEXT("special.Knot");
	}

	return ClassName;
}


