

#pragma once

#include "CoreMinimal.h"

class WEAVELANGUAGE_API FWeaveGenerator
{
public:
	
	
	
	
	static bool Generate(const TArray<class UEdGraphNode*>& SelectedNodes, class UEdGraph* Graph, FString& OutWeaveCode);

private:
	
	static void CollectDependencies(UEdGraphNode* Node, TSet<UEdGraphNode*>& OutNodes);
	
	
	static FString GetNodeSchemaId(UEdGraphNode* Node);
};
