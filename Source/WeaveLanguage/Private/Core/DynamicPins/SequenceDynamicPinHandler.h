

#pragma once

#include "CoreMinimal.h"
#include "WeaverDynamicPinHandler.h"
#include "Core/WeaveOperator.h"

/**
 * Handler for Sequence node dynamic pins (then_0, then_1, then_2...)
 */
class FSequenceDynamicPinHandler : public IWeaverDynamicPinHandler
{
public:
	virtual void PreScanLinks(const TArray<FWeaveLinkStmt>& Links, const TMap<FString, UK2Node*>& CreatedNodes) override;
	virtual void AddDynamicPins(const TMap<FString, UK2Node*>& CreatedNodes) override;

private:
	
	TMap<FString, int32> SequenceMaxPinIndex;
};
