/*
* Copyright (c) 2018 NVIDIA Corporation. All rights reserved.
* This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0
* International License.  (https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)
*/
#include "NVUtilitiesModule.h"

//#miker:lame...
IMPLEMENT_MODULE(INVUtilitiesModule, NVUtilities)

//General Log
DEFINE_LOG_CATEGORY(LogNVUtilities);

void INVUtilitiesModule::StartupModule()
{
    UE_LOG(LogNVUtilities, Warning, TEXT("Loaded NVUtilities Main"));
}

void INVUtilitiesModule::ShutdownModule()
{
}

