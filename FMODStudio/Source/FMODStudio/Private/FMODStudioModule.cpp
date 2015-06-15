// Copyright (c), Firelight Technologies Pty, Ltd. 2012-2015.

#include "FMODStudioPrivatePCH.h"

#include "FMODSettings.h"
#include "FMODStudioModule.h"
#include "FMODAudioComponent.h"
#include "FMODBlueprintStatics.h"
#include "FMODAssetTable.h"
#include "FMODFileCallbacks.h"
#include "FMODBankUpdateNotifier.h"
#include "FMODUtils.h"
#include "FMODEvent.h"
#include "FMODListener.h"
#include "FMODSnapshotReverb.h"
#include "FMODStudioOculusModule.h"

#include "fmod_studio.hpp"
#include "fmod_errors.h"

#if PLATFORM_PS4
#include "FMODPlatformLoadDll_PS4.h"
#elif PLATFORM_XBOXONE
#include "FMODPlatformLoadDll_XBoxOne.h"
#else
#include "FMODPlatformLoadDll_Generic.h"
#endif

#define LOCTEXT_NAMESPACE "FMODStudio"

DEFINE_LOG_CATEGORY(LogFMOD);

struct FFMODSnapshotEntry
{
	FFMODSnapshotEntry(UFMODSnapshotReverb* InSnapshot=nullptr, FMOD::Studio::EventInstance* InInstance=nullptr)
	:	Snapshot(InSnapshot),
		Instance(InInstance),
		StartTime(0.0),
		FadeDuration(0.0f),
		FadeIntensityStart(0.0f),
		FadeIntensityEnd(0.0f)
	{
	}

	float CurrentIntensity() const
	{
		double CurrentTime = FApp::GetCurrentTime();
		if (StartTime + FadeDuration <= CurrentTime)
		{
			return FadeIntensityEnd;
		}
		else
		{
			float Factor = (CurrentTime - StartTime) / FadeDuration;
			return FMath::Lerp<float>(FadeIntensityStart, FadeIntensityEnd, Factor);
		}
	}

	void FadeTo(float Target, float Duration)
	{
		float StartIntensity = CurrentIntensity();

		StartTime = FApp::GetCurrentTime();
		FadeDuration = Duration;
		FadeIntensityStart = StartIntensity;
		FadeIntensityEnd = Target;
	}

	UFMODSnapshotReverb* Snapshot;
	FMOD::Studio::EventInstance* Instance;
	double StartTime;
	float FadeDuration;
	float FadeIntensityStart;
	float FadeIntensityEnd;
};

class FFMODStudioModule : public IFMODStudioModule
{
public:
	/** IModuleInterface implementation */
	FFMODStudioModule()
	:	AuditioningInstance(nullptr),
        ListenerCount(1),
		bSimulating(false),
		bIsInPIE(false),
		bUseSound(true),
		bListenerMoved(true),
        bAllowLiveUpdate(true),
		LowLevelLibHandle(nullptr),
		StudioLibHandle(nullptr)
	{
		for (int i=0; i<EFMODSystemContext::Max; ++i)
		{
			StudioSystem[i] = nullptr;
		}
	}

	virtual void StartupModule() override;
	virtual void PostLoadCallback() override;
	virtual void ShutdownModule() override;

	FString GetDllPath(const TCHAR* ShortName);
	void* LoadDll(const TCHAR* ShortName);

	bool LoadLibraries();

	void LoadBanks(EFMODSystemContext::Type Type);

	/** Called when a newer version of the bank files was detected */
	void HandleBanksUpdated();

	void CreateStudioSystem(EFMODSystemContext::Type Type);
	void DestroyStudioSystem(EFMODSystemContext::Type Type);

	bool Tick( float DeltaTime );

	void UpdateViewportPosition();

	virtual FMOD::Studio::System* GetStudioSystem(EFMODSystemContext::Type Context) override;
	virtual FMOD::Studio::EventDescription* GetEventDescription(const UFMODEvent* Event, EFMODSystemContext::Type Type) override;
	virtual FMOD::Studio::EventInstance* CreateAuditioningInstance(const UFMODEvent* Event) override;
	virtual void StopAuditioningInstance() override;

	virtual void SetListenerPosition(int ListenerIndex, UWorld* World, const FTransform& ListenerTransform, float DeltaSeconds) override;
	virtual void FinishSetListenerPosition(int ListenerCount, float DeltaSeconds) override;

	virtual const FFMODListener& GetNearestListener(const FVector& Location) override;

	virtual bool HasListenerMoved() override;

	virtual void RefreshSettings();

	virtual void SetSystemPaused(bool paused) override;

	virtual void SetInPIE(bool bInPIE, bool simulating) override;

	virtual UFMODAsset* FindAssetByName(const FString& Name) override;
	virtual UFMODEvent* FindEventByName(const FString& Name) override;

	FSimpleMulticastDelegate BanksReloadedDelegate;
	virtual FSimpleMulticastDelegate& BanksReloadedEvent() override
	{
		return BanksReloadedDelegate;
	}

	virtual bool UseSound() override
	{
		return bUseSound;
	}

	virtual bool LoadPlugin(const TCHAR* ShortName) override;

	virtual void LogError(int result, const char* function) override;

	void ResetInterpolation();

	/** The studio system handle. */
	FMOD::Studio::System* StudioSystem[EFMODSystemContext::Max];
	FMOD::Studio::EventInstance* AuditioningInstance;

	/** The delegate to be invoked when this profiler manager ticks. */
	FTickerDelegate OnTick;

	/** Handle for registered TickDelegate. */
	FDelegateHandle TickDelegateHandle;

	/** Table of assets with name and guid */
	FFMODAssetTable AssetTable;

	/** Periodically checks for updates of the strings.bank file */
	FFMODBankUpdateNotifier BankUpdateNotifier;

	/** Listener information */
#if FMOD_VERSION >= 0x00010600
	static const int MAX_LISTENERS = FMOD_MAX_LISTENERS;
#else
	static const int MAX_LISTENERS = 1;
#endif
	FFMODListener Listeners[MAX_LISTENERS];
	int ListenerCount;

	/** Current snapshot applied via reverb zones*/
	TArray<FFMODSnapshotEntry> ReverbSnapshots;

	/** True if simulating */
	bool bSimulating;
	
	/** True if in PIE */
	bool bIsInPIE;

	/** True if we want sound enabled */
	bool bUseSound;

	/** True if we the listener has moved and may have changed audio settings*/
	bool bListenerMoved;

	/** True if we allow live update */
	bool bAllowLiveUpdate;

	/** Dynamic library handles */
	void* LowLevelLibHandle;
	void* StudioLibHandle;
};

IMPLEMENT_MODULE( FFMODStudioModule, FMODStudio )

void FFMODStudioModule::LogError(int result, const char* function)
{
	FString ErrorStr(ANSI_TO_TCHAR(FMOD_ErrorString((FMOD_RESULT)result)));
	FString FunctionStr(ANSI_TO_TCHAR(function));
	UE_LOG(LogFMOD, Error, TEXT("'%s' returned '%s'"), *FunctionStr, *ErrorStr);
}

bool FFMODStudioModule::LoadPlugin(const TCHAR* ShortName)
{
	UE_LOG(LogFMOD, Log, TEXT("Loading plugin '%s'"), ShortName);

	static const int ATTEMPT_COUNT = 2;
	static const TCHAR* AttemptPrefixes[ATTEMPT_COUNT] = 
	{
		TEXT(""),
#if PLATFORM_64BITS
		TEXT("64")
#else
		TEXT("32")
#endif
	};

	FMOD::System* LowLevelSystem = nullptr;
	verifyfmod(StudioSystem[EFMODSystemContext::Runtime]->getLowLevelSystem(&LowLevelSystem));

	FMOD_RESULT PluginLoadResult;
	for (int attempt=0; attempt<2; ++attempt)
	{
		FString AttemptName = FString(ShortName) + AttemptPrefixes[attempt];
		FString PluginPath = GetDllPath(*AttemptName);

		UE_LOG(LogFMOD, Log, TEXT("Trying to load plugin file at location: %s"), *PluginPath);

		unsigned int Handle = 0;
		PluginLoadResult = LowLevelSystem->loadPlugin(TCHAR_TO_UTF8(*PluginPath), &Handle, 0);
		if (PluginLoadResult == FMOD_OK)
		{
			UE_LOG(LogFMOD, Log, TEXT("Loaded plugin %s"), ShortName);
			return true;
		}
	}
	UE_LOG(LogFMOD, Error, TEXT("Failed to load plugin '%s', sounds may not play"), ShortName);
	return false;
}

void* FFMODStudioModule::LoadDll(const TCHAR* ShortName)
{
	FString LibPath = GetDllPath(ShortName);

	void* Handle = nullptr;
	UE_LOG(LogFMOD, Log, TEXT("FFMODStudioModule::LoadDll: Loading %s"), *LibPath);
	// Unfortunately Unreal's platform loading code hasn't been implemented on all platforms so we wrap it
	Handle = FMODPlatformLoadDll(*LibPath);
#if WITH_EDITOR
	if (!Handle)
	{
		FString Message = TEXT("Couldn't load FMOD DLL ") + LibPath;
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *Message, TEXT("Error"));
	}
#endif
	if (!Handle)
	{
		UE_LOG(LogFMOD, Error, TEXT("Failed to load FMOD DLL '%s', FMOD sounds will not play!"), *LibPath);
	}
	return Handle;
}

FString FFMODStudioModule::GetDllPath(const TCHAR* ShortName)
{
#if PLATFORM_MAC
	return FString::Printf(TEXT("%s/Binaries/ThirdParty/FMODStudio/Mac/lib%s.dylib"), *FPaths::EngineDir(), ShortName);
#elif PLATFORM_PS4
	return FString::Printf(TEXT("/app0/sce_sys/lib%s.prx"), ShortName);
#elif PLATFORM_XBOXONE
	return FString::Printf(TEXT("%s.dll"), ShortName);
#elif PLATFORM_ANDROID
	return FString::Printf(TEXT("lib%s.so"), ShortName);
#elif PLATFORM_64BITS
	return FString::Printf(TEXT("%s/Binaries/ThirdParty/FMODStudio/Win64/%s.dll"), *FPaths::EngineDir(), ShortName);
#else
	return FString::Printf(TEXT("%s/Binaries/ThirdParty/FMODStudio/Win32/%s.dll"), *FPaths::EngineDir(), ShortName);
#endif
}

bool FFMODStudioModule::LoadLibraries()
{
#if PLATFORM_IOS || PLATFORM_ANDROID || PLATFORM_LINUX
	return true; // Nothing to do on those platforms
#else
	UE_LOG(LogFMOD, Verbose, TEXT("FFMODStudioModule::LoadLibraries"));

#if defined(FMODSTUDIO_LINK_DEBUG)
	FString ConfigName = TEXT("D");
#elif defined(FMODSTUDIO_LINK_LOGGING)
	FString ConfigName = TEXT("L");
#elif defined(FMODSTUDIO_LINK_RELEASE)
	FString ConfigName = TEXT("");
#else
	#error FMODSTUDIO_LINK not defined
#endif

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	ConfigName += TEXT("64");
#endif

	FString LowLevelName = FString(TEXT("fmod")) + ConfigName;
	FString StudioName = FString(TEXT("fmodstudio")) + ConfigName;
	LowLevelLibHandle = LoadDll(*LowLevelName);
	StudioLibHandle = LoadDll(*StudioName);
	return (LowLevelLibHandle != nullptr && StudioLibHandle != nullptr);
#endif
}

void FFMODStudioModule::StartupModule()
{
	UE_LOG(LogFMOD, Log, TEXT("FFMODStudioModule startup"));

	if(FParse::Param(FCommandLine::Get(),TEXT("nosound")) || FApp::IsBenchmarking() || IsRunningDedicatedServer() || IsRunningCommandlet())
	{
		bUseSound = false;
	}

	if(FParse::Param(FCommandLine::Get(),TEXT("noliveupdate")))
	{
		bAllowLiveUpdate = false;
	}

	if (LoadLibraries())
	{
		// Create sandbox system just for asset loading
		AssetTable.Create();
		RefreshSettings();
		
		if (!GIsEditor)
		{
			SetInPIE(true, false);
		}
	}

	OnTick = FTickerDelegate::CreateRaw( this, &FFMODStudioModule::Tick );
	TickDelegateHandle = FTicker::GetCoreTicker().AddTicker( OnTick );

	if (GIsEditor)
	{
		BankUpdateNotifier.BanksUpdatedEvent.AddRaw(this, &FFMODStudioModule::HandleBanksUpdated);
	}
}

inline FMOD_SPEAKERMODE ConvertSpeakerMode(EFMODSpeakerMode::Type Mode)
{
	switch (Mode)
	{
		case EFMODSpeakerMode::Stereo:
			return FMOD_SPEAKERMODE_STEREO;
		case EFMODSpeakerMode::Surround_5_1:
			return FMOD_SPEAKERMODE_5POINT1;
		case EFMODSpeakerMode::Surround_7_1:
			return FMOD_SPEAKERMODE_7POINT1;
		default:
			check(0);
			return FMOD_SPEAKERMODE_DEFAULT;
	};
}

void FFMODStudioModule::CreateStudioSystem(EFMODSystemContext::Type Type)
{
	DestroyStudioSystem(Type);
	if (!bUseSound)
	{
		return;
	}

	UE_LOG(LogFMOD, Verbose, TEXT("CreateStudioSystem"));

	const UFMODSettings& Settings = *GetDefault<UFMODSettings>();

	FMOD_SPEAKERMODE OutputMode = ConvertSpeakerMode(Settings.OutputFormat);
	FMOD_STUDIO_INITFLAGS StudioInitFlags = FMOD_STUDIO_INIT_NORMAL;
	FMOD_INITFLAGS InitFlags = FMOD_INIT_NORMAL;
	if (Type == EFMODSystemContext::Auditioning)
	{
		StudioInitFlags |= FMOD_STUDIO_INIT_ALLOW_MISSING_PLUGINS;
	}
	else if (Type == EFMODSystemContext::Runtime && Settings.bEnableLiveUpdate && bAllowLiveUpdate)
	{
#if (defined(FMODSTUDIO_LINK_DEBUG) ||  defined(FMODSTUDIO_LINK_LOGGING))
		UE_LOG(LogFMOD, Verbose, TEXT("Enabling live update"));
		StudioInitFlags |= FMOD_STUDIO_INIT_LIVEUPDATE;
#endif
	}
	
	FMOD::Debug_Initialize(FMOD_DEBUG_LEVEL_WARNING, FMOD_DEBUG_MODE_CALLBACK, FMODLogCallback);

	verifyfmod(FMOD::Studio::System::create(&StudioSystem[Type]));
	FMOD::System* lowLevelSystem = nullptr;
	verifyfmod(StudioSystem[Type]->getLowLevelSystem(&lowLevelSystem));
	verifyfmod(lowLevelSystem->setSoftwareFormat(0, OutputMode, 0));
	verifyfmod(lowLevelSystem->setFileSystem(FMODOpen, FMODClose, FMODRead, FMODSeek, 0, 0, 2048));
	verifyfmod(StudioSystem[Type]->initialize(256, StudioInitFlags, InitFlags, 0));

	// Don't bother loading plugins during editor, only during PIE or in game
	if (Type == EFMODSystemContext::Runtime)
	{
		for (FString PluginName : Settings.PluginFiles)
		{
			LoadPlugin(*PluginName);
		}
	}
}

void FFMODStudioModule::DestroyStudioSystem(EFMODSystemContext::Type Type)
{
	UE_LOG(LogFMOD, Verbose, TEXT("DestroyStudioSystem"));

	if (StudioSystem[Type])
	{
		verifyfmod(StudioSystem[Type]->release());
		StudioSystem[Type] = nullptr;
	}
}

bool FFMODStudioModule::Tick( float DeltaTime )
{
	bListenerMoved = false;

	if (GIsEditor)
	{
		BankUpdateNotifier.Update();
	}

	if (StudioSystem[EFMODSystemContext::Auditioning])
	{
		verifyfmod(StudioSystem[EFMODSystemContext::Auditioning]->update());
	}
	if (StudioSystem[EFMODSystemContext::Runtime])
	{
		UpdateViewportPosition();

		verifyfmod(StudioSystem[EFMODSystemContext::Runtime]->update());
	}

	return true;
}

void FFMODStudioModule::UpdateViewportPosition()
{
	int ListenerIndex = 0;

	UWorld* ViewportWorld = nullptr;
	if(GEngine && GEngine->GameViewport)
	{
		ViewportWorld = GEngine->GameViewport->GetWorld();
	}

	bool bCameraCut = false; // Not sure how to get View->bCameraCut from here
	float DeltaSeconds = ((bCameraCut || !ViewportWorld) ? 0.f : ViewportWorld->GetDeltaSeconds());

	if (ViewportWorld)
	{
		for( FConstPlayerControllerIterator Iterator = ViewportWorld->GetPlayerControllerIterator(); Iterator; ++Iterator )
		{
			APlayerController* PlayerController = *Iterator;
			if( PlayerController )
			{
				ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(PlayerController->Player);
				if (LocalPlayer)
				{
					FVector Location;
					FVector ProjFront;
					FVector ProjRight;
					PlayerController->GetAudioListenerPosition(/*out*/ Location, /*out*/ ProjFront, /*out*/ ProjRight);
					FVector ProjUp = FVector::CrossProduct(ProjFront, ProjRight);

					FTransform ListenerTransform(FRotationMatrix::MakeFromXY(ProjFront, ProjRight));
					ListenerTransform.SetTranslation(Location);
					ListenerTransform.NormalizeRotation();

					SetListenerPosition(ListenerIndex, ViewportWorld, ListenerTransform, DeltaSeconds);

					ListenerIndex++;
				}
			}
		}
		FinishSetListenerPosition(ListenerIndex, DeltaSeconds);
	}
}

bool FFMODStudioModule::HasListenerMoved()
{
	return bListenerMoved;
}

void FFMODStudioModule::ResetInterpolation()
{
	for (FFMODListener& Listener : Listeners)
	{
		Listener = FFMODListener();
	}
}

const FFMODListener& FFMODStudioModule::GetNearestListener(const FVector& Location)
{
	float BestDistSq = FLT_MAX;
	int BestListener = 0;
	for (int i = 0; i < ListenerCount; ++i)
	{
		const float DistSq = FVector::DistSquared(Location, Listeners[i].Transform.GetTranslation());
		if (DistSq < BestDistSq)
		{
			BestListener = i;
			BestDistSq = DistSq;
		}
	}
	return Listeners[BestListener];
}

// Partially copied from FAudioDevice::SetListener
void FFMODStudioModule::SetListenerPosition(int ListenerIndex, UWorld* World, const FTransform& ListenerTransform, float DeltaSeconds)
{
	FMOD::Studio::System* StudioSystem = IFMODStudioModule::Get().GetStudioSystem(EFMODSystemContext::Runtime);
	if (StudioSystem && ListenerIndex < MAX_LISTENERS)
	{
		FVector ListenerPos = ListenerTransform.GetTranslation();

		FInteriorSettings InteriorSettings;
		AAudioVolume* Volume = World->GetAudioSettings(ListenerPos, NULL, &InteriorSettings);

		Listeners[ListenerIndex].Velocity = DeltaSeconds > 0.f ? 
												(ListenerTransform.GetTranslation() - Listeners[ ListenerIndex ].Transform.GetTranslation()) / DeltaSeconds
												: FVector::ZeroVector;

		Listeners[ListenerIndex].Transform = ListenerTransform;

		Listeners[ListenerIndex].ApplyInteriorSettings(Volume, InteriorSettings);

		// We are using a direct copy of the inbuilt transforms but the directions come out wrong.
		// Several of the audio functions use GetFront() for right, so we do the same here.
		const FVector Up = Listeners[0].GetUp();
		const FVector Right = Listeners[0].GetFront();
		const FVector Forward = Right ^ Up;

		FMOD_3D_ATTRIBUTES Attributes = {{0}};
		Attributes.position = FMODUtils::ConvertWorldVector(ListenerPos);
		Attributes.forward = FMODUtils::ConvertUnitVector(Forward);
		Attributes.up = FMODUtils::ConvertUnitVector(Up);
		Attributes.velocity = FMODUtils::ConvertWorldVector(Listeners[ListenerIndex].Velocity);

#if FMOD_VERSION >= 0x00010600
		// Expand number of listeners dynamically
		if (ListenerIndex >= ListenerCount)
		{
			Listeners[ListenerIndex] = FFMODListener();
			ListenerCount = ListenerIndex+1;
			verifyfmod(StudioSystem->setNumListeners(ListenerCount));
		}
		verifyfmod(StudioSystem->setListenerAttributes(ListenerIndex, &Attributes));
#else
		verifyfmod(StudioSystem->setListenerAttributes(&Attributes));
#endif

		bListenerMoved = true;
	}
}

void FFMODStudioModule::FinishSetListenerPosition(int NumListeners, float DeltaSeconds)
{
	FMOD::Studio::System* StudioSystem = IFMODStudioModule::Get().GetStudioSystem(EFMODSystemContext::Runtime);
	if (!StudioSystem)
	{
		return;
	}

	// Shrink number of listeners if we have less than our current count
	NumListeners = FMath::Min(NumListeners, 1);
	if (StudioSystem && NumListeners < ListenerCount)
	{
		ListenerCount = NumListeners;
#if FMOD_VERSION >= 0x00010600
		verifyfmod(StudioSystem->setNumListeners(ListenerCount));
#endif
	}

	for (int i = 0; i < ListenerCount; ++i)
	{
		Listeners[i].UpdateCurrentInteriorSettings();
	}

	// Apply a reverb snapshot from the listener position(s)
	AAudioVolume* BestVolume = nullptr;
	for (int i = 0; i < ListenerCount; ++i)
	{
		AAudioVolume* CandidateVolume = Listeners[i].Volume;
		if (BestVolume == nullptr || (CandidateVolume != nullptr && CandidateVolume->Priority > BestVolume->Priority))
		{
			BestVolume = CandidateVolume;
		}
	}
	UFMODSnapshotReverb* NewSnapshot = nullptr;
	if (BestVolume && BestVolume->Settings.bApplyReverb)
	{
		NewSnapshot = Cast<UFMODSnapshotReverb>(BestVolume->Settings.ReverbEffect);
	}

	if (NewSnapshot != nullptr)
	{
		FString NewSnapshotName = FMODUtils::LookupNameFromGuid(StudioSystem, NewSnapshot->AssetGuid);
		UE_LOG(LogFMOD, Verbose, TEXT("Starting new snapshot '%s'"), *NewSnapshotName);

		// Try to steal old entry
		FFMODSnapshotEntry SnapshotEntry;
		int SnapshotEntryIndex = -1;
		for (int i=0; i<ReverbSnapshots.Num(); ++i)
		{
			if (ReverbSnapshots[i].Snapshot == NewSnapshot)
			{
				UE_LOG(LogFMOD, Verbose, TEXT("Re-using old entry with intensity %f"), ReverbSnapshots[i].CurrentIntensity());
				SnapshotEntryIndex = i;
				break;
			}
		}
		// Create new instance
		if (SnapshotEntryIndex == -1)
		{
			UE_LOG(LogFMOD, Verbose, TEXT("Creating new instance"));

			FMOD::Studio::ID Guid = FMODUtils::ConvertGuid(NewSnapshot->AssetGuid);
			FMOD::Studio::EventInstance* NewInstance = nullptr;
			FMOD::Studio::EventDescription* EventDesc = nullptr;
			StudioSystem->getEventByID(&Guid, &EventDesc);
			if (EventDesc)
			{
				EventDesc->createInstance(&NewInstance);
				if (NewInstance)
				{
					NewInstance->setParameterValue("Intensity", 0.0f);
					NewInstance->start();
				}
			}

			SnapshotEntryIndex = ReverbSnapshots.Num();
			ReverbSnapshots.Push(FFMODSnapshotEntry(NewSnapshot, NewInstance));
		}
		// Fade up
		if (ReverbSnapshots[SnapshotEntryIndex].FadeIntensityEnd == 0.0f)
		{
			ReverbSnapshots[SnapshotEntryIndex].FadeTo(BestVolume->Settings.Volume, BestVolume->Settings.FadeTime);
		}
	}
	// Fade out all other entries
	for (int i=0; i<ReverbSnapshots.Num(); ++i)
	{
		UE_LOG(LogFMOD, Verbose, TEXT("Ramping intensity (%f,%f) -> %f"), ReverbSnapshots[i].FadeIntensityStart, ReverbSnapshots[i].FadeIntensityEnd, ReverbSnapshots[i].CurrentIntensity());
		ReverbSnapshots[i].Instance->setParameterValue("Intensity", 100.0f * ReverbSnapshots[i].CurrentIntensity());

		if (ReverbSnapshots[i].Snapshot != NewSnapshot)
		{
			// Start fading out if needed
			if (ReverbSnapshots[i].FadeIntensityEnd != 0.0f)
			{
				ReverbSnapshots[i].FadeTo(0.0f, ReverbSnapshots[i].FadeDuration);
			}
			// Finish fading out and remove
			else if (ReverbSnapshots[i].CurrentIntensity() == 0.0f)
			{
				UE_LOG(LogFMOD, Verbose, TEXT("Removing snapshot"));

				ReverbSnapshots[i].Instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
				ReverbSnapshots[i].Instance->release();
				ReverbSnapshots.RemoveAt(i);
				--i; // removed entry, redo current index for next one
			}
		}
	}
}


void FFMODStudioModule::RefreshSettings()
{
	AssetTable.Refresh();
	if (GIsEditor)
	{
		const UFMODSettings& Settings = *GetDefault<UFMODSettings>();
		BankUpdateNotifier.SetFilePath(Settings.GetMasterStringsBankPath());
	}
}


void FFMODStudioModule::SetInPIE(bool bInPIE, bool simulating)
{
	bIsInPIE = bInPIE;
	bSimulating = simulating;
	bListenerMoved = true;
	ResetInterpolation();

	if (GIsEditor)
	{
		BankUpdateNotifier.EnableUpdate(!bInPIE);
	}

	if (bInPIE)
	{
		if (StudioSystem[EFMODSystemContext::Auditioning])
		{
			// We currently don't tear down auditioning system but we do stop the playing event.
			if (AuditioningInstance)
			{
				AuditioningInstance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
				AuditioningInstance = nullptr;
			}
			// Also make sure banks are finishing loading so they aren't grabbing file handles.
			StudioSystem[EFMODSystemContext::Auditioning]->flushCommands();
		}

		UE_LOG(LogFMOD, Log, TEXT("Creating Studio System"));
		ListenerCount = 1;
		CreateStudioSystem(EFMODSystemContext::Runtime);

		UE_LOG(LogFMOD, Log, TEXT("Triggering Initialized on other modules"));
		if (IFMODStudioOculusModule::IsAvailable())
		{
			IFMODStudioOculusModule::Get().OnInitialize();
		}

		UE_LOG(LogFMOD, Log, TEXT("Loading Banks"));
		LoadBanks(EFMODSystemContext::Runtime);

	}
	else
	{
		ReverbSnapshots.Reset();
		DestroyStudioSystem(EFMODSystemContext::Runtime);
	}
}

UFMODAsset* FFMODStudioModule::FindAssetByName(const FString& Name)
{
	return AssetTable.FindByName(Name);
}

UFMODEvent* FFMODStudioModule::FindEventByName(const FString& Name)
{
	UFMODAsset* Asset = AssetTable.FindByName(Name);
	return Cast<UFMODEvent>(Asset);
}

void FFMODStudioModule::SetSystemPaused(bool paused)
{
	if (StudioSystem[EFMODSystemContext::Runtime])
	{
		FMOD::System* LowLevelSystem = nullptr;
		verifyfmod(StudioSystem[EFMODSystemContext::Runtime]->getLowLevelSystem(&LowLevelSystem));
		FMOD::ChannelGroup* MasterChannelGroup = nullptr;
		verifyfmod(LowLevelSystem->getMasterChannelGroup(&MasterChannelGroup));
		verifyfmod(MasterChannelGroup->setPaused(paused));
	}
}

void FFMODStudioModule::PostLoadCallback()
{
}

void FFMODStudioModule::ShutdownModule()
{
	UE_LOG(LogFMOD, Verbose, TEXT("FFMODStudioModule shutdown"));

	DestroyStudioSystem(EFMODSystemContext::Auditioning);
	DestroyStudioSystem(EFMODSystemContext::Runtime);

	if (GIsEditor)
	{
		BankUpdateNotifier.BanksUpdatedEvent.RemoveAll(this);
	}

	if (UObjectInitialized())
	{
		// Unregister tick function.
		FTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
	}

	UE_LOG(LogFMOD, Verbose, TEXT("FFMODStudioModule unloading dynamic libraries"));
	if (StudioLibHandle)
	{
		FPlatformProcess::FreeDllHandle(StudioLibHandle);
		StudioLibHandle = nullptr;
	}
	if (LowLevelLibHandle)
	{
		FPlatformProcess::FreeDllHandle(LowLevelLibHandle);
		LowLevelLibHandle = nullptr;
	}
	UE_LOG(LogFMOD, Verbose, TEXT("FFMODStudioModule finished unloading"));
}

void FFMODStudioModule::LoadBanks(EFMODSystemContext::Type Type)
{
	const UFMODSettings& Settings = *GetDefault<UFMODSettings>();
	if (StudioSystem[Type] != nullptr && Settings.IsBankPathSet())
	{
		/*
			Queue up all banks to load asynchronously then wait at the end.
		*/
		FMOD_STUDIO_LOAD_BANK_FLAGS BankFlags = FMOD_STUDIO_LOAD_BANK_NONBLOCKING;
		bool bLoadAllBanks = ((Type == EFMODSystemContext::Auditioning) || Settings.bLoadAllBanks);
		bool bLoadSampleData = ((Type == EFMODSystemContext::Runtime) && Settings.bLoadAllSampleData);

		// Always load the master bank at startup
		FString MasterBankPath = Settings.GetMasterBankPath();
		UE_LOG(LogFMOD, Verbose, TEXT("Loading master bank: %s"), *MasterBankPath);
		FMOD::Studio::Bank* MasterBank = nullptr;
		FMOD_RESULT Result;
		Result = StudioSystem[Type]->loadBankFile(TCHAR_TO_UTF8(*MasterBankPath), BankFlags, &MasterBank);
		if (Result != FMOD_OK)
		{
			UE_LOG(LogFMOD, Warning, TEXT("Failed to master bank: %s"), *MasterBankPath);
			return;
		}
		if (bLoadSampleData)
		{
			verifyfmod(MasterBank->loadSampleData());
		}

		// Auditioning needs string bank to get back full paths from events
		// Runtime could do without it, but if we load it we can look up guids to names which is helpful
		{
			FString StringsBankPath = Settings.GetMasterStringsBankPath();
			UE_LOG(LogFMOD, Verbose, TEXT("Loading strings bank: %s"), *StringsBankPath);
			FMOD::Studio::Bank* StringsBank = nullptr;
			FMOD_RESULT Result;
			Result = StudioSystem[Type]->loadBankFile(TCHAR_TO_UTF8(*StringsBankPath), BankFlags, &StringsBank);
			if (Result != FMOD_OK)
			{
				UE_LOG(LogFMOD, Warning, TEXT("Failed to strings bank: %s"), *MasterBankPath);
			}
		}

		// Optionally load all banks in the directory
		if (bLoadAllBanks)
		{
			UE_LOG(LogFMOD, Verbose, TEXT("Loading all banks"));
			TArray<FString> BankFiles;
			Settings.GetAllBankPaths(BankFiles);
			for ( const FString& OtherFile : BankFiles )
			{
				FMOD::Studio::Bank* OtherBank;
				FMOD_RESULT Result = StudioSystem[Type]->loadBankFile(TCHAR_TO_UTF8(*OtherFile), BankFlags, &OtherBank);
				if (Result == FMOD_OK)
				{
					if (bLoadSampleData)
					{
						verifyfmod(OtherBank->loadSampleData());
					}
				}
				else
				{
					UE_LOG(LogFMOD, Warning, TEXT("Failed to load bank (Error %d): %s"), (int32)Result, *OtherFile);
				}
			}
		}

		// Wait for all banks to load.
		StudioSystem[Type]->flushCommands();
	}
}

void FFMODStudioModule::HandleBanksUpdated()
{
	DestroyStudioSystem(EFMODSystemContext::Auditioning);

	AssetTable.Refresh();

	CreateStudioSystem(EFMODSystemContext::Auditioning);
	LoadBanks(EFMODSystemContext::Auditioning);

	BanksReloadedDelegate.Broadcast();

}

FMOD::Studio::System* FFMODStudioModule::GetStudioSystem(EFMODSystemContext::Type Context)
{
	if (Context == EFMODSystemContext::Max)
	{
		Context = (bIsInPIE ? EFMODSystemContext::Runtime : EFMODSystemContext::Auditioning);
	}
	return StudioSystem[Context];
}


FMOD::Studio::EventDescription* FFMODStudioModule::GetEventDescription(const UFMODEvent* Event, EFMODSystemContext::Type Context)
{
	if (Context == EFMODSystemContext::Max)
	{
		Context = (bIsInPIE ? EFMODSystemContext::Runtime : EFMODSystemContext::Auditioning);
	}
	if (StudioSystem[Context] != nullptr && Event != nullptr && Event->AssetGuid.IsValid())
	{
		FMOD::Studio::ID Guid = FMODUtils::ConvertGuid(Event->AssetGuid);
		FMOD::Studio::EventDescription* EventDesc = nullptr;
		StudioSystem[Context]->getEventByID(&Guid, &EventDesc);
		return EventDesc;
	}
	return nullptr;
}

FMOD::Studio::EventInstance* FFMODStudioModule::CreateAuditioningInstance(const UFMODEvent* Event)
{
	StopAuditioningInstance();

	FMOD::Studio::EventDescription* EventDesc = GetEventDescription(Event, EFMODSystemContext::Auditioning);
	if (EventDesc)
	{
		FMOD_RESULT Result = EventDesc->createInstance(&AuditioningInstance);
		if (Result == FMOD_OK)
		{
			return AuditioningInstance;
		}
	}
	return nullptr;
}

void FFMODStudioModule::StopAuditioningInstance()
{
	if (AuditioningInstance)
	{
		// Don't bother checking for errors just in case auditioning is already shutting down
		AuditioningInstance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
		AuditioningInstance->release();
		AuditioningInstance = nullptr;
	}
}