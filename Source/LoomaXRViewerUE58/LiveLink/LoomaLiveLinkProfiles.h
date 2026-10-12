#pragma once

#include "CoreMinimal.h"

/**
 * One mocap skeleton the viewer can put on a generated character, read from
 * `Config/LiveLinkProfiles.json` (HAM-316). The file says what each field
 * means; this is that file in memory.
 */
struct FLoomaLiveLinkProfile
{
    FString Name;
    /** Rig bone (bare, e.g. "LeftArm") -> the Live Link bone that drives it. */
    TMap<FString, FName> Bones;
    /** The performer's left, up, forward in a source bone's own coordinates, in the T-pose. */
    FVector BoneLeft = FVector(1, 0, 0);
    FVector BoneUp = FVector(0, 0, 1);
    FVector BoneForward = FVector(0, 1, 0);
    /** The same in the source's world. */
    FVector WorldLeft = FVector(1, 0, 0);
    FVector WorldUp = FVector(0, 0, 1);
    FVector WorldForward = FVector(0, 1, 0);
    /** Part of a Live Link source's type; empty for any source. */
    FString Source;
    /** What the source put in front of its root bone's rotation, to be taken off again. */
    FQuat RootRotation = FQuat::Identity;
};

namespace LoomaLiveLinkProfiles
{
    /** Every profile in the file, loaded once. Empty, with a logged reason, if the file is missing or malformed. */
    const TArray<FLoomaLiveLinkProfile>& All();

    /**
     * The profile for a subject with these bones from a source of this type, or
     * null: among the profiles all of whose required rig bones it has, the one
     * that names its source, else the first that names none.
     */
    const FLoomaLiveLinkProfile* For(const TArray<FName>& SubjectBones, const FString& SourceType);
}
