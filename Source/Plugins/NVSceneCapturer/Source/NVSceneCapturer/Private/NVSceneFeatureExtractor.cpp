/*
* Copyright (c) 2018 NVIDIA Corporation. All rights reserved.
* This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0
* International License.  (https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)
*/

#include "NVSceneCapturerModule.h"
#include "NVSceneCapturerUtils.h"
#include "NVSceneFeatureExtractor.h"
#include "NVSceneCapturerActor.h"
#include "NVSceneCaptureComponent2D.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/CollisionProfile.h"
#include "Components/DrawFrustumComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Kismet/KismetSystemLibrary.h"

UNVSceneFeatureExtractor::UNVSceneFeatureExtractor(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DisplayName = TEXT("");
    bIsEnabled = true;

    OwnerViewpoint = nullptr;
    OwnerCapturer = nullptr;

    bCapturing = false;
}

UWorld* UNVSceneFeatureExtractor::GetWorld() const
{
    return OwnerCapturer ? OwnerCapturer->GetWorld() : Super::GetWorld();
}

bool UNVSceneFeatureExtractor::IsEnabled() const
{
    return bIsEnabled;
}

FString UNVSceneFeatureExtractor::GetDisplayName() const
{
    return (DisplayName.IsEmpty() ? GetName() : DisplayName);
}

void UNVSceneFeatureExtractor::Init(UNVSceneCapturerViewpointComponent* InOwnerViewpoint)
{
    ensure(bCapturing == false);

    if (bCapturing == false)
    {
        OwnerViewpoint = InOwnerViewpoint;
        if (OwnerViewpoint)
        {
            OwnerViewpoint->FeatureExtractorList.AddUnique(this);

            OwnerCapturer = Cast<ANVSceneCapturerActor>(OwnerViewpoint->GetOwner());
            UpdateSettings();
        }
    }
}

//#miker: update a feature extractor throughout a simulation
void UNVSceneFeatureExtractor::updateFE(UNVSceneCapturerViewpointComponent* InOwnerViewpoint,
										int fe_index, bool enable)
{
	if (fe_index == -1) { return; }
	
	OwnerViewpoint = InOwnerViewpoint;
	if (OwnerViewpoint)
	{
		if (fe_index >= OwnerViewpoint->FeatureExtractorList.Num())
		{
			return;
		}

		UNVSceneFeatureExtractor* fe_updated = OwnerViewpoint->FeatureExtractorList[fe_index];
		if (fe_updated)
		{
			//FString fe_name = fe_updated->GetDisplayName();
			//const FString miker = FString::Printf(TEXT("         : %s - %d "), *fe_name,enable);
			//GLog->Log(miker);
			fe_updated->bIsEnabled  = enable;
		}		 
	}
}
void UNVSceneFeatureExtractor::StartCapturing()
{
    bCapturing = true;
}

void UNVSceneFeatureExtractor::StopCapturing()
{
    bCapturing = false;
}