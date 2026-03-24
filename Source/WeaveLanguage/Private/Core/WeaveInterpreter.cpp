

#include "Core/WeaveInterpreter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
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
				OutError = FString::Printf(TEXT("Failed to parse link at token %d"), Index);
				return false;
			}
			if (Link.FromNode == Link.ToNode)
			{
				OutError = FString::Printf(
					TEXT("自连死循环：节点 '%s' 的输出引脚 '%s' 连接到自身的输入引脚 '%s'。执行引脚自连会让蓝图在该节点永远循环，永远无法继续执行后续逻辑。请改为连接到其他节点，或删除此 link 语句。"),
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
		else if (Ch == TEXT(':') || Ch == TEXT('=') || Ch == TEXT('.') || Ch == TEXT('(') || Ch == TEXT(')') || Ch == TEXT(',') || Ch == TEXT('@'))
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
	while (Index < Tokens.Num() && Tokens[Index] != TEXT("@") && Tokens[Index] != TEXT("node") && Tokens[Index] != TEXT("set") && Tokens[Index] != TEXT("link"))
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
	
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("="))
	{
		return false;
	}
	Index++;
	
	FString Value;
	
	if (Index < Tokens.Num() && Tokens[Index].StartsWith(TEXT("\"")))
	{
		Value = Tokens[Index++];
	}
	else
	{
		while (Index < Tokens.Num())
		{
			const FString& Token = Tokens[Index];
			if (Token == TEXT("node") || Token == TEXT("set") || Token == TEXT("link") || Token == TEXT("graph") || Token == TEXT("graphset") || Token == TEXT("var"))
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
	Index++;
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	
	OutLink.FromNode = Tokens[Index++];
	
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		return false;
	}
	Index++;
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	OutLink.FromPin = Tokens[Index++];
	
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("->"))
	{
		return false;
	}
	Index++;
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	OutLink.ToNode = Tokens[Index++];
	
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT("."))
	{
		return false;
	}
	Index++;
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	OutLink.ToPin = Tokens[Index++];
	
	return true;
}

bool FWeaveInterpreter::ParseVar(const TArray<FString>& Tokens, int32& Index, FWeaveVarDecl& OutVar)
{
	Index++; 
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	
	OutVar.VarName = Tokens[Index++];
	
	if (Index >= Tokens.Num() || Tokens[Index] != TEXT(":"))
	{
		return false;
	}
	Index++; 
	
	if (Index >= Tokens.Num())
	{
		return false;
	}
	
	OutVar.VarType = Tokens[Index++];
	
	return true;
}


int32 FWeaveInterpreter::GenerateBlueprint(const FWeaveAST& AST, UEdGraph* Graph, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("Invalid graph");
		return 0;
	}
	
	UE_LOG(LogTemp, Log, TEXT("[Weaver] Generating blueprint for graph: %s"), *AST.GraphName);
	UE_LOG(LogTemp, Log, TEXT("[Weaver] Vars: %d, Nodes: %d, Sets: %d, Links: %d"), AST.Vars.Num(), AST.Nodes.Num(), AST.Sets.Num(), AST.Links.Num());
	
	
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
					PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int; bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("int64"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int64; bTypeResolved = true;
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
					PinType.PinCategory = UEdGraphSchema_K2::PC_String; bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("text"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Text; bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("name"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Name; bTypeResolved = true;
				}
				else if (VarDecl.VarType == TEXT("byte"))
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte; bTypeResolved = true;
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
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] Unknown variable type: %s. Use SearchType to find valid names."), *VarDecl.VarType);
					if (!OutError.IsEmpty()) OutError += TEXT("\n");
					OutError += FString::Printf(
						TEXT("var %s : %s 失败：未知类型 '%s'，请先调用 SearchType 查询正确的类型名称。"),
						*VarDecl.VarName, *VarDecl.VarType, *VarDecl.VarType);
					continue;
				}
				
				 
				 
				FName VarName = FName(*VarDecl.VarName);
				FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);
				
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Created variable: %s (%s)"), *VarDecl.VarName, *VarDecl.VarType);
			}
		}
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
			OutError += FString::Printf(TEXT("节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名 / special.类型），该节点未被创建。"), *NodeDecl.NodeId, *NodeDecl.SchemaId);
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
				FString TargetTypeName = Parts[2];
				NewNode = CreateDynamicCastNode(Graph, TargetTypeName);
			}
			else if (NodeType == TEXT("SwitchEnum") && Parts.Num() >= 3)
			{
				FString EnumName = Parts[2];
				NewNode = CreateSwitchEnumNode(Graph, EnumName);
			}
		}
		else if (Parts.Num() < 3)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Invalid schema ID: %s"), *NodeDecl.SchemaId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(TEXT("节点 '%s' 的 schema_id '%s' 格式无效（应为 call.类名.函数名 / event.类名.事件名 / macro.宏库名.宏名），该节点未被创建。"), *NodeDecl.NodeId, *NodeDecl.SchemaId);
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
			else if (NodeKind == TEXT("call"))
			{
				 
				NewNode = CreateCallNode(Graph, ClassName, FunctionName);
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
				 
				 
				 
				bool bVarExists = false;
				if (Blueprint)
				{
					UClass* GenClass = Blueprint->GeneratedClass;
					if (GenClass)
					{
						FProperty* Prop = GenClass->FindPropertyByName(FName(*FunctionName));
						bVarExists = (Prop != nullptr);
					}
					else
					{
						 
						for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
						{
							if (ExistingVar.VarName.ToString() == FunctionName)
							{
								bVarExists = true;
								break;
							}
						}
					}
				}
				 
				if (!bVarExists)
				{
					for (const FWeaveVarDecl& VarDecl : AST.Vars)
					{
						if (VarDecl.VarName == FunctionName) { bVarExists = true; break; }
					}
				}
				if (!bVarExists)
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
					NewNode = CreateVariableGetNode(Graph, Blueprint, FunctionName);
				}
				else
				{
					NewNode = CreateVariableSetNode(Graph, Blueprint, FunctionName);
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
	
	 
	for (const FWeaveSetStmt& Set : AST.Sets)
	{
		UK2Node** NodePtr = CreatedNodes.Find(Set.NodeId);
		if (!NodePtr)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: node not found (%s)"), *Set.NodeId);
			if (!OutError.IsEmpty()) OutError += TEXT("\n");
			OutError += FString::Printf(TEXT("set %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"), *Set.NodeId, *Set.PinName, *Set.NodeId);
			continue;
		}
		
		UK2Node* Node = *NodePtr;
		
		 
		if (Set.PinName == TEXT("Expression"))
		{
			if (UK2Node_MathExpression* MathNode = Cast<UK2Node_MathExpression>(Node))
			{
				FString FinalValue = Set.Value;
				if (FinalValue.StartsWith(TEXT("\"")) && FinalValue.EndsWith(TEXT("\"")) && !FinalValue.Contains(TEXT("\\\"")))
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
				UClass* ActorClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None, nullptr);
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
				UClass* ObjClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_None, nullptr);
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
					UE_LOG(LogTemp, Warning, TEXT("[Weaver] ConstructObjectFromClass: Class not found: %s"), *ClassPath);
				}
				continue;
			}
		}

		 
		UEdGraphPin* Pin = Node->FindPin(*Set.PinName, EGPD_Input);
		 
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
			UClass* PinClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassValue, nullptr, LOAD_None, nullptr);
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
				UE_LOG(LogTemp, Log, TEXT("[Weaver] Set class pin: %s.%s = %s"), *Set.NodeId, *Set.PinName, *PinClass->GetName());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Set failed: class not found %s.%s = %s"), *Set.NodeId, *Set.PinName, *FinalValue);
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
					UE_LOG(LogTemp, Log, TEXT("[Weaver] Set: %s.%s = (skipped for object pin, value='%s')"), *Set.NodeId, *Set.PinName, *FinalValue);
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
	
	
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
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
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: node not found (%s or %s)"), *FromNodeId, *ToNodeId);
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += FString::Printf(TEXT("link %s.%s -> %s.%s 失败：节点 '%s' 不存在（可能因 schema_id 无效而未被创建）。"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
					!FromNode ? *FromNodeId : *ToNodeId);
				continue;
			}
			
			
			UEdGraphPin* FromPin = FromNode->FindPin(*Link.FromPin, EGPD_Output);
			
			if (!FromPin && Link.FromPin != TEXT("ReturnValue"))
				FromPin = FromNode->FindPin(TEXT("ReturnValue"), EGPD_Output);

			UEdGraphPin* ToPin = ToNode->FindPin(*Link.ToPin, EGPD_Input);
			
			
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
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link failed: pin not found (%s.%s or %s.%s)"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);

				
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

			
			const FPinConnectionResponse ConnectResponse = Schema->CanCreateConnection(FromPin, ToPin);
			if (ConnectResponse.Response == CONNECT_RESPONSE_DISALLOW)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Link disallowed: %s.%s (%s) -> %s.%s (%s): %s"),
					*Link.FromNode, *Link.FromPin, *FromPin->PinType.PinCategory.ToString(),
					*Link.ToNode, *Link.ToPin, *ToPin->PinType.PinCategory.ToString(),
					*ConnectResponse.Message.ToString());
				FString LinkError = FString::Printf(
					TEXT("link %s.%s -> %s.%s 无法建立：引脚类型 '%s' 与 '%s' 不兼容且无可用的自动转换节点。请手动声明转换节点（如 special.ToString 等）再连接。"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin,
					*FromPin->PinType.PinCategory.ToString(), *ToPin->PinType.PinCategory.ToString());
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}

			
			
			
			
			bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);
			if (!bConnected)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] TryCreateConnection failed: %s.%s -> %s.%s"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				FString LinkError = FString::Printf(
					TEXT("link %s.%s -> %s.%s 连接失败（TryCreateConnection 返回 false），请检查引脚名称和类型是否正确。"),
					*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
				if (!OutError.IsEmpty()) OutError += TEXT("\n");
				OutError += LinkError;
				continue;
			}

			FromNode->PinConnectionListChanged(FromPin);
			ToNode->PinConnectionListChanged(ToPin);

			UE_LOG(LogTemp, Log, TEXT("[Weaver] Linked: %s.%s -> %s.%s"),
				*Link.FromNode, *Link.FromPin, *Link.ToNode, *Link.ToPin);
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

	UK2Node_Event* EventNode = Graph->CreateIntermediateNode<UK2Node_Event>();
	if (EventNode)
	{
		EventNode->CreateNewGuid();
		
		
		FString FullClassName = ClassName;
		if (ClassName == TEXT("Actor"))
		{
			FullClassName = TEXT("/Script/Engine.Actor");
		}
		
		EventNode->EventReference.SetExternalMember(*EventName, UClass::TryFindTypeSlow<UClass>(*FullClassName));
		EventNode->bOverrideFunction = true;
		EventNode->AllocateDefaultPins();
		EventNode->ReconstructNode();
	}
	return EventNode;
}

UK2Node* FWeaveInterpreter::CreateCallNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName)
{
	UK2Node_CallFunction* CallNode = Graph->CreateIntermediateNode<UK2Node_CallFunction>();
	if (CallNode)
	{
		CallNode->CreateNewGuid();
		
		
		static const TMap<FString, FString> FuncNameTranslation = {
			{ TEXT("Conv_FloatToString"),        TEXT("Conv_DoubleToString") },
			{ TEXT("Conv_FloatToInt"),           TEXT("Conv_DoubleToInt") },
			{ TEXT("Conv_FloatToBool"),          TEXT("Conv_DoubleToBool") },
			{ TEXT("Conv_FloatToVector"),        TEXT("Conv_DoubleToVector") },
			{ TEXT("Conv_FloatToVector2D"),      TEXT("Conv_DoubleToVector2D") },
			{ TEXT("Conv_IntToFloat"),           TEXT("Conv_IntToDouble") },
			{ TEXT("Conv_ByteToFloat"),          TEXT("Conv_ByteToDouble") },
			{ TEXT("Conv_BoolToFloat"),          TEXT("Conv_BoolToDouble") },
			{ TEXT("FMin"),                      TEXT("Min") },
			{ TEXT("FMax"),                      TEXT("Max") },
			{ TEXT("FClamp"),                    TEXT("Clamp_Float") },
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

		
		UClass* TargetClass = nullptr;
		if (FullClassName.StartsWith(TEXT("/")))
		{
			TargetClass = LoadObject<UClass>(nullptr, *FullClassName);
		}
		if (!TargetClass)
		{
			TargetClass = UClass::TryFindTypeSlow<UClass>(*FullClassName);
		}
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
				UE_LOG(LogTemp, Warning, TEXT("[Weaver] Function not found: %s::%s"), *FullClassName, *ResolvedFunctionName);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] Class not found: %s"), *FullClassName);
		}
	}
	return CallNode;
}

UK2Node* FWeaveInterpreter::CreateMacroNode(UEdGraph* Graph, const FString& MacroPath, const FString& MacroName)
{
	UK2Node_MacroInstance* MacroNode = Graph->CreateIntermediateNode<UK2Node_MacroInstance>();
	if (MacroNode)
	{
		MacroNode->CreateNewGuid();

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
	UK2Node_IfThenElse* BranchNode = Graph->CreateIntermediateNode<UK2Node_IfThenElse>();
	if (BranchNode)
	{
		BranchNode->CreateNewGuid();
		BranchNode->AllocateDefaultPins();
		BranchNode->ReconstructNode();
	}
	return BranchNode;
}

UK2Node* FWeaveInterpreter::CreateSequenceNode(UEdGraph* Graph)
{
	UK2Node_ExecutionSequence* SequenceNode = Graph->CreateIntermediateNode<UK2Node_ExecutionSequence>();
	if (SequenceNode)
	{
		SequenceNode->CreateNewGuid();
		SequenceNode->AllocateDefaultPins();
		SequenceNode->ReconstructNode();
	}
	return SequenceNode;
}

UK2Node* FWeaveInterpreter::CreateMathExpressionNode(UEdGraph* Graph)
{
	UK2Node_MathExpression* MathNode = Graph->CreateIntermediateNode<UK2Node_MathExpression>();
	if (MathNode)
	{
		MathNode->CreateNewGuid();
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
		UK2Node_MakeStruct* MakeNode = Graph->CreateIntermediateNode<UK2Node_MakeStruct>();
		if (MakeNode)
		{
			MakeNode->StructType = StructType;
			MakeNode->CreateNewGuid();
			MakeNode->AllocateDefaultPins();

			
			bool bHasInputPins = false;
			for (UEdGraphPin* Pin : MakeNode->Pins)
			{
				if (Pin->Direction == EGPD_Input) { bHasInputPins = true; break; }
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
		UK2Node_BreakStruct* BreakNode = Graph->CreateIntermediateNode<UK2Node_BreakStruct>();
		if (BreakNode)
		{
			BreakNode->StructType = StructType;
			BreakNode->CreateNewGuid();
			BreakNode->AllocateDefaultPins();

			
			bool bHasOutputPins = false;
			for (UEdGraphPin* Pin : BreakNode->Pins)
			{
				if (Pin->Direction == EGPD_Output) { bHasOutputPins = true; break; }
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
	UK2Node_VariableGet* VarGetNode = Graph->CreateIntermediateNode<UK2Node_VariableGet>();
	if (VarGetNode)
	{
		VarGetNode->CreateNewGuid();
		
		
		FName VarFName = FName(*VarName);
		VarGetNode->VariableReference.SetSelfMember(VarFName);
		VarGetNode->AllocateDefaultPins();
	}
	return VarGetNode;
}

UK2Node* FWeaveInterpreter::CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName)
{
	UK2Node_VariableSet* VarSetNode = Graph->CreateIntermediateNode<UK2Node_VariableSet>();
	if (VarSetNode)
	{
		VarSetNode->CreateNewGuid();
		
		
		FName VarFName = FName(*VarName);
		VarSetNode->VariableReference.SetSelfMember(VarFName);
		VarSetNode->AllocateDefaultPins();
	}
	return VarSetNode;
}

UK2Node* FWeaveInterpreter::CreateSpawnActorFromClassNode(UEdGraph* Graph)
{
	UK2Node_SpawnActorFromClass* SpawnNode = Graph->CreateIntermediateNode<UK2Node_SpawnActorFromClass>();
	if (SpawnNode)
	{
		SpawnNode->CreateNewGuid();
		SpawnNode->AllocateDefaultPins();
	}
	return SpawnNode;
}

UK2Node* FWeaveInterpreter::CreateConstructObjectFromClassNode(UEdGraph* Graph)
{
	UK2Node_ConstructObjectFromClass* ConstructNode = Graph->CreateIntermediateNode<UK2Node_ConstructObjectFromClass>();
	if (ConstructNode)
	{
		ConstructNode->CreateNewGuid();
		ConstructNode->AllocateDefaultPins();
	}
	return ConstructNode;
}

UK2Node* FWeaveInterpreter::CreateDynamicCastNode(UEdGraph* Graph, const FString& TargetTypeName)
{
	UK2Node_DynamicCast* CastNode = Graph->CreateIntermediateNode<UK2Node_DynamicCast>();
	if (CastNode)
	{
		CastNode->CreateNewGuid();

		
		UClass* TargetClass = UClass::TryFindTypeSlow<UClass>(*(TEXT("A") + TargetTypeName));
		if (!TargetClass) TargetClass = UClass::TryFindTypeSlow<UClass>(*(TEXT("U") + TargetTypeName));
		if (!TargetClass) TargetClass = UClass::TryFindTypeSlow<UClass>(*TargetTypeName);

		if (TargetClass)
		{
			CastNode->TargetType = TargetClass;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Weaver] CreateDynamicCastNode: Type not found: %s"), *TargetTypeName);
		}

		CastNode->AllocateDefaultPins();
		CastNode->ReconstructNode();
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

	UK2Node_SwitchEnum* SwitchNode = Graph->CreateIntermediateNode<UK2Node_SwitchEnum>();
	if (SwitchNode)
	{
		SwitchNode->CreateNewGuid();
		
		SwitchNode->SetEnum(TargetEnum);
		SwitchNode->ReconstructNode();
	}
	return SwitchNode;
}
