#include "Slate/SWeaverDebugger.h"
#include "Core/WeaveInterpreter.h"
#include "Core/WeaveGenerator.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "EditorStyleSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintEditor.h"

void SWeaverDebugger::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(0)
		[
			SNew(SBox)
			.WidthOverride(800)
			.HeightOverride(600)
			[
				SNew(SVerticalBox)


				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
					.Padding(FMargin(10, 8))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Weave Debugger")))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(FLinearColor::White)
					]
				]


				+ SVerticalBox::Slot()
				.FillHeight(0.6f)
				.Padding(8)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(8)
					[
						SAssignNew(CodeInputBox, SMultiLineEditableTextBox)
						.HintText(FText::FromString(TEXT(
							"Enter Weave code...\n\nExample:\ngraph MyActor.EventGraph\n\nnode e : event.Actor.ReceiveBeginPlay\nnode print : call.KismetSystemLibrary.PrintString")))
						.Font(FAppStyle::GetFontStyle("NormalFont"))
						.AutoWrapText(false)
					]
				]


				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Apply to Blueprint")))
						.HAlign(HAlign_Center)
						.OnClicked(this, &SWeaverDebugger::OnApply)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Generate from Selection")))
						.OnClicked(this, &SWeaverDebugger::OnGenerateFromSelection)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Clear")))
						.OnClicked(this, &SWeaverDebugger::OnClear)
					]
				]


				+ SVerticalBox::Slot()
				.FillHeight(0.4f)
				.Padding(8, 0, 8, 8)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(8)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SAssignNew(ResultText, STextBlock)
							.Text(FText::FromString(TEXT("Waiting for input...")))
							.Font(FAppStyle::GetFontStyle("NormalFont"))
							.AutoWrapText(true)
						]
					]
				]
			]
		]
	];
}

FReply SWeaverDebugger::OnApply()
{
	if (!CodeInputBox.IsValid())
	{
		return FReply::Handled();
	}

	FString WeaveCode = CodeInputBox->GetText().ToString();
	if (WeaveCode.IsEmpty())
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Code is empty")));
		return FReply::Handled();
	}


	FWeaveAST AST;
	FString Error;

	if (!FWeaveInterpreter::Parse(WeaveCode, AST, Error))
	{
		ResultText->SetText(FText::FromString(FString::Printf(TEXT("Parse failed: %s"), *Error)));
		return FReply::Handled();
	}


	FString Result = FString::Printf(
		TEXT("Parse successful!\n\nBlueprint: %s\nGraph: %s\nNodes: %d\nSets: %d\nLinks: %d\n\n"),
		*AST.BlueprintPath, *AST.GraphName, AST.Nodes.Num(), AST.Sets.Num(), AST.Links.Num()
	);


	for (const FWeaveNodeDecl& Node : AST.Nodes)
	{
		Result += FString::Printf(TEXT("Node %s: %s @ (%.0f, %.0f)\n"),
		                          *Node.NodeId, *Node.SchemaId, Node.Position.X, Node.Position.Y);
	}


	if (!AST.BlueprintPath.IsEmpty())
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AST.BlueprintPath);
		if (BP)
		{
			UEdGraph* TargetGraph = nullptr;


			for (UEdGraph* Graph : BP->UbergraphPages)
			{
				if (Graph && Graph->GetName().Contains(AST.GraphName))
				{
					TargetGraph = Graph;
					break;
				}
			}


			if (!TargetGraph)
			{
				for (UEdGraph* Graph : BP->FunctionGraphs)
				{
					if (Graph && Graph->GetName().Contains(AST.GraphName))
					{
						TargetGraph = Graph;
						break;
					}
				}
			}

			if (TargetGraph)
			{
				Result += FString::Printf(TEXT("\nTarget Graph: %s\n"), *TargetGraph->GetName());

				FString GenError;
				int32 NodesCreated = FWeaveInterpreter::GenerateBlueprint(AST, TargetGraph, GenError);

				if (NodesCreated > 0)
				{
					Result += FString::Printf(TEXT("Generated %d nodes successfully!"), NodesCreated);
					BP->MarkPackageDirty();
				}
				else
				{
					Result += FString::Printf(TEXT("Generation failed: %s"), *GenError);
				}
			}
			else
			{
				Result += TEXT("\nError: Graph not found in blueprint");
			}
		}
		else
		{
			Result += TEXT("\nError: Failed to load blueprint");
		}
	}
	else
	{
		Result += TEXT("\n[Note] No blueprint path specified. Use 'graphset' command.");
	}

	ResultText->SetText(FText::FromString(Result));

	UE_LOG(LogTemp, Log, TEXT("[Weaver Debugger] Parsed successfully: %s"), *AST.GraphName);

	return FReply::Handled();
}

FReply SWeaverDebugger::OnClear()
{
	if (CodeInputBox.IsValid())
	{
		CodeInputBox->SetText(FText::GetEmpty());
	}

	if (ResultText.IsValid())
	{
		ResultText->SetText(FText::FromString(TEXT("Cleared")));
	}

	return FReply::Handled();
}

FReply SWeaverDebugger::OnGenerateFromSelection()
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Cannot access asset editor subsystem")));
		return FReply::Handled();
	}
	
	TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
	const UBlueprint* CurrentBP = nullptr;
	const FBlueprintEditor* BlueprintEditor = nullptr;

	for (UObject* Asset : EditedAssets)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(Asset))
		{
			if (IAssetEditorInstance* Editor = AssetEditorSubsystem->FindEditorForAsset(BP, false))
			{
				// Safe: outer loop guarantees Asset is UBlueprint, so Editor is FBlueprintEditor
				FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Editor);
				if (BPEditor && BPEditor->GetSelectedNodes().Num() > 0)
				{
					CurrentBP = BP;
					BlueprintEditor = BPEditor;
					break;
				}
				if (!CurrentBP)
				{
					CurrentBP = BP;
					BlueprintEditor = BPEditor;
				}
			}
		}
	}

	if (!CurrentBP)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: No blueprint is currently open")));
		return FReply::Handled();
	}

	if (!BlueprintEditor)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Cannot access blueprint editor")));
		return FReply::Handled();
	}


	TSet<UObject*> SelectedObjects = BlueprintEditor->GetSelectedNodes();
	TArray<UEdGraphNode*> SelectedNodes;
	UEdGraph* CurrentGraph = nullptr;

	for (UObject* Obj : SelectedObjects)
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
		{
			SelectedNodes.Add(Node);
			if (!CurrentGraph)
			{
				CurrentGraph = Node->GetGraph();
			}
		}
	}

	if (!CurrentGraph || SelectedNodes.Num() == 0)
	{
		ResultText->SetText(FText::FromString(TEXT("Error: No nodes selected in blueprint graph")));
		return FReply::Handled();
	}


	FString GeneratedCode;
	if (FWeaveGenerator::Generate(SelectedNodes, CurrentGraph, GeneratedCode))
	{
		CodeInputBox->SetText(FText::FromString(GeneratedCode));
		ResultText->SetText(FText::FromString(FString::Printf(
			TEXT("Generated Weave code from %d selected node(s)"), SelectedNodes.Num())));
	}
	else
	{
		ResultText->SetText(FText::FromString(TEXT("Error: Failed to generate Weave code")));
	}

	return FReply::Handled();
}

void SWeaverDebugger::TriggerGenerateFromSelection()
{
	OnGenerateFromSelection();
}
