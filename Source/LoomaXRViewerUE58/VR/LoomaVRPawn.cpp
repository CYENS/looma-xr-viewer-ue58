#include "LoomaVRPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "LoomaSceneSyncSubsystem.h"
#include "LoomaSyncedActor.h"
#include "MotionControllerComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "XRDeviceVisualizationComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaVR, Log, All);

namespace
{
    constexpr float LaserReach = 4000.f;  // cm: past this the laser points at nothing
    constexpr float ArcSpeed = 900.f;     // cm/s launch speed: about 12 m of reach at 45 degrees
    constexpr int32 ArcDots = 24;

    UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
    {
        UInputAction* Action = NewObject<UInputAction>(Outer, Name);
        Action->ValueType = Type;
        return Action;
    }
}

ALoomaVRPawn::ALoomaVRPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    // Movement here moves the camera's parent; the camera itself follows the
    // headset during the frame, and late, so it must not be the one being waited on.
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    Origin = CreateDefaultSubobject<USceneComponent>(TEXT("Origin"));
    SetRootComponent(Origin);

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(Origin);
    Camera->bLockToHmd = true;

    LeftGrip = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("LeftGrip"));
    LeftGrip->SetupAttachment(Origin);
    LeftGrip->SetTrackingMotionSource(FName(TEXT("LeftGrip")));

    RightGrip = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightGrip"));
    RightGrip->SetupAttachment(Origin);
    RightGrip->SetTrackingMotionSource(FName(TEXT("RightGrip")));

    // The laser leaves from the aim pose, which points along the controller the
    // way the runtime means it to, not from the grip.
    RightAim = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightAim"));
    RightAim->SetupAttachment(Origin);
    RightAim->SetTrackingMotionSource(FName(TEXT("RightAim")));

    // The runtime's own controller models, as the VR template draws them.
    for (UMotionControllerComponent* Hand : {LeftGrip.Get(), RightGrip.Get()})
    {
        const FName Name = Hand == LeftGrip ? TEXT("LeftModel") : TEXT("RightModel");
        UXRDeviceVisualizationComponent* Model = CreateDefaultSubobject<UXRDeviceVisualizationComponent>(Name);
        Model->SetupAttachment(Hand);
        Model->bIsVisualizationActive = true;
    }

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

    // A thin cylinder from the aim pose, stretched to whatever it points at. The
    // engine's cylinder is 100 cm tall along Z and centred, hence the pitch and
    // the offset set each frame in UpdateLaser.
    Laser = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Laser"));
    Laser->SetupAttachment(RightAim);
    Laser->SetStaticMesh(Cylinder.Object);
    Laser->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Laser->SetCastShadow(false);

    Arc = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Arc"));
    Arc->SetupAttachment(Origin);
    Arc->SetUsingAbsoluteLocation(true);
    Arc->SetUsingAbsoluteRotation(true);
    Arc->SetUsingAbsoluteScale(true);
    Arc->SetStaticMesh(Sphere.Object);
    Arc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Arc->SetCastShadow(false);

    Landing = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Landing"));
    Landing->SetupAttachment(Origin);
    Landing->SetUsingAbsoluteLocation(true);
    Landing->SetUsingAbsoluteRotation(true);
    Landing->SetStaticMesh(Cylinder.Object);
    Landing->SetWorldScale3D(FVector(0.5f, 0.5f, 0.01f));
    Landing->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Landing->SetCastShadow(false);
    Landing->SetVisibility(false);
}

void ALoomaVRPawn::BeginPlay()
{
    Super::BeginPlay();
    BuildInput();
    // Floor-relative tracking: the pawn is the floor, the headset reports the
    // head's height above it.
    UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::LocalFloor);
}

void ALoomaVRPawn::BuildInput()
{
    MoveAction = MakeAction(this, TEXT("IA_LoomaVR_Move"), EInputActionValueType::Axis2D);
    LookXAction = MakeAction(this, TEXT("IA_LoomaVR_LookX"), EInputActionValueType::Axis1D);
    LookYAction = MakeAction(this, TEXT("IA_LoomaVR_LookY"), EInputActionValueType::Axis1D);
    FlyAction = MakeAction(this, TEXT("IA_LoomaVR_Fly"), EInputActionValueType::Boolean);
    SelectAction = MakeAction(this, TEXT("IA_LoomaVR_Select"), EInputActionValueType::Boolean);
    GrabAction = MakeAction(this, TEXT("IA_LoomaVR_Grab"), EInputActionValueType::Boolean);

    MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LoomaVR"));
    MappingContext->MapKey(MoveAction, EKeys::OculusTouch_Left_Thumbstick_2D);
    MappingContext->MapKey(LookXAction, EKeys::OculusTouch_Right_Thumbstick_X);
    MappingContext->MapKey(LookYAction, EKeys::OculusTouch_Right_Thumbstick_Y);
    MappingContext->MapKey(FlyAction, EKeys::OculusTouch_Left_Thumbstick_Click);
    MappingContext->MapKey(SelectAction, EKeys::OculusTouch_Right_Trigger_Click);
    MappingContext->MapKey(GrabAction, EKeys::OculusTouch_Right_Grip_Click);
}

void ALoomaVRPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!Input || !MappingContext)
    {
        UE_LOG(LogLoomaVR, Error, TEXT("VR pawn needs Enhanced Input (DefaultInputComponentClass)."));
        return;
    }
    // Polled every tick rather than bound to events: locomotion wants the
    // stick's value each frame, and the buttons are edge-detected here.
    MoveValue = &Input->BindActionValue(MoveAction);
    LookXValue = &Input->BindActionValue(LookXAction);
    LookYValue = &Input->BindActionValue(LookYAction);
    FlyValue = &Input->BindActionValue(FlyAction);
    SelectValue = &Input->BindActionValue(SelectAction);
    GrabValue = &Input->BindActionValue(GrabAction);
}

void ALoomaVRPawn::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);
    if (const APlayerController* PC = Cast<APlayerController>(NewController))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
                ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            Subsystem->AddMappingContext(MappingContext, 10);
        }
    }
}

void ALoomaVRPawn::SetHandover(const FVector& ViewLocation, float ViewYaw, float FloorZ)
{
    HandoverLocation = ViewLocation;
    HandoverYaw = ViewYaw;
    HandoverFloorZ = FloorZ;
    bPendingHandover = true;
}

void ALoomaVRPawn::Handover()
{
    // The camera's relative transform is the head in the room; until the
    // headset has reported a pose it is at the origin and tells us nothing.
    const FTransform Head = Camera->GetRelativeTransform();
    if (Head.GetLocation().IsNearlyZero())
    {
        return;
    }
    bPendingHandover = false;
    FVector Location;
    float Yaw = 0.f;
    LoomaVR::PlaceForView(HandoverLocation, HandoverYaw, HandoverFloorZ, Head.GetLocation(),
        Head.Rotator().Yaw, Location, Yaw);
    SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f));
}

FVector ALoomaVRPawn::FloorUnder(const FVector& Point, bool& bFound) const
{
    FHitResult Hit;
    FCollisionQueryParams Params(TEXT("LoomaVRFloor"), false, this);
    FCollisionObjectQueryParams Static(ECC_WorldStatic);
    bFound = GetWorld()->LineTraceSingleByObjectType(
        Hit, Point + FVector(0.f, 0.f, 200.f), Point - FVector(0.f, 0.f, 10000.f), Static, Params);
    return bFound ? Hit.ImpactPoint : Point;
}

void ALoomaVRPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bPendingHandover)
    {
        Handover();
    }

    const FVector2D Move = MoveValue ? MoveValue->GetValue().Get<FVector2D>() : FVector2D::ZeroVector;
    FVector2D Look(LookXValue ? LookXValue->GetValue().Get<float>() : 0.f,
        LookYValue ? LookYValue->GetValue().Get<float>() : 0.f);

    const bool bFlyNow = FlyValue && FlyValue->GetValue().Get<bool>();
    if (bFlyNow && !bFlyHeld)
    {
        bFly = !bFly;
        if (!bFly)
        {
            bool bFound = false;
            const FVector Floor = FloorUnder(Camera->GetComponentLocation(), bFound);
            if (bFound)
            {
                SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, Floor.Z));
            }
        }
    }
    bFlyHeld = bFlyNow;

    float Distance = 0.f;
    ALoomaSyncedActor* Pointed = UpdateLaser(Distance);

    const bool bSelectNow = SelectValue && SelectValue->GetValue().Get<bool>();
    if (bSelectNow && !bSelectHeld)
    {
        Select(Pointed);
    }
    bSelectHeld = bSelectNow;

    const bool bGrabNow = GrabValue && GrabValue->GetValue().Get<bool>();
    if (bGrabNow && !bGrabHeld)
    {
        BeginCarry(Pointed, Distance);
    }
    else if (!bGrabNow && bGrabHeld)
    {
        EndCarry();
    }
    bGrabHeld = bGrabNow;

    // A hand that carries something uses its stick for that.
    if (Carried.Num() > 0)
    {
        Carry(DeltaSeconds, Look);
        Look = FVector2D::ZeroVector;
    }
    Locomote(DeltaSeconds, Move, Look);
}

void ALoomaVRPawn::Locomote(float DeltaSeconds, const FVector2D& Move, const FVector2D& Look)
{
    const FVector Head = Camera->GetComponentLocation();
    const FRotator HeadRotation = Camera->GetComponentRotation();
    const float Dt = FMath::Min(DeltaSeconds, 0.05f);

    FVector Velocity = LoomaVR::MoveVelocity(Move, HeadRotation, bFly, bFly ? FlySpeed : LoomaVR::WalkSpeed);
    if (bFly)
    {
        Velocity.Z += LoomaVR::Axis(Look.Y) * FlySpeed;
    }
    if (!Velocity.IsNearlyZero())
    {
        FVector Location = GetActorLocation() + Velocity * Dt;
        if (!bFly)
        {
            // Feet stay on the floor, hills and stages included.
            bool bFound = false;
            const FVector Floor = FloorUnder(Head + Velocity * Dt, bFound);
            if (bFound)
            {
                Location.Z = Floor.Z;
            }
        }
        SetActorLocation(Location);
    }

    if (!bFly)
    {
        AimTeleport(Look);
    }
    if (!bAiming)
    {
        const float Turn = LoomaVR::SnapTurnStep(Look.X, bSnapArmed, SnapDegrees);
        if (Turn != 0.f)
        {
            FVector Location = GetActorLocation();
            float Yaw = GetActorRotation().Yaw;
            LoomaVR::TurnAboutHead(Location, Yaw, Camera->GetComponentLocation(), Turn);
            SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f));
        }
    }
}

void ALoomaVRPawn::AimTeleport(const FVector2D& Look)
{
    if (!bAiming && Look.Y >= LoomaVR::AimStart)
    {
        bAiming = true;
    }
    if (!bAiming)
    {
        return;
    }

    // A thrown ball's path from the controller, landing on static geometry only:
    // the floor and the set, never a carried prop or a character.
    FPredictProjectilePathParams Params;
    Params.StartLocation = RightAim->GetComponentLocation();
    Params.LaunchVelocity = RightAim->GetForwardVector() * ArcSpeed;
    Params.bTraceWithCollision = true;
    Params.ObjectTypes = {UEngineTypes::ConvertToObjectType(ECC_WorldStatic)};
    Params.ActorsToIgnore = {this};
    Params.MaxSimTime = 2.f;
    Params.SimFrequency = 15.f;
    FPredictProjectilePathResult Result;
    bHaveTeleportTarget = UGameplayStatics::PredictProjectilePath(this, Params, Result);
    TeleportTarget = Result.HitResult.ImpactPoint;

    Arc->ClearInstances();
    const int32 Points = Result.PathData.Num();
    for (int32 i = 0; i < ArcDots && Points > 1; ++i)
    {
        const FVector P = Result.PathData[FMath::Min(Points - 1, i * Points / ArcDots)].Location;
        Arc->AddInstance(FTransform(FRotator::ZeroRotator, P, FVector(0.03f)), true);
    }
    Landing->SetVisibility(bHaveTeleportTarget);
    if (bHaveTeleportTarget)
    {
        Landing->SetWorldLocation(TeleportTarget + FVector(0.f, 0.f, 1.f));
    }

    if (FMath::Abs(Look.Y) < LoomaVR::AimRelease)
    {
        bAiming = false;
        Arc->ClearInstances();
        Landing->SetVisibility(false);
        if (bHaveTeleportTarget)
        {
            SetActorLocation(LoomaVR::TeleportedPawn(GetActorLocation(), Camera->GetComponentLocation(), TeleportTarget));
        }
    }
}

ALoomaSyncedActor* ALoomaVRPawn::UpdateLaser(float& OutDistance)
{
    const FVector Start = RightAim->GetComponentLocation();
    const FVector Direction = RightAim->GetForwardVector();
    const FVector End = Start + Direction * LaserReach;

    // Nodes are found by their bounds, not by collision: the models LoomaSceneSync
    // loads at runtime carry no collision, so a trace would pass straight through
    // them. Of every node whose bounds the ray enters, the nearest wins, and on a
    // tie the smaller, so a child standing on a table is picked rather than the
    // table around it, as a click on the web picks the innermost node.
    ALoomaSyncedActor* Best = nullptr;
    float BestDistance = LaserReach;
    float BestVolume = TNumericLimits<float>::Max();
    for (TActorIterator<ALoomaSyncedActor> It(GetWorld()); It; ++It)
    {
        ALoomaSyncedActor* Node = *It;
        const FBox Bounds = Node->GetComponentsBoundingBox(false, true);
        if (!Bounds.IsValid)
        {
            continue;
        }
        FVector HitLocation;
        FVector HitNormal;
        float HitTime = 0.f;
        if (!FMath::LineExtentBoxIntersection(Bounds, Start, End, FVector::ZeroVector, HitLocation, HitNormal, HitTime))
        {
            continue;
        }
        const float Distance = HitTime * LaserReach;
        const float Volume = Bounds.GetVolume();
        if (Distance < BestDistance - 1.f || (FMath::Abs(Distance - BestDistance) <= 1.f && Volume < BestVolume))
        {
            Best = Node;
            BestDistance = Distance;
            BestVolume = Volume;
        }
    }

    // The laser stops at the node, or at whatever solid thing is nearer.
    FHitResult Hit;
    FCollisionQueryParams Params(TEXT("LoomaVRLaser"), false, this);
    float Length = BestDistance;
    if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) && Hit.Distance < Length)
    {
        Length = Hit.Distance;
        if (Best && Hit.Distance < BestDistance - 5.f)
        {
            Best = nullptr; // a wall between the hand and the node
        }
    }
    // The cylinder is 100 cm along its Z: lie it along X, centre it half way.
    Laser->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));
    Laser->SetRelativeLocation(FVector(Length * 0.5f, 0.f, 0.f));
    Laser->SetRelativeScale3D(FVector(0.005f, 0.005f, Length / 100.f));

    OutDistance = BestDistance;
    return Best;
}

void ALoomaVRPawn::Select(ALoomaSyncedActor* Node)
{
    ULoomaSceneSyncSubsystem* Sync = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr;
    if (!Sync)
    {
        return;
    }
    if (Node)
    {
        Sync->SelectNode(Node);
    }
    else
    {
        Sync->ClearSelection();
    }
}

void ALoomaVRPawn::BeginCarry(ALoomaSyncedActor* Node, float Distance)
{
    if (!Node)
    {
        return;
    }
    if (ULoomaSceneSyncSubsystem* Sync = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULoomaSceneSyncSubsystem>() : nullptr)
    {
        Sync->SelectNode(Node);
    }
    Carried = {Node};
    Grab.Begin(RightAim->GetComponentLocation(), RightAim->GetComponentQuat(), Distance, {Node->GetActorTransform()});
}

void ALoomaVRPawn::Carry(float DeltaSeconds, const FVector2D& Look)
{
    Grab.Steer(LoomaVR::Axis(Look.Y), LoomaVR::Axis(Look.X), FMath::Min(DeltaSeconds, 0.05f));
    TArray<FVector> Locations;
    TArray<FQuat> Rotations;
    Grab.Poses(RightAim->GetComponentLocation(), RightAim->GetComponentQuat(), bUpright, Locations, Rotations);
    for (int32 i = 0; i < Carried.Num() && i < Locations.Num(); ++i)
    {
        if (ALoomaSyncedActor* Node = Carried[i].Get())
        {
            // Just a local move: LoomaSceneSync's outbound diff streams it to the
            // room while it moves and sends the final pose when it settles.
            Node->SetActorLocationAndRotation(Locations[i], Rotations[i]);
        }
    }
}

void ALoomaVRPawn::EndCarry()
{
    Carried.Reset();
}
