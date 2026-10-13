#include "../LoomaLiveLink.h"
#include "LoomaSceneComponents.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Who poses a character on this client (HAM-316, HAM-329): the decision the
// Live Link driver makes twice a second, without a world, a mesh or a socket.
// The case that carries the weight is the hub's: a node the backend's Live
// Link drives is never driven here, even by a subject of the same name.

namespace
{
    FLoomaNodeComponents Parsed(const TCHAR* Json)
    {
        TArray<TSharedPtr<FJsonValue>> Components;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Components);
        return LoomaParseComponents(&Components);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoomaLiveLinkWhoDrivesTest,
    "Looma.LiveLink.WhoDrives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLoomaLiveLinkWhoDrivesTest::RunTest(const FString& Parameters)
{
    using LoomaLiveLink::EDriver;
    using LoomaLiveLink::WhoDrives;
    const TArray<FName> Live = {FName(TEXT("Actor0"))};
    const TArray<FName> None;

    // A client's subject, as before the hub existed.
    const FLoomaNodeComponents Client = Parsed(TEXT(R"([{"type":"model","assetId":"a"},{"type":"livelink","subject":"Actor0"}])"));
    TestTrue(TEXT("a client's subject live here is driven here"), WhoDrives(Client, Live, false) == EDriver::Here);
    TestTrue(TEXT("whatever else is arriving"), WhoDrives(Client, Live, true) == EDriver::Here);
    TestTrue(TEXT("not live here, somebody else's poses are shown"), WhoDrives(Client, None, true) == EDriver::Scene);
    TestTrue(TEXT("and with neither, nobody"), WhoDrives(Client, None, false) == EDriver::Nobody);

    // The hub's, in a named stream.
    const FLoomaNodeComponents Hub = Parsed(
        TEXT(R"([{"type":"model","assetId":"a"},{"type":"livelink","subject":"Actor0","source":"hub","stream":"studio-b"}])"));
    TestTrue(TEXT("the parsed component is the hub's"), Hub.IsLiveLinkFromHub());
    TestTrue(TEXT("a hub node is not driven here even with an Actor0 of our own"), WhoDrives(Hub, Live, false) == EDriver::Nobody);
    TestTrue(TEXT("it shows the hub's poses"), WhoDrives(Hub, Live, true) == EDriver::Scene);
    TestTrue(TEXT("and without our own Actor0 alike"), WhoDrives(Hub, None, true) == EDriver::Scene);

    // The hub's, in whichever stream has an Actor0.
    const FLoomaNodeComponents AnyStream = Parsed(TEXT(R"([{"type":"livelink","subject":"Actor0","source":"hub"}])"));
    TestTrue(TEXT("a hub node with no stream named is still the hub's"), WhoDrives(AnyStream, Live, false) == EDriver::Nobody);

    // Nobody named at all: only somebody else's poses can move it.
    const FLoomaNodeComponents Bare = Parsed(TEXT(R"([{"type":"model","assetId":"a"}])"));
    TestTrue(TEXT("no component and no poses is nobody"), WhoDrives(Bare, Live, false) == EDriver::Nobody);
    TestTrue(TEXT("no component with poses arriving is shown"), WhoDrives(Bare, Live, true) == EDriver::Scene);

    // How the commands and the log name them.
    TestEqual(TEXT("a client's subject by its name"), LoomaLiveLink::DescribeSubject(Client), FString(TEXT("Actor0")));
    TestEqual(TEXT("the hub's with its stream"), LoomaLiveLink::DescribeSubject(Hub), FString(TEXT("Actor0 (hub stream studio-b)")));
    TestEqual(TEXT("the hub's in any stream"), LoomaLiveLink::DescribeSubject(AnyStream), FString(TEXT("Actor0 (hub)")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
