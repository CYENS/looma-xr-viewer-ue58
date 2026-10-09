#include "LoomaJsonLiveLinkSource.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Dom/JsonObject.h"
#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLiveLink, Log, All);

namespace
{
    bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Count, double Out[4])
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object->TryGetArrayField(Field, Values) || Values->Num() != Count)
        {
            return false;
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Out[Index] = (*Values)[Index]->AsNumber();
        }
        return true;
    }
}

FLoomaJsonLiveLinkSource::FLoomaJsonLiveLinkSource(int32 InPort)
    : Port(InPort)
{
    Start();
}

FLoomaJsonLiveLinkSource::~FLoomaJsonLiveLinkSource()
{
    Stop();
}

void FLoomaJsonLiveLinkSource::Start()
{
    // 2 MB: a frame is about 3 kB and the simulator sends three subjects at 120 Hz.
    Socket = FUdpSocketBuilder(TEXT("LoomaJsonLiveLink"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToPort(Port)
        .WithReceiveBufferSize(2 * 1024 * 1024);
    if (!Socket)
    {
        UE_LOG(LogLoomaLiveLink, Error, TEXT("Could not bind UDP port %d; is something else listening on it?"), Port);
        return;
    }
    Receiver = new FUdpSocketReceiver(Socket, FTimespan::FromMilliseconds(100), TEXT("LoomaJsonLiveLinkReceiver"));
    Receiver->OnDataReceived().BindRaw(this, &FLoomaJsonLiveLinkSource::HandleDatagram);
    Receiver->Start();
    UE_LOG(LogLoomaLiveLink, Log, TEXT("Listening for JSON Live Link datagrams on UDP port %d."), Port);
}

void FLoomaJsonLiveLinkSource::Stop()
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

void FLoomaJsonLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
    Client = InClient;
    SourceGuid = InSourceGuid;
}

bool FLoomaJsonLiveLinkSource::IsSourceStillValid() const
{
    return Socket != nullptr;
}

bool FLoomaJsonLiveLinkSource::RequestSourceShutdown()
{
    Stop();
    Client = nullptr;
    return true;
}

FText FLoomaJsonLiveLinkSource::GetSourceType() const
{
    return FText::FromString(TEXT("Looma JSON (Moverse simulator)"));
}

FText FLoomaJsonLiveLinkSource::GetSourceMachineName() const
{
    return FText::FromString(FString::Printf(TEXT("UDP :%d"), Port));
}

FText FLoomaJsonLiveLinkSource::GetSourceStatus() const
{
    if (!Socket)
    {
        return FText::FromString(TEXT("Not listening"));
    }
    return FText::FromString(FString::Printf(TEXT("%d frames"), FramesReceived.GetValue()));
}

void FLoomaJsonLiveLinkSource::HandleDatagram(const TSharedPtr<FArrayReader, ESPMode::ThreadSafe>& Data, const FIPv4Endpoint& /*Sender*/)
{
    ILiveLinkClient* Target = Client;
    if (!Target || !Data.IsValid() || Data->Num() == 0)
    {
        return;
    }
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Data->GetData()), Data->Num());
    const FString Json(Text.Length(), Text.Get());

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Root->Values)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Entry.Value.IsValid() || !Entry.Value->TryGetArray(Items) || Items->Num() < 2)
        {
            continue;
        }
        const TSharedPtr<FJsonObject>* Header = nullptr;
        FString Type;
        if (!(*Items)[0]->TryGetObject(Header) || !(*Header)->TryGetStringField(TEXT("Type"), Type))
        {
            continue;
        }
        const FName Subject(*Entry.Key);
        const FLiveLinkSubjectKey Key(SourceGuid, Subject);

        if (Type == TEXT("CharacterSubject"))
        {
            TArray<FName> Names;
            TArray<int32> Parents;
            for (int32 Index = 1; Index < Items->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject>* Bone = nullptr;
                FString Name;
                int32 Parent = INDEX_NONE;
                if (!(*Items)[Index]->TryGetObject(Bone) || !(*Bone)->TryGetStringField(TEXT("Name"), Name)
                    || !(*Bone)->TryGetNumberField(TEXT("Parent"), Parent))
                {
                    Names.Reset();
                    break;
                }
                Names.Add(FName(*Name));
                Parents.Add(Parent);
            }
            const TArray<FName>* Known = KnownSkeletons.Find(Subject);
            if (Names.Num() == 0 || (Known && *Known == Names))
            {
                continue;
            }
            KnownSkeletons.Add(Subject, Names);
            FLiveLinkStaticDataStruct Static(FLiveLinkSkeletonStaticData::StaticStruct());
            FLiveLinkSkeletonStaticData* Skeleton = Static.Cast<FLiveLinkSkeletonStaticData>();
            Skeleton->SetBoneNames(Names);
            Skeleton->SetBoneParents(Parents);
            Target->PushSubjectStaticData_AnyThread(Key, ULiveLinkAnimationRole::StaticClass(), MoveTemp(Static));
            UE_LOG(LogLoomaLiveLink, Log, TEXT("Live Link subject '%s': %d bones."), *Subject.ToString(), Names.Num());
        }
        else if (Type == TEXT("CharacterAnimation"))
        {
            const TArray<FName>* Known = KnownSkeletons.Find(Subject);
            if (!Known || Known->Num() != Items->Num() - 1)
            {
                continue; // a frame for a skeleton not seen yet
            }
            FLiveLinkFrameDataStruct Frame(FLiveLinkAnimationFrameData::StaticStruct());
            FLiveLinkAnimationFrameData* Animation = Frame.Cast<FLiveLinkAnimationFrameData>();
            Animation->Transforms.Reserve(Items->Num() - 1);
            bool bComplete = true;
            for (int32 Index = 1; Index < Items->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject>* Bone = nullptr;
                double L[4] = {0, 0, 0, 0};
                double Q[4] = {0, 0, 0, 1};
                double S[4] = {1, 1, 1, 0};
                if (!(*Items)[Index]->TryGetObject(Bone) || !ReadVector(*Bone, TEXT("Location"), 3, L)
                    || !ReadVector(*Bone, TEXT("Rotation"), 4, Q))
                {
                    bComplete = false;
                    break;
                }
                ReadVector(*Bone, TEXT("Scale"), 3, S);
                FQuat Rotation(Q[0], Q[1], Q[2], Q[3]);
                Rotation.Normalize();
                Animation->Transforms.Emplace(Rotation, FVector(L[0], L[1], L[2]), FVector(S[0], S[1], S[2]));
            }
            if (!bComplete)
            {
                continue;
            }
            Animation->WorldTime = FPlatformTime::Seconds();
            Target->PushSubjectFrameData_AnyThread(Key, MoveTemp(Frame));
            FramesReceived.Increment();
        }
    }
}
