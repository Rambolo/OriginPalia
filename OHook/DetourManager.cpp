﻿#include "DetourManager.h"

#include <algorithm>

#include "PaliaOverlay.h"
#include <SDK/Palia_parameters.hpp>

#include "SDKExt.h"
#include "Utils.h"
#include "format"
#include "functional"
#include "Configuration.h"

#include <sstream>
#include <fstream>

using namespace SDK;

// Cache

void ClearActorCache(PaliaOverlay* Overlay) {
    const UWorld* World = GetWorld();

    const auto GameplayStatics = static_cast<UGameplayStatics*>(UGameplayStatics::StaticClass()->DefaultObject);
    if (!GameplayStatics)
        return;

    // Clear cache on level change
    if (Overlay->CurrentLevel != World->PersistentLevel) {
        Overlay->CachedActors.clear();
        Overlay->CurrentLevel = World->PersistentLevel;
        Overlay->CurrentMap = GameplayStatics->GetCurrentLevelName(World, false).ToString();
    }
}

void ManageActorCache(PaliaOverlay* Overlay) {
    const UWorld* World = GetWorld();

    const auto GameplayStatics = static_cast<UGameplayStatics*>(UGameplayStatics::StaticClass()->DefaultObject);
    if (!GameplayStatics)
        return;

    if (const double WorldTime = GameplayStatics->GetTimeSeconds(World); abs(WorldTime - Overlay->LastCachedTime) > 0.1) {
        Overlay->LastCachedTime = WorldTime;
        Overlay->ProcessActors(Overlay->ActorStep);

        Overlay->ActorStep++;
        if (Overlay->ActorStep >= static_cast<int>(EType::MAX)) {
            Overlay->ActorStep = 0;
        }
    }
}

// [Fun]

inline void Func_DoTeleportToTargeted(PaliaOverlay* Overlay) {
    if (!Configuration::bTeleportToTargeted)
        return;

    bool isKeyUp = IsKeyUp(VK_XBUTTON2);
    bool isScoreValid = std::abs(Overlay->BestScore - FLT_MAX) > 0.0001f;

    // Avoid teleporting if the Hotkey / Score is not valid
    if (!isKeyUp || !isScoreValid)
        return;

    // Avoid teleporting to players
    if (Configuration::bAvoidTeleportingToPlayers && Overlay->BestTargetActorType == EType::Players)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    // Avoid teleporting to targeted if there are nearby players
    if (Configuration::bDoRadiusPlayersAvoidance) {
        for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, Distance] : Overlay->CachedActors) {
            if (!Actor || ActorType != EType::Players)
                continue;

            if (!IsActorValid(Actor) || !IsActorValid(Overlay->BestTargetActor) || WorldPosition.IsZero())
                continue;

            // Don't count itself or us
            if (Actor == Overlay->BestTargetActor || Actor == ValeriaCharacter)
                continue;

            // Check for actors within X meters of this actor
            if (WorldPosition.GetDistanceToInMeters(Overlay->BestTargetLocation) < Configuration::AvoidanceRadius) {
                return;
            }
        }
    }

    // Teleportation logic
    FVector TargetLocation = Overlay->BestTargetLocation;

    // Apply horizontal offset for animal targets
    if (Overlay->BestTargetActorType == EType::Animal || Overlay->BestTargetActorType == EType::Tree) {
        FVector RightVector = ValeriaCharacter->GetActorRightVector();
        TargetLocation += RightVector * 160.0f;
    }

    TeleportPlayer(TargetLocation);
    // FHitResult HitResult;
    // ValeriaCharacter->K2_SetActorLocation(TargetLocation, false, &HitResult, true);
}

inline void Func_DoTeleportToWaypoint(const Params::TrackingComponent_RpcClient_SetUserMarkerViaWorldMap* SetUserMarkerViaWorldMap) {
    if (!Configuration::bEnableWaypointTeleport)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    FVector TargetLocation = SetUserMarkerViaWorldMap->MarkerLocation;
    if (!TargetLocation.IsZero()) {
        TeleportPlayer(TargetLocation);
    }
}

std::chrono::steady_clock::time_point lastAFKPing = std::chrono::steady_clock::now();
inline void Func_DoAntiAfk() {
    if (!Configuration::bEnableAntiAfk)
        return;

    const auto ValeriaController = GetValeriaController();
    if (!ValeriaController)
        return;

    auto currentTime = std::chrono::steady_clock::now();
    auto elapsedTime = std::chrono::duration_cast<std::chrono::minutes>(currentTime - lastAFKPing);

    if (elapsedTime.count() >= 3) {
        ValeriaController->Client_InactivityHeartbeat();
        ValeriaController->RpcServer_NotifyInactivityInterrupted();

        // Update the last execution time
        lastAFKPing = currentTime;
    }
}

// [HUD]

inline void DrawCircle(UCanvas* Canvas, const float Radius, const int32 NumSegments, const FLinearColor Color, const float Thickness = 1.0f) {
    // Calculate screen center more accurately
    const FVector2D ScreenCenter = { static_cast<double>(Canvas->ClipX) / 2.0, static_cast<double>(Canvas->ClipY) / 2.0 };

    const double Increment = 360.0 / static_cast<double>(NumSegments);
    FVector2D LastPos = { ScreenCenter.X + Radius, ScreenCenter.Y };

    for (int i = 1; i <= NumSegments; i++) {
        const float Rad = CustomMath::DegreesToRadians(static_cast<float>(Increment * i));
        FVector2D NewPos = { ScreenCenter.X + Radius * cos(Rad), ScreenCenter.Y + Radius * sin(Rad) };
        Canvas->K2_DrawLine(LastPos, NewPos, Thickness, Color);
        LastPos = NewPos;
    }
}

inline void Func_DoInteliAim(PaliaOverlay* Overlay) {
    if (!Configuration::bDrawFOVCircle)
        return;

    UWorld* World = GetWorld();
    if (!World)
        return;

    const auto PlayerController = GetPlayerController();
    if (!PlayerController)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    FVector CharacterLocation = ValeriaCharacter->K2_GetActorLocation();
    FRotator CharacterRotation = PlayerController->GetControlRotation();
    FVector ForwardVector = UKismetMathLibrary::GetForwardVector(CharacterRotation);
    double BestScore = FLT_MAX;

    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, Distance] : Overlay->CachedActors) {
        if (!Actor || !IsActorValid(Actor) || WorldPosition.IsZero())
            continue;

        bool bShouldConsider = false;

        switch (ActorType) {
        case EType::Animal:
            bShouldConsider = Overlay->Animals[Type][Variant]; // Toggle for different types of animals
            break;
        case EType::Ore:
            bShouldConsider = Overlay->Ores[Type][Variant]; // Toggle for different types of ores
            break;
        case EType::Bug:
            bShouldConsider = Overlay->Bugs[Type][Variant][Quality]; // Toggle for different types of bugs
            break;
        case EType::Forage:
            bShouldConsider = Overlay->Forageables[Type][Quality]; // Toggle for forageable items
            break;
        case EType::Players:
            if (!Configuration::bAvoidTeleportingToPlayers) {
                bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Player)]; // Toggle for player visibility
            }
            break;
        case EType::NPCs:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::NPC)]; // Toggle for NPCs
            break;
        case EType::Quest:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Quest)]; // Toggle for quest items
            break;
        case EType::Loot:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Loot)]; // Toggle for loot
            break;
        case EType::RummagePiles:
            if (Overlay->Singles[static_cast<int>(EOneOffs::RummagePiles)]) {
                auto Pile = static_cast<ATimedLootPile*>(Actor);
                if (!Pile || !IsActorValid(Pile))
                    break;

                if (Configuration::bEnableOthers || (Pile->CanGather(ValeriaCharacter) && Pile->bActivated)) {
                    bShouldConsider = true;
                }
            }
            break;
        case EType::Stables:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Stables)]; // Toggle for Stables
            break;
        case EType::Tree:
            bShouldConsider = Overlay->Trees[Type][Variant]; // Toggle for trees
            break;
        case EType::Fish:
            bShouldConsider = Overlay->Fish[Type]; // Toggle for fish types
            break;
        default:
            break;
        }

        if (!bShouldConsider)
            continue;

        FVector ActorLocation = Actor->K2_GetActorLocation();
        FVector DirectionToActor = (ActorLocation - CharacterLocation).GetNormalized();
        FVector TargetVelocity = Actor->GetVelocity();

        FVector RelativeVelocity = TargetVelocity - ValeriaCharacter->GetVelocity();
        FVector RelativeDirection = RelativeVelocity.GetNormalized();

        Distance = CharacterLocation.GetDistanceToInMeters(ActorLocation);
        float Angle = CustomMath::RadiansToDegrees(acosf(static_cast<float>(ForwardVector.Dot(DirectionToActor))));

        if (ActorLocation.IsZero())
            continue;

        // if (!(ActorType == EType::Animal || ActorType == EType::Bug) && Distance < 2.0)
        //     continue;

        if (Distance < 2.0)
            continue;

        if (Configuration::bEnableESPCulling && Distance > Configuration::CullDistance)
            continue;

        // Weighting factors for different factors
        double AngleWeight, DistanceWeight, MovementWeight;

        // Adjust weighting factors based on EType
        switch (ActorType) {
        case EType::Animal:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        case EType::Ore:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        case EType::Bug:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        default:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        }

        // Calculate score based on weighted sum of factors
        if (double Score = AngleWeight * Angle + DistanceWeight * Distance + MovementWeight * RelativeDirection.Magnitude(); Angle <= Configuration::FOVRadius / 2.0 && Score < Overlay->SelectionThreshold) {
            if (Score < BestScore) {
                BestScore = Score;
                Overlay->BestScore = Score;
                Overlay->BestTargetActor = Actor;
                Overlay->BestTargetActorType = ActorType;
                Overlay->BestTargetLocation = ActorLocation;
                Overlay->BestTargetRotation = UKismetMathLibrary::FindLookAtRotation(CharacterLocation, ActorLocation);
            }
        }
    }
}

inline void Func_DoESP(PaliaOverlay* Overlay, const AHUD* HUD) {
    if (!Configuration::bEnableESP) {
        Overlay->CachedActors.clear();
        return;
    }

    // Manage Cache Logic
    ClearActorCache(Overlay);
    ManageActorCache(Overlay);

    AValeriaCharacter* ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    APlayerController* PlayerController = GetPlayerController();
    if (!PlayerController)
        return;

    auto PawnPlayer = PlayerController->K2_GetPawn();
    if (!PawnPlayer || !IsActorValid(PawnPlayer))
        return;

    FVector PawnLocation = PlayerController->K2_GetPawn()->K2_GetActorLocation();

    // Calculate distance - actor
    for (auto& Actor : Overlay->CachedActors) {
        if (!Actor.Actor || !IsActorValid(Actor.Actor))
            continue;

        FVector ActorLocation = Actor.WorldPosition.IsZero() ? Actor.Actor->K2_GetActorLocation() : Actor.WorldPosition;
        Actor.Distance = PawnLocation.GetDistanceTo(ActorLocation);
    }

    // Sort actors based on distance - prioritizing BestTargetActor
    std::ranges::sort(Overlay->CachedActors, [&](const auto& a, const auto& b) {
        if (a.Actor == Overlay->BestTargetActor)
            return false;
        if (b.Actor == Overlay->BestTargetActor)
            return true;

        return a.Distance > b.Distance;
        });

    // Draw ESP Names Entities
    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, Distance] : Overlay->CachedActors) {
        if (!Actor || !IsActorValid(Actor) || WorldPosition.IsZero())
            continue;

        if (ActorType == EType::Players) {
            if (Actor == ValeriaCharacter)
                continue;
        }

        FVector ActorLocation = WorldPosition;
        if (ActorLocation = Actor->K2_GetActorLocation(); ActorLocation.IsZero())
            continue;

        // Adjust Z coordinate for head-level display
        float HeightAdjustment = 100.0f; // Adjust this value based on typical actor height
        ActorLocation.Z += HeightAdjustment;

        Distance = PawnLocation.GetDistanceToInMeters(ActorLocation);

        if (Distance < 2.0)
            continue;
        if (Configuration::bEnableESPCulling && Distance > Configuration::CullDistance)
            continue;

        FVector2D ScreenLocation;
        if (PlayerController->ProjectWorldLocationToScreen(ActorLocation, &ScreenLocation, true)) {
            ImU32 Color = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
            bool bShouldDraw = false;

            switch (ActorType) {
            case EType::Forage:
                if (Overlay->Forageables[Type][Quality]) {
                    bShouldDraw = true;
                    Color = Overlay->ForageableColors[Type];
                }
                break;
            case EType::Ore:
                if (Overlay->Ores[Type][Variant]) {
                    auto Ore = static_cast<ABP_ValeriaGatherableLoot_C*>(Actor);
                    if (Ore && IsActorValid(Ore)) {
                        if (Ore->IAmAlive) {
                            bShouldDraw = true;
                            Color = Overlay->OreColors[Type];
                        }
                    }
                }
                break;
            case EType::Players:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Player)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Player)];
                }
                break;
            case EType::Animal:
                if (Overlay->Animals[Type][Variant]) {
                    bShouldDraw = true;
                    Color = Overlay->AnimalColors[Type][Variant];
                }
                break;
            case EType::Tree:
                if (Overlay->Trees[Type][Variant]) {
                    bShouldDraw = true;
                    Color = Overlay->TreeColors[Type];
                }
                break;
            case EType::Bug:
                if (Overlay->Bugs[Type][Variant][Quality]) {
                    bShouldDraw = true;
                    Color = Overlay->BugColors[Type][Variant];
                }
                break;
            case EType::NPCs:
                if (Overlay->Singles[static_cast<int>(EOneOffs::NPC)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::NPC)];
                }
                break;
            case EType::Loot:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Loot)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Loot)];
                }
                break;
            case EType::Quest:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Quest)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Quest)];
                }
                break;
            case EType::RummagePiles:
                if (Overlay->Singles[static_cast<int>(EOneOffs::RummagePiles)]) {
                    auto Pile = static_cast<ATimedLootPile*>(Actor);
                    if (!Pile || !IsActorValid(Pile)) {
                        bShouldDraw = false;
                        break;
                    }

                    if (Configuration::bEnableOthers) {
                        bShouldDraw = true;
                        Color = Pile->bActivated ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xFF, 0x00, 0x00, 0xFF);
                    }
                    else if (Pile->CanGather(ValeriaCharacter) && Pile->bActivated) {
                        bShouldDraw = true;
                        Color = Overlay->SingleColors[static_cast<int>(EOneOffs::RummagePiles)];
                    }
                }
                break;
            case EType::Stables:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Stables)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Stables)];
                }
                break;
            case EType::Fish:
                if (Overlay->Fish[Type]) {
                    bShouldDraw = true;
                    Color = Overlay->FishColors[Type];
                }
                break;
            default:
                break;
            }

            if (Configuration::bEnableOthers && Type == 0)
                bShouldDraw = true;

            if (!bShouldDraw)
                continue;

            if (!Roboto) {
                Roboto = reinterpret_cast<UFont*>(UObject::FindObject("Font Roboto.Roboto", EClassCastFlags::None));

                if (!Roboto)
                    continue;
            }

            // Construct text string
            std::string qualityName = (Quality > 0) ? PaliaOverlay::GetQualityName(Quality, ActorType) : "";

            // Prepare text with optional parts depending on the index values
            std::string text = DisplayName;
            if (!qualityName.empty()) {
                text += " [" + qualityName + "]";
            }
            text += std::format(" [{:.2f}m]", Distance);

            if (Configuration::bEnableDespawnTimer) {
                double seconds = 0;

                if (Actor) {
                    if (ActorType == EType::Ore) {
                        auto GatherableLoot = static_cast<ABP_ValeriaGatherableLoot_Mining_MultiHarvest_C*>(Actor);
                        if (GatherableLoot && IsActorValid(GatherableLoot)) {
                            GatherableLoot->GetSecondsUntilDespawn(&seconds);
                        }
                    }
                    else if (ActorType == EType::Forage) {
                        auto ForageableLoot = static_cast<ABP_ValeriaGatherable_C*>(Actor);
                        if (ForageableLoot && IsActorValid(ForageableLoot)) {
                            if (ForageableLoot->Gatherable) {
                                seconds = ForageableLoot->Gatherable->GetSecondsUntilDespawn();
                            }
                        }
                    }
                    if (seconds > 0) {
                        text += " (" + std::to_string(static_cast<int>(seconds)) + "s)";
                    }
                }
            }

            std::wstring wideText(text.begin(), text.end());

            double BaseScale = 1.0; // Default scale at a reference distance
            double ReferenceDistance = 100.0; // Distance at which no scaling is applied
            double ScalingFactor = 0; // Determines how much the scale changes with distance

            double DistanceScale;
            DistanceScale = BaseScale - ScalingFactor * (Distance - ReferenceDistance);
            DistanceScale = CustomMath::Clamp(DistanceScale, 0.5, BaseScale); // Clamp the scale to a reasonable range

            const FVector2D TextScale = { DistanceScale * Configuration::ESPTextScale, DistanceScale * Configuration::ESPTextScale };
            ImColor IMC(Color);
            FLinearColor TextColor = { IMC.Value.x, IMC.Value.y, IMC.Value.z, IMC.Value.w };

            // Setup shadow properties
            ImColor IMCS(Color);
            FLinearColor ShadowColor = { IMCS.Value.x, IMCS.Value.y, IMCS.Value.z, IMCS.Value.w };

            // Calculate positions
            FVector2D TextPosition = ScreenLocation;
            FVector2D ShadowPosition = { TextPosition.X + 1.0, TextPosition.Y + 1.0 };

            // Draw shadow text
            HUD->Canvas->K2_DrawText(Roboto, FString(wideText.data()), ShadowPosition, TextScale, TextColor, 0, { 0, 0, 0, 1 }, { 1.0f, 1.0f }, true, true, true, { 0, 0, 0, 1 });
            // Draw main text
            HUD->Canvas->K2_DrawText(Roboto, FString(wideText.data()), TextPosition, TextScale, ShadowColor, 0, { 0, 0, 0, 1 }, { 1.0f, 1.0f }, true, true, true, { 0, 0, 0, 1 });
        }
    }

    // Logic for FOV and Targeting Drawing
    if (Configuration::bDrawFOVCircle) {
        FVector2D PlayerScreenPosition;
        FVector2D TargetScreenPosition;

        if (PlayerController->ProjectWorldLocationToScreen(PawnLocation, &PlayerScreenPosition, true)) {
            // Calculate the center of the FOV circle based on the player's screen position
            FVector2D FOVCenter = { HUD->Canvas->ClipX * 0.5f, HUD->Canvas->ClipY * 0.5f };
            DrawCircle(HUD->Canvas, Configuration::FOVRadius, 1200, { 0.485f, 0.485f, 0.485f, 0.485f }, 1.0f);

            if (Overlay->BestTargetLocation.IsZero())
                return;
            if (!PlayerController->ProjectWorldLocationToScreen(Overlay->BestTargetLocation, &TargetScreenPosition, true))
                return;
            if (!(CustomMath::DistanceBetweenPoints(TargetScreenPosition, FOVCenter) <= Configuration::FOVRadius))
                return;

            HUD->Canvas->K2_DrawLine(FOVCenter, TargetScreenPosition, 0.5f, { 0.485f, 0.485f, 0.485f, 0.485f });
        }
    }
}

// [Movement]

inline void Func_DoNoClip(PaliaOverlay* Overlay) {
    if (!Overlay->bEnableNoclip && Overlay->bEnableNoclip == Overlay->bPreviousNoclipState)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UValeriaCharacterMoveComponent* ValeriaMovementComponent = ValeriaCharacter->GetValeriaCharacterMovementComponent();
    if (!ValeriaMovementComponent || !ValeriaMovementComponent->IsValidLowLevel() || ValeriaMovementComponent->IsDefaultObject())
        return;

    if (Overlay->bEnableNoclip != Overlay->bPreviousNoclipState) {
        if (Overlay->bEnableNoclip) {
            ValeriaMovementComponent->SetMovementMode(EMovementMode::MOVE_Flying, 5);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
        }
        else {
            ValeriaMovementComponent->SetMovementMode(EMovementMode::MOVE_Walking, 1);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Block);
        }

        Overlay->bPreviousNoclipState = Overlay->bEnableNoclip;
    }

    // Logic for Noclip Camera
    if (Overlay->bEnableNoclip) {
        if (Overlay->ShowOverlay() || !IsGameWindowActive())
            return;

        const auto PlayerController = GetPlayerController();
        if (!PlayerController)
            return;

        // Calculate forward and right vectors based on the camera's yaw
        const FRotator& CameraRot = PlayerController->PlayerCameraManager->GetCameraRotation();

        FVector CameraForward = UKismetMathLibrary::GetForwardVector(CameraRot);
        FVector CameraRight = UKismetMathLibrary::GetRightVector(CameraRot);
        constexpr FVector CameraUp = { 0.f, 0.f, 1.f };

        CameraForward.Normalize();
        CameraRight.Normalize();

        FVector MovementDirection = { 0.f, 0.f, 0.f };
        float FlySpeed = 800.0f;

        if (IsKeyHeld('W')) {
            MovementDirection += CameraForward * FlySpeed;
        }
        if (IsKeyHeld('S')) {
            MovementDirection -= CameraForward * FlySpeed;
        }
        if (IsKeyHeld('D')) {
            MovementDirection += CameraRight * FlySpeed;
        }
        if (IsKeyHeld('A')) {
            MovementDirection -= CameraRight * FlySpeed;
        }
        if (IsKeyHeld(VK_SPACE)) {
            MovementDirection += CameraUp * FlySpeed;
        }
        if (IsKeyHeld(VK_CONTROL)) {
            MovementDirection -= CameraUp * FlySpeed;
        }
        if (IsKeyHeld(VK_SHIFT)) {
            FlySpeed *= 2.0f;
        }

        // Normalize the total movement direction
        MovementDirection.Normalize();
        MovementDirection *= FlySpeed;

        // Time delta
        constexpr float DeltaTime = 1.0f / 60.0f; // Assuming 60 FPS

        const FVector MovementDelta = MovementDirection * DeltaTime;

        // Update character position
        FHitResult HitResult;
        ValeriaCharacter->K2_SetActorLocation(ValeriaCharacter->K2_GetActorLocation() + MovementDelta, false, &HitResult, false);
    }
}

inline void Func_DoPersistentMovement(const PaliaOverlay* Overlay) {
    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UValeriaCharacterMoveComponent* ValeriaMovementComponent = ValeriaCharacter->GetValeriaCharacterMovementComponent();
    if (!ValeriaMovementComponent || !ValeriaMovementComponent->IsValidLowLevel() || ValeriaMovementComponent->IsDefaultObject())
        return;

    ValeriaMovementComponent->MaxWalkSpeed = Configuration::CustomWalkSpeed;
    ValeriaMovementComponent->SprintSpeedMultiplier = Configuration::CustomSprintSpeedMultiplier;
    ValeriaMovementComponent->ClimbingSpeed = Configuration::CustomClimbingSpeed;
    ValeriaMovementComponent->GlidingMaxSpeed = Configuration::CustomGlidingSpeed;
    ValeriaMovementComponent->GlidingFallSpeed = Configuration::CustomGlidingFallSpeed;
    ValeriaMovementComponent->JumpZVelocity = Configuration::CustomJumpVelocity;
    ValeriaMovementComponent->MaxStepHeight = Configuration::CustomMaxStepHeight;
}

// [Placement]

inline void Func_DoPlaceAnywhere(const PaliaOverlay* Overlay) {
    if (!Configuration::bPlaceAnywhere)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UPlacementComponent* PlacementComponent = ValeriaCharacter->GetPlacement();
    if (!PlacementComponent || !PlacementComponent->IsValidLowLevel() || PlacementComponent->IsDefaultObject())
        return;

    PlacementComponent->CanPlaceHere = true;
    PlacementComponent->MaxPlacementUpAngle = Configuration::fMaxUpAngle;
}

// [Fishing]

inline void ToggleFishingDelays(const bool RemoveDelays) {
    const auto ValeriaController = GetValeriaController();
    if (!ValeriaController) {
        return;
    }

    UValeriaGameInstance* ValeriaGameInstance = ValeriaController->GameInst;
    if (!ValeriaGameInstance || !ValeriaGameInstance->IsValidLowLevel() || ValeriaGameInstance->IsDefaultObject()) {
        return;
    }

    auto& CastSettings = ValeriaGameInstance->Configs.Globals.Fishing->CastSettings;

    // Avoid continuously setting values if already set properly
    float newCastDelay = RemoveDelays ? 0.0f : 0.150f;
    if (std::abs(CastSettings.CastDelay - newCastDelay) < 0.0001f) {
        return;
    }

    auto& FishingSettings = ValeriaGameInstance->Configs.Globals.Fishing;
    auto& EndSettings = ValeriaGameInstance->Configs.Globals.Fishing->EndSettings;

    CastSettings.CastDelay = newCastDelay;
    CastSettings.MaxDistanceToCast = 1500.0f;
    CastSettings.MinDistanceToCast = RemoveDelays ? 1500.0f : 500.0f;
    CastSettings.LaunchOffset = RemoveDelays ? FVector{ 1500, 0, -300 } : FVector{};
    CastSettings.WindupSpeed = RemoveDelays ? FLT_MAX : 0.350f;

    FishingSettings->AfterFinishDestroyBobberWhenAtDistanceToRod = RemoveDelays ? FLT_MAX : 50.0;
    FishingSettings->FishingFinishReelInSpeed = RemoveDelays ? FLT_MAX : 1600.0f;
    FishingSettings->TotalCelebrationDuration = RemoveDelays ? 0.0f : 4.0f;
    FishingSettings->OnBeginReelingInitialCooldown = RemoveDelays ? 0.0f : 1.0f;

    EndSettings.MaxTimeOfEndFishingCelebrate = RemoveDelays ? 0.0f : 4.5f;
    EndSettings.MaxTimeOfEndFishingDefault = RemoveDelays ? 0.0f : 2.0f;
    EndSettings.MaxTimeOfEndFishingEmptyHanded = RemoveDelays ? 0.0f : 2.2f;
    EndSettings.MaxTimeOfEndFishingFailure = RemoveDelays ? 0.0f : 1.75f;
}

inline void Func_DoFastAutoFishing(const PaliaOverlay* Overlay) {
    // Toggle values (Safe to leave looped)
    ToggleFishingDelays(Overlay->bEnableAutoFishing);

    if (!Overlay->bEnableAutoFishing) {
        return;
    }

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter || !ValeriaCharacter->GetEquippedItem().ItemType->IsFishingRod()) {
        return;
    }

    if (Configuration::bRequireClickFishing ? !Overlay->ShowOverlay() && IsGameWindowActive() && IsKeyHeld(VK_LBUTTON) : true) {
        // Instant Catch
        auto FishingComponent = ValeriaCharacter->GetFishing();
        if (FishingComponent) {
            if (static_cast<EFishingState_NEW>(FishingComponent->GetFishingState()) == EFishingState_NEW::Bite) {
                FFishingEndContext Context;
                FishingComponent->RpcServer_EndFishing(Context);
                FishingComponent->SetFishingState(EFishingState_OLD::None);
            }
        }

        // Cast the rod
        ValeriaCharacter->ToolPrimaryActionPressed();
        ValeriaCharacter->ToolPrimaryActionReleased();
    }
}

inline void Func_DoInstantCatch() {
    if (!Configuration::bFishingInstantCatch)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UFishingComponent* FishingComponent = ValeriaCharacter->GetFishing();
    if (!FishingComponent)
        return;

    if (static_cast<EFishingState_NEW>(FishingComponent->GetFishingState()) == EFishingState_NEW::Bite) {
        FFishingEndContext Context;
        FishingComponent->RpcServer_EndFishing(Context);
        FishingComponent->SetFishingState(EFishingState_OLD::None);
    }
}

int fishingFlushCounter = 0;

inline void Func_DoFishingCleanup() {
    const auto ValeriaController = GetValeriaController();
    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaController || !ValeriaCharacter) {
        return;
    }

    // Avoid doing extra work
    if (!Configuration::bFishingSell && !Configuration::bFishingDiscard && !Configuration::bFishingOpenStoreWaterlogged) {
        return;
    }

    UVillagerStoreComponent* StoreComponent = ValeriaCharacter->StoreComponent;
    const UInventoryComponent* InventoryComponent = ValeriaCharacter->GetInventory();
    if (!InventoryComponent) {
        return;
    }

    // Sell / Discard / Storage
    for (int BagIndex = 0; BagIndex < InventoryComponent->Bags.Num(); BagIndex++) {
        for (int SlotIndex = 0; SlotIndex < 8; SlotIndex++) {
            FBagSlotLocation Slot{ BagIndex, SlotIndex };
            FValeriaItem Item = InventoryComponent->GetItemAt(Slot);

            if (Configuration::bFishingSell && Item.ItemType->Category == EItemCategory::Fish && StoreComponent) {
                if (!StoreComponent->StoreCanBuyItem(Slot)) {
                    StoreComponent->Client_SetVillagerStore(2);
                    StoreComponent->Client_OpenStore();
                }

                StoreComponent->RpcServer_SellItem(Slot, 10);
            }
            else if (Configuration::bFishingDiscard && Item.ItemType->Category == EItemCategory::Junk) {
                // Don't ever discard more than the amount of the stack
                ValeriaController->DiscardItem(Slot, Item.Amount);
            }
            else if (Item.ItemType->PersistId == 2810) { // Waterlogged Chest
                if (!Configuration::bFishingOpenStoreWaterlogged) {
                    // Don't ever discard more than the amount of the stack
                    ValeriaController->DiscardItem(Slot, Item.Amount);
                }
                else {
                    ValeriaController->ConsumeItem(Slot);
                }
            }
            else if (Configuration::bFishingOpenStoreWaterlogged && Item.ItemType->Name.ToString().find("DA_ItemType_Decor_Makeshift_") != std::string::npos) {
                ValeriaController->MoveItemSlotToStorage(Slot, 1, EStoragePoolType::Primary);
            }
        }
    }

    fishingFlushCounter++;
    if (fishingFlushCounter >= 30) {
        if (APlayerController* PlayerController = GetPlayerController()) {
            PlayerController->ClientFlushLevelStreaming();
            PlayerController->ClientForceGarbageCollection();

            fishingFlushCounter = 0;
        }
    }
}

inline void Func_DoFishingCaptureOverride(PaliaOverlay* Overlay, Params::FishingComponent_RpcServer_SelectLoot* SelectLoot) {
    if (Overlay->bCaptureFishingSpot) {
        memcpy(&Overlay->sOverrideFishingSpot, &SelectLoot->RPCLootParams.WaterType_Deprecated, sizeof(FName));
        Overlay->bCaptureFishingSpot = false;
    }
    if (Overlay->bOverrideFishingSpot) {
        memcpy(&SelectLoot->RPCLootParams.WaterType_Deprecated, &Overlay->sOverrideFishingSpot, sizeof(FName));
    }
}

Params::FishingComponent_RpcServer_EndFishing* EndFishingDetoured(const PaliaOverlay* Overlay, Params::FishingComponent_RpcServer_EndFishing* EndFishing) {
    if (Configuration::bFishingInstantCatch || Overlay->bEnableAutoFishing) {
        EndFishing->Context.Result = EFishingMiniGameResult::Success;
    }

    if (Configuration::bFishingNoDurability) {
        EndFishing->Context.DurabilityReduction = 0;
    }

    EndFishing->Context.Perfect = Configuration::bFishingPerfectCatch ? true : Configuration::bFishingInstantCatch ? false : EndFishing->Context.Perfect;
    EndFishing->Context.SourceWaterBody = nullptr;
    EndFishing->Context.bUsedMultiplayerHelp = Configuration::bFishingMultiplayerHelp;
    EndFishing->Context.StartRodHealth = 100.0f;
    EndFishing->Context.EndRodHealth = 100.0f;
    EndFishing->Context.StartFishHealth = 100.0f;
    EndFishing->Context.EndFishHealth = 100.0f;
    return EndFishing;
}

// [Firing]

inline void Func_DoSilentAim(const PaliaOverlay* Overlay, void* Params) {
    if (!Configuration::bEnableSilentAimbot || !Overlay->BestTargetActor)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    auto FiringComponent = ValeriaCharacter->GetFiringComponent();
    if (!FiringComponent || !FiringComponent->IsValidLowLevel() || FiringComponent->IsDefaultObject())
        return;

    auto FireProjectile = static_cast<Params::ProjectileFiringComponent_RpcServer_FireProjectile*>(Params);

    // Initial Target Check
    if (!Overlay->BestTargetActor || !IsActorValid(Overlay->BestTargetActor))
        return;

    AActor* TargetActor = Overlay->BestTargetActor;
    FVector TargetLocation = TargetActor->K2_GetActorLocation();
    FVector HitLocation = TargetLocation;

    for (auto& [ProjectileId, Pad_22C8, ProjectileActor, HasHit, Pad_22C9] : FiringComponent->FiredProjectiles) {
        // Projectile Check
        if (!ProjectileActor || !IsActorValid(ProjectileActor))
            continue;

        if (ProjectileId != FireProjectile->ProjectileId)
            continue;


        FVector ProjectileLocation = ProjectileActor->K2_GetActorLocation();

        // Extra check ~ just in case
        if (!IsActorValid(TargetActor))
            continue;

        FVector FiringTargetLocation = TargetActor->K2_GetActorLocation();
        FVector DirectionToTarget = (FiringTargetLocation - ProjectileLocation).GetNormalized();
        float DistanceBeforeTarget = 50.0f;
        FVector NewProjectileLocation = FiringTargetLocation - (DirectionToTarget * DistanceBeforeTarget);
        HasHit = true;
        FHitResult HitResult;
        ProjectileActor->K2_SetActorLocation(NewProjectileLocation, false, &HitResult, false);
        HitResult.Location = { NewProjectileLocation };
        FiringComponent->RpcServer_NotifyProjectileHit(FireProjectile->ProjectileId, Overlay->BestTargetActor, HitLocation);
    }
}

inline void Func_DoLegacyAim(const PaliaOverlay* Overlay) {
    if (!Configuration::bEnableAimbot || !Overlay->BestTargetActor || Overlay->ShowOverlay())
        return;

    bool isKeyHeld = IsKeyHeld(VK_LBUTTON);
    bool isScoreValid = std::abs(Overlay->BestScore - FLT_MAX) > 0.0001f;

    // Avoid teleporting if the Hotkey / Score is not valid
    if (!isKeyHeld || !isScoreValid)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    // Only aimbot when a bow is equipped
    if (ValeriaCharacter->GetEquippedItem().ItemType->Name.ToString().find("Tool_Bow_") == std::string::npos)
        return;

    bool IsAnimal = false;
    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, Distance] : Overlay->CachedActors) {
        if (ActorType != EType::Animal || !IsActorValid(Actor))
            continue;

        if (Actor == Overlay->BestTargetActor) {
            IsAnimal = true;
            break;
        }
    }

    if (IsAnimal) {
        auto World = GetWorld();
        if (!World)
            return;

        const auto PlayerController = GetPlayerController();
        if (!PlayerController)
            return;

        FVector CharacterLocation = ValeriaCharacter->K2_GetActorLocation();
        FRotator CharacterRotation = PlayerController->GetControlRotation();

        // Apply offset to pitch and yaw directly
        FRotator TargetRotation = UKismetMathLibrary::FindLookAtRotation(CharacterLocation, Overlay->BestTargetLocation);
        TargetRotation.Pitch += Overlay->AimOffset.X;
        TargetRotation.Yaw += Overlay->AimOffset.Y;

        // Smooth rotation adjustment
        FRotator NewRotation = CustomMath::RInterpTo(CharacterRotation, TargetRotation, UGameplayStatics::GetTimeSeconds(World), Overlay->SmoothingFactor);
        PlayerController->SetControlRotation(NewRotation);
    }
}

// [Minigames]

inline void Func_DoCompleteMinigame() {
    if (!Configuration::bEnableMinigameSkip)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    const auto MinigameComponent = ValeriaCharacter->MinigameQTE;
    if (!MinigameComponent || !MinigameComponent->IsValidLowLevel() || MinigameComponent->IsDefaultObject())
        return;

    if (MinigameComponent->IsPlaying()) {
        MinigameComponent->RpcServer_ChangeState(EMinigameState::Success);
    }
}

// Detouring

void DetourManager::ProcessEventDetour(const UObject* Class, const UFunction* Function, void* Params) {
    const auto Overlay = dynamic_cast<PaliaOverlay*>(OverlayBase::Instance);
    const auto fn = Function->GetFullName();
    invocations.insert(fn);

    // PlayerTick
    if (fn == "Function Engine.Actor.ReceiveTick") {
        Func_DoFastAutoFishing(Overlay);
        Func_DoPersistentMovement(Overlay);
        Func_DoNoClip(Overlay);
        Func_DoAntiAfk();
    }
    // HUD
    else if (fn == "Function Engine.HUD.ReceiveDrawHUD") {
        Func_DoESP(Overlay, reinterpret_cast<const AHUD*>(Class));
        Func_DoInteliAim(Overlay);
        Func_DoLegacyAim(Overlay);
        Func_DoTeleportToTargeted(Overlay);
        Func_DoPlaceAnywhere(Overlay);
        Func_DoCompleteMinigame();
    }
    // Fishing Capture/Override
    else if (fn == "Function Palia.FishingComponent.RpcServer_SelectLoot") {
        Func_DoFishingCaptureOverride(Overlay, static_cast<Params::FishingComponent_RpcServer_SelectLoot*>(Params));
    }
    // Fishing Instant Catch
    else if (fn == "Function Palia.FishingComponent.RpcClient_StartFishingAt_Deprecated") {
        Func_DoInstantCatch();
    }
    // Fishing End (Perfect/Durability/PlayerHelp)
    else if (fn == "Function Palia.FishingComponent.RpcServer_EndFishing") {
        EndFishingDetoured(Overlay, static_cast<Params::FishingComponent_RpcServer_EndFishing*>(Params));
    }
    // Fishing Cleanup (Sell/Discard/Move)
    else if (fn == "Function Palia.FishingComponent.RpcClient_FishCaught") {
        Func_DoFishingCleanup();
    }
    // Teleport To Waypoint
    else if (fn == "Function Palia.TrackingComponent.RpcClient_SetUserMarkerViaWorldMap") {
        Func_DoTeleportToWaypoint(static_cast<Params::TrackingComponent_RpcClient_SetUserMarkerViaWorldMap*>(Params));
    }
    // Silent Aim
    else if (fn == "Function Palia.ProjectileFiringComponent.RpcServer_FireProjectile") {
        Func_DoSilentAim(Overlay, Params);
    }
    // ??
    else if (fn == "Function Palia.ValeriaClientPriMovementComponent.RpcServer_SendMovement") {
        static_cast<Params::ValeriaClientPriMovementComponent_RpcServer_SendMovement*>(Params)->MoveInfo.TargetVelocity = { 0, 0, 0 };
    }

    if (OriginalProcessEvent) {
        OriginalProcessEvent(Class, Function, Params);
    }
}

// Essentials

void DetourManager::SetupDetour(void* Instance, void (*DetourFunc)(const UObject*, const UFunction*, void*)) {
    const void** Vtable = *static_cast<const void***>(Instance);

    DWORD OldProtection;
    VirtualProtect(Vtable, sizeof(DWORD) * 1024, PAGE_EXECUTE_READWRITE, &OldProtection);

    const int32_t Idx = Offsets::ProcessEventIdx;
    OriginalProcessEvent = reinterpret_cast<void(*)(const UObject*, const UFunction*, void*)>(reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + Offsets::ProcessEvent);
    Vtable[Idx] = DetourFunc;

    HookedClient = Instance;
    VirtualProtect(Vtable, sizeof(DWORD) * 1024, OldProtection, &OldProtection);
}

void DetourManager::SetupDetour(void* Instance) {
    SetupDetour(Instance, &DetourManager::ProcessEventDetour);
}

void DetourManager::ProcessEventDetourCallback(const UObject* Class, const UFunction* Function, void* Params, const DetourManager* manager) {
    manager->ProcessEventDetour(Class, Function, Params);
}