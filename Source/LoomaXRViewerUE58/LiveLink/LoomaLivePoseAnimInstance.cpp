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

    // The floor is a low percentile of where the feet rested over a window. Chosen
    // by measuring, on generated characters with the Moverse simulator's three
    // performers, how close the lowest vertex then stays to the floor and how much
    // the floor estimate itself wanders: 5 s and the 35th percentile kept the sole
    // within about a centimetre on average with the least wander.
    const float FloorSampleSeconds = 0.05f;
    const int32 FloorWindowSamples = 100;
    const double FloorPercentile = 0.35;
    const double FloorEaseSeconds = 0.25;

    /** A body bone of the rig (after `mixamorig:`) and the row of its parent, -1 for the hips. */
    struct FRigBone
    {
        const TCHAR* Target;
        int32 Parent;
    };

    // The rig format's BODY order, the 52 bones with the fingers left out: it is
    // the order the wire's `pose` carries them in, and parents come before
    // children. Which Live Link bone drives each is a profile's business
    // (Config/LiveLinkProfiles.json), not this table's.
    const FRigBone BoneTable[] = {
        {TEXT("Hips"), -1},
        {TEXT("Spine"), 0},
        {TEXT("Spine1"), 1},
        {TEXT("Spine2"), 2},
        {TEXT("Neck"), 3},
        {TEXT("Head"), 4},
        {TEXT("LeftShoulder"), 3},
        {TEXT("LeftArm"), 6},
        {TEXT("LeftForeArm"), 7},
        {TEXT("LeftHand"), 8},
        {TEXT("RightShoulder"), 3},
        {TEXT("RightArm"), 10},
        {TEXT("RightForeArm"), 11},
        {TEXT("RightHand"), 12},
        {TEXT("LeftUpLeg"), 0},
        {TEXT("LeftLeg"), 14},
        {TEXT("LeftFoot"), 15},
        {TEXT("LeftToeBase"), 16},
        {TEXT("RightUpLeg"), 0},
        {TEXT("RightLeg"), 18},
        {TEXT("RightFoot"), 19},
        {TEXT("RightToeBase"), 20},
    };
    const int32 TableRows = UE_ARRAY_COUNT(BoneTable);

    int32 Row(const TCHAR* Target)
    {
        for (int32 Index = 0; Index < TableRows; ++Index)
        {
            if (FCString::Strcmp(BoneTable[Index].Target, Target) == 0)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

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

// --- FLoomaBasis ------------------------------------------------------------------

FLoomaBasis FLoomaBasis::FromColumns(const FVector& X, const FVector& Y, const FVector& Z)
{
    FLoomaBasis Out;
    const FVector* Columns[3] = {&X, &Y, &Z};
    for (int32 C = 0; C < 3; ++C)
    {
        Out.M[0][C] = Columns[C]->X;
        Out.M[1][C] = Columns[C]->Y;
        Out.M[2][C] = Columns[C]->Z;
    }
    return Out;
}

FLoomaBasis FLoomaBasis::FromQuat(const FQuat& Q)
{
    // The map v -> q v q^-1, which is what FQuat::RotateVector computes.
    return FromColumns(Q.RotateVector(FVector(1, 0, 0)), Q.RotateVector(FVector(0, 1, 0)),
                       Q.RotateVector(FVector(0, 0, 1)));
}

FLoomaBasis FLoomaBasis::operator*(const FLoomaBasis& Other) const
{
    FLoomaBasis Out;
    for (int32 R = 0; R < 3; ++R)
    {
        for (int32 C = 0; C < 3; ++C)
        {
            Out.M[R][C] = M[R][0] * Other.M[0][C] + M[R][1] * Other.M[1][C] + M[R][2] * Other.M[2][C];
        }
    }
    return Out;
}

FLoomaBasis FLoomaBasis::Transposed() const
{
    FLoomaBasis Out;
    for (int32 R = 0; R < 3; ++R)
    {
        for (int32 C = 0; C < 3; ++C)
        {
            Out.M[R][C] = M[C][R];
        }
    }
    return Out;
}

FVector FLoomaBasis::Apply(const FVector& V) const
{
    return FVector(M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
                   M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
                   M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
}

FQuat FLoomaBasis::ToQuat() const
{
    // Shepperd's method, for the same v -> q v q^-1 convention as FromQuat.
    const double Trace = M[0][0] + M[1][1] + M[2][2];
    double X, Y, Z, W;
    if (Trace > 0.0)
    {
        const double S = 2.0 * FMath::Sqrt(1.0 + Trace);
        W = S / 4.0;
        X = (M[2][1] - M[1][2]) / S;
        Y = (M[0][2] - M[2][0]) / S;
        Z = (M[1][0] - M[0][1]) / S;
    }
    else if (M[0][0] >= M[1][1] && M[0][0] >= M[2][2])
    {
        const double S = 2.0 * FMath::Sqrt(1.0 + M[0][0] - M[1][1] - M[2][2]);
        X = S / 4.0;
        Y = (M[0][1] + M[1][0]) / S;
        Z = (M[0][2] + M[2][0]) / S;
        W = (M[2][1] - M[1][2]) / S;
    }
    else if (M[1][1] >= M[2][2])
    {
        const double S = 2.0 * FMath::Sqrt(1.0 + M[1][1] - M[0][0] - M[2][2]);
        X = (M[0][1] + M[1][0]) / S;
        Y = S / 4.0;
        Z = (M[1][2] + M[2][1]) / S;
        W = (M[0][2] - M[2][0]) / S;
    }
    else
    {
        const double S = 2.0 * FMath::Sqrt(1.0 + M[2][2] - M[0][0] - M[1][1]);
        X = (M[0][2] + M[2][0]) / S;
        Y = (M[1][2] + M[2][1]) / S;
        Z = S / 4.0;
        W = (M[1][0] - M[0][1]) / S;
    }
    FQuat Out(X, Y, Z, W);
    Out.Normalize();
    return Out;
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
    bHasOrigin = false;
    bHasPose = false;
    bSceneStarted = false;
}

void ULoomaLivePoseAnimInstance::FollowScene(bool bFollow)
{
    if (bFromScene != bFollow)
    {
        bFromScene = bFollow;
        bSceneStarted = false;
        bHasOrigin = false;
    }
}

void ULoomaLivePoseAnimInstance::SetFromWire(const FVector* Hips, const TArray<FQuat>& Wire, float Alpha)
{
    const FLoomaBasis BoneToCharacter = CharacterInBone.Transposed();
    for (int32 Index = 0; Index < TableRows && Index < Wire.Num(); ++Index)
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
        if (BoneTable[Index].Parent < 0)
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

double ULoomaLivePoseAnimInstance::UpdateFloor(double Support, float DeltaSeconds)
{
    if (!bHasFloor)
    {
        // The first frame is all there is to go on; a performer who starts in
        // the air is put right within the window.
        bHasFloor = true;
        FloorSamples.Reset();
        FloorNext = 0;
        SinceFloorSample = FloorSampleSeconds;
        FloorLevel = Support;
    }
    SinceFloorSample += DeltaSeconds;
    if (SinceFloorSample >= FloorSampleSeconds)
    {
        SinceFloorSample = 0.0f;
        if (FloorSamples.Num() < FloorWindowSamples)
        {
            FloorSamples.Add(Support);
        }
        else
        {
            FloorSamples[FloorNext] = Support;
            FloorNext = (FloorNext + 1) % FloorWindowSamples;
        }
    }
    TArray<double> Sorted = FloorSamples;
    Sorted.Sort();
    const double Raw = Sorted[FMath::FloorToInt32(FloorPercentile * (Sorted.Num() - 1))];
    FloorLevel += (Raw - FloorLevel) * (1.0 - FMath::Exp(-double(DeltaSeconds) / FloorEaseSeconds));
    return FloorLevel;
}

void ULoomaLivePoseAnimInstance::UpdateFromScene(float DeltaSeconds)
{
    const USkeletalMeshComponent* Component = GetSkelMeshComponent();
    const ALoomaSyncedActor* Actor = Component ? Cast<ALoomaSyncedActor>(Component->GetOwner()) : nullptr;
    const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    const ULoomaSceneSyncSubsystem* Sync = GameInstance ? GameInstance->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    FLoomaLivePose Pose;
    if (!Actor || !Sync || !Sync->GetLivePose(Actor->Id, Pose))
    {
        return; // hold the last pose; the driver lets go of a character whose frames stopped
    }
    // Close on the newest frame at a speed that suits how often frames come, as
    // the web does (livePose.js): about three arrivals' worth a second, so one
    // character at 30 a second is crisp and a crowd sharing the budget, a few a
    // second each, glides between poses instead of jumping. The first frame is
    // taken whole, so the character does not swing in from its rest pose.
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
    TargetBones.Init(INDEX_NONE, TableRows);
    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        if (const int32* Found = ByName.Find(BoneTable[Index].Target))
        {
            TargetBones[Index] = *Found;
        }
    }
    auto Bone = [this](const TCHAR* Name) { return TargetBones[Row(Name)]; };
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
    WireRotations.Init(FQuat::Identity, TableRows);
    HipsLocation = Local[HipsBone].GetLocation();

    const int32 Spine = TargetBones[Row(TEXT("Spine"))];
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
    SourceBones.Init(INDEX_NONE, TableRows);
    RootUndo = FQuat::Identity;
    const FLoomaLiveLinkProfile* Profile = LoomaLiveLinkProfiles::For(BoneNames, SourceType);
    if (!Profile)
    {
        UE_LOG(LogLoomaLivePose, Warning,
               TEXT("Live Link subject '%s' matches no profile in Config/LiveLinkProfiles.json; its first bone is '%s'."),
               *SubjectName.ToString(), BoneNames.Num() > 0 ? *BoneNames[0].ToString() : TEXT("-"));
        return false;
    }
    int32 Mapped = 0;
    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        if (const FName* Source = Profile->Bones.Find(BoneTable[Index].Target))
        {
            SourceBones[Index] = BoneNames.IndexOfByKey(*Source);
            Mapped += SourceBones[Index] != INDEX_NONE ? 1 : 0;
        }
    }
    const FVector Left = Profile->BoneLeft.GetSafeNormal();
    const FVector Up = Profile->BoneUp.GetSafeNormal();
    const FVector Forward = Profile->BoneForward.GetSafeNormal();
    CharacterInSource = FLoomaBasis::FromColumns(Left, Up, Forward);
    SourceBoneUp = Up;
    RootUndo = Profile->RootRotation.Inverse();
    WorldToCharacter = FLoomaBasis::FromColumns(
        Profile->WorldLeft.GetSafeNormal(), Profile->WorldUp.GetSafeNormal(), Profile->WorldForward.GetSafeNormal()
    ).Transposed();
    UE_LOG(LogLoomaLivePose, Log, TEXT("Live Link subject '%s' from '%s' is a '%s' skeleton: %d of %d bones mapped."),
           *SubjectName.ToString(), *SourceType, *Profile->Name, Mapped, TableRows);
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
        bHasOrigin = false;
    }
    if (!bSourceReady)
    {
        return;
    }
    const TArray<FTransform>& Local = Animation->Transforms;
    const TArray<int32>& Parents = Skeleton->GetBoneParents();

    // Every source bone in the source's world. A Live Link frame is parent-relative.
    SourceWorld.SetNum(Local.Num());
    SourceDone.Init(false, Local.Num());
    TFunction<const FTransform&(int32, int32)> WorldOf = [&](int32 Bone, int32 Depth) -> const FTransform& {
        if (!SourceDone[Bone])
        {
            const int32 Parent = Parents.IsValidIndex(Bone) ? Parents[Bone] : INDEX_NONE;
            if (Local.IsValidIndex(Parent) && Depth < Local.Num())
            {
                SourceWorld[Bone] = Local[Bone] * WorldOf(Parent, Depth + 1);
            }
            else
            {
                // A root: whatever the source put in front of its rotation comes off.
                SourceWorld[Bone] = Local[Bone];
                SourceWorld[Bone].SetRotation((RootUndo * Local[Bone].GetRotation()).GetNormalized());
            }
            SourceDone[Bone] = true;
        }
        return SourceWorld[Bone];
    };

    // Each rig bone's turn from its T-pose, in the character's own left, up,
    // forward and in the world: W_source written in the character's axes on both
    // sides. In the T-pose that is the identity for every bone, which is the one
    // thing a profile's skeleton has to have in common with the rig. A rig bone
    // nothing drives takes its parent's, so it keeps its T-pose rotation
    // relative to it. In world space, so a source with a bone the rig has no
    // room for (Xsens has four spine segments, the rig three) loses nothing.
    FLoomaBasis Turn[UE_ARRAY_COUNT(BoneTable)];
    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        const int32 From = SourceBones[Index];
        if (From != INDEX_NONE)
        {
            Turn[Index] = WorldToCharacter * FLoomaBasis::FromQuat(WorldOf(From, 0).GetRotation()) * CharacterInSource;
        }
        else if (BoneTable[Index].Parent >= 0)
        {
            Turn[Index] = Turn[BoneTable[Index].Parent];
        }
    }

    // Relative to the rig's parent bone: the wire's rotation as it stands, since
    // the character's left, up, forward are the rig file's X, Y, Z.
    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        const int32 Parent = BoneTable[Index].Parent;
        WireRotations[Index] = (Parent >= 0 ? Turn[Parent].Transposed() * Turn[Index] : Turn[Index]).ToQuat();
    }

    // The hips: height by leg length, travel across the floor from the first
    // frame. Leg length from the bones' own offsets, which do not move: the drop
    // from pelvis to hip joint, thigh, shin, and the drop from ankle to toe.
    auto Offset = [this, &Local](const TCHAR* Target) {
        const int32 Bone = SourceBones[Row(Target)];
        return Bone != INDEX_NONE ? Local[Bone].GetLocation() : FVector::ZeroVector;
    };
    const double SourceLeg =
        FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftUpLeg")), SourceBoneUp))
        + Offset(TEXT("LeftLeg")).Size() + Offset(TEXT("LeftFoot")).Size()
        + FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftToeBase")), SourceBoneUp));
    const int32 Pelvis = SourceBones[0];
    const bool bHips = SourceLeg > KINDA_SMALL_NUMBER && Pelvis != INDEX_NONE;
    if (bHips)
    {
        const FVector InCharacter = WorldToCharacter.Apply(WorldOf(Pelvis, 0).GetLocation());
        if (!bHasOrigin)
        {
            bHasOrigin = true;
            Origin = FVector(InCharacter.X, 0.0, InCharacter.Z);
            bHasFloor = false;
        }
        // The lowest point a foot rests on: a toe joint, or an ankle less the
        // height it rests at over the toe. `SourceLeg` ends at the toe joint too,
        // so standing straight on that floor is a height of exactly one.
        auto Rests = [&](const TCHAR* Target, double Less) {
            const int32 Bone = SourceBones[Row(Target)];
            return Bone != INDEX_NONE ? WorldToCharacter.Apply(WorldOf(Bone, 0).GetLocation()).Y - Less : TNumericLimits<double>::Max();
        };
        const double AnkleRest = FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftToeBase")), SourceBoneUp));
        const double Support = FMath::Min(
            FMath::Min(Rests(TEXT("LeftToeBase"), 0.0), Rests(TEXT("RightToeBase"), 0.0)),
            FMath::Min(Rests(TEXT("LeftFoot"), AnkleRest), Rests(TEXT("RightFoot"), AnkleRest)));
        const bool bFindFloor = CVarFindFloor.GetValueOnGameThread() != 0 && Support < TNumericLimits<double>::Max();
        const double Floor = bFindFloor ? UpdateFloor(Support, DeltaSeconds) : 0.0;
        WireHips = (InCharacter - Origin - FVector(0.0, Floor, 0.0)) / SourceLeg;
    }
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
