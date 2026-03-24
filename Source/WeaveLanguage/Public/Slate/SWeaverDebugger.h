

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SMultiLineEditableTextBox;
class STextBlock;

class SWeaverDebugger : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWeaverDebugger) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void TriggerGenerateFromSelection();

private:
	FReply OnApply();
	FReply OnClear();
	FReply OnGenerateFromSelection();
	TSharedPtr<SMultiLineEditableTextBox> CodeInputBox;
	TSharedPtr<STextBlock> ResultText;
};
