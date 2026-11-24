#include "../LoomaLiveTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"

// The target half of the retarget with no mesh: a reference skeleton built the
// way glTFRuntime builds one from a rig file, posed from the wire as the anim
// instance's proxy poses it, and held to where the rig format says the joints
// go: the wire's rotations set on the bones, in the file's own axes, and the
// hips at `hips * hips_height` (docs/rig-format.md in looma-xr-asset-demo, "Live
// pose"). That is all the web client does (livePose.js), so it is also where
// the web puts them.
//
// The body is a real one. Ocean Whisper's head joint is 5.5 cm behind its hips
// joint, and its hip joints are not level: axes measured from that anatomy lean
// 6.3 degrees from the file's. Posed in leaning axes it rose and sank by more
// than 10 cm as its performer walked, and tipped over when they turned.

namespace LoomaLiveTargetTests
{
    const double Degrees = UE_DOUBLE_PI / 180.0;

    /** A rig file's rest pose: each body bone's offset from its parent in the file's axes, X left, Y up, Z forward, metres. */
    struct FRigFile
    {
        FVector Offsets[LoomaRigBody::Num];
        /** The A-pose: how far each upper arm is turned down about the file's Z. */
        double ArmDegrees = 45.0;
        /** Where the whole skeleton sits from the file's origin: zero in a conforming file. */
        FVector Shift = FVector::ZeroVector;
    };

    // rig-259843b3988c in the develop catalogue, made by Make-It-Animatable v1 on 2026-11-24.
    const FRigFile OceanWhisper = {{
        FVector(0.000000, 0.880827, 0.000000),   // Hips, from the floor
        FVector(-0.001012, 0.094747, -0.003173), // Spine
        FVector(0.001281, 0.084804, -0.009922),  // Spine1
        FVector(0.001483, 0.095346, -0.019481),  // Spine2
        FVector(0.000947, 0.133942, -0.023104),  // Neck
        FVector(0.002185, 0.093004, 0.000446),   // Head
        FVector(0.051652, 0.101711, -0.022930),  // LeftShoulder
        FVector(0.095094, -0.029529, 0.000339),  // LeftArm
        FVector(0.224891, 0.000000, 0.000000),   // LeftForeArm
        FVector(0.237949, 0.000000, 0.000000),   // LeftHand
        FVector(-0.051412, 0.103299, -0.028911), // RightShoulder
        FVector(-0.099192, -0.029974, 0.000346), // RightArm
        FVector(-0.227156, 0.000000, 0.000000),  // RightForeArm
        FVector(-0.241430, 0.000000, 0.000000),  // RightHand
        FVector(0.086239, -0.046200, 0.003166),  // LeftUpLeg
        FVector(0.015738, -0.357072, 0.047888),  // LeftLeg
        FVector(-0.001387, -0.382451, -0.008652), // LeftFoot
        FVector(0.004541, -0.085354, 0.120004),  // LeftToeBase
        FVector(-0.087980, -0.052072, 0.001717), // RightUpLeg
        FVector(-0.006434, -0.355322, 0.034045), // RightLeg
        FVector(0.004351, -0.375570, -0.006785), // RightFoot
        FVector(-0.006731, -0.088640, 0.118022), // RightToeBase
    }};

    /** The file's axes and metres as glTFRuntime's default loader maps them: (x, y, z) to (-z, x, y), in centimetres. */
    FVector InUnreal(const FVector& InFile)
    {
        return FVector(-InFile.Z, InFile.X, InFile.Y) * 100.0;
    }

    /**
     * A joint's rest transform as that loader hands it to the reference
     * skeleton: its matrix in the file, the translation scaled, conjugated by
     * the scene basis (TraverseJoints in glTFRuntimeParser.cpp).
     */
    FTransform FromFile(const FQuat& Rotation, const FVector& Metres)
    {
        const FMatrix Basis = FBasisVectorMatrix(FVector(0, 0, -1), FVector(1, 0, 0), FVector(0, 1, 0), FVector::ZeroVector);
        const FMatrix InFile = FTransform(Rotation, Metres * 100.0).ToMatrixNoScale();
        return FTransform(Basis.Inverse() * InFile * Basis);
    }

    /** The file's skeleton as the loader builds it. `Rows` short of the whole table leaves the last bones out. */
    FReferenceSkeleton SkeletonOf(const FRigFile& File, int32 Rows = LoomaRigBody::Num)
    {
        FReferenceSkeleton Out;
        {
            FReferenceSkeletonModifier Modifier(Out, nullptr);
            for (int32 Row = 0; Row < Rows; ++Row)
            {
                const FString Bare = LoomaRigBody::Name(Row);
                const FString Name = TEXT("mixamorig:") + Bare;
                // The rest pose is the T-pose with each upper arm turned down about +Z, and nothing else.
                const double Arm = Bare == TEXT("LeftArm") ? -File.ArmDegrees : Bare == TEXT("RightArm") ? File.ArmDegrees : 0.0;
                const FQuat Rest(FVector(0, 0, 1), Arm * Degrees);
                const FVector Offset = File.Offsets[Row] + (Row == 0 ? File.Shift : FVector::ZeroVector);
                Modifier.Add(FMeshBoneInfo(FName(*Name), Name, LoomaRigBody::Parent(Row)), FromFile(Rest, Offset));
            }
        }
        return Out;
    }

    /**
     * The skeleton in component space, posed as FLoomaLivePoseProxy::Evaluate
     * poses it: the reference pose, each body bone's rotation replaced and the
     * hips moved. With no wire it is the reference pose.
     */
    TArray<FTransform> Posed(const FReferenceSkeleton& Skeleton, const FLoomaLiveTarget& Target,
                             const FVector* Hips = nullptr, const TArray<FQuat>* Wire = nullptr)
    {
        TArray<FTransform> Local = Skeleton.GetRefBonePose();
        if (Wire)
        {
            for (int32 Row = 0; Row < LoomaRigBody::Num; ++Row)
            {
                Local[Target.Bones[Row]].SetRotation(Target.Rotation(Row, (*Wire)[Row].GetNormalized()));
            }
        }
        if (Hips)
        {
            Local[Target.HipsBone].SetTranslation(Target.HipsLocation(*Hips));
        }
        TArray<FTransform> World;
        World.SetNum(Local.Num());
        for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
        {
            const int32 Parent = Skeleton.GetParentIndex(Bone);
            World[Bone] = Parent >= 0 ? Local[Bone] * World[Parent] : Local[Bone];
        }
        return World;
    }

    /**
     * How far the soles are from the floor, the worst of the four: the points
     * of the floor under each ankle and each toe joint in the reference pose,
     * carried by the foot and the toe bone. The floor is the mesh's origin plane.
     */
    double SolesOffTheFloor(const FLoomaLiveTarget& Target, const TArray<FTransform>& Rest, const TArray<FTransform>& Now)
    {
        double Worst = 0.0;
        for (const TCHAR* Name : {TEXT("LeftFoot"), TEXT("LeftToeBase"), TEXT("RightFoot"), TEXT("RightToeBase")})
        {
            const int32 Bone = Target.Bones[LoomaRigBody::Row(Name)];
            FVector OnTheFloor = Rest[Bone].GetLocation();
            OnTheFloor.Z = 0.0;
            const FVector Sole = Now[Bone].TransformPosition(Rest[Bone].InverseTransformPosition(OnTheFloor));
            Worst = FMath::Max(Worst, FMath::Abs(Sole.Z));
        }
        return Worst;
    }

    /** Standing straight: every bone as in the T-pose, the whole body turned about up. */
    TArray<FQuat> Standing(double TurnDegrees)
    {
        TArray<FQuat> Wire;
        Wire.Init(FQuat::Identity, LoomaRigBody::Num);
        Wire[0] = FQuat(FVector(0, 1, 0), TurnDegrees * Degrees);
        // The wire is measured from the T-pose, so the arms it sends are level;
        // they are let down here as the rest pose has them, to keep the body's shape.
        Wire[LoomaRigBody::Row(TEXT("LeftArm"))] = FQuat(FVector(0, 0, 1), -45.0 * Degrees);
        Wire[LoomaRigBody::Row(TEXT("RightArm"))] = FQuat(FVector(0, 0, 1), 45.0 * Degrees);
        return Wire;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveTargetSolesTest,
    "Looma.LiveLink.Target.Soles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveTargetSolesTest::RunTest(const FString& Parameters)
{
    using namespace LoomaLiveTargetTests;
    const FReferenceSkeleton Skeleton = SkeletonOf(OceanWhisper);
    FLoomaLiveTarget Target;
    FString Why;
    if (!TestTrue(TEXT("a rig that leans is still a rig"), Target.Build(Skeleton, Why)))
    {
        AddError(Why);
        return false;
    }
    const TArray<FTransform> Rest = Posed(Skeleton, Target);
    const double HipsHeight = OceanWhisper.Offsets[0].Y * 100.0;
    const int32 Head = Target.Bones[LoomaRigBody::Row(TEXT("Head"))];

    // The skeleton is the loader's: the hips straight over the origin, left on
    // +Y, the toes towards -X.
    TestTrue(TEXT("the hips rest straight over the origin"), Rest[Target.HipsBone].GetLocation().Equals(FVector(0, 0, HipsHeight), 1e-3));
    TestTrue(TEXT("and this body's head rests behind them"), Rest[Head].GetLocation().X > 5.0);

    // The character's axes are the file's, however the body is built.
    TestTrue(TEXT("left is the file's X"), Target.Left.Equals(InUnreal(FVector(1, 0, 0)) / 100.0, 1e-4));
    TestTrue(TEXT("up is the file's Y"), Target.Up.Equals(InUnreal(FVector(0, 1, 0)) / 100.0, 1e-4));
    TestTrue(TEXT("forward is the file's Z"), Target.Forward.Equals(InUnreal(FVector(0, 0, 1)) / 100.0, 1e-4));
    TestEqual(TEXT("the hips' height is the file's hips_height"), Target.HipsHeight, HipsHeight, 1e-3);

    // Standing straight, wherever the performer has walked to and whichever way
    // they face: the body keeps its rest shape, so its soles stay on the floor
    // and its head at its height, and its hips are where the wire says.
    struct FStance
    {
        const TCHAR* What;
        FVector Hips; // left, up, forward, in hips heights
        double Turn;  // degrees about up
    };
    const FStance Stances[] = {
        {TEXT("where they started"), FVector(0, 1, 0), 0.0},
        {TEXT("one hips height forward"), FVector(0, 1, 1), 0.0},
        {TEXT("back and to the left"), FVector(1.4, 1, -1.04), 0.0},
        {TEXT("turned a quarter"), FVector(0, 1, 0), 90.0},
        {TEXT("turned right round"), FVector(0, 1, 0), 180.0},
        {TEXT("turned and walked off"), FVector(-0.6, 1, 0.8), 135.0},
    };
    for (const FStance& Stance : Stances)
    {
        const TArray<FQuat> Wire = Standing(Stance.Turn);
        const TArray<FTransform> Now = Posed(Skeleton, Target, &Stance.Hips, &Wire);
        const double Off = SolesOffTheFloor(Target, Rest, Now);
        TestTrue(FString::Printf(TEXT("standing %s, the soles are on the floor (%.2f cm off it)"), Stance.What, Off), Off < 1.0);
        const double HeadOff = FMath::Abs(Now[Head].GetLocation().Z - Rest[Head].GetLocation().Z);
        TestTrue(FString::Printf(TEXT("standing %s, the head is at its height (%.2f cm off it)"), Stance.What, HeadOff), HeadOff < 1.0);
        const FVector Hips = Now[Target.HipsBone].GetLocation();
        TestTrue(FString::Printf(TEXT("standing %s, the hips are where the wire says (%s)"), Stance.What, *Hips.ToCompactString()),
                 Hips.Equals(InUnreal(Stance.Hips * OceanWhisper.Offsets[0].Y), 1e-2));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveTargetWebPoseTest,
    "Looma.LiveLink.Target.WebPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveTargetWebPoseTest::RunTest(const FString& Parameters)
{
    using namespace LoomaLiveTargetTests;
    // One pose the hub sent for this body on 2026-11-24, from the Moverse
    // simulator's take: the performer a hips height back and to the left of
    // where they started, turned to their left, mid-step.
    const FVector Hips(0.674, 0.961, -1.032);
    const TArray<FQuat> Wire = {
        FQuat(0.035289, 0.354417, 0.011787, -0.934347),
        FQuat(0.113093, 0.010019, -0.001416, 0.993533),
        FQuat(0.053708, -0.006474, 0.010887, 0.998476),
        FQuat(0.004095, 0.004177, 0.000485, 0.999983),
        FQuat(0.107838, 0.162850, -0.068885, 0.978318),
        FQuat(0.023270, -0.005120, 0.013960, 0.999619),
        FQuat(0.008086, -0.020554, -0.165573, 0.985950),
        FQuat(0.123027, -0.036894, -0.452407, 0.882514),
        FQuat(0.132868, -0.282418, 0.040796, 0.949169),
        FQuat(0.011246, -0.068228, 0.047753, 0.996463),
        FQuat(-0.002593, 0.065496, 0.141493, 0.987767),
        FQuat(0.060334, 0.195524, 0.394460, 0.895841),
        FQuat(0.124001, 0.221995, -0.022997, 0.966857),
        FQuat(0.012145, 0.065673, -0.041906, 0.996887),
        FQuat(-0.242794, 0.044365, -0.014559, 0.968953),
        FQuat(0.245346, 0.030638, -0.044271, 0.967940),
        FQuat(-0.045526, -0.055272, 0.090343, 0.993333),
        FQuat(0.000000, 0.000010, 0.000000, 1.000000),
        FQuat(0.036429, -0.245604, -0.024326, 0.968380),
        FQuat(0.181853, -0.070140, 0.029262, 0.980384),
        FQuat(-0.150804, -0.008383, -0.101961, 0.983256),
        FQuat(0.000000, -0.000010, 0.000000, 1.000000),
    };
    // Where the web client's own code (createLivePoser in livePose.js, on this
    // rig's joints) puts them for that pose, in the file's axes and metres.
    const TPair<const TCHAR*, FVector> Web[] = {
        {TEXT("Hips"), FVector(0.59368, 0.84647, -0.90901)},
        {TEXT("Head"), FVector(0.56986, 1.34074, -0.86081)},
        {TEXT("LeftFoot"), FVector(0.45648, 0.14679, -0.68431)},
        {TEXT("LeftToeBase"), FVector(0.37577, 0.08196, -0.57948)},
        {TEXT("RightFoot"), FVector(0.62219, 0.09050, -0.98704)},
        {TEXT("RightToeBase"), FVector(0.50671, -0.00167, -0.98652)},
    };

    const FReferenceSkeleton Skeleton = SkeletonOf(OceanWhisper);
    FLoomaLiveTarget Target;
    FString Why;
    if (!TestTrue(TEXT("the rig builds"), Target.Build(Skeleton, Why)))
    {
        AddError(Why);
        return false;
    }
    const TArray<FTransform> Now = Posed(Skeleton, Target, &Hips, &Wire);
    for (const TPair<const TCHAR*, FVector>& Joint : Web)
    {
        const FVector Here = Now[Target.Bones[LoomaRigBody::Row(Joint.Key)]].GetLocation();
        const FVector There = InUnreal(Joint.Value);
        // A tenth of a millimetre: the web's numbers were written to five places.
        TestTrue(FString::Printf(TEXT("%s is where the web has it (%.2f cm from it, %.2f cm higher)"), Joint.Key,
                                 FVector::Dist(Here, There), Here.Z - There.Z),
                 Here.Equals(There, 1e-2));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveTargetOtherRigsTest,
    "Looma.LiveLink.Target.OtherRigs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveTargetOtherRigsTest::RunTest(const FString& Parameters)
{
    using namespace LoomaLiveTargetTests;
    FString Why;
    const FVector Walked(-0.6, 1, 0.8);
    const TArray<FQuat> Turned = Standing(135.0);

    // A rig as the format lays it out gives both axes itself, and says how far
    // its shape is from them.
    {
        FLoomaLiveTarget Target;
        TestTrue(TEXT("a conforming rig builds"), Target.Build(SkeletonOf(OceanWhisper), Why));
        TestTrue(TEXT("its up is from the origin to the hips"), Target.bUpFromOrigin);
        TestTrue(TEXT("its forward is the axis its arms rest turned about"), Target.bForwardFromArms);
        TestEqual(TEXT("this one's shape leans 6.3 degrees from that"), Target.Lean, 6.3, 0.1);
    }

    // One that rests in a T-pose has no arm turn to read. Its forward comes from
    // its toes, which only turns it a little about an up that is still exact, so
    // it stands on the floor all the same.
    {
        FRigFile Level = OceanWhisper;
        Level.ArmDegrees = 0.0;
        const FReferenceSkeleton Skeleton = SkeletonOf(Level);
        FLoomaLiveTarget Target;
        TestTrue(TEXT("a rig resting in the T-pose builds"), Target.Build(Skeleton, Why));
        TestTrue(TEXT("its up is still from the origin"), Target.bUpFromOrigin);
        TestFalse(TEXT("its forward is from its toes"), Target.bForwardFromArms);
        const double Off = SolesOffTheFloor(Target, Posed(Skeleton, Target), Posed(Skeleton, Target, &Walked, &Turned));
        TestTrue(FString::Printf(TEXT("and its soles are on the floor (%.2f cm off it)"), Off), Off < 1.0);
    }

    // A skeleton that does not stand over its origin is not laid out as the
    // format says, so nothing about it is exact: its axes are its shape's, as
    // every rig's were, and it is still posed.
    {
        FRigFile Elsewhere = OceanWhisper;
        Elsewhere.Shift = FVector(0, 0, 0.8);
        FLoomaLiveTarget Target;
        TestTrue(TEXT("a skeleton away from its origin still builds"), Target.Build(SkeletonOf(Elsewhere), Why));
        TestFalse(TEXT("its up is not from the origin"), Target.bUpFromOrigin);
        // The line from hips to head, less the little of it that runs hip to hip.
        TestTrue(FString::Printf(TEXT("but from the hips to the head (%.1f degrees from it)"), Target.Lean), Target.Lean < 3.0);
    }

    // And one without the bones is refused, with the bone it lacks.
    {
        FLoomaLiveTarget Target;
        TestFalse(TEXT("a skeleton with a toe missing is not a rig"), Target.Build(SkeletonOf(OceanWhisper, LoomaRigBody::Num - 1), Why));
        TestTrue(TEXT("and the reason names the bone"), Why.Contains(TEXT("RightToeBase")));
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
