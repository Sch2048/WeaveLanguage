

#pragma once

#include "CoreMinimal.h"

class UK2Node;
struct FWeaveLinkStmt;

/**
 * Base class for handling dynamic pin creation in special nodes
 */
class IWeaverDynamicPinHandler
{
public:
	virtual ~IWeaverDynamicPinHandler() = default;
	
	/**
	 * Scan links and determine required dynamic pins for nodes
	 * @param Links - All link statements from Weave AST
	 * @param CreatedNodes - Map of NodeId to created UK2Node
	 */
	virtual void PreScanLinks(const TArray<FWeaveLinkStmt>& Links, const TMap<FString, UK2Node*>& CreatedNodes) = 0;
	
	/**
	 * Add dynamic pins to nodes based on pre-scan results
	 * @param CreatedNodes - Map of NodeId to created UK2Node
	 */
	virtual void AddDynamicPins(const TMap<FString, UK2Node*>& CreatedNodes) = 0;
};
