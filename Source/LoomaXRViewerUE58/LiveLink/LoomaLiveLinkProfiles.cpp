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
            if (!Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Profile.Name))
            {
                UE_LOG(LogLoomaLiveLinkProfiles, Warning, TEXT("A profile in %s has no name; skipped."), *Path);
                continue;
            }
            // `like` first, so whatever this profile gives itself replaces it. The
            // other profile has to come earlier in the file.
            FString Like;
            if ((*Object)->TryGetStringField(TEXT("like"), Like))
            {
                const FLoomaLiveLinkProfile* Base = GProfiles.FindByPredicate(
                    [&Like](const FLoomaLiveLinkProfile& Other) { return Other.Name == Like; });
                if (!Base)
                {
                    UE_LOG(LogLoomaLiveLinkProfiles, Warning,
                           TEXT("Profile '%s' in %s is like '%s', which is not above it; skipped."), *Profile.Name, *Path, *Like);
                    continue;
                }
                const FString Name = Profile.Name;
                Profile = *Base;
                Profile.Name = Name;
                Profile.Source.Reset();
                Profile.RootRotation = FQuat::Identity;
            }
            const TSharedPtr<FJsonObject>* Bones = nullptr;
            if ((*Object)->TryGetObjectField(TEXT("bones"), Bones))
            {
                Profile.Bones.Reset();
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Bone : (*Bones)->Values)
                {
                    FString Source;
                    if (Bone.Value.IsValid() && Bone.Value->TryGetString(Source) && !Source.IsEmpty())
                    {
                        Profile.Bones.Add(Bone.Key, FName(*Source));
                    }
                }
            }
            const bool bBoneAxes = ReadAxes(*Object, TEXT("boneAxes"), Profile.BoneLeft, Profile.BoneUp, Profile.BoneForward);
            const bool bWorldAxes = ReadAxes(*Object, TEXT("worldAxes"), Profile.WorldLeft, Profile.WorldUp, Profile.WorldForward);
            if (Like.IsEmpty() && (!Bones || !bBoneAxes || !bWorldAxes))
            {
                UE_LOG(LogLoomaLiveLinkProfiles, Warning,
                       TEXT("Profile '%s' in %s lacks bones, boneAxes or worldAxes; skipped."), *Profile.Name, *Path);
                continue;
            }
            (*Object)->TryGetStringField(TEXT("source"), Profile.Source);
            const TArray<TSharedPtr<FJsonValue>>* Turn = nullptr;
            if ((*Object)->TryGetArrayField(TEXT("rootRotation"), Turn) && Turn->Num() == 4)
            {
                const FQuat Q((*Turn)[0]->AsNumber(), (*Turn)[1]->AsNumber(), (*Turn)[2]->AsNumber(), (*Turn)[3]->AsNumber());
                if (Q.SizeSquared() > KINDA_SMALL_NUMBER)
                {
                    Profile.RootRotation = Q.GetNormalized();
                }
            }
            UE_LOG(LogLoomaLiveLinkProfiles, Log, TEXT("Live Link profile '%s': %d bones%s."), *Profile.Name, Profile.Bones.Num(),
                   Profile.Source.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", for '%s' sources"), *Profile.Source));
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

const FLoomaLiveLinkProfile* LoomaLiveLinkProfiles::For(const TArray<FName>& SubjectBones, const FString& SourceType)
{
    const FLoomaLiveLinkProfile* Any = nullptr;
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
        if (!bHasAll)
        {
            continue;
        }
        if (Profile.Source.IsEmpty())
        {
            Any = Any ? Any : &Profile;
        }
        else if (SourceType.Contains(Profile.Source, ESearchCase::IgnoreCase))
        {
            return &Profile;
        }
    }
    return Any;
}
