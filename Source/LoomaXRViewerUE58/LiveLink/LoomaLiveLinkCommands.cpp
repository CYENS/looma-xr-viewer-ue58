// Console commands for the Live Link proof (HAM-316): drive the generated
// characters in the scene from the Moverse simulator, with no assets to author.
//
//   Looma.LiveLink.Listen [port]     Start reading the simulator's datagrams (54321).
//   Looma.LiveLink.Drive [n] [subject...]
//                                    Make the LoomaMixamo characters follow a subject
//                                    (Actor0). Several subjects, e.g. "Actor0 Actor1",
//                                    are dealt out to the characters in turn. A leading
//                                    number drives only that many, e.g. "3 Actor0".
//                                    The others on the scene are sent a fixed budget of
//                                    frames a second shared between the driven
//                                    characters, so a few are smooth on the web and a
//                                    crowd is not.
//   Looma.LiveLink.Assign <subject> [name]
//                                    Put a Live Link component on a character, in the
//                                    SCENE: the web shows it and it is saved, the same
//                                    as adding it in the web inspector. The character
//                                    is the selected one, or every character whose name
//                                    contains [name].
//   Looma.LiveLink.Unassign [name]   Remove that component again.
//   Looma.LiveLink.Release           Give the characters back: they return to their
//                                    reference pose, and a clip plays again the next
//                                    time the scene sets one.
//   Looma.LiveLink.Status            What is listening and who is following.
//
// The simulator: `cd code/moverse-simulation-python && python live_link_broadcaster.py`.

#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "ILiveLinkClient.h"
#include "LoomaJsonLiveLinkSource.h"
#include "LoomaLiveLink.h"
#include "LoomaLivePoseAnimInstance.h"
#include "LoomaMvnLiveLinkSource.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/Selection.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLiveLinkCommands, Log, All);

namespace
{
    TSharedPtr<FLoomaJsonLiveLinkSource> GSource;
    TSharedPtr<FLoomaMvnLiveLinkSource> GMvnSource;

    ILiveLinkClient* LiveLinkClient()
    {
        IModularFeatures& Features = IModularFeatures::Get();
        return Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName)
            ? &Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName)
            : nullptr;
    }

    bool IsLoomaRig(const USkeletalMeshComponent* Component)
    {
        const USkeletalMesh* Mesh = Component ? Component->GetSkeletalMeshAsset() : nullptr;
        if (!Mesh)
        {
            return false;
        }
        const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
        for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone)
        {
            const FString Name = Skeleton.GetBoneName(Bone).ToString();
            if (Name.StartsWith(TEXT("mixamorig"), ESearchCase::IgnoreCase) && Name.EndsWith(TEXT("Hips")))
            {
                return true;
            }
        }
        return false;
    }

    TArray<USkeletalMeshComponent*> RigsIn(const UWorld* World)
    {
        TArray<USkeletalMeshComponent*> Out;
        for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
        {
            if (It->GetWorld() == World && IsValid(*It) && IsLoomaRig(*It))
            {
                Out.Add(*It);
            }
        }
        return Out;
    }

    // Frames a second this client sends to the other clients, over all the
    // characters it drives. A frame is about 650 bytes, so this is about 80 kB/s.
    // The first end-to-end run sent 29 characters at 30 Hz each through a tunnel
    // and lost its sockets to keepalive timeouts.
    const float PublishBudget = 120.0f;

    bool Listen(int32 Port)
    {
        ILiveLinkClient* Client = LiveLinkClient();
        if (!Client)
        {
            UE_LOG(LogLoomaLiveLinkCommands, Error, TEXT("Live Link is not available; is the LiveLink plugin enabled?"));
            return false;
        }
        if (GSource.IsValid())
        {
            Client->RemoveSource(GSource);
            GSource.Reset();
        }
        TSharedPtr<FLoomaJsonLiveLinkSource> Source = MakeShared<FLoomaJsonLiveLinkSource>(Port);
        if (!Source->IsListening())
        {
            return false; // the source said why
        }
        GSource = Source;
        Client->AddSource(Source);
        UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Listening for the Moverse simulator on UDP %d."), Port);
        return true;
    }

    bool ListenMvn(int32 Port)
    {
        ILiveLinkClient* Client = LiveLinkClient();
        if (!Client)
        {
            return false;
        }
        if (GMvnSource.IsValid())
        {
            Client->RemoveSource(GMvnSource);
            GMvnSource.Reset();
        }
        TSharedPtr<FLoomaMvnLiveLinkSource> Source = MakeShared<FLoomaMvnLiveLinkSource>(Port);
        if (!Source->IsListening())
        {
            return false; // the source said why
        }
        GMvnSource = Source;
        Client->AddSource(Source);
        UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Listening for Xsens MVN on UDP %d."), Port);
        return true;
    }

    FAutoConsoleCommandWithWorldAndArgs GListenMvn(
        TEXT("Looma.LiveLink.ListenXsens"),
        TEXT("Read Xsens MVN network-protocol datagrams: Looma.LiveLink.ListenXsens [port=9763]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld*) {
            ListenMvn(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : LoomaLiveLink::DefaultMvnPort);
        }));

    FAutoConsoleCommandWithWorldAndArgs GListen(
        TEXT("Looma.LiveLink.Listen"),
        TEXT("Read the Moverse simulator's JSON Live Link datagrams: Looma.LiveLink.Listen [port=54321]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld*) {
            Listen(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : LoomaLiveLink::DefaultPort);
        }));

    FAutoConsoleCommandWithWorldAndArgs GDrive(
        TEXT("Looma.LiveLink.Drive"),
        TEXT("Make every LoomaMixamo character follow a Live Link subject: Looma.LiveLink.Drive [subject=Actor0] [subject...]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
            TArray<FName> Subjects;
            int32 Limit = MAX_int32;
            for (const FString& Arg : Args)
            {
                if (Arg.IsNumeric() && Subjects.Num() == 0)
                {
                    Limit = FMath::Max(1, FCString::Atoi(*Arg));
                    continue;
                }
                Subjects.Add(FName(*Arg));
            }
            if (Subjects.Num() == 0)
            {
                Subjects.Add(TEXT("Actor0"));
            }
            TArray<USkeletalMeshComponent*> Rigs = RigsIn(World);
            // By name, so "the first three" are the same three every time.
            Rigs.Sort([](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B) {
                return GetNameSafe(A.GetOwner()) < GetNameSafe(B.GetOwner());
            });
            if (Rigs.Num() > Limit)
            {
                Rigs.SetNum(Limit);
            }
            const float Rate = Rigs.Num() > 0 ? FMath::Clamp(PublishBudget / Rigs.Num(), 1.0f, 30.0f) : 30.0f;
            for (int32 Index = 0; Index < Rigs.Num(); ++Index)
            {
                USkeletalMeshComponent* Component = Rigs[Index];
                Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
                Component->SetAnimInstanceClass(ULoomaLivePoseAnimInstance::StaticClass());
                if (ULoomaLivePoseAnimInstance* Instance = Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance()))
                {
                    Instance->SubjectName = Subjects[Index % Subjects.Num()];
                    Instance->PublishRate = Rate;
                    // Spread over the interval, so the frames do not all leave together.
                    Instance->SetPublishPhase(Rigs.Num() > 0 ? float(Index) / Rigs.Num() : 0.0f);
                }
#if WITH_EDITOR
                // An editor world does not tick animation unless asked to.
                Component->SetUpdateAnimationInEditor(true);
#endif
            }
            UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("%d character(s) now follow Live Link, sent on at %.0f frames a second each%s."), Rigs.Num(), Rate,
                   GSource.IsValid() ? TEXT("") : TEXT("; nothing is listening yet, run Looma.LiveLink.Listen"));
        }));

    FAutoConsoleCommandWithWorldAndArgs GRelease(
        TEXT("Looma.LiveLink.Release"),
        TEXT("Stop the characters following Live Link."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>&, UWorld* World) {
            int32 Released = 0;
            for (USkeletalMeshComponent* Component : RigsIn(World))
            {
                if (Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance()))
                {
                    Component->SetAnimInstanceClass(nullptr);
                    Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
                    ++Released;
                }
            }
            UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("%d character(s) released."), Released);
        }));

    bool IsCharacter(const ALoomaSyncedActor* Actor)
    {
        return IsValid(Actor) && IsLoomaRig(Actor->FindComponentByClass<USkeletalMeshComponent>());
    }

    // The characters a command is about: the ones whose name contains `Name`, or
    // with no name, the ones selected (in the viewer, or in the editor's outliner).
    TArray<ALoomaSyncedActor*> CharactersFor(UWorld* World, const ULoomaSceneSyncSubsystem& Sync, const FString& Name)
    {
        TArray<ALoomaSyncedActor*> Out;
        if (!Name.IsEmpty())
        {
            for (TObjectIterator<ALoomaSyncedActor> It; It; ++It)
            {
                if (It->GetWorld() == World && IsCharacter(*It)
                    && (It->DisplayName.Contains(Name, ESearchCase::IgnoreCase) || It->Id.StartsWith(Name, ESearchCase::IgnoreCase)))
                {
                    Out.Add(*It);
                }
            }
            return Out;
        }
        for (ALoomaSyncedActor* Actor : Sync.GetLocalSelection())
        {
            if (IsCharacter(Actor))
            {
                Out.AddUnique(Actor);
            }
        }
#if WITH_EDITOR
        if (GEditor)
        {
            for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
            {
                ALoomaSyncedActor* Actor = Cast<ALoomaSyncedActor>(*It);
                if (Actor && Actor->GetWorld() == World && IsCharacter(Actor))
                {
                    Out.AddUnique(Actor);
                }
            }
        }
#endif
        return Out;
    }

    void Assign(UWorld* World, const FString& Subject, const FString& Name)
    {
        UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
        ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
        if (!Sync || !Sync->IsSyncConnected())
        {
            UE_LOG(LogLoomaLiveLinkCommands, Warning, TEXT("Not connected to a scene, so there is nothing to write the choice into. Press Play first."));
            return;
        }
        const TArray<ALoomaSyncedActor*> Characters = CharactersFor(World, *Sync, Name);
        if (Characters.Num() == 0)
        {
            UE_LOG(LogLoomaLiveLinkCommands, Warning, TEXT("%s Rigged characters in this scene:"),
                   Name.IsEmpty() ? TEXT("No rigged character is selected; select one or give part of its name.")
                                  : *FString::Printf(TEXT("No rigged character's name contains '%s'."), *Name));
            for (TObjectIterator<ALoomaSyncedActor> It; It; ++It)
            {
                if (It->GetWorld() == World && IsCharacter(*It))
                {
                    const FString& Now = It->GetComponents().LiveLinkSubject;
                    UE_LOG(LogLoomaLiveLinkCommands, Warning, TEXT("  %s%s"), *It->DisplayName,
                           Now.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  (follows %s)"), *Now));
                }
            }
            return;
        }
        if (!Subject.IsEmpty())
        {
            bool bLive = false;
            if (ILiveLinkClient* Client = LiveLinkClient())
            {
                for (const FLiveLinkSubjectKey& Key : Client->GetSubjects(/*bIncludeDisabledSubject=*/false, /*bIncludeVirtualSubject=*/false))
                {
                    bLive |= Key.SubjectName.Name == FName(*Subject);
                }
            }
            if (!bLive)
            {
                UE_LOG(LogLoomaLiveLinkCommands, Warning, TEXT("No Live Link subject is called '%s' right now. Set anyway; the character moves when one is."), *Subject);
            }
        }
        for (ALoomaSyncedActor* Character : Characters)
        {
            const bool bSent = Sync->SetLiveLinkSubject(Character, Subject);
            UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("%s: %s%s"), *Character->DisplayName,
                   Subject.IsEmpty() ? TEXT("Live Link component removed") : *FString::Printf(TEXT("Live Link subject set to %s"), *Subject),
                   bSent ? TEXT("") : TEXT(" (NOT sent)"));
        }
    }

    FAutoConsoleCommandWithWorldAndArgs GAssign(
        TEXT("Looma.LiveLink.Assign"),
        TEXT("Put a Live Link component on the selected character, in the scene: Looma.LiveLink.Assign <subject> [part of a character's name]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
            if (Args.Num() == 0)
            {
                UE_LOG(LogLoomaLiveLinkCommands, Warning, TEXT("Which subject? Looma.LiveLink.Assign <subject> [part of a character's name]. Looma.LiveLink.Status lists the subjects."));
                return;
            }
            TArray<FString> Rest = Args;
            Rest.RemoveAt(0);
            Assign(World, Args[0], FString::Join(Rest, TEXT(" ")));
        }));

    FAutoConsoleCommandWithWorldAndArgs GUnassign(
        TEXT("Looma.LiveLink.Unassign"),
        TEXT("Remove the Live Link component from the selected character, in the scene: Looma.LiveLink.Unassign [part of a character's name]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) {
            Assign(World, FString(), FString::Join(Args, TEXT(" ")));
        }));

    FAutoConsoleCommandWithWorldAndArgs GStatus(
        TEXT("Looma.LiveLink.Status"),
        TEXT("What is listening for Live Link, and which characters follow it."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>&, UWorld* World) {
            if (GSource.IsValid())
            {
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Moverse: listening on UDP %d, %s."), GSource->GetPort(),
                       *GSource->GetSourceStatus().ToString());
            }
            if (GMvnSource.IsValid())
            {
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Xsens: listening on UDP %d, %s."), GMvnSource->GetPort(),
                       *GMvnSource->GetSourceStatus().ToString());
            }
            if (!GSource.IsValid() && !GMvnSource.IsValid())
            {
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Not listening."));
            }
            if (ILiveLinkClient* Client = LiveLinkClient())
            {
                for (const FLiveLinkSubjectKey& Key : Client->GetSubjects(/*bIncludeDisabledSubject=*/true, /*bIncludeVirtualSubject=*/false))
                {
                    UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("  subject %s"), *Key.SubjectName.Name.ToString());
                }
            }
            for (const USkeletalMeshComponent* Component : RigsIn(World))
            {
                const ULoomaLivePoseAnimInstance* Instance = Cast<ULoomaLivePoseAnimInstance>(Component->GetAnimInstance());
                FString Following = TEXT("not following");
                if (Instance && Instance->IsFollowingScene())
                {
                    Following = TEXT("follows the pose another client sends");
                    // How far behind it is shown, and how unevenly its poses arrive (HAM-325).
                    const ALoomaSyncedActor* Actor = Cast<ALoomaSyncedActor>(Component->GetOwner());
                    const UGameInstance* GameInstance = World->GetGameInstance();
                    const ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
                    double Delay = 0.0, Jitter = 0.0;
                    if (Actor && Sync && Sync->GetLivePoseTiming(Actor->Id, Delay, Jitter))
                    {
                        Following += FString::Printf(TEXT(", shown %.0f ms behind (arrivals stray %.0f ms)"), Delay * 1000.0, Jitter * 1000.0);
                    }
                }
                else if (Instance)
                {
                    Following = FString::Printf(TEXT("follows %s"), *Instance->SubjectName.ToString());
                }
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("  character %s: %s"), *GetNameSafe(Component->GetOwner()), *Following);
            }
        }));
}

// --- What the driver shares with the commands (LoomaLiveLink.h) -------------------

bool LoomaLiveLink::EnsureListening(int32 Port)
{
    // Each on its own: one port being taken is no reason not to open the other.
    const bool bMoverse = (GSource.IsValid() && GSource->IsListening()) || Listen(Port);
    const bool bXsens = (GMvnSource.IsValid() && GMvnSource->IsListening()) || ListenMvn(DefaultMvnPort);
    return bMoverse || bXsens;
}

ILiveLinkClient* LoomaLiveLink::Client()
{
    return LiveLinkClient();
}

bool LoomaLiveLink::IsRig(const USkeletalMeshComponent* Component)
{
    return IsLoomaRig(Component);
}
