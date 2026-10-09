#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/ArrayReader.h"

class FSocket;
class FUdpSocketReceiver;
class ILiveLinkClient;

/**
 * A Live Link source for the JSON-over-UDP stream the Moverse simulator sends
 * (`code/moverse-simulation-python`, port 54321), HAM-316's local proof.
 *
 * Each datagram is one JSON object, `{"<subject>": [ {"Type": ...}, ... ]}`:
 *
 *  - `CharacterSubject`: the skeleton, one `{"Name", "Parent"}` per bone.
 *  - `CharacterAnimation`: a frame, one `{"Location", "Rotation", "Scale"}` per
 *    bone in the same order, each a bone's transform relative to its parent,
 *    already in Unreal's units and handedness. `Rotation` is x, y, z, w.
 *
 * It is the format of Epic's old JSONLiveLink sample, which is what Moverse's
 * own Unreal plugin reads. That plugin lives in the mocap repository, which is
 * not on this machine, so this is a small reader of the same datagrams.
 *
 * The simulator repeats the skeleton before every frame. Pushing static data
 * resets a Live Link subject, so the skeleton is pushed only when it changes.
 */
class FLoomaJsonLiveLinkSource : public ILiveLinkSource
{
public:
    explicit FLoomaJsonLiveLinkSource(int32 InPort);
    virtual ~FLoomaJsonLiveLinkSource() override;

    virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
    virtual bool IsSourceStillValid() const override;
    virtual bool RequestSourceShutdown() override;
    virtual FText GetSourceType() const override;
    virtual FText GetSourceMachineName() const override;
    virtual FText GetSourceStatus() const override;

    bool IsListening() const { return Socket != nullptr; }
    int32 GetPort() const { return Port; }

private:
    void Start();
    void Stop();
    void HandleDatagram(const TSharedPtr<FArrayReader, ESPMode::ThreadSafe>& Data, const FIPv4Endpoint& Sender);

    int32 Port = 0;
    FSocket* Socket = nullptr;
    FUdpSocketReceiver* Receiver = nullptr;

    ILiveLinkClient* Client = nullptr;
    FGuid SourceGuid;

    /** Receiver thread only: the bone names last pushed for each subject. */
    TMap<FName, TArray<FName>> KnownSkeletons;
    FThreadSafeCounter FramesReceived;
};
