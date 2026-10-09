#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "LoomaLivePoseAnimInstance.generated.h"

/** A 3x3 in the column-vector convention, kept apart from FMatrix's row vectors. */
struct FLoomaBasis
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

/** Hands the game thread's pose to the worker thread that evaluates it. */
USTRUCT()
struct FLoomaLivePoseProxy : public FAnimInstanceProxy
{
    GENERATED_BODY()

    FLoomaLivePoseProxy() = default;
    explicit FLoomaLivePoseProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

    virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
    virtual bool Evaluate(FPoseContext& Output) override;

    /** Indexed by the mesh's bone index. */
    TArray<FQuat> Rotations;
    TArray<bool> HasRotation;
    FVector HipsLocation = FVector::ZeroVector;
    int32 HipsBone = INDEX_NONE;
    bool bHasPose = false;
};

/**
 * Poses a LoomaMixamo-A/1 character from a Live Link subject (HAM-316's local proof).
 *
 * No IK Retargeter and no assets: the rig format makes it arithmetic. Every
 * generated rig has the same 52 bones, and in its T-pose every bone's local
 * rotation is identity (`docs/rig-format.md` in looma-xr-asset-demo). The
 * Moverse stream is an SMPL skeleton whose bones are identity in its T-pose
 * too. So a source bone's rotation relative to its parent IS the target
 * bone's, once it is written in the target's axes:
 *
 *     target_local = A_t * (A_s^-1 * source_local * A_s) * A_t^-1
 *
 * `A_s` is the character's (left, up, forward) in a source bone's own
 * coordinates and `A_t` the same in a target bone's. Both are measured from
 * the skeletons, left hip to right hip, hips to head, heel to toe, so neither
 * side's handedness or up-axis is written down here. The pelvis is the one
 * bone whose parent is the world, and the source's world is `Source*Axis`.
 *
 * Rotations only, as the rig format intends: bone lengths stay the generated
 * character's own. The hips' height is scaled by leg length; their travel
 * across the floor is scaled the same and measured from the first frame.
 *
 * Checked outside Unreal first, by posing a generated rig with the
 * simulator's recorded frames in Python: feet on the floor, walking, turning
 * and bending as the recording does.
 */
UCLASS(Transient, NotBlueprintable)
class LOOMAXRVIEWERUE58_API ULoomaLivePoseAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    /** The Live Link subject to follow, e.g. "Actor0". */
    UPROPERTY(EditAnywhere, Category = "Looma Live Link")
    FName SubjectName = TEXT("Actor0");

    /** The source's world: which way a performer's left, up and forward point. Moverse's. */
    FVector SourceLeftAxis = FVector(1, 0, 0);
    FVector SourceUpAxis = FVector(0, 0, 1);
    FVector SourceForwardAxis = FVector(0, 1, 0);

    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

protected:
    virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

private:
    friend struct FLoomaLivePoseProxy;

    bool BuildTarget();
    bool BuildSource(const TArray<FName>& BoneNames);

    // --- The target rig, measured once from its reference pose ---------------
    bool bTargetReady = false;
    int32 BoneCount = 0;
    int32 HipsBone = INDEX_NONE;
    /** Mesh bone index for each row of the bone table, or INDEX_NONE. */
    TArray<int32> TargetBones;
    FLoomaBasis CharacterInComponent; // columns: left, up, forward
    FLoomaBasis CharacterInBone;      // A_t
    FLoomaBasis HipsRest;             // the hips' T-pose rotation in component space
    FLoomaBasis HipsParentInverse;
    double HipsHeight = 0.0;          // component units

    // --- The source skeleton, mapped when its bone names first arrive ---------
    TArray<FName> SourceNames;
    TArray<int32> SourceBones;        // Live Link bone index for each row of the table
    bool bSourceReady = false;
    bool bHasOrigin = false;
    FVector Origin = FVector::ZeroVector;

    // --- This frame, for the proxy -----------------------------------------------
    TArray<FQuat> Rotations;
    TArray<bool> HasRotation;
    FVector HipsLocation = FVector::ZeroVector;
    bool bHasPose = false;
    bool bWarned = false;
};
