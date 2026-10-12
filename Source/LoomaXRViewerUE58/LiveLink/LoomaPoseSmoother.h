#pragma once

#include "CoreMinimal.h"

struct FLoomaSmoothing;

/**
 * Steadies a wire pose from a source that shakes (HAM-324).
 *
 * A One Euro filter on each bone's rotation and on the hips: a low-pass whose
 * cutoff rises with how fast the thing is moving. A performer standing still
 * is smoothed hard, which is where shaking shows; a fast movement opens the
 * filter, so it does not lag behind. The wire pose is what is filtered, so the
 * steadied pose is the one every other client is sent.
 *
 * Live Link is read every rendered frame and gives the source's latest frame
 * each time, so the same frame comes round more than once. The filter moves
 * only when the rotations change, over the time since they last did.
 */
struct LOOMAXRVIEWERUE58_API FLoomaPoseSmoother
{
    /** The next frame is taken as it comes. */
    void Reset();

    /** Steady `Wire`, and `Hips` if given, in place. `DeltaSeconds` is the time since the last call. */
    void Apply(TArray<FQuat>& Wire, FVector* Hips, double DeltaSeconds, const FLoomaSmoothing& Settings);

private:
    TArray<FQuat> Raw;
    TArray<FQuat> Smooth;
    /** Each bone's turning speed, itself smoothed, radians a second. */
    TArray<double> Speed;
    FVector RawHips = FVector::ZeroVector;
    FVector SmoothHips = FVector::ZeroVector;
    double HipsSpeed = 0.0;
    bool bHasHips = false;
    /** Time since the rotations last changed. */
    double Waited = 0.0;
};
