/*
* Copyright (c) 2018 NVIDIA Corporation. All rights reserved.
* This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0
* International License.  (https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)
*/

#include "NVSceneCapturerModule.h"
#include "NVSceneCapturerUtils.h"
#include "NVSceneCapturerActor.h"
#include "NVObjectMaskManager.h"
#include "NVSceneManager.h"
#include "NVSceneMarker.h"
#include "Components/StaticMeshComponent.h"
#include "Engine.h"
#if WITH_EDITOR
#include "Factories/FbxAssetImportData.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#endif

namespace
{
    static TWeakObjectPtr<ANVSceneManager> globalANVSceneManagerPtr=nullptr;
}

ANVSceneManager::ANVSceneManager(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    globalANVSceneManagerPtr = nullptr;

    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    bIsActive = true;
    bCaptureAtAllMarkers = true;
    SceneManagerState = ENVSceneManagerState::NotActive;
    CurrentSceneMarker = nullptr;
    bAutoExitAfterExportingComplete = false;
}

ANVSceneManager* ANVSceneManager::GetANVSceneManagerPtr()
{
    return globalANVSceneManagerPtr.Get();
}

ENVSceneManagerState ANVSceneManager::GetState() const
{
    return SceneManagerState;
}

void ANVSceneManager::ResetState()
{
    if (SceneManagerState == ENVSceneManagerState::Captured)
    {
        SceneManagerState = ENVSceneManagerState::Ready;
    }
}

bool ANVSceneManager::IsAllSceneCaptured() const
{
    return !bCaptureAtAllMarkers || (CurrentMarkerIndex >= SceneMarkers.Num() - 1);
}

void ANVSceneManager::PreInitializeComponents()
{
    Super::PreInitializeComponents();

    globalANVSceneManagerPtr = nullptr;
}

void ANVSceneManager::PostInitializeComponents()
{
    Super::PostInitializeComponents();

    // bIsActive is public for UI, we need to copy the state to a protected value.
    // because we need a state which user can not change.

    // ANVSceneManager should be singleton. but currentry ANVSceneManager is also used for UI.
    // we need to decide which ANVSceneManager to use.
    // this selects one ANVSceneManager which is called PostInitializeComponents() first and it is active.
    // now only one ANVSceneManager will be active and only one instance will be used.
    if (bIsActive)
    {
        if (globalANVSceneManagerPtr == nullptr)
        {
            SceneManagerState = ENVSceneManagerState::Active;
            globalANVSceneManagerPtr = this;
        }
        else
        {
            SceneManagerState = ENVSceneManagerState::NotActive;

            // If user put multiple ANVSceneManagers,
            // We need disable bIsActive to show user which ANVSceneManagers is not used.
            bIsActive = false;
        }
    }
    else
    {
        SceneManagerState =  ENVSceneManagerState::NotActive;
    }
}

void ANVSceneManager::RestartSceneManager()
{
	UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: restartSceneManager"));

	ResetState();
	//Reset();
	RepeatingCallsRemaining = 1;
	//this needs to be called from within the plugins completion
	//state
	//m_simpleCapturer->restartCaptureActor();
	
}

void ANVSceneManager::BGControllerIsCurrentlyDone(bool state, int sim_run)
{
	if (m_simpleCapturer)
	{
		m_simpleCapturer->BGControllerIsCurrentlyDone(state, sim_run);
	}
}

void ANVSceneManager::setBGTargetFolderOverride(bool useBGTargetOverride, FString simulationSave)
{
	if (m_simpleCapturer)
	{
		m_simpleCapturer->setBGTargetFolderOverride(useBGTargetOverride, simulationSave);
	}
}
void ANVSceneManager::RepeatingFunction()
{
	//#miker: effectively a poor mans lazy loading
	// may need this delay for packaged builds

	if (--RepeatingCallsRemaining <= 0)
	{

		UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: RepeatingFunction ANVSceneManager"));
		GetWorldTimerManager().ClearTimer(MemberTimerHandle);
		// MemberTimerHandle can now be reused for any other Timer.
	}
}

const float DELAYBEGINDELAYSM = 2.0f;

void ANVSceneManager::BeginPlay()
{
    Super::BeginPlay();
	UWorld* World = GetWorld();
#if WITH_EDITOR
	bool bIsSimulating = GUnrealEd ? (GUnrealEd->bIsSimulatingInEditor || GUnrealEd->bIsSimulateInEditorQueued) : false;
	if (!World || !World->IsGameWorld() || bIsSimulating)
	{
		return;
	}
#endif
	ensure(World);
	if (World)
	{
		for (int i = SceneMarkers.Num() - 1; i >= 0; i--)
		{
			if (!SceneMarkers[i])
			{
				SceneMarkers.RemoveAt(i);
			}
		}
		if (SceneMarkers.Num() <= 0)
		{
			bCaptureAtAllMarkers = false;
		}

		if (SceneManagerState == ENVSceneManagerState::Active)
		{
			SceneCapturers.Reset();
			for (TActorIterator<ANVSceneCapturerActor> It(World); It; ++It)
			{
				ANVSceneCapturerActor* CheckCapturer = *It;
				if (CheckCapturer)
				{
					if (m_simpleCapturer == nullptr)
					{
						m_simpleCapturer = CheckCapturer;
					}
					SceneCapturers.Add(CheckCapturer);
					CheckCapturer->OnCompletedEvent.AddDynamic(this, &ANVSceneManager::OnCapturingCompleted);
				}
			}

			UpdateSettingsFromCommandLine();

			ObjectClassSegmentation.Init(this);
			ObjectInstanceSegmentation.Init(this);

			if (bCaptureAtAllMarkers)
			{
				CurrentMarkerIndex = -1;
				FocusNextMarker();
			}
			else
			{
				CurrentMarkerIndex = 0;
				SetupScene();
			}
		}
	}
}

void ANVSceneManager::UpdateSettingsFromCommandLine()
{
    const auto CommandLine = FCommandLine::Get();

    FString OverrideCapturers = TEXT("");
    if (FParse::Value(CommandLine, TEXT("-Capturers="), OverrideCapturers))
    {
        TArray<FString> CapturerNames;
        OverrideCapturers.ParseIntoArray(CapturerNames, TEXT(","));
        if (CapturerNames.Num() > 0)
        {
            // Make sure all the capturers are deactivated first
            for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
            {
                if (CheckCapturer)
                {
                    CheckCapturer->StopCapturing();
                    CheckCapturer->bIsActive = false;
                }
            }

            // Only activate the capturers specified in the command line
            for (const FString& CheckCapturerName : CapturerNames)
            {
                for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
                {
                    if (CheckCapturer)
                    {
                        const FString& CapturerName = CheckCapturer->GetName();
                        const FString& CapturerHumanReadableName = CheckCapturer->GetHumanReadableName();
                        if ((CapturerHumanReadableName == CheckCapturerName) || (CapturerName == CheckCapturerName))
                        {
                            CheckCapturer->bIsActive = true;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void ANVSceneManager::SetupScene()
{
    UWorld* World = GetWorld();
    if (World)
    {
        const int32 POICount = SceneMarkers.Num();
        if ((POICount > 0) && (CurrentMarkerIndex < POICount) && (CurrentMarkerIndex >= 0))
        {
            INVSceneMarkerInterface* SceneMarker = Cast<INVSceneMarkerInterface>(CurrentSceneMarker);
            if (SceneMarker)
            {
                SceneMarker->RemoveAllObservers();
            }

            CurrentSceneMarker = SceneMarkers[CurrentMarkerIndex];
        }

        SetupSceneInternal();

        // After the scene setup is done then start applying class and instance segmentation marks
		// miker:
        UpdateSegmentationMask();

        // TODO: Broadcast Ready event
        SceneManagerState = ENVSceneManagerState::Ready;

        OnSetupCompleted.Broadcast(this, SceneManagerState == ENVSceneManagerState::Ready);
    }
}

void ANVSceneManager::SetupSceneInternal()
{
    INVSceneMarkerInterface* SceneMarker = Cast<INVSceneMarkerInterface>(CurrentSceneMarker);
    if (SceneMarker)
    {
        for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
        {
            // ToDo: Use GetCurrentState() instead of bIsActive.
            // CheckCapturer->GetCurrentState() is available after BeginPlay life cycle.            
            if (CheckCapturer && CheckCapturer->bIsActive)
            {
                SceneMarker->AddObserver(CheckCapturer);
            }
        }
    }
}

void ANVSceneManager::UpdateSegmentationMask()
{
    UWorld* World = GetWorld();
    if (World)
    {
        ObjectClassSegmentation.ScanActors(World);

        bool bNeedInstanceSegmentation = false;
        for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
        {
            if (CheckCapturer && CheckCapturer->bIsActive)
            {
                for (const auto& CheckFeatureExtractor : CheckCapturer->FeatureExtractorSettings)
                {
                    UNVSceneFeatureExtractor* CheckFeatureExtractorRef = CheckFeatureExtractor.FeatureExtractorRef;
                    if (CheckFeatureExtractorRef && CheckFeatureExtractorRef->IsEnabled() && CheckFeatureExtractorRef->IsA(UNVSceneFeatureExtractor_VertexColorMask::StaticClass()))
                    {
                        bNeedInstanceSegmentation = true;
                        break;
                    }
                }
            }
        }
        // Only update the objects' instance segmentation if it need to be captured
        if (bNeedInstanceSegmentation)
        {
            ObjectInstanceSegmentation.ScanActors(World);
        }
    }
}

void ANVSceneManager::FocusNextMarker()
{
    const int32 POICount = SceneMarkers.Num();

    if (bCaptureAtAllMarkers && (CurrentMarkerIndex < POICount - 1))
    {
        CurrentMarkerIndex++;
        SetupScene();

        for (int i = 0; i < SceneCapturers.Num(); i++)
        {
            ANVSceneCapturerActor* CheckCapturer = SceneCapturers[i];
            if (CheckCapturer && (CheckCapturer->GetCurrentState() != ENVSceneCapturerState::NotActive))
            {
                if (bUseMarkerNameAsPostfix)
                {
                    const FString CurrentExportFolderName = SceneCaptureExportDirNames[i];
                    const FString NewExportFolderName = FString::Printf(TEXT("%s_%d"), *CurrentExportFolderName, CurrentMarkerIndex);
                    // FIXME: Let the SceneDataExporter handle the directory
                    //CheckCapturer->CustomDirectoryName = NewExportFolderName;
                }

                CheckCapturer->StartCapturing();
            }
        }
    }
}

void ANVSceneManager::OnCapturingCompleted(ANVSceneCapturerActor* SceneCapturer, bool bIsSucceeded)
{
    if (bIsActive)
    {
        bool bAllCapturerCompleted = true;
        for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
        {
            if (CheckCapturer && !(CheckCapturer->GetCurrentState()== ENVSceneCapturerState::Completed))
            {
                bAllCapturerCompleted = false;
                break;
            }
        }

        if (bAllCapturerCompleted)
        {
			//#miker: if we're complete then let's restart...
			//this cannot be done from the controller as there
			//is some janky time delayed statefulness that needs to be adhered to
			//in this plugin...
			for (ANVSceneCapturerActor* CheckCapturer : SceneCapturers)
			{
				CheckCapturer->restartCaptureActor();
			}			

            if (IsAllSceneCaptured())
            {
				UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: nvscenemanager::...scene captured in its entirety"));

                SceneManagerState = ENVSceneManagerState::Captured;
                if (bAutoExitAfterExportingComplete)
                {
                    UWorld* World = GetWorld();
                    if (World && GEngine)
                    {

                        GEngine->Exec(World, TEXT("exit"));
                    }
                }
            }
            else
            {
                SceneManagerState = ENVSceneManagerState::Active;
                FocusNextMarker();
            }
        }
    }
}

#if WITH_EDITORONLY_DATA
void ANVSceneManager::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
    const UProperty* PropertyThatChanged = PropertyChangedEvent.MemberProperty;
    // Don't add ensure(PropertyThatChanged) here. 
    // When ANVSceneManagerActor is deplicated, PropertyThatChanged can be null.
    if (PropertyThatChanged)
    {
        const FName ChangedPropName = PropertyThatChanged->GetFName();
        Super::PostEditChangeProperty(PropertyChangedEvent);
    }
}
#endif // WITH_EDITORONLY_DATA

