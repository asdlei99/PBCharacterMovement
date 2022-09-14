// Copyright 2017-2019 Project Borealis

#include "Character/PBPlayerCharacter.h"

#include "Components/CapsuleComponent.h"
#include "HAL/IConsoleManager.h"

#include "Character/PBPlayerMovement.h"

static TAutoConsoleVariable<int32> CVarAutoBHop(TEXT("move.Pogo"), 1, TEXT("If holding spacebar should make the player jump whenever possible.\n"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarBunnyhop(TEXT("move.Bunnyhopping"), 0, TEXT("Enable normal bunnyhopping.\n"), ECVF_Default);

// Sets default values
APBPlayerCharacter::APBPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UPBPlayerMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(30.48f, 68.58f);
	// Set collision settings. We are the invisible player with no 3rd person mesh.
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	GetCapsuleComponent()->bReturnMaterialOnMove = true;

	// set our turn rates for input
	BaseTurnRate = 45.0f;
	BaseLookUpRate = 45.0f;

	// Camera eye level
	DefaultBaseEyeHeight = 53.34f;
	BaseEyeHeight = DefaultBaseEyeHeight;
	const float CrouchedHalfHeight = 68.58f / 2.0f;
	CrouchedEyeHeight = 53.34f - CrouchedHalfHeight;

	// Fall Damage Initializations
	// PLAYER_MAX_SAFE_FALL_SPEED
	MinSpeedForFallDamage = 1002.9825f;
	// PLAYER_MIN_BOUNCE_SPEED
	MinLandBounceSpeed = 329.565f;

	// get pointer to movement component
	MovementPtr = Cast<UPBPlayerMovement>(ACharacter::GetMovementComponent());

	CapDamageMomentumZ = 476.25f;
}

void APBPlayerCharacter::BeginPlay()
{
	// Call the base class
	Super::BeginPlay();
	// Max jump time to get to the top of the arc
	MaxJumpTime = -4.0f * GetCharacterMovement()->JumpZVelocity / (3.0f * GetCharacterMovement()->GetGravityZ());
}

void APBPlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);


	if (bDeferJumpStop)
	{
		bDeferJumpStop = false;
		Super::StopJumping();
	}
}

void APBCharacter::ApplyDamageMomentum(float DamageTaken, FDamageEvent const& DamageEvent, APawn* PawnInstigator, AActor* DamageCauser)
{
	UDamageType const* const DmgTypeCDO = DamageEvent.DamageTypeClass->GetDefaultObject<UDamageType>();
	if (GetCharacterMovement())
	{
		FVector ImpulseDir;

		if (IsValid(DamageCauser))
		{
			ImpulseDir = (GetActorLocation() - (DamageCauser->GetActorLocation() + FVector(0.0f, 0.0f, HU_TO_UU(-10.0f)))).GetSafeNormal();
		}
		else
		{
			ImpulseDir = DamageEvent.GetImpulse(DamageTaken, this, PawnInstigator).GetSafeNormal();
		}

		const float SizeFactor = (FMath::Square(HU_TO_UU(32.0f)) * HU_TO_UU(72.0f)) / (FMath::Square(GetCapsuleComponent()->GetScaledCapsuleRadius() * 2.0f) * GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0f);

		float Magnitude = HU_TO_UU(DamageTaken) * SizeFactor * 5.0f;
		Magnitude = FMath::Min(Magnitude, HU_TO_UU(1000.0f));

		FVector Impulse = ImpulseDir * Magnitude;
		bool const bMassIndependentImpulse = !DmgTypeCDO->bScaleMomentumByMass;
		float MassScale = 1.f;
		if (!bMassIndependentImpulse && GetCharacterMovement()->Mass > SMALL_NUMBER)
		{
			MassScale = 1.f / GetCharacterMovement()->Mass;
		}
		if (CapDamageMomentumZ > 0.f)
		{
			Impulse.Z = FMath::Min(Impulse.Z * MassScale, CapDamageMomentumZ) / MassScale;
		}

		GetCharacterMovement()->AddImpulse(Impulse, bMassIndependentImpulse);
	}
}

void APBPlayerCharacter::ClearJumpInput(float DeltaTime)
{
	// Don't clear jump input right away if we're auto hopping or noclipping (holding to go up), or if we are deferring a jump stop
	if (CVarAutoBHop.GetValueOnGameThread() != 0 || bAutoBunnyhop || GetCharacterMovement()->bCheatFlying || bDeferJumpStop)
	{
		return;
	}
	Super::ClearJumpInput(DeltaTime);
}

void APBPlayerCharacter::Jump()
{
	if (GetCharacterMovement()->IsFalling())
	{
		bDeferJumpStop = true;
	}

	Super::Jump();
}

void APBPlayerCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PrevCustomMode)
{
	if (!bPressedJump)
	{
		ResetJumpState();
	}

	if (GetCharacterMovement()->IsFalling())
	{
		// Record jump force start time for proxies. Allows us to expire the jump even if not continually ticking down a timer.
		if (bProxyIsJumpForceApplied)
		{
			ProxyJumpForceStartedTime = GetWorld()->GetTimeSeconds();
		}
	}
	else
	{
		JumpCurrentCount = 0;
		JumpKeyHoldTime = 0.0f;
		JumpForceTimeRemaining = 0.0f;
		bWasJumping = false;
	}

	K2_OnMovementModeChanged(PrevMovementMode, GetCharacterMovement()->MovementMode, PrevCustomMode, GetCharacterMovement()->CustomMovementMode);
	MovementModeChangedDelegate.Broadcast(this, PrevMovementMode, PrevCustomMode);
}

void APBPlayerCharacter::StopJumping()
{
	if (!bDeferJumpStop)
	{
		Super::StopJumping();
	}
}

void APBPlayerCharacter::OnJumped_Implementation()
{
	if (MovementPtr->IsOnLadder())
	{
		return;
	}

	if (GetWorld()->GetTimeSeconds() >= LastJumpBoostTime + MaxJumpTime)
	{
		LastJumpBoostTime = GetWorld()->GetTimeSeconds();
		// Boost forward speed on jump
		FVector Facing = GetActorForwardVector();
		FVector Input = MovementPtr->GetLastInputVector().GetClampedToMaxSize2D(1.0f) * MovementPtr->GetMaxAcceleration();
		float ForwardSpeed = Input | Facing;
		// Adjust how much the boost is
		float SpeedBoostPerc = bIsSprinting || bIsCrouched ? 0.1f : 0.5f;
		// How much we are boosting by
		float SpeedAddition = FMath::Abs(ForwardSpeed * SpeedBoostPerc);
		// We can only boost up to this much
		float MaxBoostedSpeed = GetCharacterMovement()->GetMaxSpeed() + GetCharacterMovement()->GetMaxSpeed() * SpeedBoostPerc;
		// Calculate new speed
		float NewSpeed = SpeedAddition + GetMovementComponent()->Velocity.Size2D();
		float SpeedAdditionNoClamp = SpeedAddition;

		// Scale the boost down if we are going over
		if (NewSpeed > MaxBoostedSpeed)
		{
			SpeedAddition -= NewSpeed - MaxBoostedSpeed;
		}

		if (ForwardSpeed < -MovementPtr->GetMaxAcceleration() * FMath::Sin(0.6981f))
		{
			// Boost backwards if we're going backwards
			SpeedAddition *= -1.0f;
			SpeedAdditionNoClamp *= -1.0f;
		}

		// Boost our velocity
		FVector JumpBoostedVel = GetMovementComponent()->Velocity + Facing * SpeedAddition;
		float JumpBoostedSizeSq = JumpBoostedVel.SizeSquared2D();
		if (CVarBunnyhop.GetValueOnGameThread() != 0)
		{
			FVector JumpBoostedUnclampVel = GetMovementComponent()->Velocity + Facing * SpeedAdditionNoClamp;
			float JumpBoostedUnclampSizeSq = JumpBoostedUnclampVel.SizeSquared2D();
			if (JumpBoostedUnclampSizeSq > JumpBoostedSizeSq)
			{
				JumpBoostedVel = JumpBoostedUnclampVel;
				JumpBoostedSizeSq = JumpBoostedUnclampSizeSq;
			}
		}
		if (GetMovementComponent()->Velocity.SizeSquared2D() < JumpBoostedSizeSq)
		{
			GetMovementComponent()->Velocity = JumpBoostedVel;
		}
	}
}

void APBPlayerCharacter::ToggleNoClip()
{
	MovementPtr->ToggleNoClip();
}

bool APBPlayerCharacter::CanJumpInternal_Implementation() const
{
	// UE-COPY: ACharacter::CanJumpInternal_Implementation()

	bool bCanJump = GetCharacterMovement() && GetCharacterMovement()->IsJumpAllowed();

	if (bCanJump)
	{
		// Ensure JumpHoldTime and JumpCount are valid.
		if (!bWasJumping || GetJumpMaxHoldTime() <= 0.0f)
		{
			if (JumpCurrentCount == 0 && GetCharacterMovement()->IsFalling())
			{
				bCanJump = JumpCurrentCount + 1 < JumpMaxCount;
			}
			else
			{
				bCanJump = JumpCurrentCount < JumpMaxCount;
			}
		}
		else
		{
			// Only consider JumpKeyHoldTime as long as:
			// A) We are on the ground
			// B) The jump limit hasn't been met OR
			// C) The jump limit has been met AND we were already jumping
			const bool bJumpKeyHeld = (bPressedJump && JumpKeyHoldTime < GetJumpMaxHoldTime());
			bCanJump = bJumpKeyHeld &&
					   (GetCharacterMovement()->IsMovingOnGround() || (JumpCurrentCount < JumpMaxCount) || (bWasJumping && JumpCurrentCount == JumpMaxCount));
		}
		if (GetCharacterMovement()->IsMovingOnGround())
		{
			float FloorZ = FVector(0.0f, 0.0f, 1.0f) | GetCharacterMovement()->CurrentFloor.HitResult.ImpactNormal;
			float WalkableFloor = GetCharacterMovement()->GetWalkableFloorZ();
			bCanJump &= (FloorZ >= WalkableFloor || FMath::IsNearlyEqual(FloorZ, WalkableFloor));
		}
	}

	return bCanJump;
}

void APBPlayerCharacter::Move(FVector Direction, float Value)
{
	if (!FMath::IsNearlyZero(Value))
	{
		// add movement in that direction
		AddMovementInput(Direction, Value);
	}
}

void APBPlayerCharacter::Turn(bool bIsPure, float Rate)
{
	if (!bIsPure)
	{
		Rate = Rate * BaseTurnRate * GetWorld()->GetDeltaSeconds();
	}

	// calculate delta for this frame from the rate information
	AddControllerYawInput(Rate);
}

void APBPlayerCharacter::LookUp(bool bIsPure, float Rate)
{
	if (!bIsPure)
	{
		Rate = Rate * BaseLookUpRate * GetWorld()->GetDeltaSeconds();
	}

	// calculate delta for this frame from the rate information
	AddControllerPitchInput(Rate);
}

void APBPlayerCharacter::ApplyDamageMomentum(float DamageTaken, FDamageEvent const& DamageEvent, APawn* PawnInstigator, AActor* DamageCauser)
{
	UDamageType const* const DmgTypeCDO = DamageEvent.DamageTypeClass->GetDefaultObject<UDamageType>();
	if (GetCharacterMovement())
	{
		FHitResult HitInfo;
		FVector ImpulseDir;
		DamageEvent.GetBestHitInfo(this, DamageCauser, HitInfo, ImpulseDir);

		FVector Impulse = ImpulseDir;
		float SizeFactor = (60.96f * 60.96f * 137.16f) /
						   (FMath::Square(GetCapsuleComponent()->GetScaledCapsuleRadius() * 2.0f) * GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0f);
		Impulse *= DamageTaken * SizeFactor * 5.0f * 3.0f / 4.0f;
		bool const bMassIndependentImpulse = !DmgTypeCDO->bScaleMomentumByMass;
		FVector MassScaledImpulse = Impulse;
		if (!bMassIndependentImpulse && GetCharacterMovement()->Mass > SMALL_NUMBER)
		{
			MassScaledImpulse = MassScaledImpulse / GetCharacterMovement()->Mass;
		}
		if (MassScaledImpulse.Z > 1238.25f)
		{
			Impulse.Z = 1238.25f;
		}
		if (MassScaledImpulse.SizeSquared2D() > 1714.5f * 1714.5f)
		{
			// Impulse = Impulse.GetClampedToMaxSize2D(1714.5f);
		}

		GetCharacterMovement()->AddImpulse(Impulse, bMassIndependentImpulse);
	}
}

void APBPlayerCharacter::RecalculateBaseEyeHeight()
{
	const ACharacter* DefaultCharacter = GetClass()->GetDefaultObject<ACharacter>();
	const float OldUnscaledHalfHeight = DefaultCharacter->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float CrouchedHalfHeight = GetCharacterMovement()->GetCrouchedHalfHeight();
	const float FullCrouchDiff = OldUnscaledHalfHeight - CrouchedHalfHeight;
	const UCapsuleComponent* CharacterCapsule = GetCapsuleComponent();
	const float CurrentUnscaledHalfHeight = CharacterCapsule->GetUnscaledCapsuleHalfHeight();
	const float CurrentAlpha = 1.0f - (CurrentUnscaledHalfHeight - CrouchedHalfHeight) / FullCrouchDiff;
	BaseEyeHeight = FMath::Lerp(DefaultCharacter->BaseEyeHeight, CrouchedEyeHeight, UPBUtilityLibrary::SimpleSpline(CurrentAlpha));
}

bool APBPlayerCharacter::CanCrouch() const
{
	return !GetCharacterMovement()->bCheatFlying && Super::CanCrouch() && !(MovementPtr->IsOnLadder() || MovementPtr->IsInForcedMove());
}
