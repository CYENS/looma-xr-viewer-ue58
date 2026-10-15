#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "LoomaVRMath.h"
#include "LoomaVRPawn.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class UInstancedStaticMeshComponent;
class UMotionControllerComponent;
class UStaticMeshComponent;
class ALoomaSyncedActor;
struct FEnhancedInputActionValueBinding;

// The headset's body in the viewer (VR-9): what `Looma.VR` possesses.
//
// The pawn is the floor point the headset's room is anchored to. The camera is
// locked to the headset and the two motion controllers to the hands; the pawn
// itself only moves when the person asks to:
//
// | Left stick        | walk where you look (flying: along the view)            |
// | Left stick click  | fly on / off; off puts you back on the floor            |
// | Right stick L / R | snap turn, 45 degrees                                   |
// | Right stick up    | aim the teleport arc; let go to jump to it (walking)    |
// | Right stick U / D | rise and sink (flying)                                   |
// | Right trigger     | select what the laser points at; empty floor clears      |
// | Right grip        | carry it (stick: push, pull and turn while carrying)     |
//
// The same controls as the web client's VR mode, so a person moves and edits the
// same way in either. Nothing here talks to the hub: a carried actor is simply
// moved, and LoomaSceneSync's outbound diff sends the move as it would any other
// local move; selecting goes through the subsystem's SelectNode.
//
// The input is built here in code, not authored as assets: the actions, and a
// mapping context onto the Touch controllers' keys that `Looma.VR` hands to
// OpenXR when it starts the session (OpenXR has to know every binding before a
// session begins).
UCLASS()
class ALoomaVRPawn : public APawn
{
    GENERATED_BODY()

public:
    ALoomaVRPawn();

    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual void PossessedBy(AController* NewController) override;

    // The mapping context `Looma.VR` passes to OpenXR when the session begins.
    UInputMappingContext* GetMappingContext() const { return MappingContext; }

    // Where the desktop view was when VR was entered: the first tracked frame
    // puts the head here (see LoomaVR::PlaceForView).
    void SetHandover(const FVector& ViewLocation, float ViewYaw, float FloorZ);

    // The pawn that was possessed before, to hand control back to on exit.
    TWeakObjectPtr<APawn> PreviousPawn;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<USceneComponent> Origin;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UCameraComponent> Camera;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UMotionControllerComponent> LeftGrip;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UMotionControllerComponent> RightGrip;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UMotionControllerComponent> RightAim;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UStaticMeshComponent> Laser;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UInstancedStaticMeshComponent> Arc;

    UPROPERTY(VisibleAnywhere, Category = "Looma|VR")
    TObjectPtr<UStaticMeshComponent> Landing;

    UPROPERTY(EditAnywhere, Category = "Looma|VR")
    float SnapDegrees = 45.f;

    UPROPERTY(EditAnywhere, Category = "Looma|VR")
    float FlySpeed = 600.f;

    UPROPERTY(EditAnywhere, Category = "Looma|VR")
    bool bUpright = true;

protected:
    virtual void BeginPlay() override;

private:
    void BuildInput();
    void Handover();
    void Locomote(float DeltaSeconds, const FVector2D& Move, const FVector2D& Look);
    void AimTeleport(const FVector2D& Look);
    // The node the right hand's ray points at, and how far; null for none.
    ALoomaSyncedActor* UpdateLaser(float& OutDistance);
    void Select(ALoomaSyncedActor* Node);
    void BeginCarry(ALoomaSyncedActor* Node, float Distance);
    void Carry(float DeltaSeconds, const FVector2D& Look);
    void EndCarry();
    FVector FloorUnder(const FVector& Point, bool& bFound) const;

    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> MappingContext;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> MoveAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> LookXAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> LookYAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> FlyAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> SelectAction;
    UPROPERTY(Transient)
    TObjectPtr<UInputAction> GrabAction;

    FEnhancedInputActionValueBinding* MoveValue = nullptr;
    FEnhancedInputActionValueBinding* LookXValue = nullptr;
    FEnhancedInputActionValueBinding* LookYValue = nullptr;
    FEnhancedInputActionValueBinding* FlyValue = nullptr;
    FEnhancedInputActionValueBinding* SelectValue = nullptr;
    FEnhancedInputActionValueBinding* GrabValue = nullptr;

    bool bFly = false;
    bool bFlyHeld = false;
    bool bSelectHeld = false;
    bool bGrabHeld = false;
    bool bSnapArmed = true;
    bool bAiming = false;
    bool bHaveTeleportTarget = false;
    FVector TeleportTarget = FVector::ZeroVector;

    bool bPendingHandover = false;
    FVector HandoverLocation = FVector::ZeroVector;
    float HandoverYaw = 0.f;
    float HandoverFloorZ = 0.f;

    // What the right hand is carrying, while it is.
    TArray<TWeakObjectPtr<ALoomaSyncedActor>> Carried;
    LoomaVR::FRayGrab Grab;
};
