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
 * Poses a LoomaMixamo-A/1 character from a Live Link subject (HAM-316).
 *
 * No IK Retargeter and no assets: the rig format makes it arithmetic. Every
 * generated rig has the same 52 bones, and in its T-pose every bone's local
 * rotation is identity (`docs/rig-format.md` in looma-xr-asset-demo). A mocap
 * skeleton whose bones are identity in ITS T-pose, relative to their parents,
 * is then the same pose in other axes. For each bone, in world space:
 *
 *     turn = C_w^-1 * W_source * A_s
 *
 * `W_source` is the source bone's world rotation, `A_s` the performer's (left,
 * up, forward) in a source bone's own coordinates and `C_w` the same in the
 * source's world. `turn` is how far the bone has turned from its T-pose, in the
 * performer's own left, up, forward: the identity in the T-pose, whatever the
 * vendor. The rig bone takes the same turn, written in the rig's axes, which
 * are measured from its reference pose (hip to hip, hips to head, heel to toe).
 *
 * Which Live Link bone drives which rig bone, and the two sets of axes, are a
 * PROFILE: data, in `Config/LiveLinkProfiles.json`. Adding a vendor is adding a
 * profile. Moverse (SMPL) and Xsens are there.
 *
 * Rotations only, as the rig format intends: bone lengths stay the generated
 * character's own. The hips' height is scaled by leg length; their travel
 * across the floor is scaled the same and measured from the first frame.
 *
 * The hips' height is measured from the floor the performer's FEET rest on, not
 * from the mocap system's zero, which is often off: the Moverse simulator's
 * three performers stand 1, 3 and 6 cm into their floor, and a character given
 * those heights sinks by as much. The floor is found from the feet themselves
 * (UpdateFloor). Looma.LiveLink.FindFloor 0 turns that off, to compare.
 *
 * The Moverse maths was checked outside Unreal first, by posing a generated rig
 * with the simulator's recorded frames in Python.
 *
 * It also plays back a pose ANOTHER client published (FollowScene): the same
 * wire frame this class sends, read from the scene sync and written into this
 * mesh's bones by the same last step. So a character driven on one machine
 * moves on every other, with no Live Link there.
 */
UCLASS(Transient, NotBlueprintable)
class LOOMAXRVIEWERUE58_API ULoomaLivePoseAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    /** The Live Link subject to follow, e.g. "Actor0". */
    UPROPERTY(EditAnywhere, Category = "Looma Live Link")
    FName SubjectName = TEXT("Actor0");

    /**
     * Send the pose on to the other clients, through the scene socket
     * (ULoomaSceneSyncSubsystem::PublishPose). Only for a character the scene
     * sync spawned, since the wire names a scene node.
     */
    bool bPublish = true;
    /** Frames a second sent to the other clients. */
    float PublishRate = 30.0f;
    /** Where in its interval this character's next frame falls, 0 to 1. */
    void SetPublishPhase(float Phase) { SincePublish = PublishRate > 0.0f ? Phase / PublishRate : 0.0f; }

    /**
     * Follow the pose other clients publish for this character's scene node
     * (ULoomaSceneSyncSubsystem::GetLivePose) rather than a Live Link subject.
     * Nothing is published back.
     */
    void FollowScene(bool bFollow);
    bool IsFollowingScene() const { return bFromScene; }

    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

protected:
    virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

private:
    friend struct FLoomaLivePoseProxy;

    bool BuildTarget();
    bool BuildSource(const TArray<FName>& BoneNames);
    /** The type of the Live Link source the subject comes from, as the Live Link panel shows it. */
    FString SubjectSourceType() const;
    void UpdateFromScene(float DeltaSeconds);
    /**
     * Pose the mesh from a frame in the wire's terms. `Alpha` is how far to move
     * towards it, 1 for all the way. `Hips` null leaves the hips where they are.
     */
    void SetFromWire(const FVector* Hips, const TArray<FQuat>& Wire, float Alpha);

    bool bFromScene = false;
    /** Following the scene: a first frame has been taken whole. */
    bool bSceneStarted = false;

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
    double HipsHeight = 0.0;          // the hips above the floor in the reference pose, component units

    // --- The source skeleton, mapped when its bone names first arrive ---------
    TArray<FName> SourceNames;
    TArray<int32> SourceBones;        // Live Link bone index for each row of the table
    bool bSourceReady = false;
    FLoomaBasis CharacterInSource;    // A_s, from the profile
    FLoomaBasis WorldToCharacter;     // C_w^-1, from the profile
    FVector SourceBoneUp = FVector(0, 0, 1);
    /** The profile's rootRotation, inverted: put in front of a root bone's rotation to take it off. */
    FQuat RootUndo = FQuat::Identity;
    /** The source the profile was chosen for. The same subject name can move to another source. */
    FString SourceType;
    float SinceSourceCheck = 0.0f;
    /** Scratch for a frame: each source bone in the source's world. */
    TArray<FTransform> SourceWorld;
    TArray<bool> SourceDone;
    bool bHasOrigin = false;
    FVector Origin = FVector::ZeroVector;

    /**
     * The height of the floor under the performer, in the source's units along
     * the character's up, from `Support`: the lowest point a foot rests on this
     * frame. A low percentile of the last few seconds, so a step, a jump and the
     * dip of a toe pushing off do not move it, and eased so a sample leaving the
     * window is not a jolt.
     */
    double UpdateFloor(double Support, float DeltaSeconds);
    TArray<double> FloorSamples;
    int32 FloorNext = 0;
    float SinceFloorSample = 0.0f;
    double FloorLevel = 0.0;
    bool bHasFloor = false;

    // --- This frame, for the proxy -----------------------------------------------
    TArray<FQuat> Rotations;
    TArray<bool> HasRotation;
    FVector HipsLocation = FVector::ZeroVector;
    bool bHasPose = false;
    bool bWarned = false;

    // --- The same frame in the wire's terms (docs/rig-format.md, "Live pose") ---
    /** One per row of the bone table, which is the wire's own order. */
    TArray<FQuat> WireRotations;
    /** Left, up, forward, in leg lengths. */
    FVector WireHips = FVector::ZeroVector;
    float SincePublish = 0.0f;
};
