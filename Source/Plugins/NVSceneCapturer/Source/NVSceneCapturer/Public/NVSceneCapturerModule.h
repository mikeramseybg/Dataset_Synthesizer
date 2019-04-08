/*
* Copyright (c) 2018 NVIDIA Corporation. All rights reserved.
* This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0
* International License.  (https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)
*/

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNVSceneCapturer, Log, All)

class INVSceneCapturerModule: public IModuleInterface
{
public:
    // IModuleInterface implementation
    void StartupModule();
    void ShutdownModule();
};
