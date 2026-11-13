// Entering and leaving VR in the viewer (VR-9).
//
//   Looma.VR [1|0]   Toggle VR, or set it. Entering puts you on the floor under
//                    the current view, facing the way it faced; leaving puts the
//                    previous pawn where your head was.
//   -LoomaVR         On the command line: enter VR as soon as the first world
//                    begins play.
//
// PC VR through OpenXR: a Quest over Link or Air Link, or any SteamVR headset.
// Run it on a PC that is not the generation box: stereo at 72 to 90 Hz on the
// RTX 5090 competes with TRELLIS.2 and Kimodo for the GPU.
//
// The project renders with Lumen, hardware ray tracing and virtual shadow
// maps, which is right for the desktop and too much for stereo at headset rates.
// Entering VR swaps in a lighter profile and leaving puts every value back (see
// VRProfile below).

#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "LoomaVRPawn.h"
#include "Misc/CommandLine.h"
#include "OpenXRInputFunctionLibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaVRCommands, Log, All);

namespace
{
    // Console values VR replaces, and what they were, so leaving restores them.
    const TCHAR* const VRProfile[][2] = {
        {TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("0")}, // no Lumen GI
        {TEXT("r.ReflectionMethod"), TEXT("2")},                // screen space
        {TEXT("r.Shadow.Virtual.Enable"), TEXT("0")},           // classic shadow maps
        {TEXT("r.RayTracing.Shadows"), TEXT("0")},
        {TEXT("r.ScreenPercentage"), TEXT("90")},
    };
    TMap<FString, FString> GSavedValues;

    void ApplyVRProfile(bool bOn)
    {
        IConsoleManager& Console = IConsoleManager::Get();
        for (const auto& Entry : VRProfile)
        {
            IConsoleVariable* Var = Console.FindConsoleVariable(Entry[0]);
            if (!Var)
            {
                continue;
            }
            if (bOn)
            {
                GSavedValues.Add(Entry[0], Var->GetString());
                Var->Set(Entry[1], ECVF_SetByConsole);
            }
            else if (const FString* Saved = GSavedValues.Find(Entry[0]))
            {
                Var->Set(**Saved, ECVF_SetByConsole);
            }
        }
        if (!bOn)
        {
            GSavedValues.Reset();
        }
    }

    ALoomaVRPawn* CurrentVRPawn(UWorld* World)
    {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        return PC ? Cast<ALoomaVRPawn>(PC->GetPawn()) : nullptr;
    }

    void EnterVR(UWorld* World)
    {
        APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
        if (!PC || CurrentVRPawn(World))
        {
            return;
        }
        FVector ViewLocation;
        FRotator ViewRotation;
        PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

        // The floor under the view; the view itself if there is none.
        FHitResult Hit;
        FCollisionObjectQueryParams Static(ECC_WorldStatic);
        const bool bFloor = World->LineTraceSingleByObjectType(
            Hit, ViewLocation, ViewLocation - FVector(0.f, 0.f, 100000.f), Static);
        const float FloorZ = bFloor ? Hit.ImpactPoint.Z : ViewLocation.Z - 170.f;

        FActorSpawnParameters Spawn;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ALoomaVRPawn* Pawn = World->SpawnActor<ALoomaVRPawn>(
            FVector(ViewLocation.X, ViewLocation.Y, FloorZ), FRotator(0.f, ViewRotation.Yaw, 0.f), Spawn);
        if (!Pawn)
        {
            UE_LOG(LogLoomaVRCommands, Error, TEXT("Could not spawn the VR pawn."));
            return;
        }
        Pawn->PreviousPawn = PC->GetPawn();
        Pawn->SetHandover(ViewLocation, ViewRotation.Yaw, FloorZ);
        PC->Possess(Pawn);

        // OpenXR has to know every binding before the session starts.
        TSet<UInputMappingContext*> Contexts = {Pawn->GetMappingContext()};
        if (!UOpenXRInputFunctionLibrary::BeginXRSession(Contexts))
        {
            UE_LOG(LogLoomaVRCommands, Warning,
                TEXT("No headset session started. Is an OpenXR runtime (Quest Link, SteamVR) running, with the headset connected?"));
        }
        ApplyVRProfile(true);
        UE_LOG(LogLoomaVRCommands, Log, TEXT("Entered VR at %s."), *ViewLocation.ToString());
    }

    void ExitVR(UWorld* World)
    {
        ALoomaVRPawn* Pawn = CurrentVRPawn(World);
        if (!Pawn)
        {
            return;
        }
        APlayerController* PC = World->GetFirstPlayerController();
        const FVector Head = Pawn->Camera->GetComponentLocation();
        const FRotator HeadRotation = Pawn->Camera->GetComponentRotation();

        UOpenXRInputFunctionLibrary::EndXRSession();
        ApplyVRProfile(false);

        // The desktop view picks up where the head was.
        APawn* Previous = Pawn->PreviousPawn.Get();
        if (Previous)
        {
            Previous->SetActorLocationAndRotation(Head, FRotator(0.f, HeadRotation.Yaw, 0.f));
            PC->Possess(Previous);
        }
        else
        {
            PC->UnPossess();
        }
        PC->SetControlRotation(HeadRotation);
        Pawn->Destroy();
        UE_LOG(LogLoomaVRCommands, Log, TEXT("Left VR."));
    }

    FAutoConsoleCommandWithWorldAndArgs GLoomaVRCommand(
        TEXT("Looma.VR"),
        TEXT("Enter or leave VR: Looma.VR toggles, Looma.VR 1 enters, Looma.VR 0 leaves."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
        {
            const bool bIn = CurrentVRPawn(World) != nullptr;
            const bool bWant = Args.Num() > 0 ? Args[0] != TEXT("0") : !bIn;
            if (bWant && !bIn)
            {
                EnterVR(World);
            }
            else if (!bWant && bIn)
            {
                ExitVR(World);
            }
        }));

    // -LoomaVR: enter as soon as a game world begins play, for a kiosk shortcut.
    struct FLoomaVRAutoEnter
    {
        FLoomaVRAutoEnter()
        {
            FWorldDelegates::OnPostWorldInitialization.AddLambda([](UWorld* World, const UWorld::InitializationValues)
            {
                if (!World || !World->IsGameWorld() || !FParse::Param(FCommandLine::Get(), TEXT("LoomaVR")))
                {
                    return;
                }
                TWeakObjectPtr<UWorld> Weak(World);
                World->OnWorldBeginPlay.AddLambda([Weak]()
                {
                    if (UWorld* W = Weak.Get())
                    {
                        EnterVR(W);
                    }
                });
            });
        }
    } GLoomaVRAutoEnter;
}
