#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LoomaLiveLinkDriver.generated.h"

class USkeletalMeshComponent;

/**
 * Drives the characters the scene says to drive (HAM-316).
 *
 * A character's node carries a `livelink` component naming a Live Link subject
 * (docs/rig-format.md, "Who drives a character", in looma-xr-asset-demo). It is
 * set from any client, the web's inspector above all. This is the client that
 * has Live Link, so twice a second it looks at every character the scene sync
 * spawned and:
 *
 *  - gives a character whose subject is live a ULoomaLivePoseAnimInstance, which
 *    poses it here and publishes the pose to the other clients;
 *  - takes it back off one whose component went, or whose subject did, and
 *    restarts the clip the node names.
 *
 * It also starts listening for the Moverse simulator, and tells the other
 * clients every two seconds which subjects are live, which is what their
 * pickers offer.
 *
 * It releases only characters it attached itself, so Looma.LiveLink.Drive from
 * the console still works on a character with no component.
 *
 * Game worlds only: an editor world's characters are not ticking a scene sync.
 */
UCLASS()
class ULoomaLiveLinkDriver : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

private:
    void Refresh();

    float SinceRefresh = 0.0f;
    float SinceAnnounce = 0.0f;
    bool bTriedListening = false;
    TSet<TWeakObjectPtr<USkeletalMeshComponent>> Driven;
};
