#include "LoomaLiveRetarget.h"

#include "LoomaLiveLinkProfiles.h"

namespace
{
    // The floor is a low percentile of where the feet rested over a window. Chosen
    // by measuring, on generated characters with the Moverse simulator's three
    // performers, how close the lowest vertex then stays to the floor and how much
    // the floor estimate itself wanders: 5 s and the 35th percentile kept the sole
    // within about a centimetre on average with the least wander.
    const float FloorSampleSeconds = 0.05f;
    const int32 FloorWindowSamples = 100;
    const double FloorPercentile = 0.35;
    const double FloorEaseSeconds = 0.25;

    struct FRigBone
    {
        const TCHAR* Target;
        int32 Parent;
    };

    // Which Live Link bone drives each is a profile's business
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
    static_assert(UE_ARRAY_COUNT(BoneTable) == LoomaRigBody::Num, "the body bone table and LoomaRigBody::Num disagree");
}

const TCHAR* LoomaRigBody::Name(int32 Row)
{
    return BoneTable[Row].Target;
}

int32 LoomaRigBody::Parent(int32 Row)
{
    return BoneTable[Row].Parent;
}

int32 LoomaRigBody::Row(const TCHAR* Name)
{
    for (int32 Index = 0; Index < Num; ++Index)
    {
        if (FCString::Strcmp(BoneTable[Index].Target, Name) == 0)
        {
            return Index;
        }
    }
    return INDEX_NONE;
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

// --- FLoomaLiveRetarget -------------------------------------------------------------

int32 FLoomaLiveRetarget::Build(const FLoomaLiveLinkProfile& Profile, const TArray<FName>& BoneNames)
{
    SourceBones.Init(INDEX_NONE, LoomaRigBody::Num);
    int32 Mapped = 0;
    for (int32 Index = 0; Index < LoomaRigBody::Num; ++Index)
    {
        if (const FName* Source = Profile.Bones.Find(BoneTable[Index].Target))
        {
            SourceBones[Index] = BoneNames.IndexOfByKey(*Source);
            Mapped += SourceBones[Index] != INDEX_NONE ? 1 : 0;
        }
    }
    const FVector Left = Profile.BoneLeft.GetSafeNormal();
    const FVector Up = Profile.BoneUp.GetSafeNormal();
    const FVector Forward = Profile.BoneForward.GetSafeNormal();
    CharacterInSource = FLoomaBasis::FromColumns(Left, Up, Forward);
    SourceBoneUp = Up;
    RootUndo = Profile.RootRotation.Inverse();
    WorldToCharacter = FLoomaBasis::FromColumns(
        Profile.WorldLeft.GetSafeNormal(), Profile.WorldUp.GetSafeNormal(), Profile.WorldForward.GetSafeNormal()
    ).Transposed();
    Restart();
    return Mapped;
}

void FLoomaLiveRetarget::Restart()
{
    bHasOrigin = false;
    bHasFloor = false;
}

double FLoomaLiveRetarget::UpdateFloor(double Support, float DeltaSeconds)
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

bool FLoomaLiveRetarget::Evaluate(const TArray<FTransform>& Local, const TArray<int32>& Parents, float DeltaSeconds,
                                  bool bFindFloor, TArray<FQuat>& OutWire, FVector& OutHips)
{
    OutWire.SetNum(LoomaRigBody::Num);
    if (SourceBones.Num() != LoomaRigBody::Num)
    {
        for (FQuat& Rotation : OutWire)
        {
            Rotation = FQuat::Identity;
        }
        return false;
    }

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
    auto Mapped = [this, &Local](int32 Row) {
        return Local.IsValidIndex(SourceBones[Row]) ? SourceBones[Row] : INDEX_NONE;
    };

    // Each rig bone's turn from its T-pose, in the character's own left, up,
    // forward and in the world: W_source written in the character's axes on both
    // sides. In the T-pose that is the identity for every bone, which is the one
    // thing a profile's skeleton has to have in common with the rig. A rig bone
    // nothing drives takes its parent's, so it keeps its T-pose rotation
    // relative to it. In world space, so a source with a bone the rig has no
    // room for (Xsens has four spine segments, the rig three) loses nothing.
    FLoomaBasis Turn[LoomaRigBody::Num];
    for (int32 Index = 0; Index < LoomaRigBody::Num; ++Index)
    {
        const int32 From = Mapped(Index);
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
    for (int32 Index = 0; Index < LoomaRigBody::Num; ++Index)
    {
        const int32 Parent = BoneTable[Index].Parent;
        OutWire[Index] = (Parent >= 0 ? Turn[Parent].Transposed() * Turn[Index] : Turn[Index]).ToQuat();
    }

    // The hips: height by leg length, travel across the floor from the first
    // frame. Leg length from the bones' own offsets, which do not move: the drop
    // from pelvis to hip joint, thigh, shin, and the drop from ankle to toe.
    auto Offset = [&](const TCHAR* Target) {
        const int32 Bone = Mapped(LoomaRigBody::Row(Target));
        return Bone != INDEX_NONE ? Local[Bone].GetLocation() : FVector::ZeroVector;
    };
    const double SourceLeg =
        FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftUpLeg")), SourceBoneUp))
        + Offset(TEXT("LeftLeg")).Size() + Offset(TEXT("LeftFoot")).Size()
        + FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftToeBase")), SourceBoneUp));
    const int32 Pelvis = Mapped(0);
    if (SourceLeg <= KINDA_SMALL_NUMBER || Pelvis == INDEX_NONE)
    {
        return false;
    }
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
        const int32 Bone = Mapped(LoomaRigBody::Row(Target));
        return Bone != INDEX_NONE ? WorldToCharacter.Apply(WorldOf(Bone, 0).GetLocation()).Y - Less : TNumericLimits<double>::Max();
    };
    const double AnkleRest = FMath::Abs(FVector::DotProduct(Offset(TEXT("LeftToeBase")), SourceBoneUp));
    const double Support = FMath::Min(
        FMath::Min(Rests(TEXT("LeftToeBase"), 0.0), Rests(TEXT("RightToeBase"), 0.0)),
        FMath::Min(Rests(TEXT("LeftFoot"), AnkleRest), Rests(TEXT("RightFoot"), AnkleRest)));
    const double Floor = bFindFloor && Support < TNumericLimits<double>::Max() ? UpdateFloor(Support, DeltaSeconds) : 0.0;
    OutHips = (InCharacter - Origin - FVector(0.0, Floor, 0.0)) / SourceLeg;
    return true;
}
