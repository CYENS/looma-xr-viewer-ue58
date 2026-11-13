#pragma once

// VR mode's maths (VR-9), kept free of the world, the headset and input so the
// automation tests can pin it down without either. Unreal's frame throughout:
// centimetres, Z up, X forward, yaw in degrees and positive clockwise seen from
// above, which is a turn to the right.
//
// It is the web client's VR maths (frontend/src/xr/runtime/ in
// looma-xr-asset-demo) in Unreal units, so a person moves the same way in either
// client: the pawn is the room's floor point, and every kind of movement moves
// the pawn, never the camera, which belongs to the headset.

#include "CoreMinimal.h"

namespace LoomaVR
{
    // Touch thumbsticks rest a little off zero and drift with wear.
    constexpr float DeadZone = 0.15f;
    constexpr float WalkSpeed = 180.f;      // cm/s at full deflection
    constexpr float SnapArm = 0.7f;         // stick past this turns once
    constexpr float SnapRelease = 0.3f;     // and must come back inside this first
    constexpr float AimStart = 0.7f;        // right stick forward past this aims a teleport
    constexpr float AimRelease = 0.3f;      // and coming back inside this jumps
    constexpr float MinGrabDistance = 25.f; // nearest a ray can pull what it holds
    constexpr float PushSpeed = 300.f;      // cm/s along the ray at full stick
    constexpr float TwistSpeed = 90.f;      // degrees/s about the vertical at full stick

    // A stick axis with the dead zone taken out: 0 inside it, 1 at full deflection.
    inline float Axis(float Value)
    {
        const float Magnitude = FMath::Abs(Value);
        if (Magnitude < DeadZone)
        {
            return 0.f;
        }
        return FMath::Sign(Value) * FMath::Min(1.f, (Magnitude - DeadZone) / (1.f - DeadZone));
    }

    // Velocity for a stick deflection (X right, Y forward). Walking stays level:
    // forward is where the head faces, flattened, so looking down while walking
    // does not slow you. Flying follows the view, pitch included.
    inline FVector MoveVelocity(const FVector2D& Stick, const FRotator& Head, bool bFly, float Speed)
    {
        const float X = Axis(Stick.X);
        const float Y = Axis(Stick.Y);
        if (X == 0.f && Y == 0.f)
        {
            return FVector::ZeroVector;
        }
        const FRotator Frame = bFly ? Head : FRotator(0.f, Head.Yaw, 0.f);
        const FRotationMatrix M(Frame);
        FVector V = M.GetUnitAxis(EAxis::X) * Y + M.GetUnitAxis(EAxis::Y) * X;
        if (V.SizeSquared() > 1.f)
        {
            V.Normalize();
        }
        return V * Speed;
    }

    // The snap-turn latch: one step per flick. Returns the yaw to turn this frame
    // (0 almost always) and updates bArmed.
    inline float SnapTurnStep(float X, bool& bArmed, float Degrees)
    {
        if (bArmed && FMath::Abs(X) >= SnapArm)
        {
            bArmed = false;
            return X > 0.f ? Degrees : -Degrees;
        }
        if (!bArmed && FMath::Abs(X) <= SnapRelease)
        {
            bArmed = true;
        }
        return 0.f;
    }

    // Turn the pawn about the vertical line through the head, so the person spins
    // on the spot instead of swinging around the middle of their room.
    inline void TurnAboutHead(FVector& PawnLocation, float& PawnYaw, const FVector& Head, float DeltaYaw)
    {
        FVector Offset = PawnLocation - Head;
        const float Z = PawnLocation.Z;
        Offset.Z = 0.f;
        Offset = Offset.RotateAngleAxis(DeltaYaw, FVector::UpVector);
        PawnLocation = FVector(Head.X + Offset.X, Head.Y + Offset.Y, Z);
        PawnYaw += DeltaYaw;
    }

    // Move the pawn so the head lands straight above Target, feet on it.
    inline FVector TeleportedPawn(const FVector& PawnLocation, const FVector& Head, const FVector& Target)
    {
        return FVector(PawnLocation.X + Target.X - Head.X, PawnLocation.Y + Target.Y - Head.Y, Target.Z);
    }

    // Where to put the pawn so the head, which stands HeadRelative from the pawn
    // in its own frame, ends up over ViewLocation facing ViewYaw, feet at FloorZ.
    // The camera's height is not carried over: the desktop view usually looks
    // down from several metres up, and a session that starts floating there is
    // the fastest way to make somebody grab for a table.
    inline void PlaceForView(const FVector& ViewLocation, float ViewYaw, float FloorZ,
        const FVector& HeadRelative, float HeadRelativeYaw, FVector& OutLocation, float& OutYaw)
    {
        OutYaw = ViewYaw - HeadRelativeYaw;
        const FVector Offset = FVector(HeadRelative.X, HeadRelative.Y, 0.f).RotateAngleAxis(OutYaw, FVector::UpVector);
        OutLocation = FVector(ViewLocation.X - Offset.X, ViewLocation.Y - Offset.Y, FloorZ);
    }

    // One-handed grab: the selection hangs off the point where the ray met it, at
    // that distance. Pointing moves it, the stick pushes, pulls and turns it, and
    // turning the controller turns it about that point; only about the vertical
    // unless Upright is off, so a chair keeps its feet down.
    struct FRayGrab
    {
        struct FItem
        {
            FVector Offset;
            FQuat Rotation;
        };

        float Distance = 0.f;
        float Twist = 0.f;
        FQuat StartRotation = FQuat::Identity;
        TArray<FItem> Items;

        void Begin(const FVector& RayOrigin, const FQuat& RayRotation, float HitDistance, const TArray<FTransform>& Objects)
        {
            Distance = FMath::Max(MinGrabDistance, HitDistance);
            Twist = 0.f;
            StartRotation = RayRotation;
            const FVector Anchor = RayOrigin + RayRotation.GetForwardVector() * HitDistance;
            Items.Reset();
            for (const FTransform& Object : Objects)
            {
                Items.Add({Object.GetLocation() - Anchor, Object.GetRotation()});
            }
        }

        // Forward on the stick (positive Y) pushes away; right (positive X) turns
        // clockwise seen from above.
        void Steer(float Push, float TwistInput, float DeltaSeconds)
        {
            Distance = FMath::Max(MinGrabDistance, Distance + Push * PushSpeed * DeltaSeconds);
            Twist += TwistInput * TwistSpeed * DeltaSeconds;
        }

        void Poses(const FVector& RayOrigin, const FQuat& RayRotation, bool bUpright,
            TArray<FVector>& OutLocations, TArray<FQuat>& OutRotations) const
        {
            const FVector Anchor = RayOrigin + RayRotation.GetForwardVector() * Distance;
            FQuat Turn;
            if (bUpright)
            {
                const float DeltaYaw = RayRotation.Rotator().Yaw - StartRotation.Rotator().Yaw;
                Turn = FQuat(FVector::UpVector, FMath::DegreesToRadians(DeltaYaw + Twist));
            }
            else
            {
                Turn = FQuat(FVector::UpVector, FMath::DegreesToRadians(Twist)) * RayRotation * StartRotation.Inverse();
            }
            OutLocations.Reset();
            OutRotations.Reset();
            for (const FItem& Item : Items)
            {
                OutLocations.Add(Anchor + Turn.RotateVector(Item.Offset));
                OutRotations.Add(Turn * Item.Rotation);
            }
        }
    };
}
