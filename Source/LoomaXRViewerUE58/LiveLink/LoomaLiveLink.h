#pragma once

#include "CoreMinimal.h"

class ILiveLinkClient;
class USkeletalMeshComponent;
struct FLoomaNodeComponents;

/** What the Live Link commands and the Live Link driver share (HAM-316). */
namespace LoomaLiveLink
{
    /** Who poses a character on this client. */
    enum class EDriver : uint8
    {
        /** Nobody: it plays the clip its node names, or stands at rest. */
        Nobody,
        /** This client, from a Live Link subject of its own, sending the pose on to the others. */
        Here,
        /** The poses the scene sends for it: another client's, or the backend hub's. */
        Scene,
    };

    /**
     * Who should pose a character whose node carries `Components` (its `livelink`
     * component, docs/rig-format.md in looma-xr-asset-demo), given the Live Link
     * subjects this client has and whether poses for the node are arriving.
     *
     * A node the backend's hub drives ("source": "hub", HAM-329) is never driven
     * here, even when this client has a subject of that name: that is another
     * performer, on another machine or another stream, and the hub would drop
     * whatever this client sent for it. It shows the hub's poses instead.
     */
    EDriver WhoDrives(const FLoomaNodeComponents& Components, const TArray<FName>& LocalSubjects, bool bPosesArriving);

    /** "Actor0", "Actor0 (hub)", or "Actor0 (hub stream studio-b)": how the commands and the log name a node's subject. */
    FString DescribeSubject(const FLoomaNodeComponents& Components);

    /** The port the Moverse simulator sends to. */
    const int32 DefaultPort = 54321;
    /** MVN's own streaming port, which Xsens's Live Link plugin would otherwise hold. */
    const int32 DefaultMvnPort = 9763;

    /** Start reading both simulators' datagrams, each unless something here already is. */
    bool EnsureListening(int32 Port = DefaultPort);

    /** The engine's Live Link client, or null when the plugin is off. */
    ILiveLinkClient* Client();

    /** Whether a component shows a LoomaMixamo-A/1 character. */
    bool IsRig(const USkeletalMeshComponent* Component);
}
