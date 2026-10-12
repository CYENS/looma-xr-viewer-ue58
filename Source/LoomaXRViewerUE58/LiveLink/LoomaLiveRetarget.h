#pragma once

#include "CoreMinimal.h"

struct FLoomaLiveLinkProfile;

/** A 3x3 in the column-vector convention, kept apart from FMatrix's row vectors. */
struct LOOMAXRVIEWERUE58_API FLoomaBasis
{
    double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static FLoomaBasis FromColumns(const FVector& X, const FVector& Y, const FVector& Z);
    static FLoomaBasis FromQuat(const FQuat& Q);
    FLoomaBasis operator*(const FLoomaBasis& Other) const;
    FLoomaBasis Transposed() const;
    FVector Apply(const FVector& V) const;
    /** Only meaningful for a rotation. */
    FQuat ToQuat() const;
};

/**
 * The rig format's BODY bones, the 52 with the fingers left out, in the order the
 * wire's `pose` carries them (`docs/rig-format.md` in looma-xr-asset-demo).
 * Parents come before children.
 */
namespace LoomaRigBody
{
    constexpr int32 Num = 22;
    /** The bone's name after `mixamorig:`, e.g. "LeftArm". */
    LOOMAXRVIEWERUE58_API const TCHAR* Name(int32 Row);
    /** The row of the bone's parent, -1 for the hips. */
    LOOMAXRVIEWERUE58_API int32 Parent(int32 Row);
    LOOMAXRVIEWERUE58_API int32 Row(const TCHAR* Name);
}

/**
 * The source half of the live retarget: a Live Link frame in, the wire's pose
 * out (HAM-316). It needs no mesh and no Live Link client, so a test can call
 * it; ULoomaLivePoseAnimInstance, which has both, explains the arithmetic.
 */
struct LOOMAXRVIEWERUE58_API FLoomaLiveRetarget
{
    /** Read a subject's bones with a profile. Returns how many of the rig's body bones something drives. */
    int32 Build(const FLoomaLiveLinkProfile& Profile, const TArray<FName>& BoneNames);

    /** Forget where the performer started and where their floor is. */
    void Restart();

    /**
     * One frame, parent-relative as Live Link gives it. `OutWire` gets one
     * rotation per body bone. Returns whether `OutHips` was set: left, up,
     * forward in leg lengths, the up measured from the performer's floor if
     * `bFindFloor`, else from the source's own zero.
     */
    bool Evaluate(const TArray<FTransform>& Local, const TArray<int32>& Parents, float DeltaSeconds, bool bFindFloor,
                  TArray<FQuat>& OutWire, FVector& OutHips);

private:
    /**
     * The height of the floor under the performer, in the source's units along
     * the character's up, from `Support`: the lowest point a foot rests on this
     * frame. A low percentile of the last few seconds, so a step, a jump and the
     * dip of a toe pushing off do not move it, and eased so a sample leaving the
     * window is not a jolt.
     */
    double UpdateFloor(double Support, float DeltaSeconds);

    /** Live Link bone index for each body bone, or INDEX_NONE. */
    TArray<int32> SourceBones;
    FLoomaBasis CharacterInSource;    // A_s, from the profile
    FLoomaBasis WorldToCharacter;     // C_w^-1, from the profile
    FVector SourceBoneUp = FVector(0, 0, 1);
    /** The profile's rootRotation, inverted: put in front of a root bone's rotation to take it off. */
    FQuat RootUndo = FQuat::Identity;
    /** Scratch for a frame: each source bone in the source's world. */
    TArray<FTransform> SourceWorld;
    TArray<bool> SourceDone;
    bool bHasOrigin = false;
    FVector Origin = FVector::ZeroVector;

    TArray<double> FloorSamples;
    int32 FloorNext = 0;
    float SinceFloorSample = 0.0f;
    double FloorLevel = 0.0;
    bool bHasFloor = false;
};
