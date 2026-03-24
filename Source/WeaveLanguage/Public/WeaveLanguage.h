

#pragma once

#include "Modules/ModuleManager.h"
#include "Framework/Application/IInputProcessor.h"

class FWeaveLanguageModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
private:


	void RegisterMenus();
	void FillWeaverMenu(class UToolMenu* Menu);
	void OnGenerateWeave();
	void OnOpenDebugger();
};
