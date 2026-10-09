#include "LoomaLiveLinkDriver.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ILiveLinkClient.h"
#include "LoomaLiveLink.h"
#include "LoomaLivePoseAnimInstance.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLiveLinkDriver, Log, All);

namespace
{
    const float RefreshSeconds = 0.5f;
    const float AnnounceSeconds = 2.0f;
    // Frames a second sent to the other clients over every character driven:
    // see PublishBudget in LoomaLiveLinkCommands.cpp for where the number is from.
    const float PublishBudget = 120.0f;
}

bool ULoomaLiveLinkDriver::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId ULoomaLiveLinkDriver::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULoomaLiveLinkDriver, STATGROUP_Tickables);
}

void ULoomaLiveLinkDriver::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    SinceRefresh += DeltaTime;
    SinceAnnounce += DeltaTime;
    if (SinceRefresh >= RefreshSeconds)
    {
        SinceRefresh = 0.0f;
        Refresh();
    }
}

void ULoomaLiveLinkDriver::Refresh()
{
    UWorld* World = GetWorld();
    ILiveLinkClient* Client = LoomaLiveLink::Client();
    if (!World || !Client)
    {
        return;
    }
    if (!bTriedListening)
    {
        // Once. If the port is taken the source says so, and Looma.LiveLink.Listen
        // can be given another.
        bTriedListening = true;
        LoomaLiveLink::EnsureListening();
    }

    TArray<FName> Subjects;
    for (const FLiveLinkSubjectKey& Key : Client->GetSubjects(/*bIncludeDisabledSubject=*/false, /*bIncludeVirtualSubject=*/false))
    {
        Subjects.AddUnique(Key.SubjectName.Name);
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    if (Sync && SinceAnnounce >= AnnounceSeconds && Subjects.Num() > 0)
    {
        SinceAnnounce = 0.0f;
        Sync->PublishLiveSubjects(Subjects);
    }

    // Who should be driven, and by whom.
    TMap<USkeletalMeshComponent*, FName> Wanted;
    TMap<USkeletalMeshComponent*, ALoomaSyncedActor*> Owner;
    for (TActorIterator<ALoomaSyncedActor> It(World); It; ++It)
    {
        USkeletalMeshComponent* Component = It->FindComponentByClass<USkeletalMeshComponent>();
        if (!Component || !LoomaLiveLink::IsRig(Component))
        {
            continue;
        }
        Owner.Add(Component, *It);
        const FString& Subject = It->GetComponents().LiveLinkSubject;
        if (!Subject.IsEmpty() && Subjects.Contains(FName(*Subject)))
        {
            Wanted.Add(Component, FName(*Subject));
        }
    }

    // Give back what is no longer wanted.
    for (auto It = Driven.CreateIterator(); It; ++It)
    {
        USkeletalMeshComponent* Component = It->Get();
        if (Component && Wanted.Contains(Component))
        {
            continue;
        }
        if (Component && Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance()))
        {
            Component->SetAnimInstanceClass(nullptr);
            Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
            if (ALoomaSyncedActor** Actor = Owner.Find(Component))
            {
                (*Actor)->ReplayAnimation();
                UE_LOG(LogLoomaLiveLinkDriver, Log, TEXT("Character '%s' released from Live Link."), *(*Actor)->Id);
            }
        }
        It.RemoveCurrent();
    }

    // Take what is wanted, and share the budget out.
    const float Rate = Wanted.Num() > 0 ? FMath::Clamp(PublishBudget / Wanted.Num(), 1.0f, 30.0f) : 30.0f;
    int32 Index = 0;
    for (const TPair<USkeletalMeshComponent*, FName>& Pair : Wanted)
    {
        USkeletalMeshComponent* Component = Pair.Key;
        ULoomaLivePoseAnimInstance* Instance = Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance());
        if (!Instance)
        {
            // Not driven yet, or a clip was played over it since: take it (back).
            Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
            Component->SetAnimInstanceClass(ULoomaLivePoseAnimInstance::StaticClass());
            Instance = Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance());
            if (Instance)
            {
                Instance->SetPublishPhase(Wanted.Num() > 0 ? float(Index) / Wanted.Num() : 0.0f);
                UE_LOG(LogLoomaLiveLinkDriver, Log, TEXT("Character '%s' now follows Live Link subject '%s'."),
                       *Owner[Component]->Id, *Pair.Value.ToString());
            }
        }
        if (Instance)
        {
            Instance->SubjectName = Pair.Value;
            Instance->PublishRate = Rate;
            Driven.Add(Component);
        }
        ++Index;
    }
}
