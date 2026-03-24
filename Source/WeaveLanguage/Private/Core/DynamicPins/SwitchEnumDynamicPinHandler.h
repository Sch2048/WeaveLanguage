

#pragma once

#include "CoreMinimal.h"
#include "WeaverDynamicPinHandler.h"

/**
 * Handler for SwitchEnum node — records which enum each switch node uses.
 * K2Node_SwitchEnum auto-creates case pins via SetEnum(), so AddDynamicPins is a no-op.
 * The enum name is carried in the schema_id: special.SwitchEnum.<EnumName>
 */
class FSwitchEnumDynamicPinHandler : public IWeaverDynamicPinHandler
{
public:
	virtual void PreScanLinks(const TArray<FWeaveLinkStmt>& Links, const TMap<FString, UK2Node*>& CreatedNodes) override;
	virtual void AddDynamicPins(const TMap<FString, UK2Node*>& CreatedNodes) override;

private:
	
	TMap<FString, FString> SwitchEnumNodes;
};
