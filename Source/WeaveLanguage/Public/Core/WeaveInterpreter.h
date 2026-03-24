

#pragma once

#include "CoreMinimal.h"


struct FWeaveNodeDecl
{
	FString NodeId;      
	FString SchemaId;    
	FVector2D Position;  
	TMap<FString, FString> InlineProps;  
};

struct FWeaveSetStmt
{
	FString NodeId;
	FString PinName;
	FString Value;
};

struct FWeaveLinkStmt
{
	FString FromNode;
	FString FromPin;
	FString ToNode;
	FString ToPin;
};

struct FWeaveVarDecl
{
	FString VarName;  
	FString VarType;  
};

struct FWeaveAST
{
	FString BlueprintPath;  
	FString GraphName;  
	TArray<FWeaveVarDecl> Vars;
	TArray<FWeaveNodeDecl> Nodes;
	TArray<FWeaveSetStmt> Sets;
	TArray<FWeaveLinkStmt> Links;
};

class WEAVELANGUAGE_API FWeaveInterpreter
{
public:
	
	static bool Parse(const FString& WeaveCode, FWeaveAST& OutAST, FString& OutError);
	
	
	static int32 GenerateBlueprint(const FWeaveAST& AST, class UEdGraph* Graph, FString& OutError);

private:
	
	static TArray<FString> Tokenize(const FString& Code);
	
	
	static bool ParseGraph(const TArray<FString>& Tokens, int32& Index, FString& OutGraphName);
	
	
	static bool ParseNode(const TArray<FString>& Tokens, int32& Index, FWeaveNodeDecl& OutNode);
	
	
	static bool ParseSet(const TArray<FString>& Tokens, int32& Index, FWeaveSetStmt& OutSet);
	
	
	static bool ParseLink(const TArray<FString>& Tokens, int32& Index, FWeaveLinkStmt& OutLink);
	
	
	static bool ParseVar(const TArray<FString>& Tokens, int32& Index, FWeaveVarDecl& OutVar);
	
	
	
	static UK2Node* CreateEventNode(UEdGraph* Graph, const FString& ClassName, const FString& EventName);
	static UK2Node* CreateCallNode(UEdGraph* Graph, const FString& ClassName, const FString& FunctionName);
	static UK2Node* CreateMacroNode(UEdGraph* Graph, const FString& MacroPath, const FString& MacroName);
	static UK2Node* CreateBranchNode(UEdGraph* Graph);
	static UK2Node* CreateSequenceNode(UEdGraph* Graph);
	static UK2Node* CreateMathExpressionNode(UEdGraph* Graph);
	static UK2Node* CreateMakeStructNode(UEdGraph* Graph, const FString& StructTypeName);
	static UK2Node* CreateBreakStructNode(UEdGraph* Graph, const FString& StructTypeName);
	static UK2Node* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName);
	static UK2Node* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VarName);
	static UK2Node* CreateSpawnActorFromClassNode(UEdGraph* Graph);
	static UK2Node* CreateConstructObjectFromClassNode(UEdGraph* Graph);
	static UK2Node* CreateDynamicCastNode(UEdGraph* Graph, const FString& TargetTypeName);
	static UK2Node* CreateSwitchEnumNode(UEdGraph* Graph, const FString& EnumName);
};
