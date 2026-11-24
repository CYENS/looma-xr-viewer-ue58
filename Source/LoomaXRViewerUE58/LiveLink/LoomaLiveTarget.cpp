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

    Left = (At(TEXT("LeftUpLeg")) - At(TEXT("RightUpLeg"))).GetSafeNormal();
    Up = Orthogonal(At(TEXT("Head")) - At(TEXT("Hips")), Left);
    Forward = Orthogonal(
        (At(TEXT("LeftToeBase")) - At(TEXT("LeftFoot"))) + (At(TEXT("RightToeBase")) - At(TEXT("RightFoot"))),
        Left, &Up);
    if (Left.IsNearlyZero() || Up.IsNearlyZero() || Forward.IsNearlyZero())
    {
        OutWhy = TEXT("the reference pose is degenerate");
        return false;
    }
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
