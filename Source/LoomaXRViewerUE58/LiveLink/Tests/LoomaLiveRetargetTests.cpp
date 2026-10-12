#include "../LoomaLiveLinkProfiles.h"
#include "../LoomaLiveRetarget.h"
#include "../LoomaPoseSmoother.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

// The retarget with no mesh and no Live Link client (HAM-323): a made-up
// performer for each profile in Config/LiveLinkProfiles.json, built from that
// profile's own bone names and axes, and posed. The check that carries the
// weight is WhereTheWirePutsThem: a body built from the wire's rotations has to
// have every joint where the source skeleton has it, and the source's positions
// come from FTransform, not from the retarget's own arithmetic.

namespace
{
    const double Degrees = UE_DOUBLE_PI / 180.0;

    /** Each body bone's offset from its parent, in the performer's left, up, forward. Legs add up to 95. */
    const FVector BodyOffsets[LoomaRigBody::Num] = {
        FVector(0, 95, 0),                                                              // Hips, from the floor
        FVector(0, 10, 0), FVector(0, 12, 0), FVector(0, 12, 0),                        // Spine, Spine1, Spine2
        FVector(0, 15, 0), FVector(0, 10, 0),                                           // Neck, Head
        FVector(5, 10, 0), FVector(12, 0, 0), FVector(28, 0, 0), FVector(26, 0, 0),     // left arm
        FVector(-5, 10, 0), FVector(-12, 0, 0), FVector(-28, 0, 0), FVector(-26, 0, 0), // right arm
        FVector(9, -8, 0), FVector(0, -42, 0), FVector(0, -40, 0), FVector(0, -5, 12),  // left leg
        FVector(-9, -8, 0), FVector(0, -42, 0), FVector(0, -40, 0), FVector(0, -5, 12), // right leg
    };
    const double LegLength = 95.0;

    FLoomaBasis About(const FVector& Axis, double Angle)
    {
        // Rodrigues, right-handed: the rotation the wire means by "about this axis".
        const FVector N = Axis.GetSafeNormal();
        const double C = FMath::Cos(Angle), S = FMath::Sin(Angle), T = 1.0 - C;
        FLoomaBasis Out;
        Out.M[0][0] = C + N.X * N.X * T;
        Out.M[0][1] = N.X * N.Y * T - N.Z * S;
        Out.M[0][2] = N.X * N.Z * T + N.Y * S;
        Out.M[1][0] = N.Y * N.X * T + N.Z * S;
        Out.M[1][1] = C + N.Y * N.Y * T;
        Out.M[1][2] = N.Y * N.Z * T - N.X * S;
        Out.M[2][0] = N.Z * N.X * T - N.Y * S;
        Out.M[2][1] = N.Z * N.Y * T + N.X * S;
        Out.M[2][2] = C + N.Z * N.Z * T;
        return Out;
    }

    double AngleBetween(const FQuat& A, const FQuat& B)
    {
        return A.GetNormalized().AngularDistance(B.GetNormalized());
    }

    const FLoomaLiveLinkProfile* Named(const TCHAR* Name)
    {
        return LoomaLiveLinkProfiles::All().FindByPredicate(
            [Name](const FLoomaLiveLinkProfile& Profile) { return Profile.Name == Name; });
    }

    /** A performer as a Live Link source would send them: bone names, parents and a parent-relative frame. */
    struct FPerformer
    {
        TArray<FName> Names;
        TArray<int32> Parents;
        TArray<FTransform> Local;
        /** Source bone index for each body bone. */
        int32 Bones[LoomaRigBody::Num];
        FLoomaBasis CharacterInSource; // the performer's left, up, forward in a bone's coordinates
        FLoomaBasis CharacterInWorld;  // the same in the source's world

        /** Standing in the T-pose with the toes `ToesAt` above the source's zero. */
        FPerformer(const FLoomaLiveLinkProfile& Profile, double ToesAt = 0.0)
        {
            CharacterInSource = FLoomaBasis::FromColumns(Profile.BoneLeft, Profile.BoneUp, Profile.BoneForward);
            CharacterInWorld = FLoomaBasis::FromColumns(Profile.WorldLeft, Profile.WorldUp, Profile.WorldForward);
            // In the T-pose every bone, in the world, is the one rotation that
            // takes a bone's own left, up, forward to the world's.
            const FQuat Rest = (CharacterInWorld * CharacterInSource.Transposed()).ToQuat();
            for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
            {
                Bones[Row] = Names.Add(Profile.Bones.FindChecked(LoomaRigBody::Name(Row)));
                const int32 Parent = LoomaRigBody::Parent(Row);
                Parents.Add(Parent >= 0 ? Bones[Parent] : INDEX_NONE);
                Local.Add(Parent >= 0
                    ? FTransform(FQuat::Identity, CharacterInSource.Apply(BodyOffsets[Row]))
                    : FTransform(Rest, CharacterInWorld.Apply(BodyOffsets[Row] + FVector(0, ToesAt, 0))));
            }
        }

        /** Turn a bone from where it is, about an axis given in the performer's left, up, forward. */
        void Bend(const TCHAR* Bone, const FVector& Axis, double Angle)
        {
            const int32 Row = LoomaRigBody::Row(Bone);
            const FLoomaBasis Turn = About(Axis, Angle);
            FTransform& Target = Local[Bones[Row]];
            if (LoomaRigBody::Parent(Row) >= 0)
            {
                Target.SetRotation(Target.GetRotation() * (CharacterInSource * Turn * CharacterInSource.Transposed()).ToQuat());
            }
            else
            {
                // The hips turn in the world, about the performer's own axes.
                Target.SetRotation((CharacterInWorld * Turn * CharacterInWorld.Transposed()).ToQuat() * Target.GetRotation());
            }
        }

        void MoveHips(const FVector& InCharacter)
        {
            Local[Bones[0]].AddToTranslation(CharacterInWorld.Apply(InCharacter));
        }

        /** Where a body bone is, by Unreal's own transforms, in the performer's left, up, forward. */
        FVector Where(int32 Row) const
        {
            FTransform World = Local[Bones[Row]];
            for (int32 Parent = Parents[Bones[Row]]; Parent != INDEX_NONE; Parent = Parents[Parent])
            {
                World = World * Local[Parent];
            }
            return CharacterInWorld.Transposed().Apply(World.GetLocation());
        }
    };

    /** A body built from a wire pose: where each bone ends up, relative to the hips, in left, up, forward. */
    TArray<FVector> WhereTheWirePutsThem(const TArray<FQuat>& Wire)
    {
        TArray<FVector> Out;
        FLoomaBasis Turn[LoomaRigBody::Num];
        for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
        {
            const int32 Parent = LoomaRigBody::Parent(Row);
            const FLoomaBasis Own = FLoomaBasis::FromQuat(Wire[Row]);
            Turn[Row] = Parent >= 0 ? Turn[Parent] * Own : Own;
            Out.Add(Parent >= 0 ? Out[Parent] + Turn[Parent].Apply(BodyOffsets[Row]) : FVector::ZeroVector);
        }
        return Out;
    }

    /** A pose with something bent in every limb and the whole body turned. */
    void Strike(FPerformer& Performer)
    {
        Performer.Bend(TEXT("Hips"), FVector(0, 1, 0), 40 * Degrees);
        Performer.Bend(TEXT("Hips"), FVector(1, 0, 0), 10 * Degrees);
        Performer.Bend(TEXT("Spine1"), FVector(0, 0, 1), 15 * Degrees);
        Performer.Bend(TEXT("Head"), FVector(0, 1, 0), -30 * Degrees);
        Performer.Bend(TEXT("LeftArm"), FVector(0, 0, 1), -70 * Degrees);
        Performer.Bend(TEXT("LeftForeArm"), FVector(0, 1, 0), 60 * Degrees);
        Performer.Bend(TEXT("RightArm"), FVector(0, 1, 0), 35 * Degrees);
        Performer.Bend(TEXT("RightHand"), FVector(1, 0, 0), 25 * Degrees);
        Performer.Bend(TEXT("LeftUpLeg"), FVector(1, 0, 0), -50 * Degrees);
        Performer.Bend(TEXT("LeftLeg"), FVector(1, 0, 0), 80 * Degrees);
        Performer.Bend(TEXT("RightFoot"), FVector(1, 0, 0), -20 * Degrees);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveLinkProfilesTest,
    "Looma.LiveLink.Retarget.Profiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveLinkProfilesTest::RunTest(const FString& Parameters)
{
    const FLoomaLiveLinkProfile* Moverse = Named(TEXT("moverse"));
    const FLoomaLiveLinkProfile* Plugin = Named(TEXT("moverse-plugin"));
    const FLoomaLiveLinkProfile* Xsens = Named(TEXT("xsens"));
    if (!TestNotNull(TEXT("the file has a moverse profile"), Moverse) || !TestNotNull(TEXT("a moverse-plugin profile"), Plugin)
        || !TestNotNull(TEXT("and an xsens profile"), Xsens))
    {
        return false;
    }
    for (const FLoomaLiveLinkProfile* Profile : {Moverse, Plugin, Xsens})
    {
        TestEqual(*FString::Printf(TEXT("%s drives every body bone"), *Profile->Name), Profile->Bones.Num(), LoomaRigBody::Num);
        const FLoomaBasis Bone = FLoomaBasis::FromColumns(Profile->BoneLeft, Profile->BoneUp, Profile->BoneForward);
        const FLoomaBasis World = FLoomaBasis::FromColumns(Profile->WorldLeft, Profile->WorldUp, Profile->WorldForward);
        for (const FLoomaBasis& Axes : {Bone, World})
        {
            // Left, up, forward at right angles and unit length: Axes^T * Axes is the identity.
            const FLoomaBasis Square = Axes.Transposed() * Axes;
            double Off = 0.0;
            for (int32 R = 0; R < 3; ++R)
            {
                for (int32 C = 0; C < 3; ++C)
                {
                    Off = FMath::Max(Off, FMath::Abs(Square.M[R][C] - (R == C ? 1.0 : 0.0)));
                }
            }
            TestTrue(*FString::Printf(TEXT("%s: left, up, forward are at right angles"), *Profile->Name), Off < 1e-6);
        }
    }
    TestEqual(TEXT("a profile like another takes its bones"), Plugin->Bones.FindRef(TEXT("LeftArm")), Moverse->Bones.FindRef(TEXT("LeftArm")));
    // The quarter turn about X that MoversePluginSource.cpp puts on its root bone.
    TestTrue(TEXT("moverse-plugin undoes the plugin's quarter turn"),
             AngleBetween(Plugin->RootRotation, FQuat(0.7071068, 0, 0, 0.7071068)) < 1e-4);
    TestTrue(TEXT("a profile like another does not take its root rotation"), Moverse->RootRotation.IsIdentity());
    TestTrue(TEXT("a camera-based capture is smoothed"), Moverse->Smoothing.IsOn());
    TestEqual(TEXT("a profile like another takes its smoothing"), Plugin->Smoothing.MinCutoff, Moverse->Smoothing.MinCutoff);
    TestFalse(TEXT("an inertial suit is not"), Xsens->Smoothing.IsOn());

    const FPerformer AsMoverse(*Moverse);
    const FPerformer AsXsens(*Xsens);
    TestTrue(TEXT("the viewer's reader gets moverse"), LoomaLiveLinkProfiles::For(AsMoverse.Names, TEXT("Looma JSON")) == Moverse);
    TestTrue(TEXT("a source of no known type gets moverse too"), LoomaLiveLinkProfiles::For(AsMoverse.Names, FString()) == Moverse);
    TestTrue(TEXT("Moverse's own plugin gets moverse-plugin"), LoomaLiveLinkProfiles::For(AsMoverse.Names, TEXT("Moverse Plugin (Actor0)")) == Plugin);
    TestTrue(TEXT("Xsens bones get xsens"), LoomaLiveLinkProfiles::For(AsXsens.Names, TEXT("Looma MVN")) == Xsens);
    TestTrue(TEXT("whatever the source says it is"), LoomaLiveLinkProfiles::For(AsXsens.Names, TEXT("Moverse Plugin (Actor0)")) == Xsens);
    TestNull(TEXT("bones no profile knows get none"),
             LoomaLiveLinkProfiles::For({FName(TEXT("root")), FName(TEXT("pelvis"))}, FString()));
    TArray<FName> NoToes = AsMoverse.Names;
    NoToes.Remove(Moverse->Bones.FindRef(TEXT("LeftToeBase")));
    TestNull(TEXT("a subject missing a required bone gets none"), LoomaLiveLinkProfiles::For(NoToes, FString()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveRetargetPoseTest,
    "Looma.LiveLink.Retarget.Pose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveRetargetPoseTest::RunTest(const FString& Parameters)
{
    for (const TCHAR* Name : {TEXT("moverse"), TEXT("xsens")})
    {
        const FLoomaLiveLinkProfile* Profile = Named(Name);
        if (!TestNotNull(Name, Profile))
        {
            continue;
        }
        auto Says = [Name](const TCHAR* What) { return FString::Printf(TEXT("%s: %s"), Name, What); };
        FLoomaLiveRetarget Retarget;
        TArray<FQuat> Wire;
        FVector Hips;

        // Standing in the T-pose is the wire's identity pose, one leg high.
        FPerformer Standing(*Profile);
        TestEqual(*Says(TEXT("every body bone is mapped")), Retarget.Build(*Profile, Standing.Names), LoomaRigBody::Num);
        TestTrue(*Says(TEXT("the T-pose has hips")), Retarget.Evaluate(Standing.Local, Standing.Parents, 0.016f, false, Wire, Hips));
        double Worst = 0.0;
        for (const FQuat& Rotation : Wire)
        {
            Worst = FMath::Max(Worst, AngleBetween(Rotation, FQuat::Identity));
        }
        TestTrue(*Says(TEXT("the T-pose is the identity on the wire")), Worst < 1e-4);
        TestTrue(*Says(TEXT("standing is one leg high, at the start")), Hips.Equals(FVector(0, 1, 0), 1e-4));

        // One joint bent: that turn, about that axis, on that bone and no other.
        FPerformer Elbow(*Profile);
        Elbow.Bend(TEXT("LeftForeArm"), FVector(0, 1, 0), 60 * Degrees);
        Retarget.Build(*Profile, Elbow.Names);
        Retarget.Evaluate(Elbow.Local, Elbow.Parents, 0.016f, false, Wire, Hips);
        const int32 ForeArm = LoomaRigBody::Row(TEXT("LeftForeArm"));
        TestTrue(*Says(TEXT("a bent elbow is that turn about up")),
                 AngleBetween(Wire[ForeArm], About(FVector(0, 1, 0), 60 * Degrees).ToQuat()) < 1e-4);
        Worst = 0.0;
        for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
        {
            Worst = Row == ForeArm ? Worst : FMath::Max(Worst, AngleBetween(Wire[Row], FQuat::Identity));
        }
        TestTrue(*Says(TEXT("and nothing else turns")), Worst < 1e-4);
        const FVector Hand = WhereTheWirePutsThem(Wire)[LoomaRigBody::Row(TEXT("LeftHand"))];
        TestTrue(*Says(TEXT("the hand goes where the source's hand went")),
                 Hand.Equals(Elbow.Where(LoomaRigBody::Row(TEXT("LeftHand"))) - Elbow.Where(0), 1e-3));

        // A whole pose: every joint where the source skeleton has it.
        FPerformer Posed(*Profile);
        Strike(Posed);
        Posed.MoveHips(FVector(30, -12, 50));
        Retarget.Build(*Profile, Standing.Names);
        Retarget.Evaluate(Standing.Local, Standing.Parents, 0.016f, false, Wire, Hips); // where the performer started
        Retarget.Evaluate(Posed.Local, Posed.Parents, 0.016f, false, Wire, Hips);
        const TArray<FVector> Body = WhereTheWirePutsThem(Wire);
        double Furthest = 0.0;
        for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
        {
            Furthest = FMath::Max(Furthest, FVector::Dist(Body[Row], Posed.Where(Row) - Posed.Where(0)));
        }
        TestTrue(*Says(TEXT("a posed body has every joint where the source has it")), Furthest < 1e-3);
        TestTrue(*Says(TEXT("the hips travel from where they started, in leg lengths")),
                 Hips.Equals(FVector(30, LegLength - 12, 50) / LegLength, 1e-4));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveRetargetRootTest,
    "Looma.LiveLink.Retarget.RootRotation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveRetargetRootTest::RunTest(const FString& Parameters)
{
    const FLoomaLiveLinkProfile* Moverse = Named(TEXT("moverse"));
    const FLoomaLiveLinkProfile* Plugin = Named(TEXT("moverse-plugin"));
    if (!TestNotNull(TEXT("moverse"), Moverse) || !TestNotNull(TEXT("moverse-plugin"), Plugin))
    {
        return false;
    }
    FPerformer Posed(*Moverse);
    Strike(Posed);
    FLoomaLiveRetarget Plain;
    TArray<FQuat> Wire;
    FVector Hips;
    Plain.Build(*Moverse, Posed.Names);
    Plain.Evaluate(Posed.Local, Posed.Parents, 0.016f, false, Wire, Hips);

    // The same performer as Moverse's plugin pushes them: a quarter turn in
    // front of the root bone's rotation, its translation left alone.
    FPerformer Turned = Posed;
    FTransform& Root = Turned.Local[Turned.Bones[0]];
    Root.SetRotation(FQuat(0.7071068, 0, 0, 0.7071068) * Root.GetRotation());
    FLoomaLiveRetarget ThroughPlugin;
    TArray<FQuat> PluginWire;
    FVector PluginHips;
    ThroughPlugin.Build(*Plugin, Turned.Names);
    ThroughPlugin.Evaluate(Turned.Local, Turned.Parents, 0.016f, false, PluginWire, PluginHips);

    double Worst = 0.0;
    for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
    {
        Worst = FMath::Max(Worst, AngleBetween(Wire[Row], PluginWire[Row]));
    }
    TestTrue(TEXT("the plugin's stream reads as the same pose"), Worst < 1e-4);
    TestTrue(TEXT("with the hips in the same place"), Hips.Equals(PluginHips, 1e-4));

    // And read with the wrong profile it does not: the character lies down.
    Plain.Build(*Moverse, Turned.Names);
    Plain.Evaluate(Turned.Local, Turned.Parents, 0.016f, false, PluginWire, PluginHips);
    TestTrue(TEXT("which the plain profile would get wrong by a quarter turn"),
             FMath::Abs(AngleBetween(Wire[0], PluginWire[0]) - 90 * Degrees) < 1e-3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveRetargetFloorTest,
    "Looma.LiveLink.Retarget.Floor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveRetargetFloorTest::RunTest(const FString& Parameters)
{
    const FLoomaLiveLinkProfile* Profile = Named(TEXT("moverse"));
    if (!TestNotNull(TEXT("moverse"), Profile))
    {
        return false;
    }
    const float Frame = 1.0f / 60.0f;
    TArray<FQuat> Wire;
    FVector Hips;

    // A performer whose mocap system has them 6 into its floor.
    const FPerformer Sunk(*Profile, -6.0);
    FLoomaLiveRetarget Trusting;
    Trusting.Build(*Profile, Sunk.Names);
    Trusting.Evaluate(Sunk.Local, Sunk.Parents, Frame, false, Wire, Hips);
    TestTrue(TEXT("the source's own zero leaves them sunk"), FMath::IsNearlyEqual(Hips.Y, (LegLength - 6.0) / LegLength, 1e-4));

    FLoomaLiveRetarget Finding;
    Finding.Build(*Profile, Sunk.Names);
    Finding.Evaluate(Sunk.Local, Sunk.Parents, Frame, true, Wire, Hips);
    TestTrue(TEXT("the floor under the feet has them standing, from the first frame"), FMath::IsNearlyEqual(Hips.Y, 1.0, 1e-4));
    for (int32 Step = 0; Step < 120; ++Step)
    {
        Finding.Evaluate(Sunk.Local, Sunk.Parents, Frame, true, Wire, Hips);
    }
    TestTrue(TEXT("and still standing two seconds on"), FMath::IsNearlyEqual(Hips.Y, 1.0, 1e-4));

    // Half a second in the air: the floor stays where the feet were resting.
    FPerformer Jumping = Sunk;
    Jumping.MoveHips(FVector(0, 30, 0));
    for (int32 Step = 0; Step < 30; ++Step)
    {
        Finding.Evaluate(Jumping.Local, Jumping.Parents, Frame, true, Wire, Hips);
    }
    TestTrue(TEXT("a jump is a jump, not a higher floor"), FMath::IsNearlyEqual(Hips.Y, (LegLength + 30.0) / LegLength, 0.01));

    // Stepping up onto something and staying there: the floor follows, within its window.
    FPerformer Raised = Sunk;
    Raised.MoveHips(FVector(0, 20, 0));
    for (int32 Step = 0; Step < 600; ++Step)
    {
        Finding.Evaluate(Raised.Local, Raised.Parents, Frame, true, Wire, Hips);
    }
    TestTrue(TEXT("a performer who stays higher is standing again ten seconds on"), FMath::IsNearlyEqual(Hips.Y, 1.0, 0.01));

    // A new start forgets the old floor.
    Finding.Restart();
    Finding.Evaluate(Sunk.Local, Sunk.Parents, Frame, true, Wire, Hips);
    TestTrue(TEXT("restarting finds the floor afresh"), FMath::IsNearlyEqual(Hips.Y, 1.0, 1e-4));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaPoseSmootherTest,
    "Looma.LiveLink.Retarget.Smoothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaPoseSmootherTest::RunTest(const FString& Parameters)
{
    const FLoomaLiveLinkProfile* Profile = Named(TEXT("moverse"));
    if (!TestNotNull(TEXT("moverse"), Profile))
    {
        return false;
    }
    const FLoomaSmoothing Settings = Profile->Smoothing;
    const double Frame = 1.0 / 60.0;
    const FVector Up(0, 1, 0);
    auto Pose = [](const FQuat& Rotation) {
        TArray<FQuat> Wire;
        Wire.Init(Rotation, LoomaRigBody::Num);
        return Wire;
    };
    FRandomStream Random(7);
    auto Shaken = [&Random](const FQuat& Rotation, double Sigma) {
        const FVector Axis(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f));
        return (Rotation * FQuat(Axis.GetSafeNormal(), Random.FRandRange(-1.0f, 1.0f) * Sigma)).GetNormalized();
    };

    // Off: the pose goes through untouched.
    FLoomaPoseSmoother Off;
    TArray<FQuat> Wire = Pose(About(Up, 20 * Degrees).ToQuat());
    const TArray<FQuat> Before = Wire;
    Off.Apply(Wire, nullptr, Frame, FLoomaSmoothing());
    TestTrue(TEXT("with no smoothing asked for, the pose is used as it comes"), Wire[3].Equals(Before[3], 0.0));

    // A still performer who shakes by two degrees is steadied.
    FLoomaPoseSmoother Still;
    const FQuat Held = About(Up, 20 * Degrees).ToQuat();
    double RawShake = 0.0, SmoothShake = 0.0, Drift = 0.0;
    FQuat LastRaw = Held, LastSmooth = Held;
    for (int32 Step = 0; Step < 600; ++Step)
    {
        Wire = Pose(Shaken(Held, 2 * Degrees));
        const FQuat RawNow = Wire[3];
        FVector Hips(0, 1, 0);
        Still.Apply(Wire, &Hips, Frame, Settings);
        if (Step >= 60)
        {
            RawShake += LastRaw.AngularDistance(RawNow);
            SmoothShake += LastSmooth.AngularDistance(Wire[3]);
            Drift = FMath::Max(Drift, Wire[3].AngularDistance(Held));
        }
        LastRaw = RawNow;
        LastSmooth = Wire[3];
    }
    TestTrue(TEXT("a still performer shakes less than a third as much"), SmoothShake < RawShake / 3.0);
    TestTrue(TEXT("and stays where they are"), Drift < 1.5 * Degrees);

    // A fast, clean movement is followed closely: a bone turning 180 degrees a second.
    FLoomaPoseSmoother Moving;
    double Behind = 0.0;
    for (int32 Step = 0; Step < 120; ++Step)
    {
        const FQuat Now = About(Up, Step * Frame * 180 * Degrees).ToQuat();
        Wire = Pose(Now);
        Moving.Apply(Wire, nullptr, Frame, Settings);
        Behind = Step >= 60 ? FMath::Max(Behind, Wire[3].AngularDistance(Now)) : Behind;
    }
    TestTrue(TEXT("a bone turning half a turn a second is followed within eight degrees"), Behind < 8 * Degrees);

    // The same frame coming round again is not a new one.
    FLoomaPoseSmoother Repeating;
    Wire = Pose(FQuat::Identity);
    Repeating.Apply(Wire, nullptr, Frame, Settings);
    Wire = Pose(About(Up, 30 * Degrees).ToQuat());
    Repeating.Apply(Wire, nullptr, Frame, Settings);
    const FQuat AfterOne = Wire[3];
    for (int32 Step = 0; Step < 3; ++Step)
    {
        Wire = Pose(About(Up, 30 * Degrees).ToQuat());
        Repeating.Apply(Wire, nullptr, Frame / 4, Settings);
    }
    TestTrue(TEXT("a frame that comes round again moves nothing"), Wire[3].Equals(AfterOne, 0.0));
    TestTrue(TEXT("a jump is not taken whole"), AfterOne.AngularDistance(FQuat::Identity) < 25 * Degrees);

    // After a gap the next frame is a new start.
    Wire = Pose(About(Up, 90 * Degrees).ToQuat());
    Repeating.Apply(Wire, nullptr, 2.0, Settings);
    TestTrue(TEXT("after a gap the pose is taken whole"), Wire[3].AngularDistance(About(Up, 90 * Degrees).ToQuat()) < 1e-4);

    // The hips are steadied with the rest.
    FLoomaPoseSmoother Walking;
    double HipsShake = 0.0, RawHipsShake = 0.0;
    FVector LastHips(0, 1, 0), LastRawHips(0, 1, 0);
    for (int32 Step = 0; Step < 300; ++Step)
    {
        Wire = Pose(Shaken(Held, 1 * Degrees));
        FVector Hips(0, 1.0 + Random.FRandRange(-0.01f, 0.01f), 0);
        const FVector RawHips = Hips;
        Walking.Apply(Wire, &Hips, Frame, Settings);
        if (Step >= 60)
        {
            RawHipsShake += FVector::Dist(LastRawHips, RawHips);
            HipsShake += FVector::Dist(LastHips, Hips);
        }
        LastRawHips = RawHips;
        LastHips = Hips;
    }
    TestTrue(TEXT("hips that shake are steadied too"), HipsShake < RawHipsShake / 3.0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
