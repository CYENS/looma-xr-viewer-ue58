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

    /** "mixamorig:Hips", "mixamorig_Hips" -> the part after the rig prefix. */
    FString Bare(const FName& Name)
    {
        FString Text = Name.ToString();
        int32 Cut = INDEX_NONE;
        if (Text.FindLastChar(TEXT(':'), Cut))
        {
            Text.RightChopInline(Cut + 1);
        }
        for (const TCHAR* Prefix : {TEXT("mixamorig_"), TEXT("mixamorig")})
        {
            if (Text.StartsWith(Prefix, ESearchCase::IgnoreCase))
            {
                Text.RightChopInline(FCString::Strlen(Prefix));
                break;
            }
        }
        return Text;
    }

    /** `Raw` with its parts along the given unit axes removed, normalised. */
    FVector Orthogonal(FVector Raw, const FVector& A, const FVector* B = nullptr)
    {
        Raw -= FVector::DotProduct(Raw, A) * A;
        if (B)
        {
            Raw -= FVector::DotProduct(Raw, *B) * (*B);
        }
        return Raw.GetSafeNormal();
    }
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
        HipsBone = Instance->HipsBone;
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
    const FLoomaBasis BoneToCharacter = CharacterInBone.Transposed();
    for (int32 Index = 0; Index < LoomaRigBody::Num && Index < Wire.Num(); ++Index)
    {
        const int32 To = TargetBones[Index];
        FQuat Sent = Wire[Index];
        if (To == INDEX_NONE || Sent.SizeSquared() < 1e-12)
        {
            continue;
        }
        Sent.Normalize();
        // The wire's rotation is the bone's turn from its T-pose in the
        // character's left, up, forward; here it is written in this mesh's bone.
        const FLoomaBasis LocalTurn = FLoomaBasis::FromQuat(Sent);
        FQuat Target;
        if (LoomaRigBody::Parent(Index) < 0)
        {
            const FLoomaBasis InComponent = CharacterInComponent * LocalTurn * CharacterInComponent.Transposed() * HipsRest;
            Target = (HipsParentInverse * InComponent).ToQuat();
        }
        else
        {
            Target = (CharacterInBone * LocalTurn * BoneToCharacter).ToQuat();
        }
        Rotations[To] = Alpha >= 1.0f || !HasRotation[To] ? Target : FQuat::Slerp(Rotations[To], Target, Alpha);
        HasRotation[To] = true;
    }
    if (Hips)
    {
        const FVector Target = HipsParentInverse.Apply(CharacterInComponent.Apply(*Hips * HipsHeight));
        HipsLocation = Alpha >= 1.0f ? Target : FMath::Lerp(HipsLocation, Target, Alpha);
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
    BoneCount = Skeleton.GetNum();

    TMap<FString, int32> ByName;
    for (int32 Bone = 0; Bone < BoneCount; ++Bone)
    {
        ByName.Add(Bare(Skeleton.GetBoneName(Bone)), Bone);
    }
    TargetBones.Init(INDEX_NONE, LoomaRigBody::Num);
    for (int32 Index = 0; Index < LoomaRigBody::Num; ++Index)
    {
        if (const int32* Found = ByName.Find(LoomaRigBody::Name(Index)))
        {
            TargetBones[Index] = *Found;
        }
    }
    auto Bone = [this](const TCHAR* Name) { return TargetBones[LoomaRigBody::Row(Name)]; };
    HipsBone = Bone(TEXT("Hips"));
    for (const TCHAR* Needed : {TEXT("Hips"), TEXT("Head"), TEXT("LeftUpLeg"), TEXT("RightUpLeg"),
                                TEXT("LeftFoot"), TEXT("LeftToeBase"), TEXT("RightFoot"), TEXT("RightToeBase")})
    {
        if (Bone(Needed) == INDEX_NONE)
        {
            UE_LOG(LogLoomaLivePose, Warning, TEXT("%s is not a LoomaMixamo rig: no %s bone."),
                   *Mesh->GetName(), Needed);
            return false;
        }
    }

    // The reference pose in component space.
    const TArray<FTransform>& Local = Skeleton.GetRefBonePose();
    TArray<FTransform> World;
    World.SetNum(BoneCount);
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        const int32 Parent = Skeleton.GetParentIndex(Index);
        World[Index] = Parent >= 0 ? Local[Index] * World[Parent] : Local[Index];
    }
    auto At = [&World, &Bone](const TCHAR* Name) { return World[Bone(Name)].GetLocation(); };

    const FVector Left = (At(TEXT("LeftUpLeg")) - At(TEXT("RightUpLeg"))).GetSafeNormal();
    const FVector Up = Orthogonal(At(TEXT("Head")) - At(TEXT("Hips")), Left);
    const FVector Forward = Orthogonal(
        (At(TEXT("LeftToeBase")) - At(TEXT("LeftFoot"))) + (At(TEXT("RightToeBase")) - At(TEXT("RightFoot"))),
        Left, &Up);
    if (Left.IsNearlyZero() || Up.IsNearlyZero() || Forward.IsNearlyZero())
    {
        UE_LOG(LogLoomaLivePose, Warning, TEXT("%s: the reference pose is degenerate."), *Mesh->GetName());
        return false;
    }
    CharacterInComponent = FLoomaBasis::FromColumns(Left, Up, Forward);

    // Every bone is identity in the T-pose, so every bone's T-pose frame is the hips'.
    HipsRest = FLoomaBasis::FromQuat(World[HipsBone].GetRotation());
    CharacterInBone = HipsRest.Transposed() * CharacterInComponent;
    const int32 HipsParent = Skeleton.GetParentIndex(HipsBone);
    HipsParentInverse = HipsParent >= 0
        ? FLoomaBasis::FromQuat(World[HipsParent].GetRotation()).Transposed()
        : FLoomaBasis();
    // Above the floor, which the rig format puts at the mesh's origin ("feet at
    // y = 0"): the `hips_height` the wire's hips are in units of, and what the
    // web multiplies by. Not above the toe joints, which sit a centimetre or two
    // over the floor and would leave this client's character that much lower
    // than everybody else's.
    HipsHeight = FVector::DotProduct(At(TEXT("Hips")), Up);
    if (HipsHeight <= KINDA_SMALL_NUMBER)
    {
        UE_LOG(LogLoomaLivePose, Warning, TEXT("%s: the hips are not above the mesh's origin."), *Mesh->GetName());
        return false;
    }

    Rotations.Init(FQuat::Identity, BoneCount);
    HasRotation.Init(false, BoneCount);
    WireRotations.Init(FQuat::Identity, LoomaRigBody::Num);
    HipsLocation = Local[HipsBone].GetLocation();

    const int32 Spine = TargetBones[LoomaRigBody::Row(TEXT("Spine"))];
    UE_LOG(LogLoomaLivePose, Log,
           TEXT("%s: %d bones, hips %.1f above the floor; left %s up %s forward %s; hips rest %s, spine rest %s."),
           *Mesh->GetName(), BoneCount, HipsHeight, *Left.ToCompactString(), *Up.ToCompactString(),
           *Forward.ToCompactString(), *World[HipsBone].GetRotation().Rotator().ToCompactString(),
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
            // Stamped with when Live Link says the frame was captured, on the
            // engine's clock, so a take the hub records keeps the capture's
            // spacing and not whatever the network did to it on the way.
            Sync->PublishPose(Actor->Id, WireHips, WireRotations,
                              Frame.FrameData.GetBaseData()->WorldTime.GetOffsettedTime());
        }
    }
}
