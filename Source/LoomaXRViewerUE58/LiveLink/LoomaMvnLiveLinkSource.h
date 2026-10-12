#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/ArrayReader.h"

class FSocket;
class FUdpSocketReceiver;
class ILiveLinkClient;

/**
 * A Live Link source for Xsens's MVN network streaming protocol (HAM-316), as
 * MVN Animate sends it and as `code/xsens-simulation-python` imitates it, on
 * UDP 9763.
 *
 * Xsens's own Live Link plugin reads the same stream, but it is installed for
 * UE 5.6 here and not for 5.8, and it does far more than this needs. This reads
 * one datagram type, `MXTP02`, the pose of the 23 body segments as a position
 * and a quaternion each (Xsens document MV0305P, the same one the simulator's
 * `mvn_protocol.py` packs from):
 *
 *  - a 24-byte big-endian header: "MXTP02", sample counter, datagram counter,
 *    item count, time code, character id, body segment count, ...;
 *  - per segment, 32 bytes: id (1-based), position x y z in centimetres,
 *    quaternion w x y z. Z up, right-handed, and GLOBAL: every segment's pose
 *    is in the world, not relative to its parent.
 *
 * A Live Link animation frame is parent-relative, so each segment is rewritten
 * relative to its parent in Xsens's own hierarchy before it is pushed. The
 * numbers stay Xsens's (right-handed, Z up); what they mean is the `xsens`
 * profile's business (Config/LiveLinkProfiles.json).
 *
 * One subject per character id: "Xsens0", "Xsens1", ...
 */
class FLoomaMvnLiveLinkSource : public ILiveLinkSource
{
public:
    explicit FLoomaMvnLiveLinkSource(int32 InPort);
    virtual ~FLoomaMvnLiveLinkSource() override;

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

    /** Receiver thread only: the characters whose skeleton has been pushed. */
    TSet<uint8> KnownCharacters;
    FThreadSafeCounter FramesReceived;
};
