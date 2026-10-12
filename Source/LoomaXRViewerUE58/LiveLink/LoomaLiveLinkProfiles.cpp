#include "LoomaLiveLinkProfiles.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogLoomaLiveLinkProfiles, Log, All);

namespace
{
    TArray<FLoomaLiveLinkProfile> GProfiles;
    TArray<FString> GRequired;
    bool bLoaded = false;

    bool ReadAxis(const TSharedPtr<FJsonObject>& Axes, const TCHAR* Field, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Axes.IsValid() || !Axes->TryGetArrayField(Field, Values) || Values->Num() != 3)
        {
            return false;
        }
        Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
        return !Out.IsNearlyZero();
    }

    bool ReadAxes(const TSharedPtr<FJsonObject>& Profile, const TCHAR* Field, FVector& Left, FVector& Up, FVector& Forward)
    {
        const TSharedPtr<FJsonObject>* Axes = nullptr;
        return Profile->TryGetObjectField(Field, Axes) && ReadAxis(*Axes, TEXT("left"), Left)
            && ReadAxis(*Axes, TEXT("up"), Up) && ReadAxis(*Axes, TEXT("forward"), Forward);
    }

    void Load()
    {
        bLoaded = true;
        const FString Path = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("LiveLinkProfiles.json"));
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path))
        {
            UE_LOG(LogLoomaLiveLinkProfiles, Error, TEXT("Cannot read %s: no mocap skeleton can drive a character."), *Path);
            return;
        }
        TSharedPtr<FJsonObject> Root;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
        {
            UE_LOG(LogLoomaLiveLinkProfiles, Error, TEXT("%s is not valid JSON."), *Path);
            return;
        }
        Root->TryGetStringArrayField(TEXT("required"), GRequired);
        const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
        if (!Root->TryGetArrayField(TEXT("profiles"), Profiles))
        {
            UE_LOG(LogLoomaLiveLinkProfiles, Error, TEXT("%s has no 'profiles'."), *Path);
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Profiles)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FLoomaLiveLinkProfile Profile;
            const TSharedPtr<FJsonObject>* Bones = nullptr;
            if (!Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Profile.Name)
                || !(*Object)->TryGetObjectField(TEXT("bones"), Bones)
                || !ReadAxes(*Object, TEXT("boneAxes"), Profile.BoneLeft, Profile.BoneUp, Profile.BoneForward)
                || !ReadAxes(*Object, TEXT("worldAxes"), Profile.WorldLeft, Profile.WorldUp, Profile.WorldForward))
            {
                UE_LOG(LogLoomaLiveLinkProfiles, Warning,
                       TEXT("A profile in %s lacks a name, bones, boneAxes or worldAxes; skipped."), *Path);
                continue;
            }
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Bone : (*Bones)->Values)
            {
                FString Source;
                if (Bone.Value.IsValid() && Bone.Value->TryGetString(Source) && !Source.IsEmpty())
                {
                    Profile.Bones.Add(Bone.Key, FName(*Source));
                }
            }
            UE_LOG(LogLoomaLiveLinkProfiles, Log, TEXT("Live Link profile '%s': %d bones."), *Profile.Name, Profile.Bones.Num());
            GProfiles.Add(MoveTemp(Profile));
        }
    }
}

const TArray<FLoomaLiveLinkProfile>& LoomaLiveLinkProfiles::All()
{
    if (!bLoaded)
    {
        Load();
    }
    return GProfiles;
}

const FLoomaLiveLinkProfile* LoomaLiveLinkProfiles::For(const TArray<FName>& SubjectBones)
{
    for (const FLoomaLiveLinkProfile& Profile : All())
    {
        bool bHasAll = GRequired.Num() > 0 || Profile.Bones.Num() > 0;
        for (const FString& Required : GRequired)
        {
            const FName* Source = Profile.Bones.Find(Required);
            if (!Source || !SubjectBones.Contains(*Source))
            {
                bHasAll = false;
                break;
            }
        }
        if (bHasAll)
        {
            return &Profile;
        }
    }
    return nullptr;
}
