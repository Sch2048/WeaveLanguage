

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Dom/JsonObject.h"
#include "WeaveOperator.generated.h"

UCLASS()
class WEAVELANGUAGE_API UWeaveOperator : public UObject
{
	GENERATED_BODY()

public:
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static void ExecuteOperation();
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static void GenerateWeaveLanguage();
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchNode(const FString& Query);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchType(const FString& Query);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchBlueprintVariables(const FString& BlueprintPath, const FString& Query);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchContextVar(const FString& BlueprintPath, const FString& Query);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static bool ModifyVar(const FString& BlueprintPath, const FString& VarName, const FString& NewValue, FString& OutError);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static bool DeleteVar(const FString& BlueprintPath, const FString& VarName, FString& OutError);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchContextFunctions(const FString& BlueprintPath, const FString& Query);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> SearchAsset(const FString& Query, int32 MaxResults = 20);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> GetAssetReferences(const FString& AssetPath, int32 MaxResults = 20);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static FString GetBlueprintWeave(const FString& BlueprintPath, const FString& GraphName, const FString& EntryNode = TEXT(""));
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static FString ApplyWeaveDiff(const FString& OriginalWeave, const FString& DiffCode, FString& OutError);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static FString DiffCheck(const FString& BlueprintPath, const FString& GraphName, const FString& DiffCode, FString& OutError);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static bool ApplyDiff(FString& OutError);
	
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static bool ApplyWeaveToBlueprintWithUndo(const FString& WeaveCode, const FString& BlueprintPath, const FString& GraphName, FString& OutError);
	
	 
	static void SearchNodeAsync(const FString& Query, TFunction<void(const TArray<FString>&)> OnComplete);
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static FString GetNodeById(const FString& Id);
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> GetNodesByCategory(const FString& Category);
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static void AddNodeFromJson(const FString& JsonString);
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static bool RemoveNode(const FString& Id);
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static TArray<FString> GetAllNodesAsJson();
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static void ClearNodes();
	
	UFUNCTION(BlueprintCallable, Category="Weaver")
	static int32 GetNodeCount();
	
private:
	 
	static TArray<TSharedPtr<FJsonObject>> NodeCatalog;
	static bool bCatalogInitialized;
	
	 
	static TArray<TSharedPtr<FJsonObject>> TypeCatalog;
	static bool bTypeCatalogInitialized;
	
	 
	static FString CachedBlueprintPath;
	static FString CachedGraphName;
	static FString CachedResultWeave;
	
	 
	static TMap<FString, TArray<int32>> KeywordIndex;   
	static TMap<FString, int32> IdIndex;   
	
	 
	static void BuildSearchIndex();
	 
	static TSharedPtr<FJsonObject> NodeToJson(class UEdGraphNode* Node);
	
	 
	static TArray<FString> GenerateKeywords(const FString& Title);
};
