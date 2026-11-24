#pragma once

#include "CoreMinimal.h"
#include "LoomaLiveRetarget.h"

struct FReferenceSkeleton;

/**
 * The target half of the live retarget: the wire's pose in, one mesh's own bone
 * rotations out (HAM-316). FLoomaLiveRetarget is the source half. This one reads
 * a reference skeleton and needs no mesh, so a test can call it;
 * ULoomaLivePoseAnimInstance, which has the mesh, explains the arithmetic.
 */
struct LOOMAXRVIEWERUE58_API FLoomaLiveTarget
{
    /**
     * Measure a rig from its reference pose. False, with the reason in `OutWhy`,
     * for a skeleton that cannot be posed from the wire.
     */
    bool Build(const FReferenceSkeleton& Skeleton, FString& OutWhy);

    /**
     * One of the wire's rotations, a normalised turn from the T-pose in the
     * character's left, up, forward, as the local rotation of this mesh's bone
     * for that row of the bone table.
     */
    FQuat Rotation(int32 Row, const FQuat& Wire) const;

    /** The wire's hips, left, up, forward in hips heights, as the Hips bone's local translation. */
    FVector HipsLocation(const FVector& WireHips) const;

    int32 BoneCount = 0;
    int32 HipsBone = INDEX_NONE;
    /** Mesh bone index for each row of the bone table, or INDEX_NONE. */
    TArray<int32> Bones;
    /** The hips above the floor in the reference pose, component units. */
    double HipsHeight = 0.0;
    /** The character's left, up and forward in component space. */
    FVector Left = FVector::ZeroVector;
    FVector Up = FVector::ZeroVector;
    FVector Forward = FVector::ZeroVector;
    /** The hips' rotation in the reference pose, in component space. */
    FQuat HipsRestRotation = FQuat::Identity;

private:
    FLoomaBasis CharacterInComponent; // columns: left, up, forward
    FLoomaBasis CharacterInBone;      // A_t
    FLoomaBasis BoneToCharacter;      // A_t, transposed
    FLoomaBasis HipsRest;             // the hips' T-pose rotation in component space
    FLoomaBasis HipsParentInverse;
};
