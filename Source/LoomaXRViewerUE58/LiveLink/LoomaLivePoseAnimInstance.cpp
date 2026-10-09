#include "LoomaLivePoseAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"
#include "ReferenceSkeleton.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLivePose, Log, All);

namespace
{
    /** A Moverse bone (after `moverserig_`) and the rig bone (after `mixamorig:`) it drives. */
    struct FBonePair
    {
        const TCHAR* Source;
        const TCHAR* Target;
    };

    // Row 0 is the pelvis. SMPL has three spine joints and so has the rig; its
    // "collar" is the rig's shoulder and its "shoulder" the rig's arm.
    //
    // The rows are in the rig format's BODY order, the 52 bones with the fingers
    // left out, because that is the order the wire's `pose` carries them in.
    const FBonePair BoneTable[] = {
        {TEXT("Pelvis"), TEXT("Hips")},
        {TEXT("Spine1"), TEXT("Spine")},
        {TEXT("Spine2"), TEXT("Spine1")},
        {TEXT("Spine3"), TEXT("Spine2")},
        {TEXT("Neck"), TEXT("Neck")},
        {TEXT("Head"), TEXT("Head")},
        {TEXT("LeftCollar"), TEXT("LeftShoulder")},
        {TEXT("LeftShoulder"), TEXT("LeftArm")},
        {TEXT("LeftElbow"), TEXT("LeftForeArm")},
        {TEXT("LeftWrist"), TEXT("LeftHand")},
        {TEXT("RightCollar"), TEXT("RightShoulder")},
        {TEXT("RightShoulder"), TEXT("RightArm")},
        {TEXT("RightElbow"), TEXT("RightForeArm")},
        {TEXT("RightWrist"), TEXT("RightHand")},
        {TEXT("LeftHip"), TEXT("LeftUpLeg")},
        {TEXT("LeftKnee"), TEXT("LeftLeg")},
        {TEXT("LeftAnkle"), TEXT("LeftFoot")},
        {TEXT("LeftFoot"), TEXT("LeftToeBase")},
        {TEXT("RightHip"), TEXT("RightUpLeg")},
        {TEXT("RightKnee"), TEXT("RightLeg")},
        {TEXT("RightAnkle"), TEXT("RightFoot")},
        {TEXT("RightFoot"), TEXT("RightToeBase")},
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

    /** "mixamorig:Hips", "mixamorig_Hips", "moverserig_Pelvis" -> the part after the rig prefix. */
    FString Bare(const FName& Name)
    {
        FString Text = Name.ToString();
        int32 Cut = INDEX_NONE;
        if (Text.FindLastChar(TEXT(':'), Cut))
        {
            Text.RightChopInline(Cut + 1);
        }
        for (const TCHAR* Prefix : {TEXT("mixamorig_"), TEXT("mixamorig"), TEXT("moverserig_")})
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
    HipsHeight = FVector::DotProduct(At(TEXT("Hips")), Up)
        - FMath::Min(FVector::DotProduct(At(TEXT("LeftToeBase")), Up), FVector::DotProduct(At(TEXT("RightToeBase")), Up));

    Rotations.Init(FQuat::Identity, BoneCount);
    HasRotation.Init(false, BoneCount);
    WireRotations.Init(FQuat::Identity, TableRows);
    HipsLocation = Local[HipsBone].GetLocation();

    const int32 Spine = TargetBones[Row(TEXT("Spine"))];
    UE_LOG(LogLoomaLivePose, Log,
           TEXT("%s: %d bones, hips %.1f above the toes; left %s up %s forward %s; hips rest %s, spine rest %s."),
           *Mesh->GetName(), BoneCount, HipsHeight, *Left.ToCompactString(), *Up.ToCompactString(),
           *Forward.ToCompactString(), *World[HipsBone].GetRotation().Rotator().ToCompactString(),
           Spine != INDEX_NONE ? *Local[Spine].GetRotation().Rotator().ToCompactString() : TEXT("-"));
    return true;
}

bool ULoomaLivePoseAnimInstance::BuildSource(const TArray<FName>& BoneNames)
{
    SourceNames = BoneNames;
    TMap<FString, int32> ByName;
    for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone)
    {
        ByName.Add(Bare(BoneNames[Bone]), Bone);
    }
    SourceBones.Init(INDEX_NONE, TableRows);
    int32 Mapped = 0;
    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        if (const int32* Found = ByName.Find(BoneTable[Index].Source))
        {
            SourceBones[Index] = *Found;
            ++Mapped;
        }
    }
    const bool bUsable = SourceBones[0] != INDEX_NONE
        && SourceBones[Row(TEXT("LeftUpLeg"))] != INDEX_NONE && SourceBones[Row(TEXT("RightUpLeg"))] != INDEX_NONE
        && SourceBones[Row(TEXT("LeftLeg"))] != INDEX_NONE && SourceBones[Row(TEXT("RightLeg"))] != INDEX_NONE
        && SourceBones[Row(TEXT("LeftFoot"))] != INDEX_NONE && SourceBones[Row(TEXT("RightFoot"))] != INDEX_NONE
        && SourceBones[Row(TEXT("LeftToeBase"))] != INDEX_NONE && SourceBones[Row(TEXT("RightToeBase"))] != INDEX_NONE;
    UE_LOG(LogLoomaLivePose, Log, TEXT("Live Link subject '%s': %d of %d bones mapped%s."),
           *SubjectName.ToString(), Mapped, TableRows, bUsable ? TEXT("") : TEXT("; not a Moverse skeleton"));
    return bUsable;
}

void ULoomaLivePoseAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!bTargetReady)
    {
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
    if (!bSourceReady || SourceNames != Skeleton->GetBoneNames())
    {
        bSourceReady = BuildSource(Skeleton->GetBoneNames());
        bHasOrigin = false;
    }
    if (!bSourceReady)
    {
        return;
    }
    const TArray<FTransform>& T = Animation->Transforms;
    auto Source = [this, &T](const TCHAR* Target) -> const FTransform& { return T[SourceBones[Row(Target)]]; };

    // The character's axes in a source bone's own coordinates, from this frame's
    // offsets: hip to hip, hip to knee, ankle to toe. Bone offsets do not move.
    const FVector Left = (Source(TEXT("LeftUpLeg")).GetLocation() - Source(TEXT("RightUpLeg")).GetLocation()).GetSafeNormal();
    const FVector Up = Orthogonal(
        -(Source(TEXT("LeftLeg")).GetLocation() + Source(TEXT("RightLeg")).GetLocation()), Left);
    const FVector Forward = Orthogonal(
        Source(TEXT("LeftToeBase")).GetLocation() + Source(TEXT("RightToeBase")).GetLocation(), Left, &Up);
    if (Left.IsNearlyZero() || Up.IsNearlyZero() || Forward.IsNearlyZero())
    {
        return;
    }
    const FLoomaBasis CharacterInSource = FLoomaBasis::FromColumns(Left, Up, Forward); // A_s
    const FLoomaBasis SourceToCharacter = CharacterInSource.Transposed();
    const FLoomaBasis WorldToCharacter =
        FLoomaBasis::FromColumns(SourceLeftAxis, SourceUpAxis, SourceForwardAxis).Transposed();
    const FLoomaBasis BoneToCharacter = CharacterInBone.Transposed();

    // The pelvis in the source's world: its ancestors' transforms are applied first.
    const int32 Pelvis = SourceBones[0];
    FTransform PelvisWorld = T[Pelvis];
    for (int32 Parent = Skeleton->GetBoneParents()[Pelvis]; Parent >= 0 && Parent < T.Num();
         Parent = Skeleton->GetBoneParents()[Parent])
    {
        PelvisWorld = PelvisWorld * T[Parent];
    }

    for (int32 Index = 0; Index < TableRows; ++Index)
    {
        const int32 From = SourceBones[Index];
        const int32 To = TargetBones[Index];
        if (From == INDEX_NONE || To == INDEX_NONE)
        {
            continue;
        }
        if (Index == 0)
        {
            const FLoomaBasis Turn =
                WorldToCharacter * FLoomaBasis::FromQuat(PelvisWorld.GetRotation()) * CharacterInSource;
            const FLoomaBasis InComponent = CharacterInComponent * Turn * CharacterInComponent.Transposed() * HipsRest;
            Rotations[To] = (HipsParentInverse * InComponent).ToQuat();
            WireRotations[Index] = Turn.ToQuat();
        }
        else
        {
            const FLoomaBasis Turn = SourceToCharacter * FLoomaBasis::FromQuat(T[From].GetRotation()) * CharacterInSource;
            Rotations[To] = (CharacterInBone * Turn * BoneToCharacter).ToQuat();
            // `Turn` is the rotation in the character's own left, up, forward,
            // which are the rig file's X, Y, Z: the wire's rotation as it stands.
            WireRotations[Index] = Turn.ToQuat();
        }
        HasRotation[To] = true;
    }

    // The hips: height by leg length, travel across the floor from the first frame.
    const double SourceLeg =
        FMath::Abs(FVector::DotProduct(Source(TEXT("LeftUpLeg")).GetLocation(), Up))
        + Source(TEXT("LeftLeg")).GetLocation().Size() + Source(TEXT("LeftFoot")).GetLocation().Size()
        + FMath::Abs(FVector::DotProduct(Source(TEXT("LeftToeBase")).GetLocation(), Up));
    if (SourceLeg > KINDA_SMALL_NUMBER)
    {
        const double Scale = HipsHeight / SourceLeg;
        const FVector InCharacter = WorldToCharacter.Apply(PelvisWorld.GetLocation());
        if (!bHasOrigin)
        {
            bHasOrigin = true;
            Origin = FVector(InCharacter.X, 0.0, InCharacter.Z);
        }
        const FVector Scaled = (InCharacter - Origin) * Scale;
        HipsLocation = HipsParentInverse.Apply(CharacterInComponent.Apply(Scaled));
        WireHips = (InCharacter - Origin) / SourceLeg;
    }
    bHasPose = true;

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
