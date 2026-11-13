#include "../LoomaVRMath.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

// VR mode's maths (VR-9), without a headset: what a person feels is where their
// head ends up, so every case checks the head.

namespace
{
    // The head's world position for a pawn at Location turned by Yaw, with the
    // head standing HeadRelative from it in the pawn's own frame.
    FVector HeadWorld(const FVector& Location, float Yaw, const FVector& HeadRelative)
    {
        return Location + HeadRelative.RotateAngleAxis(Yaw, FVector::UpVector);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaVRSticksTest, "Looma.VR.Sticks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaVRSticksTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("a resting stick is zero"), LoomaVR::Axis(0.1f), 0.f);
    TestEqual(TEXT("full deflection is one"), LoomaVR::Axis(-1.f), -1.f);

    // Facing +90 yaw (the world's +Y), looking well down.
    const FRotator Head(-60.f, 90.f, 0.f);
    const FVector Walk = LoomaVR::MoveVelocity(FVector2D(0.f, 1.f), Head, false, 100.f);
    TestTrue(TEXT("walking stays level, whatever the pitch"), FMath::IsNearlyZero(Walk.Z, 1e-3f));
    TestTrue(TEXT("and goes where the head faces"), Walk.Equals(FVector(0.f, 100.f, 0.f), 1e-2f));
    const FVector Fly = LoomaVR::MoveVelocity(FVector2D(0.f, 1.f), Head, true, 100.f);
    TestTrue(TEXT("flying follows the view down"), Fly.Z < -80.f);
    const FVector Diagonal = LoomaVR::MoveVelocity(FVector2D(1.f, 1.f), FRotator::ZeroRotator, false, 100.f);
    TestTrue(TEXT("no faster on a diagonal"), FMath::IsNearlyEqual(Diagonal.Size(), 100.f, 1e-2f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaVRSnapTurnTest, "Looma.VR.SnapTurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaVRSnapTurnTest::RunTest(const FString& Parameters)
{
    bool bArmed = true;
    TArray<float> Turns;
    for (const float X : {0.f, 0.5f, 0.9f, 1.f, 0.9f, 0.5f, 0.2f, 0.95f})
    {
        const float Turn = LoomaVR::SnapTurnStep(X, bArmed, 45.f);
        if (Turn != 0.f)
        {
            Turns.Add(Turn);
        }
    }
    TestEqual(TEXT("one turn per flick, right is positive yaw"), Turns, TArray<float>({45.f, 45.f}));

    // Spinning on the spot: the head stays put.
    FVector Location(300.f, -200.f, 0.f);
    float Yaw = 20.f;
    const FVector HeadRelative(60.f, -30.f, 170.f);
    const FVector Before = HeadWorld(Location, Yaw, HeadRelative);
    LoomaVR::TurnAboutHead(Location, Yaw, Before, 45.f);
    const FVector After = HeadWorld(Location, Yaw, HeadRelative);
    TestTrue(TEXT("the head does not move when turning"), After.Equals(Before, 1e-2f));
    TestEqual(TEXT("and the pawn turned"), Yaw, 65.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaVRHandoverTest, "Looma.VR.Handover",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaVRHandoverTest::RunTest(const FString& Parameters)
{
    // Standing 1.5 m off the middle of the room, turned 120 degrees.
    const FVector HeadRelative(150.f, -80.f, 165.f);
    const float HeadRelativeYaw = 120.f;
    const FVector View(700.f, 1100.f, 500.f);
    const float ViewYaw = -32.f;
    FVector Location;
    float Yaw = 0.f;
    LoomaVR::PlaceForView(View, ViewYaw, 10.f, HeadRelative, HeadRelativeYaw, Location, Yaw);
    const FVector Head = HeadWorld(Location, Yaw, HeadRelative);
    TestTrue(TEXT("the head lands under the desktop view"), FMath::IsNearlyEqual(Head.X, 700.f, 1e-2f) && FMath::IsNearlyEqual(Head.Y, 1100.f, 1e-2f));
    TestEqual(TEXT("feet on the floor below it"), Location.Z, 10.0);
    TestTrue(TEXT("facing the way the view faced"), FMath::IsNearlyEqual(FRotator::NormalizeAxis(Yaw + HeadRelativeYaw), ViewYaw, 1e-3f));

    const FVector Target(-400.f, 250.f, 30.f);
    const FVector Moved = LoomaVR::TeleportedPawn(Location, Head, Target);
    const FVector NewHead = HeadWorld(Moved, Yaw, HeadRelative);
    TestTrue(TEXT("a teleport puts the head over the ring"), FMath::IsNearlyEqual(NewHead.X, -400.f, 1e-2f) && FMath::IsNearlyEqual(NewHead.Y, 250.f, 1e-2f));
    TestEqual(TEXT("feet on it"), Moved.Z, 30.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaVRGrabTest, "Looma.VR.Grab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaVRGrabTest::RunTest(const FString& Parameters)
{
    const FVector Origin(0.f, 0.f, 150.f);
    LoomaVR::FRayGrab Grab;
    Grab.Begin(Origin, FQuat::Identity, 300.f, {FTransform(FVector(300.f, 0.f, 150.f))});

    TArray<FVector> Locations;
    TArray<FQuat> Rotations;
    Grab.Poses(Origin + FVector(0.f, 100.f, 50.f), FQuat::Identity, true, Locations, Rotations);
    TestTrue(TEXT("carried with the end of the ray"), Locations[0].Equals(FVector(300.f, 100.f, 200.f), 1e-2f));

    // A tilted, rolled wrist turned 30 degrees: upright keeps only the 30.
    const FQuat Wrist = FRotator(-25.f, 30.f, 40.f).Quaternion();
    Grab.Poses(Origin, Wrist, true, Locations, Rotations);
    const FRotator Kept = Rotations[0].Rotator();
    TestTrue(TEXT("upright: no pitch"), FMath::IsNearlyZero(Kept.Pitch, 1e-2f));
    TestTrue(TEXT("upright: no roll"), FMath::IsNearlyZero(Kept.Roll, 1e-2f));
    TestTrue(TEXT("upright: the yaw"), FMath::IsNearlyEqual(Kept.Yaw, 30.f, 1e-2f));

    Grab.Steer(1.f, 0.f, 0.5f);
    TestTrue(TEXT("the stick pushes it away"), FMath::IsNearlyEqual(Grab.Distance, 300.f + LoomaVR::PushSpeed * 0.5f, 1e-2f));
    Grab.Steer(-1.f, 0.f, 10.f);
    TestEqual(TEXT("and pulls it no nearer than the minimum"), Grab.Distance, LoomaVR::MinGrabDistance);
    return true;
}

#endif
