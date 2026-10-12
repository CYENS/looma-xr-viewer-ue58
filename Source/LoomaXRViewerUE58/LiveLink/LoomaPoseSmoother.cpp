#include "LoomaPoseSmoother.h"

#include "LoomaLiveLinkProfiles.h"

namespace
{
    // The cutoff the speed estimate is itself smoothed with, as the One Euro
    // paper has it. Low, so one noisy frame does not open the filter.
    const double SpeedCutoff = 1.0;
    // The hips are a position, in leg lengths, where the bones are angles. One
    // leg length a second, a brisk walk, opens the filter as far as a bone
    // turning this many radians a second.
    const double HipsRadiansPerLeg = 4.0;
    // A gap this long is a new start, not a movement to smooth across.
    const double LongestGap = 0.5;

    double Blend(double Cutoff, double DeltaSeconds)
    {
        const double Tau = 1.0 / (2.0 * UE_DOUBLE_PI * Cutoff);
        return 1.0 / (1.0 + Tau / DeltaSeconds);
    }
}

void FLoomaPoseSmoother::Reset()
{
    Raw.Reset();
    Smooth.Reset();
    Speed.Reset();
    bHasHips = false;
    Waited = 0.0;
}

void FLoomaPoseSmoother::Apply(TArray<FQuat>& Wire, FVector* Hips, double DeltaSeconds, const FLoomaSmoothing& Settings)
{
    if (!Settings.IsOn())
    {
        Reset();
        return;
    }
    Waited += FMath::Max(0.0, DeltaSeconds);
    const bool bStart = Raw.Num() != Wire.Num() || Waited > LongestGap;
    if (bStart)
    {
        Raw = Wire;
        Smooth = Wire;
        Speed.Init(0.0, Wire.Num());
        bHasHips = false;
        Waited = 0.0;
    }
    bool bNewFrame = false;
    for (int32 Index = 0; Index < Wire.Num() && !bNewFrame; ++Index)
    {
        bNewFrame = !Wire[Index].Equals(Raw[Index], 0.0);
    }
    if (bNewFrame && Waited > 0.0)
    {
        const double SpeedBlend = Blend(SpeedCutoff, Waited);
        for (int32 Index = 0; Index < Wire.Num(); ++Index)
        {
            const FQuat Next = Wire[Index].GetNormalized();
            Speed[Index] += (Raw[Index].AngularDistance(Next) / Waited - Speed[Index]) * SpeedBlend;
            Smooth[Index] = FQuat::Slerp(Smooth[Index], Next, Blend(Settings.MinCutoff + Settings.Beta * Speed[Index], Waited));
            Raw[Index] = Wire[Index];
        }
        if (Hips && bHasHips)
        {
            HipsSpeed += (FVector::Dist(RawHips, *Hips) / Waited - HipsSpeed) * SpeedBlend;
            SmoothHips = FMath::Lerp(SmoothHips, *Hips,
                                     Blend(Settings.MinCutoff + Settings.Beta * HipsSpeed * HipsRadiansPerLeg, Waited));
            RawHips = *Hips;
        }
        Waited = 0.0;
    }
    if (Hips && !bHasHips)
    {
        bHasHips = true;
        RawHips = SmoothHips = *Hips;
        HipsSpeed = 0.0;
    }
    Wire = Smooth;
    if (Hips)
    {
        *Hips = SmoothHips;
    }
}
