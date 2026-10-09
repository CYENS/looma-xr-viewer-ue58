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
//   Looma.LiveLink.Release           Give the characters back: they return to their
//                                    reference pose, and a clip plays again the next
//                                    time the scene sets one.
//   Looma.LiveLink.Status            What is listening and who is following.
//
// The simulator: `cd code/moverse-simulation-python && python live_link_broadcaster.py`.

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "ILiveLinkClient.h"
#include "LoomaJsonLiveLinkSource.h"
#include "LoomaLivePoseAnimInstance.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLiveLinkCommands, Log, All);

namespace
{
    TSharedPtr<FLoomaJsonLiveLinkSource> GSource;

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

    FAutoConsoleCommandWithWorldAndArgs GListen(
        TEXT("Looma.LiveLink.Listen"),
        TEXT("Read the Moverse simulator's JSON Live Link datagrams: Looma.LiveLink.Listen [port=54321]."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld*) {
            ILiveLinkClient* Client = LiveLinkClient();
            if (!Client)
            {
                UE_LOG(LogLoomaLiveLinkCommands, Error, TEXT("Live Link is not available; is the LiveLink plugin enabled?"));
                return;
            }
            const int32 Port = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 54321;
            if (GSource.IsValid())
            {
                Client->RemoveSource(GSource);
                GSource.Reset();
            }
            TSharedPtr<FLoomaJsonLiveLinkSource> Source = MakeShared<FLoomaJsonLiveLinkSource>(Port);
            if (!Source->IsListening())
            {
                return; // the source said why
            }
            GSource = Source;
            Client->AddSource(Source);
            UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Listening on UDP %d. Start the simulator, then Looma.LiveLink.Drive."), Port);
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

    FAutoConsoleCommandWithWorldAndArgs GStatus(
        TEXT("Looma.LiveLink.Status"),
        TEXT("What is listening for Live Link, and which characters follow it."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>&, UWorld* World) {
            if (GSource.IsValid())
            {
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("Listening on UDP %d: %s."), GSource->GetPort(),
                       *GSource->GetSourceStatus().ToString());
            }
            else
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
                UE_LOG(LogLoomaLiveLinkCommands, Display, TEXT("  character %s: %s"), *GetNameSafe(Component->GetOwner()),
                       Instance ? *FString::Printf(TEXT("follows %s"), *Instance->SubjectName.ToString()) : TEXT("not following"));
            }
        }));
}
