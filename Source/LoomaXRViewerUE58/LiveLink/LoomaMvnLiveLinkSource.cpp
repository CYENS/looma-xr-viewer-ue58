#include "LoomaMvnLiveLinkSource.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaMvn, Log, All);

namespace
{
    // MVN's 23 body segments, in the protocol's order (segment id = index + 1),
    // and each one's parent in that list.
    struct FSegment
    {
        const TCHAR* Name;
        int32 Parent;
    };
    const FSegment Segments[] = {
        {TEXT("Pelvis"), -1},
        {TEXT("L5"), 0},
        {TEXT("L3"), 1},
        {TEXT("T12"), 2},
        {TEXT("T8"), 3},
        {TEXT("Neck"), 4},
        {TEXT("Head"), 5},
        {TEXT("RightShoulder"), 4},
        {TEXT("RightUpperArm"), 7},
        {TEXT("RightForeArm"), 8},
        {TEXT("RightHand"), 9},
        {TEXT("LeftShoulder"), 4},
        {TEXT("LeftUpperArm"), 11},
        {TEXT("LeftForeArm"), 12},
        {TEXT("LeftHand"), 13},
        {TEXT("RightUpperLeg"), 0},
        {TEXT("RightLowerLeg"), 15},
        {TEXT("RightFoot"), 16},
        {TEXT("RightToe"), 17},
        {TEXT("LeftUpperLeg"), 0},
        {TEXT("LeftLowerLeg"), 19},
        {TEXT("LeftFoot"), 20},
        {TEXT("LeftToe"), 21},
    };
    const int32 SegmentCount = UE_ARRAY_COUNT(Segments);

    const int32 HeaderBytes = 24;
    const int32 SegmentBytes = 32;

    uint32 ReadU32(const uint8* At)
    {
        return (uint32(At[0]) << 24) | (uint32(At[1]) << 16) | (uint32(At[2]) << 8) | uint32(At[3]);
    }

    float ReadFloat(const uint8* At)
    {
        const uint32 Bits = ReadU32(At);
        float Value;
        FMemory::Memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }
}

FLoomaMvnLiveLinkSource::FLoomaMvnLiveLinkSource(int32 InPort)
    : Port(InPort)
{
    Start();
}

FLoomaMvnLiveLinkSource::~FLoomaMvnLiveLinkSource()
{
    Stop();
}

void FLoomaMvnLiveLinkSource::Start()
{
    Socket = FUdpSocketBuilder(TEXT("LoomaMvnLiveLink"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToPort(Port)
        .WithReceiveBufferSize(2 * 1024 * 1024);
    if (!Socket)
    {
        UE_LOG(LogLoomaMvn, Error, TEXT("Could not bind UDP port %d; is something else listening on it?"), Port);
        return;
    }
    Receiver = new FUdpSocketReceiver(Socket, FTimespan::FromMilliseconds(100), TEXT("LoomaMvnLiveLinkReceiver"));
    Receiver->OnDataReceived().BindRaw(this, &FLoomaMvnLiveLinkSource::HandleDatagram);
    Receiver->Start();
    UE_LOG(LogLoomaMvn, Log, TEXT("Listening for Xsens MVN datagrams on UDP port %d."), Port);
}

void FLoomaMvnLiveLinkSource::Stop()
{
    if (Receiver)
    {
        Receiver->Stop();
        delete Receiver;
        Receiver = nullptr;
    }
    if (Socket)
    {
        Socket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        Socket = nullptr;
    }
}

void FLoomaMvnLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    Client = InClient;
    SourceGuid = InSourceGuid;
}

bool FLoomaMvnLiveLinkSource::IsSourceStillValid() const
{
    return Socket != nullptr;
}

bool FLoomaMvnLiveLinkSource::RequestSourceShutdown()
{
    Stop();
    Client = nullptr;
    return true;
}

FText FLoomaMvnLiveLinkSource::GetSourceType() const
{
    return FText::FromString(TEXT("Looma MVN (Xsens)"));
}

FText FLoomaMvnLiveLinkSource::GetSourceMachineName() const
{
    return FText::FromString(FString::Printf(TEXT("UDP :%d"), Port));
}

FText FLoomaMvnLiveLinkSource::GetSourceStatus() const
{
    if (!Socket)
    {
        return FText::FromString(TEXT("Not listening"));
    }
    return FText::FromString(FString::Printf(TEXT("%d frames"), FramesReceived.GetValue()));
}

void FLoomaMvnLiveLinkSource::HandleDatagram(const TSharedPtr<FArrayReader, ESPMode::ThreadSafe>& Data, const FIPv4Endpoint& /*Sender*/)
{
    ILiveLinkClient* Target = Client;
    if (!Target || !Data.IsValid() || Data->Num() < HeaderBytes)
    {
        return;
    }
    const uint8* Bytes = Data->GetData();
    // Only the pose-quaternion datagram. Metadata (12) and scale (13) are not needed.
    if (FMemory::Memcmp(Bytes, "MXTP02", 6) != 0)
    {
        return;
    }
    const int32 Items = Bytes[11];
    const uint8 Character = Bytes[16];
    if (Items < SegmentCount || Data->Num() < HeaderBytes + SegmentCount * SegmentBytes)
    {
        return;
    }

    // The global pose of each body segment, by its id. Props and fingers follow
    // the 23 and are ignored.
    FTransform Global[UE_ARRAY_COUNT(Segments)];
    bool bSeen[UE_ARRAY_COUNT(Segments)] = {};
    const int32 Available = FMath::Min(Items, (Data->Num() - HeaderBytes) / SegmentBytes);
    for (int32 Item = 0; Item < Available; ++Item)
    {
        const uint8* At = Bytes + HeaderBytes + Item * SegmentBytes;
        const int32 Index = int32(ReadU32(At)) - 1;
        if (Index < 0 || Index >= SegmentCount)
        {
            continue;
        }
        const FVector Position(ReadFloat(At + 4), ReadFloat(At + 8), ReadFloat(At + 12));
        // On the wire: w, x, y, z.
        FQuat Rotation(ReadFloat(At + 20), ReadFloat(At + 24), ReadFloat(At + 28), ReadFloat(At + 16));
        if (Rotation.SizeSquared() < KINDA_SMALL_NUMBER)
        {
            return;
        }
        Rotation.Normalize();
        Global[Index] = FTransform(Rotation, Position);
        bSeen[Index] = true;
    }
    for (int32 Index = 0; Index < SegmentCount; ++Index)
    {
        if (!bSeen[Index])
        {
            return; // a body segment short: not a frame to pose from
        }
    }

    const FName Subject(*FString::Printf(TEXT("Xsens%d"), int32(Character)));
    const FLiveLinkSubjectKey Key(SourceGuid, Subject);
    if (!KnownCharacters.Contains(Character))
    {
        KnownCharacters.Add(Character);
        TArray<FName> Names;
        TArray<int32> Parents;
        for (const FSegment& Segment : Segments)
        {
            Names.Add(FName(Segment.Name));
            Parents.Add(Segment.Parent);
        }
        FLiveLinkStaticDataStruct Static(FLiveLinkSkeletonStaticData::StaticStruct());
        FLiveLinkSkeletonStaticData* Skeleton = Static.Cast<FLiveLinkSkeletonStaticData>();
        Skeleton->SetBoneNames(Names);
        Skeleton->SetBoneParents(Parents);
        Target->PushSubjectStaticData_AnyThread(Key, ULiveLinkAnimationRole::StaticClass(), MoveTemp(Static));
        UE_LOG(LogLoomaMvn, Log, TEXT("Live Link subject '%s': %d segments."), *Subject.ToString(), SegmentCount);
    }

    FLiveLinkFrameDataStruct Frame(FLiveLinkAnimationFrameData::StaticStruct());
    FLiveLinkAnimationFrameData* Animation = Frame.Cast<FLiveLinkAnimationFrameData>();
    Animation->Transforms.Reserve(SegmentCount);
    for (int32 Index = 0; Index < SegmentCount; ++Index)
    {
        const int32 Parent = Segments[Index].Parent;
        if (Parent < 0)
        {
            Animation->Transforms.Add(Global[Index]);
            continue;
        }
        // Relative to the parent: local * parent == global, as FTransform composes.
        const FQuat ParentInverse = Global[Parent].GetRotation().Inverse();
        const FQuat Rotation = ParentInverse * Global[Index].GetRotation();
        const FVector Position = ParentInverse.RotateVector(Global[Index].GetLocation() - Global[Parent].GetLocation());
        Animation->Transforms.Emplace(Rotation, Position);
    }
    Animation->WorldTime = FPlatformTime::Seconds();
    Target->PushSubjectFrameData_AnyThread(Key, MoveTemp(Frame));
    FramesReceived.Increment();
}
