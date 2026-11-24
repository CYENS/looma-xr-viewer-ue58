#include "LoomaLivePoseAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "ILiveLinkClient.h"
#include "LoomaLiveLinkProfiles.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"
#include "ReferenceSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLivePose, Log, All);

namespace
{
    TAutoConsoleVariable<int32> CVarFindFloor(
        TEXT("Looma.LiveLink.FindFloor"), 1,
        TEXT("1: stand a Live Link character on the floor its performer's feet rest on. 0: trust the mocap system's own zero height."));

    TAutoConsoleVariable<int32> CVarSmooth(
        TEXT("Looma.LiveLink.Smooth"), 1,
        TEXT("1: steady a Live Link pose as its profile in Config/LiveLinkProfiles.json says. 0: use every pose as it comes."));
    TAutoConsoleVariable<float> CVarSmoothMinCutoff(
        TEXT("Looma.LiveLink.SmoothMinCutoff"), -1.0f,
        TEXT("Hz. How hard a still performer is smoothed: lower is steadier and slower to follow. Negative: the profile's own."));
    TAutoConsoleVariable<float> CVarSmoothBeta(
        TEXT("Looma.LiveLink.SmoothBeta"), -1.0f,
        TEXT("Hz for each radian a second a bone turns: higher follows a fast movement more closely. Negative: the profile's own."));

    TAutoConsoleVariable<int32> CVarBuffer(
        TEXT("Looma.LiveLink.Buffer"), 1,
        TEXT("1: show a character another client drives a little in the past, between two received poses, so uneven arrival does not show. 0: chase the newest pose."));
}

// --- The proxy ----------------------------------------------------------------------

void FLoomaLivePoseProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
    FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
    if (const ULoomaLivePoseAnimInstance* Instance = Cast<ULoomaLivePoseAnimInstance>(InAnimInstance))
    {
        Rotations = Instance->Rotations;
        HasRotation = Instance->HasRotation;
        HipsLocation = Instance->HipsLocation;
        HipsBone = Instance->Target.HipsBone;
        bHasPose = Instance->bHasPose;
    }
}

bool FLoomaLivePoseProxy::Evaluate(FPoseContext& Output)
{
    Output.ResetToRefPose();
    if (!bHasPose)
    {
        return true;
    }
    const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
    for (int32 Bone = 0; Bone < Rotations.Num(); ++Bone)
    {
        if (!HasRotation[Bone])
        {
            continue;
        }
        const FCompactPoseBoneIndex Compact = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Bone));
        if (!Compact.IsValid())
        {
            continue;
        }
        Output.Pose[Compact].SetRotation(Rotations[Bone]);
        if (Bone == HipsBone)
        {
            Output.Pose[Compact].SetTranslation(HipsLocation);
        }
    }
    return true;
}

// --- The instance -----------------------------------------------------------------

FAnimInstanceProxy* ULoomaLivePoseAnimInstance::CreateAnimInstanceProxy()
{
    return new FLoomaLivePoseProxy(this);
}

void ULoomaLivePoseAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    bTargetReady = BuildTarget();
    bSourceReady = false;
    Retarget.Restart();
    bHasPose = false;
    bSceneStarted = false;
}

void ULoomaLivePoseAnimInstance::FollowScene(bool bFollow)
{
    if (bFromScene != bFollow)
    {
        bFromScene = bFollow;
        bSceneStarted = false;
        Retarget.Restart();
    }
}

void ULoomaLivePoseAnimInstance::SetFromWire(const FVector* Hips, const TArray<FQuat>& Wire, float Alpha)
{
    for (int32 Index = 0; Index < LoomaRigBody::Num && Index < Wire.Num(); ++Index)
    {
        const int32 To = Target.Bones[Index];
        FQuat Sent = Wire[Index];
        if (To == INDEX_NONE || Sent.SizeSquared() < 1e-12)
        {
            continue;
        }
        Sent.Normalize();
        const FQuat Wanted = Target.Rotation(Index, Sent);
        Rotations[To] = Alpha >= 1.0f || !HasRotation[To] ? Wanted : FQuat::Slerp(Rotations[To], Wanted, Alpha);
        HasRotation[To] = true;
    }
    if (Hips)
    {
        const FVector Wanted = Target.HipsLocation(*Hips);
        HipsLocation = Alpha >= 1.0f ? Wanted : FMath::Lerp(HipsLocation, Wanted, Alpha);
    }
    bHasPose = true;
}

void ULoomaLivePoseAnimInstance::UpdateFromScene(float DeltaSeconds)
{
    const USkeletalMeshComponent* Component = GetSkelMeshComponent();
    const ALoomaSyncedActor* Actor = Component ? Cast<ALoomaSyncedActor>(Component->GetOwner()) : nullptr;
    const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    const ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    FLoomaLivePose Pose;
    if (CVarBuffer.GetValueOnGameThread() != 0)
    {
        // Where the character was a moment ago, between two received poses
        // (FLoomaLivePoseBuffer): poses arrive unevenly, and a character that
        // chases the newest speeds up and slows down with them.
        if (Actor && Sync && Sync->SampleLivePose(Actor->Id, Pose))
        {
            bSceneStarted = true;
            SetFromWire(&Pose.Hips, Pose.Rotations, 1.0f);
        }
        return; // else hold the last pose; the driver lets go of a character whose frames stopped
    }
    if (!Actor || !Sync || !Sync->GetLivePose(Actor->Id, Pose))
    {
        return;
    }
    // The older way: close on the newest frame at a speed that suits how often
    // frames come, as the web does (livePose.js): about three arrivals' worth a
    // second. The first frame is taken whole, so the character does not swing in
    // from its rest pose.
    const double Rate = Pose.Gap > 0.0 ? FMath::Clamp(3.0 / Pose.Gap, 5.0, 25.0) : 25.0;
    const float Alpha = bSceneStarted && DeltaSeconds > 0.0f ? float(1.0 - FMath::Exp(-Rate * DeltaSeconds)) : 1.0f;
    bSceneStarted = true;
    SetFromWire(&Pose.Hips, Pose.Rotations, Alpha);
}

bool ULoomaLivePoseAnimInstance::BuildTarget()
{
    const USkeletalMeshComponent* Component = GetSkelMeshComponent();
    const USkeletalMesh* Mesh = Component ? Component->GetSkeletalMeshAsset() : nullptr;
    if (!Mesh)
    {
        return false;
    }
    const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
    FString Why;
    if (!Target.Build(Skeleton, Why))
    {
        UE_LOG(LogLoomaLivePose, Warning, TEXT("%s: %s."), *Mesh->GetName(), *Why);
        return false;
    }

    Rotations.Init(FQuat::Identity, Target.BoneCount);
    HasRotation.Init(false, Target.BoneCount);
    WireRotations.Init(FQuat::Identity, LoomaRigBody::Num);
    const TArray<FTransform>& Local = Skeleton.GetRefBonePose();
    HipsLocation = Local[Target.HipsBone].GetLocation();

    const int32 Spine = Target.Bones[LoomaRigBody::Row(TEXT("Spine"))];
    UE_LOG(LogLoomaLivePose, Log,
           TEXT("%s: %d bones, hips %.1f above the floor; left %s up %s forward %s; hips rest %s, spine rest %s."),
           *Mesh->GetName(), Target.BoneCount, Target.HipsHeight, *Target.Left.ToCompactString(),
           *Target.Up.ToCompactString(), *Target.Forward.ToCompactString(),
           *Target.HipsRestRotation.Rotator().ToCompactString(),
           Spine != INDEX_NONE ? *Local[Spine].GetRotation().Rotator().ToCompactString() : TEXT("-"));
    return true;
}

FString ULoomaLivePoseAnimInstance::SubjectSourceType() const
{
    IModularFeatures& Features = IModularFeatures::Get();
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return FString();
    }
    ILiveLinkClient& Client = Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    // The enabled subject of that name: the one a frame is evaluated from.
    for (const FLiveLinkSubjectKey& Key : Client.GetSubjects(/*bIncludeDisabledSubject=*/false, /*bIncludeVirtualSubject=*/false))
    {
        if (Key.SubjectName.Name == SubjectName)
        {
            return Client.GetSourceType(Key.Source).ToString();
        }
    }
    return FString();
}

bool ULoomaLivePoseAnimInstance::BuildSource(const TArray<FName>& BoneNames)
{
    SourceNames = BoneNames;
    SourceType = SubjectSourceType();
    const FLoomaLiveLinkProfile* Profile = LoomaLiveLinkProfiles::For(BoneNames, SourceType);
    if (!Profile)
    {
        UE_LOG(LogLoomaLivePose, Warning,
               TEXT("Live Link subject '%s' matches no profile in Config/LiveLinkProfiles.json; its first bone is '%s'."),
               *SubjectName.ToString(), BoneNames.Num() > 0 ? *BoneNames[0].ToString() : TEXT("-"));
        return false;
    }
    const int32 Mapped = Retarget.Build(*Profile, BoneNames);
    Smoothing = Profile->Smoothing;
    Smoother.Reset();
    UE_LOG(LogLoomaLivePose, Log, TEXT("Live Link subject '%s' from '%s' is a '%s' skeleton: %d of %d bones mapped."),
           *SubjectName.ToString(), *SourceType, *Profile->Name, Mapped, LoomaRigBody::Num);
    return true;
}

void ULoomaLivePoseAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!bTargetReady)
    {
        return;
    }
    if (bFromScene)
    {
        UpdateFromScene(DeltaSeconds);
        return;
    }
    IModularFeatures& Features = IModularFeatures::Get();
    if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
    {
        return;
    }
    ILiveLinkClient& Client = Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
    FLiveLinkSubjectFrameData Frame;
    if (!Client.EvaluateFrame_AnyThread(SubjectName, ULiveLinkAnimationRole::StaticClass(), Frame))
    {
        if (!bWarned)
        {
            bWarned = true;
            UE_LOG(LogLoomaLivePose, Warning, TEXT("No Live Link frame for subject '%s' yet."), *SubjectName.ToString());
        }
        return;
    }
    const FLiveLinkSkeletonStaticData* Skeleton = Frame.StaticData.Cast<FLiveLinkSkeletonStaticData>();
    const FLiveLinkAnimationFrameData* Animation = Frame.FrameData.Cast<FLiveLinkAnimationFrameData>();
    if (!Skeleton || !Animation || Animation->Transforms.Num() != Skeleton->GetBoneNames().Num())
    {
        return;
    }
    // The same subject name can pass to another source that sends the same bones
    // another way (Moverse's own plugin and the viewer's reader both say
    // "Actor0"), so the source is looked at again now and then.
    SinceSourceCheck += DeltaSeconds;
    if (bSourceReady && SinceSourceCheck >= 0.5f)
    {
        SinceSourceCheck = 0.0f;
        if (SubjectSourceType() != SourceType)
        {
            bSourceReady = false;
        }
    }
    if (!bSourceReady || SourceNames != Skeleton->GetBoneNames())
    {
        bSourceReady = BuildSource(Skeleton->GetBoneNames());
    }
    if (!bSourceReady)
    {
        return;
    }
    const bool bHips = Retarget.Evaluate(Animation->Transforms, Skeleton->GetBoneParents(), DeltaSeconds,
                                         CVarFindFloor.GetValueOnGameThread() != 0, WireRotations, WireHips);
    // Steadied before anyone sees it, this client included.
    FLoomaSmoothing Steady = CVarSmooth.GetValueOnGameThread() != 0 ? Smoothing : FLoomaSmoothing();
    if (CVarSmooth.GetValueOnGameThread() != 0)
    {
        const float MinCutoff = CVarSmoothMinCutoff.GetValueOnGameThread();
        const float Beta = CVarSmoothBeta.GetValueOnGameThread();
        Steady.MinCutoff = MinCutoff >= 0.0f ? MinCutoff : Steady.MinCutoff;
        Steady.Beta = Beta >= 0.0f ? Beta : Steady.Beta;
    }
    Smoother.Apply(WireRotations, bHips ? &WireHips : nullptr, DeltaSeconds, Steady);
    // Posed here from the very frame the other clients are sent, by the step
    // they use, so what this client shows is what they show.
    SetFromWire(bHips ? &WireHips : nullptr, WireRotations, 1.0f);

    // On to the other clients, at its own rate: this runs every rendered frame.
    SincePublish += DeltaSeconds;
    if (bPublish && PublishRate > 0.0f && SincePublish >= 1.0f / PublishRate)
    {
        SincePublish = 0.0f;
        const USkeletalMeshComponent* Component = GetSkelMeshComponent();
        const ALoomaSyncedActor* Actor = Component ? Cast<ALoomaSyncedActor>(Component->GetOwner()) : nullptr;
        const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
        ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
        if (Actor && Sync)
        {
            Sync->PublishPose(Actor->Id, WireHips, WireRotations);
        }
    }
}
