#pragma once

#include "CoreMinimal.h"

class ILiveLinkClient;
class USkeletalMeshComponent;

/** What the Live Link commands and the Live Link driver share (HAM-316). */
namespace LoomaLiveLink
{
    /** The port the Moverse simulator sends to. */
    const int32 DefaultPort = 54321;

    /** Start reading the simulator's datagrams unless something here already is. */
    bool EnsureListening(int32 Port = DefaultPort);

    /** The engine's Live Link client, or null when the plugin is off. */
    ILiveLinkClient* Client();

    /** Whether a component shows a LoomaMixamo-A/1 character. */
    bool IsRig(const USkeletalMeshComponent* Component);
}
