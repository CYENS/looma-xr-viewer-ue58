#include "LoomaLiveTarget.h"

#include "ReferenceSkeleton.h"

namespace
{
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

bool FLoomaLiveTarget::Build(const FReferenceSkeleton& Skeleton, FString& OutWhy)
{
    BoneCount = Skeleton.GetNum();

    TMap<FString, int32> ByName;
    for (int32 Index = 0; Index < BoneCount; ++Index)
    {
        ByName.Add(Bare(Skeleton.GetBoneName(Index)), Index);
    }
    Bones.Init(INDEX_NONE, LoomaRigBody::Num);
    for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
    {
        if (const int32* Found = ByName.Find(LoomaRigBody::Name(Row)))
        {
            Bones[Row] = *Found;
        }
    }
    auto Bone = [this](const TCHAR* Name) { return Bones[LoomaRigBody::Row(Name)]; };
    HipsBone = Bone(TEXT("Hips"));
    for (const TCHAR* Needed : {TEXT("Hips"), TEXT("Head"), TEXT("LeftUpLeg"), TEXT("RightUpLeg"),
                                TEXT("LeftFoot"), TEXT("LeftToeBase"), TEXT("RightFoot"), TEXT("RightToeBase")})
    {
        if (Bone(Needed) == INDEX_NONE)
        {
            OutWhy = FString::Printf(TEXT("not a LoomaMixamo rig, it has no %s bone"), Needed);
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

    // What the body's own shape says: hip to hip, hips to head, heel to toe. Close,
    // and never exact: a head that rests a few centimetres behind the hips leans
    // this "up" by several degrees. It is only what the rig is checked against,
    // and the fallback for a skeleton the rig format does not describe.
    const FVector ShapeLeft = (At(TEXT("LeftUpLeg")) - At(TEXT("RightUpLeg"))).GetSafeNormal();
    const FVector ShapeUp = Orthogonal(At(TEXT("Head")) - At(TEXT("Hips")), ShapeLeft);
    const FVector Toes = (At(TEXT("LeftToeBase")) - At(TEXT("LeftFoot"))) + (At(TEXT("RightToeBase")) - At(TEXT("RightFoot")));
    const FVector ShapeForward = Orthogonal(Toes, ShapeLeft, &ShapeUp);
    if (ShapeLeft.IsNearlyZero() || ShapeUp.IsNearlyZero() || ShapeForward.IsNearlyZero())
    {
        OutWhy = TEXT("the reference pose is degenerate");
        return false;
    }

    // The character's left, up, forward are the rig FILE's X, Y, Z: the wire's
    // rotations and its hips are written in those, and the web sets them as they
    // come. So they are read here from what the format fixes for every rig
    // (docs/rig-format.md in looma-xr-asset-demo), not from how this body is built:
    //
    //  * up, from the origin to the hips. The origin is on the floor straight
    //    below the hips joint ("glTF layout", rule 7).
    //  * forward, the axis the upper arms rest turned about. The rest pose is the
    //    T-pose with each upper arm let down about +Z and nothing else ("The rest
    //    pose is an A-pose"), so that axis is the file's Z whatever the angle.
    //
    // Which way round each runs is taken from the shape, which is good to tens of
    // degrees and is asked only for a sign. A skeleton that does not keep to the
    // format (hips not over the origin, arms not turned about forward) takes the
    // axis it lacks from its shape, as every rig once did, and says so.
    const double Near = FMath::Cos(FMath::DegreesToRadians(30.0));
    Up = At(TEXT("Hips")).GetSafeNormal();
    bUpFromOrigin = FVector::DotProduct(Up, ShapeUp) >= Near;
    if (!bUpFromOrigin)
    {
        Up = ShapeUp;
    }
    const FVector ToesAcross = Orthogonal(Toes, Up);
    FVector Turned = FVector::ZeroVector;
    for (const TCHAR* Arm : {TEXT("LeftArm"), TEXT("RightArm")})
    {
        const int32 ArmBone = Bone(Arm);
        const int32 Parent = ArmBone != INDEX_NONE ? Skeleton.GetParentIndex(ArmBone) : INDEX_NONE;
        if (Parent < 0)
        {
            continue;
        }
        const FQuat Rest = Local[ArmBone].GetRotation().GetNormalized();
        if (FMath::Abs(Rest.W) > FMath::Cos(FMath::DegreesToRadians(2.5)))
        {
            continue; // turned less than five degrees: this rig rests in a T-pose
        }
        const FVector Axis = World[Parent].GetRotation().RotateVector(FVector(Rest.X, Rest.Y, Rest.Z).GetSafeNormal());
        Turned += FVector::DotProduct(Axis, ToesAcross) >= 0.0 ? Axis : -Axis;
    }
    Forward = Orthogonal(Turned, Up);
    bForwardFromArms = FVector::DotProduct(Forward, ToesAcross) >= Near;
    if (!bForwardFromArms)
    {
        Forward = ToesAcross;
    }
    Left = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    if (FVector::DotProduct(Left, ShapeLeft) < 0.0)
    {
        Left = -Left;
    }
    if (Left.IsNearlyZero() || Up.IsNearlyZero() || Forward.IsNearlyZero())
    {
        OutWhy = TEXT("the reference pose is degenerate");
        return false;
    }
    Lean = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
        FVector::DotProduct(Up, (At(TEXT("Head")) - At(TEXT("Hips"))).GetSafeNormal()), -1.0, 1.0)));
    CharacterInComponent = FLoomaBasis::FromColumns(Left, Up, Forward);

    // Every bone is identity in the T-pose, so every bone's T-pose frame is the hips'.
    HipsRestRotation = World[HipsBone].GetRotation();
    HipsRest = FLoomaBasis::FromQuat(HipsRestRotation);
    CharacterInBone = HipsRest.Transposed() * CharacterInComponent;
    BoneToCharacter = CharacterInBone.Transposed();
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
        OutWhy = TEXT("the hips are not above the mesh's origin");
        return false;
    }
    return true;
}

FQuat FLoomaLiveTarget::Rotation(int32 Row, const FQuat& Wire) const
{
    // The wire's rotation is the bone's turn from its T-pose in the character's
    // left, up, forward; here it is written in this mesh's bone.
    const FLoomaBasis LocalTurn = FLoomaBasis::FromQuat(Wire);
    if (LoomaRigBody::Parent(Row) < 0)
    {
        const FLoomaBasis InComponent = CharacterInComponent * LocalTurn * CharacterInComponent.Transposed() * HipsRest;
        return (HipsParentInverse * InComponent).ToQuat();
    }
    return (CharacterInBone * LocalTurn * BoneToCharacter).ToQuat();
}

FVector FLoomaLiveTarget::HipsLocation(const FVector& WireHips) const
{
    return HipsParentInverse.Apply(CharacterInComponent.Apply(WireHips * HipsHeight));
}
