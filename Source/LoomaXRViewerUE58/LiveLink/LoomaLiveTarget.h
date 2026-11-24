#pragma once

#include "CoreMinimal.h"
#include "LoomaLiveRetarget.h"

struct FReferenceSkeleton;

/**
 * The target half of the live retarget: the wire's pose in, one mesh's own bone
 * rotations out (HAM-316). FLoomaLiveRetarget is the source half. This one reads
 * a reference skeleton and needs no mesh, so a test can call it;
 * ULoomaLivePoseAnimInstance, which has the mesh, explains the arithmetic.
 *
 * The wire is in the rig FILE's axes, X to the character's left, Y up, Z
 * forward, and the web sets what it is sent on its bones as it comes. For this
 * mesh to stand where the web's does, those three axes have to be found in its
 * component space exactly. They are read from what the format fixes for every
 * rig, the origin under the hips and the axis the arms rest turned about, not
 * from the body's shape (hip to hip, hips to head, heel to toe): a generated
 * body's head rests up to seven degrees off the vertical through its hips, and
 * a character posed in axes that lean by that much rises or sinks by a tenth
 * of the distance its performer walks, and tips over when they turn.
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
    /**
     * The character's left, up and forward in component space: the rig file's X,
     * Y and Z, read from what the rig format fixes for every rig, whatever the
     * body's proportions and posture.
     */
    FVector Left = FVector::ZeroVector;
    FVector Up = FVector::ZeroVector;
    FVector Forward = FVector::ZeroVector;
    /** Up runs from the mesh's origin to the hips, as the format has it. False: taken from the body's shape instead. */
    bool bUpFromOrigin = false;
    /** Forward is the axis the upper arms rest turned about, as the format has it. False: taken from the toes instead. */
    bool bForwardFromArms = false;
    /** Degrees between up and the line from the hips to the head: how far this body's shape leans from its file's axes. */
    double Lean = 0.0;
    /** The hips' rotation in the reference pose, in component space. */
    FQuat HipsRestRotation = FQuat::Identity;

private:
    FLoomaBasis CharacterInComponent; // columns: left, up, forward
    FLoomaBasis CharacterInBone;      // A_t
    FLoomaBasis BoneToCharacter;      // A_t, transposed
    FLoomaBasis HipsRest;             // the hips' T-pose rotation in component space
    FLoomaBasis HipsParentInverse;
};
