/*
* Copyright (c) 2018 NVIDIA Corporation. All rights reserved.
* This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0
* International License.  (https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)
*/

#include "NVSceneCapturerModule.h"
#include "NVSceneCapturerUtils.h"
#include "NVSceneFeatureExtractor.h"
#include "NVSceneCapturerViewpointComponent.h"
#include "NVSceneCapturerActor.h"
#include "NVSceneManager.h"
#include "NVAnnotatedActor.h"
#include "NVSceneDataHandler.h"
#include "Engine.h"
#include "JsonObjectConverter.h"
#if WITH_EDITOR
#include "Factories/FbxAssetImportData.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#endif // WITH_EDITOR

//#miker:
//debug defines
//#define BGDEBUG_CAPTURESCENE_FILE
//#define BGDEBUG_LOG_FRAME_TIMING_DATA

const float MAX_StartCapturingDuration = 5.0f; // max duration to wait for ANVSceneCapturerActor::StartCapturing to successfully begin capturing before emitting warning messages

ANVSceneCapturerActor::ANVSceneCapturerActor(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("Root"));
    CollisionComponent->SetSphereRadius(20.f);
    CollisionComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    CollisionComponent->SetCollisionResponseToAllChannels(ECR_Block);
    RootComponent = CollisionComponent;

    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
    PrimaryActorTick.bTickEvenWhenPaused = true;


    TimeBetweenSceneCapture = 0.f;
    LastCaptureTimestamp = 0.f;

    bIsActive = true;
    CurrentState = ENVSceneCapturerState::Active;
    bAutoStartCapturing = false;
    bPauseGameLogicWhenFlushing = true;

    MaxNumberOfFramesToCapture = 0;
    NumberOfFramesToCapture = MaxNumberOfFramesToCapture;

    CachedPlayerControllerViewTarget = nullptr;

    CapturedDuration = 0.f;
    StartCapturingDuration = 0.0f;
    StartCapturingTimestamp = 0.f;
    bNeedToExportScene = false;
    bTakingOverViewport = false;
    bSkipFirstFrame = false;

#if WITH_EDITORONLY_DATA
    USelection::SelectObjectEvent.AddUObject(this, &ANVSceneCapturerActor::OnActorSelected);
#endif //WITH_EDITORONLY_DATA
}

void ANVSceneCapturerActor::PostLoad()
{
    Super::PostLoad();

    NumberOfFramesToCapture = MaxNumberOfFramesToCapture;
}

void ANVSceneCapturerActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

	StartCapturing();

	CheckCaptureScene();

}

void ANVSceneCapturerActor::UpdateSettingsFromCommandLine()
{
    const auto CommandLine = FCommandLine::Get();

    // TODO: Scan the setting's properties and auto check for overrided value
    FString OutputPathOverride;
    if (FParse::Value(CommandLine, TEXT("-OutputPath="), OutputPathOverride))
    {
        // FIXME: Let the SceneDataExporter update the settings from the commandline itself
        UNVSceneDataExporter* CurrentSceneDataExporter = Cast<UNVSceneDataExporter>(SceneDataHandler);
        if (CurrentSceneDataExporter)
        {
            CurrentSceneDataExporter->CustomDirectoryName = OutputPathOverride;
            CurrentSceneDataExporter->bUseMapNameForCapturedDirectory = false;
        }
    }

    int32 NumberOfFrameToCaptureOverride = 0;
    if (FParse::Value(CommandLine, TEXT("-NumberOfFrame="), NumberOfFrameToCaptureOverride))
    {
        NumberOfFramesToCapture = NumberOfFrameToCaptureOverride;
        // TODO: Separated bAutoStartExporting to a different switch?
        bAutoStartCapturing = true;
    }

    FString SettingsFilePath;
    if (FParse::Value(CommandLine, TEXT("-SettingsPath="), SettingsFilePath))
    {
        const FString SettingsFullpath = FPaths::Combine(FPaths::ProjectDir(), SettingsFilePath);
        FString SettingsStr = TEXT("");
        if (FFileHelper::LoadFileToString(SettingsStr, *SettingsFullpath))
        {
            TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(SettingsStr);

            TSharedPtr<FJsonObject> SettingsJsonObject;
            if (FJsonSerializer::Deserialize(JsonReader, SettingsJsonObject) && SettingsJsonObject.IsValid())
            {
                FNVSceneCapturerSettings OverridedSettings;
                int64 CheckFlags = 0;
                int64 SkipFlags = 0;
                if (FJsonObjectConverter::JsonObjectToUStruct(SettingsJsonObject.ToSharedRef(), FNVSceneCapturerSettings::StaticStruct(), &OverridedSettings, CheckFlags, SkipFlags))
                {
                    CapturerSettings = OverridedSettings;
                }
            }
        }
    }
}

//#miker: effectively a poor mans lazy loading
// may need this delay for packaged builds
void ANVSceneCapturerActor::RepeatingFunction()
{
	if (--RepeatingCallsRemaining <= 0)
	{
		//UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: RepeatingFunction ANVSceneCapturerActor"));
		GetWorldTimerManager().ClearTimer(MemberTimerHandle);
	}
}

//#miker:
const float DELAYBEGINDELAYSCA = 2.0f;

void ANVSceneCapturerActor::BeginPlay()
{
    Super::BeginPlay();
	//GetWorldTimerManager().SetTimer(MemberTimerHandle, this, &ANVSceneCapturerActor::RepeatingFunction, 1.0f, true, DELAYBEGINDELAYSCA);	
	UpdateSettingsFromCommandLine();

	UWorld* World = GetWorld();
#if WITH_EDITOR
	bool bIsSimulating = GUnrealEd ? (GUnrealEd->bIsSimulatingInEditor || GUnrealEd->bIsSimulateInEditorQueued) : false;
	if (!World || !World->IsGameWorld() || bIsSimulating)
	{
		return;
	}
#endif

	bTakingOverViewport = bTakeOverGameViewport;
	if (bTakeOverGameViewport)
	{
		TakeOverViewport();
	}

	bNeedToExportScene = false;
	bSkipFirstFrame = true;

	UpdateViewpointList();

	// Create the feature extractors for each viewpoint
	for (UNVSceneCapturerViewpointComponent* CheckViewpointComp : ViewpointList)
	{
		if (CheckViewpointComp && CheckViewpointComp->IsEnabled())
		{
			CheckViewpointComp->SetupFeatureExtractors();
		}
	}

	// bIsActive is public property for UI. so we need to copy to protected property
	// to avoid access from user while we are captuering.
	CurrentState = bIsActive ? ENVSceneCapturerState::Active : ENVSceneCapturerState::NotActive;
	if (bAutoStartCapturing && (CurrentState == ENVSceneCapturerState::Active))
	{
		// NOTE: We need to delay the capturing so the scene have time to set up
		//#miker:
		// scene capturing is now state driven via item drop controller
		/*const float DelayDuration = 1.f;
		GetWorldTimerManager().SetTimer(TimeHandle_StartCapturingDelay,
			this,
			&ANVSceneCapturerActor::StartCapturing,
			DelayDuration,
			false,
			DelayDuration);
			*/
		StartCapturingDuration = 0;
	}

	if (SceneDataVisualizer)
	{
		SceneDataVisualizer->Init();
	}
}

//#miker:
void ANVSceneCapturerActor::restartCaptureActor()
{
	//UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: restartCaptureActor"));
	m_BGCapturing = false;
	StartCapturingDuration = 0;
	BGNumberOfFramesToCapture = 1;

	// #miker: clear out timing data
	// otherwise there is accumulation when conducting multiple
	// sim runs within a single "play" session
	//
	CapturedDuration = 0.f;
	StartCapturingDuration = 0.0f;
	StartCapturingTimestamp = 0.f;

	GetWorldTimerManager().ClearTimer(TimeHandle_StartCapturingDelay);
}

void ANVSceneCapturerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    GetWorldTimerManager().ClearTimer(TimeHandle_StartCapturingDelay);

    Super::EndPlay(EndPlayReason);
}

void ANVSceneCapturerActor::PostInitializeComponents()
{
    Super::PostInitializeComponents();

    UpdateViewpointList();
}

#if WITH_EDITORONLY_DATA
void ANVSceneCapturerActor::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
    const UProperty* PropertyThatChanged = PropertyChangedEvent.MemberProperty;
    if (PropertyThatChanged)
    {
        const FName ChangedPropName = PropertyThatChanged->GetFName();
        if ((ChangedPropName == GET_MEMBER_NAME_CHECKED(ANVSceneCapturerActor, CapturerSettings)))
        {
            CapturerSettings.PostEditChangeProperty(PropertyChangedEvent);
        }

        Super::PostEditChangeProperty(PropertyChangedEvent);
    }
}

void ANVSceneCapturerActor::OnActorSelected(UObject* Object)
{
    if (Object == this)
    {
        TArray<UActorComponent*> ChildComponents = GetComponentsByClass(UNVSceneCapturerViewpointComponent::StaticClass());
        for (int i = 0; i < ChildComponents.Num(); i++)
        {
            UNVSceneCapturerViewpointComponent* CheckViewpointComp = Cast<UNVSceneCapturerViewpointComponent>(ChildComponents[i]);
            if (CheckViewpointComp && CheckViewpointComp->IsEnabled())
            {
                GSelectedComponentAnnotation.Set(CheckViewpointComp);
                break;
            }
        }
    }
}

#endif //WITH_EDITORONLY_DATA

void ANVSceneCapturerActor::CheckCaptureScene()
{
#if WITH_EDITOR
    bool bIsSimulating = GUnrealEd ? (GUnrealEd->bIsSimulatingInEditor || GUnrealEd->bIsSimulateInEditorQueued) : false;
    if (bIsSimulating)
    {
        return;
    }
#endif

    if (CurrentState == ENVSceneCapturerState::Running)
    {
        const float CurrentTime = GetWorld()->GetTimeSeconds();
        const float TimeSinceLastCapture = CurrentTime - LastCaptureTimestamp;
        if (TimeSinceLastCapture >= TimeBetweenSceneCapture)
        {
            bNeedToExportScene = true;
        }
        CapturedDuration = GetWorld()->GetRealTimeSeconds() - StartCapturingTimestamp;
    }
    else
    {
        bNeedToExportScene = false;
    }

    ANVSceneManager* ANVSceneManagerPtr = ANVSceneManager::GetANVSceneManagerPtr();
    // if ANVSceneManagerPtr is nullptr, then there's no scene manager and it's 
	// assumed the scene is static and thus ready, else check with the scene manager
    const bool bSceneIsReady = !ANVSceneManagerPtr || ANVSceneManagerPtr->GetState()== ENVSceneManagerState::Ready;
    UWorld* World = GetWorld();
    AGameModeBase* CurrentGameMode = World ? World->GetAuthGameMode() : nullptr;
	ENVSceneManagerState state = ANVSceneManagerPtr->GetState();

    if (bNeedToExportScene && bSceneIsReady )
    {
        if (CanHandleMoreSceneData())
        {
            // Must unpause the game in this frame first before resuming capturing it
            if (CurrentGameMode && CurrentGameMode->IsPaused())
            {
                CurrentGameMode->ClearPause();
            }
            else
            {
				//UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: writing pixels..."));					
				CaptureSceneToPixelsData();					
				// Update the capturer settings at the end of the frame after we already captured data of this frame
				UpdateCapturerSettings();				
            }
        }
        if (!CanHandleMoreSceneData())
        {
            // Pause the game logic if we can't handle more scene data right now
            if (bPauseGameLogicWhenFlushing && CurrentGameMode && !CurrentGameMode->IsPaused())
            {
                CurrentGameMode->SetPause(World->GetFirstPlayerController());
            }
        }
    }
}

void ANVSceneCapturerActor::ResetCounter()
{
    CapturedFrameCounter.Reset();
}

void ANVSceneCapturerActor::CaptureSceneToPixelsData()
{
    const int32 CurrentFrameIndex = CapturedFrameCounter.GetTotalFrameCount();
	const int FrameIndexForFile = m_overallFrameAccumulator;
	const int FramePicksetForFile = m_picksetFrameAccumulator;
	const int PicksetSubImage = m_picksetSubImage;

#ifdef BGDEBUG_CAPTURESCENE_FILE
	UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: CaptureSceneToPixelsData %d.%d"), FrameIndexForFile, FramePicksetForFile);
#endif
    bool bFinishedCapturing = (NumberOfFramesToCapture > 0) && (CurrentFrameIndex >= NumberOfFramesToCapture);

    const float CurrentTime = GetWorld()->GetTimeSeconds();
    const float TimePassSinceLastCapture = CurrentTime - LastCaptureTimestamp;

    // Let all the child exporter components know it need to export the scene
    if (!bFinishedCapturing)
    {		
        for (UNVSceneCapturerViewpointComponent* ViewpointComp : ViewpointList)
        {
            if (ViewpointComp && ViewpointComp->IsEnabled())
            {
			    ViewpointComp->CaptureSceneToPixelsData(
                    [this, FrameIndexForFile,FramePicksetForFile,PicksetSubImage](const FNVTexturePixelData& CapturedPixelData, 
						UNVSceneFeatureExtractor_PixelData* CapturedFeatureExtractor, 
						UNVSceneCapturerViewpointComponent* CapturedViewpoint)
                {
                    if (SceneDataHandler)
                    {
						
						if (CapturedFeatureExtractor->bIsEnabled)
						{
							SceneDataHandler->HandleScenePixelsData(CapturedPixelData,
								CapturedFeatureExtractor,
								CapturedViewpoint,
								FrameIndexForFile, FramePicksetForFile,PicksetSubImage);
						}						
                    }
					

                    if (SceneDataVisualizer)
                    {
						if (CapturedFeatureExtractor->bIsEnabled)
						{
							SceneDataVisualizer->HandleScenePixelsData(CapturedPixelData,
								CapturedFeatureExtractor,
								CapturedViewpoint,
								FrameIndexForFile, FramePicksetForFile,PicksetSubImage);
						}
                    }
                });

                ViewpointComp->CaptureSceneAnnotationData(
                    [this, FrameIndexForFile, FramePicksetForFile, PicksetSubImage]
						(const TSharedPtr<FJsonObject>& CapturedData,
							UNVSceneFeatureExtractor_AnnotationData* CapturedFeatureExtractor,
							UNVSceneCapturerViewpointComponent* CapturedViewpoint
							)
                {
                    if (SceneDataHandler)
                    {
                        SceneDataHandler->HandleSceneAnnotationData(CapturedData,
                                CapturedFeatureExtractor,
                                CapturedViewpoint,
							FrameIndexForFile, FramePicksetForFile, PicksetSubImage);
                    }
                });
            }
        }

        if (bSkipFirstFrame)
        {
            bSkipFirstFrame = false;
        }
        else
        {
            CapturedFrameCounter.IncreaseFrameCount();
        }
        CapturedFrameCounter.AddFrameDuration(TimePassSinceLastCapture);
    }
    else
    {
        bool bFinishedProcessingData = true;
        // Make sure all the captured scene data are processed
        if (SceneDataHandler)
        {
            bFinishedProcessingData = !SceneDataHandler->IsHandlingData();
        }

        if (bFinishedProcessingData)
        {
			//#miker: update file index
            OnCompleted();
			// use current frame counter to repurpose as a file index
			m_currentFrameIndex = CurrentFrameIndex;
			//rebase for use on upcoming frame
			//if (m_currentFrameIndex == m_lastFrameIndex)
			//{
			//	++m_accumulatedFrameIndex;
			//}
			m_lastFrameIndex = CurrentFrameIndex;
        }
    }

    LastCaptureTimestamp = CurrentTime;
}

void ANVSceneCapturerActor::UpdateCapturerSettings()
{
    CapturerSettings.RandomizeSettings();
    for (auto* ViewpointComp : ViewpointList)
    {
        ViewpointComp->UpdateCapturerSettings();
    }
}

void ANVSceneCapturerActor::SetNumberOfFramesToCapture(int32 NewSceneCount)
{
    NumberOfFramesToCapture = NewSceneCount;
}

void ANVSceneCapturerActor::StartCapturing()
{
	//#miker: 
	// do NOT capture if the simulation controller is inserting objects into tote
	if (!isBGControllerDone() || m_BGCapturing) 
	{
		return; 
	}

	m_BGCapturing = true;
    bNeedToExportScene = false;
    bSkipFirstFrame = true;
    ANVSceneManager* ANVSceneManagerPtr = ANVSceneManager::GetANVSceneManagerPtr();
    // if ANVSceneManagerPtr is nullptr, then there's no scene manager and it's assumed
	//  the scene is static and thus ready, else check with the scene manager
	const bool bSceneIsReady =  !ANVSceneManagerPtr || ANVSceneManagerPtr->GetState() == ENVSceneManagerState::Ready;
    if (bSceneIsReady)
    {
        StartCapturing_Internal();
    }	
    else if (!TimeHandle_StartCapturingDelay.IsValid())
    {
        const float DelayDuration = 1.f; 
        // NOTE: We need to delay the capturing so the scene have time to set up
        GetWorldTimerManager().SetTimer(TimeHandle_StartCapturingDelay,
                                        this,
                                        &ANVSceneCapturerActor::StartCapturing,
                                        DelayDuration,
                                        false,
                                        DelayDuration);
		
        StartCapturingDuration += DelayDuration;
        if (StartCapturingDuration > MAX_StartCapturingDuration)
        {
            UE_LOG(LogNVSceneCapturer, Warning, TEXT("Capturing could not Start -- did you set up the Game Mode?\nStartCapturingDuration: %.6f"),
                StartCapturingDuration);
        }
    }
    else
    {
        UE_LOG(LogNVSceneCapturer, Error, TEXT("Capturing could not Start -- did you set up the Game Mode?"));
    }
	
}
void ANVSceneCapturerActor::StartCapturing_Internal()
{
    if (!bIsActive)
    {
        // bIsActive is public. we need to copy bIsActive state into the protected value.
        CurrentState = ENVSceneCapturerState::NotActive;
    }
    else
    {
        // To start capture, SceneDataHandler is requirement.
        if (!SceneDataHandler)
        {
            UE_LOG(LogNVSceneCapturer, Error, TEXT("SceneCapturer SceneDataHandler is empty. Please select data handler in details panel."));
        }
        else
        {			
            ANVSceneManager* NVSceneManagerPtr = ANVSceneManager::GetANVSceneManagerPtr();
		
			//#miker:
            if (ensure(NVSceneManagerPtr))
            {
                // Make sure the segmentation mask of objects in the scene are up-to-date before capturing them
                NVSceneManagerPtr->UpdateSegmentationMask(m_vertColor,0,m_bgAlternateFECount);
				updateComponentFeatureExtractorList();
		   }

            // Now we can start capture.
            UpdateCapturerSettings();
			
			
            OnStartedEvent.Broadcast(this);

			//#miker: update write folder overwrite and folder
			SceneDataHandler->setBGTargetFolderOverride(m_useBGTargetOverride, m_simulationSave);

            SceneDataHandler->OnStartCapturingSceneData();

            GetWorldTimerManager().ClearTimer(TimeHandle_StartCapturingDelay);
            TimeHandle_StartCapturingDelay.Invalidate();

            // Reset the counter and stats
            ResetCounter();
            // bIsActive is public. we need to copy bIsActive state into the protected value.
            CurrentState = ENVSceneCapturerState::Running;
            // NOTE: Make it wait till the next frame to start exporting since the scene capturer only just start capturing now
			//#miker: this is terrible... if there is every any sort
			// of frame stall caused by really anything than this just accumulates
			// perpetually...so it appears to only be used ensuring that next grab
			// just use the last capture plus some fudge to ensure exports occur on the
			// next frame and not 13frames in the future :-(
			StartCapturingTimestamp = LastCaptureTimestamp + 0.1f;// GetWorld()->GetRealTimeSeconds();
            LastCaptureTimestamp = StartCapturingTimestamp + 0.1f;

            // Let all the viewpoint component start capturing
            for (UNVSceneCapturerViewpointComponent* ViewpointComp : ViewpointList)
           {
                ViewpointComp->StartCapturing();
            }
			
        }
    }
}
//#miker:
//pump updated fe's into the scenes feature extractor list
// (which is used for providing some base capturing data to the 
// exporter)
//
void ANVSceneCapturerActor::updateComponentFeatureExtractorList()
{
	for (UNVSceneCapturerViewpointComponent* viewpoint_comp : ViewpointList)
	{
		//update any previously registered fe's with the scene feature extractor
		const int fe_cnt = FeatureExtractorSettings.Num();

		for (int i = 0; i < fe_cnt; ++i)
		{
			auto& fe = FeatureExtractorSettings[i];
			UNVSceneFeatureExtractor* ccref = fe.FeatureExtractorRef;
			FString fe_name = ccref->GetDisplayName();
			
			//#miker
			const bool update_enable_to = ccref->IsEnabled();
			//const FString miker = FString::Printf(TEXT("#FeaureExtractor Status: %s - %d"), *fe_name,update_enable_to);
			//GLog->Log(miker);

			ccref->updateFE(viewpoint_comp, i, update_enable_to);
		}
	}
}
void ANVSceneCapturerActor::StopCapturing()
{
    ensure(CurrentState != ENVSceneCapturerState::NotActive);
    ensure(CurrentState != ENVSceneCapturerState::Active);

    GetWorldTimerManager().ClearTimer(TimeHandle_StartCapturingDelay);
    TimeHandle_StartCapturingDelay.Invalidate();

    for (UNVSceneCapturerViewpointComponent* ViewpointComp : ViewpointList)
    {
        ViewpointComp->StopCapturing();
    }

    CurrentState = ENVSceneCapturerState::Active;

    OnStoppedEvent.Broadcast(this);

    if (SceneDataHandler)
    {
        SceneDataHandler->OnStopCapturingSceneData();
    }
    ResetCounter();
}

void ANVSceneCapturerActor::PauseCapturing()
{
    if (CurrentState == ENVSceneCapturerState::Running)
    {
        CurrentState = ENVSceneCapturerState::Paused;
    }
}

void ANVSceneCapturerActor::ResumeCapturing()
{
    if (CurrentState == ENVSceneCapturerState::Paused)
    {
        CurrentState = ENVSceneCapturerState::Running;
    }
}

void ANVSceneCapturerActor::OnCompleted()
{
    if (CurrentState == ENVSceneCapturerState::Running)
    {
        const float CompletedCapturingTimestamp = GetWorld()->GetRealTimeSeconds();
        const float CapturingDuration = CompletedCapturingTimestamp - StartCapturingTimestamp;

		//#miker:
#ifdef BGDEBUG_LOG_FRAME_TIMING_DATA
		 UE_LOG(LogNVSceneCapturer, Log,
			  TEXT("Capture stats: duration: %.6f"),// Start: %.6f End: %.6f"),
            CapturedDuration, StartCapturingTimestamp, CompletedCapturingTimestamp);
#endif
        for (UNVSceneCapturerViewpointComponent* ViewpointComp : ViewpointList)
        {
            ViewpointComp->StopCapturing();
        }
		
        CurrentState = ENVSceneCapturerState::Completed; 
		if (SceneDataHandler)
        {
            SceneDataHandler->OnCapturingCompleted();
        }
		
        OnCompletedEvent.Broadcast(this, true);
    }
}

bool ANVSceneCapturerActor::ToggleTakeOverViewport()
{
    bTakingOverViewport = !bTakingOverViewport;
    if (bTakingOverViewport)
    {
        TakeOverViewport();
    }
    else
    {
        ReturnViewportToPlayerController();
    }
    return bTakingOverViewport;
}

void ANVSceneCapturerActor::TakeOverViewport()
{
    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController)
    {
        AActor* CurrentPCViewTarget = PlayerController->GetViewTarget();
        if (CurrentPCViewTarget != this)
        {
            CachedPlayerControllerViewTarget = CurrentPCViewTarget;
        }
        PlayerController->SetViewTarget(this);
    }
}

void ANVSceneCapturerActor::ReturnViewportToPlayerController()
{
    APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
    if (PlayerController)
    {
        AActor* CurrentPCViewTarget = PlayerController->GetViewTarget();
        if (CurrentPCViewTarget == this)
        {
            PlayerController->SetViewTarget(CachedPlayerControllerViewTarget);
            CachedPlayerControllerViewTarget = nullptr;
        }
    }
}

float ANVSceneCapturerActor::GetCapturedFPS() const
{
    return CapturedFrameCounter.GetFPS();
}

int32 ANVSceneCapturerActor::GetNumberOfFramesLeftToCapture() const
{
    // NOTE: -1 mean there is infinite number of frames need to be captured
    return (NumberOfFramesToCapture > 0) ? (NumberOfFramesToCapture - CapturedFrameCounter.GetTotalFrameCount()) : -1;
}

float ANVSceneCapturerActor::GetEstimatedTimeUntilFinishCapturing() const
{
    float EstRemainTime = -1.f;

    const float CapturedProgressFraction = GetCaptureProgressFraction();
    if (CapturedProgressFraction < 1.f)
    {
        const float FullTimeEstimated = (CapturedProgressFraction > 0.f) ? (CapturedDuration / CapturedProgressFraction) : 0.f;
        EstRemainTime = FullTimeEstimated * (1 - CapturedProgressFraction);
    }

    return EstRemainTime;
}

float ANVSceneCapturerActor::GetCaptureProgressFraction() const
{
    return (NumberOfFramesToCapture > 0) ? (float(CapturedFrameCounter.GetTotalFrameCount()) / NumberOfFramesToCapture) : -1.f;
}

float ANVSceneCapturerActor::GetCapturedDuration() const
{
    return CapturedDuration;
}

TArray<FNVNamedImageSizePreset> const& ANVSceneCapturerActor::GetImageSizePresets()
{
    return GetDefault<ANVSceneCapturerActor>()->ImageSizePresets;
}

TArray<UNVSceneCapturerViewpointComponent*> ANVSceneCapturerActor::GetViewpointList()
{
    // TODO: Should only update viewpoint list if there are viewpoint components added or removed
    UpdateViewpointList();

    return ViewpointList;
}

UNVSceneDataHandler* ANVSceneCapturerActor::GetSceneDataHandler() const
{
	return SceneDataHandler;
}

UNVSceneDataVisualizer* ANVSceneCapturerActor::GetSceneDataVisualizer() const
{
	return SceneDataVisualizer;
}

void ANVSceneCapturerActor::UpdateViewpointList()
{
    ensure(CurrentState != ENVSceneCapturerState::Running);
    ensure(CurrentState != ENVSceneCapturerState::Paused);
    if ((CurrentState != ENVSceneCapturerState::Running) &&
            (CurrentState != ENVSceneCapturerState::Paused))
    {
        // Keep track of all the child viewpoint components
        TArray<UActorComponent*> ChildComponents = this->GetComponentsByClass(UNVSceneCapturerViewpointComponent::StaticClass());
        ViewpointList.Reset(ChildComponents.Num());
        for (int i = 0; i < ChildComponents.Num(); i++)
        {
            UNVSceneCapturerViewpointComponent* CheckViewpoint = Cast<UNVSceneCapturerViewpointComponent>(ChildComponents[i]);
            if (CheckViewpoint)
            {
                ViewpointList.Add(CheckViewpoint);
            }
        }

        ViewpointList.Sort([](const UNVSceneCapturerViewpointComponent& A, const UNVSceneCapturerViewpointComponent& B)
        {
            return A.GetDisplayName() < B.GetDisplayName();
        });
    }
}

bool ANVSceneCapturerActor::CanHandleMoreSceneData() const
{
    return (SceneDataHandler && SceneDataHandler->CanHandleMoreData());
}


void ANVSceneCapturerActor::storeBGSimItemActor(AActor* sim_item)
{
	//UE_LOG(LogNVSceneCapturer, Warning, TEXT("#miker: storeBGSimItemActor %s"), *(sim_item->GetName()));
	m_simItem = sim_item;
}

void ANVSceneCapturerActor::resetBGSimItemActor()
{
	m_simItem = nullptr;
}
