#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "LoomaLiveLinkProfiles.h"
#include "LoomaLiveRetarget.h"
#include "LoomaPoseSmoother.h"
#include "LoomaLivePoseAnimInstance.generated.h"

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
 * (FLoomaLiveRetarget). Looma.LiveLink.FindFloor 0 turns that off, to compare.
 *
 * The Moverse maths was checked outside Unreal first, by posing a generated rig
 * with the simulator's recorded frames in Python. The source half, a Live Link
 * frame to a wire pose, is FLoomaLiveRetarget, apart from this class so that
 * Tests/LoomaLiveRetargetTests.cpp can run it with no mesh.
 *
 * A source that shakes, such as a camera-based capture, is steadied on the way
 * (FLoomaPoseSmoother), by as much as its profile says. The wire pose is what
 * is steadied, so every client gets the same one. Looma.LiveLink.Smooth 0
 * turns that off; Looma.LiveLink.SmoothMinCutoff and SmoothBeta try other
 * settings without editing the profile.
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
    bool bSourceReady = false;
    FLoomaLiveRetarget Retarget;
    /** What the source's profile asks for, and the filter that does it (HAM-324). */
    FLoomaSmoothing Smoothing;
    FLoomaPoseSmoother Smoother;
    /** The source the profile was chosen for. The same subject name can move to another source. */
    FString SourceType;
    float SinceSourceCheck = 0.0f;

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
