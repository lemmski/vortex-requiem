// Copyright Epic Games, Inc. All Rights Reserved.


#include "ShooterCharacter.h"
#include "ShooterWeapon.h"
#include "EnhancedInputComponent.h"
#include "Components/InputComponent.h"
#include "Components/PawnNoiseEmitterComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "Net/UnrealNetwork.h"

AShooterCharacter::AShooterCharacter()
{
	// create the noise emitter component
	PawnNoiseEmitter = CreateDefaultSubobject<UPawnNoiseEmitterComponent>(TEXT("Pawn Noise Emitter"));

	// configure movement
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 600.0f, 0.0f);
}

void AShooterCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AShooterCharacter, CurrentWeapon);
	DOREPLIFETIME(AShooterCharacter, CurrentHP);
	DOREPLIFETIME(AShooterCharacter, OwnedWeapons);
}

void AShooterCharacter::OnRep_CurrentHP()
{
	// a bit of a hack to update the hud, but it works
	OnBulletCountUpdated.Broadcast(500, CurrentHP);
}

void AShooterCharacter::OnRep_CurrentWeapon(AShooterWeapon* LastWeapon)
{
	if (CurrentWeapon)
	{
		CurrentWeapon->ActivateWeapon();
	}

	if (LastWeapon)
	{
		LastWeapon->DeactivateWeapon();
	}
}

void AShooterCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// base class handles move, aim and jump inputs
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Firing
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Started, this, &AShooterCharacter::DoStartFiring);
		EnhancedInputComponent->BindAction(FireAction, ETriggerEvent::Completed, this, &AShooterCharacter::DoStopFiring);

		// Switch weapon
		EnhancedInputComponent->BindAction(SwitchWeaponAction, ETriggerEvent::Triggered, this, &AShooterCharacter::DoSwitchWeapon);
	}

}

float AShooterCharacter::TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (HasAuthority())
	{
		// ignore if already dead
		if (CurrentHP <= 0.0f)
		{
			return 0.0f;
		}

		// Reduce HP
		CurrentHP -= Damage;
		OnRep_CurrentHP();

		// Have we depleted HP?
		if (CurrentHP <= 0.0f)
		{
			Multicast_OnDeath();
		}
	}

	return Damage;
}

void AShooterCharacter::Multicast_OnDeath_Implementation()
{
	// deactivate the weapon
	if (IsValid(CurrentWeapon))
	{
		CurrentWeapon->DeactivateWeapon();
	}


	// reset the bullet counter UI
	OnBulletCountUpdated.Broadcast(0, 0);

	// destroy this character
	Destroy();
}

void AShooterCharacter::DoStartFiring()
{
	if (HasAuthority())
	{
		if (CurrentWeapon)
		{
			CurrentWeapon->StartFiring();
		}
	}
	else
	{
		Server_StartFiring();
	}
}

void AShooterCharacter::Server_StartFiring_Implementation()
{
	if (CurrentWeapon)
	{
		CurrentWeapon->StartFiring();
	}
}


void AShooterCharacter::DoStopFiring()
{
	if (HasAuthority())
	{
		if (CurrentWeapon)
		{
			CurrentWeapon->StopFiring();
		}
	}
	else
	{
		Server_StopFiring();
	}
}

void AShooterCharacter::Server_StopFiring_Implementation()
{
	if (CurrentWeapon)
	{
		CurrentWeapon->StopFiring();
	}
}

void AShooterCharacter::DoSwitchWeapon()
{
	if(HasAuthority())
	{
		// ensure we have at least two weapons two switch between
		if (OwnedWeapons.Num() > 1)
		{
			// find the index of the current weapon in the owned list
			int32 WeaponIndex = OwnedWeapons.Find(CurrentWeapon);

			// is this the last weapon?
			if (WeaponIndex == OwnedWeapons.Num() - 1)
			{
				// loop back to the beginning of the array
				WeaponIndex = 0;
			}
			else {
				// select the next weapon index
				++WeaponIndex;
			}

			// set the new weapon as current
			AShooterWeapon* LastWeapon = CurrentWeapon;
			CurrentWeapon = OwnedWeapons[WeaponIndex];
			OnRep_CurrentWeapon(LastWeapon);
		}
	}
	else
	{
		Server_SwitchWeapon();
	}
}

void AShooterCharacter::Server_SwitchWeapon_Implementation()
{
	DoSwitchWeapon();
}

void AShooterCharacter::AttachWeaponMeshes(AShooterWeapon* Weapon)
{
	const FAttachmentTransformRules AttachmentRule(EAttachmentRule::SnapToTarget, false);

	// attach the weapon actor
	Weapon->AttachToActor(this, AttachmentRule);

	// attach the weapon meshes
	Weapon->GetFirstPersonMesh()->AttachToComponent(GetFirstPersonMesh(), AttachmentRule, FirstPersonWeaponSocket);
	Weapon->GetThirdPersonMesh()->AttachToComponent(GetMesh(), AttachmentRule, FirstPersonWeaponSocket);
	
}

void AShooterCharacter::PlayFiringMontage(UAnimMontage* Montage)
{
	
}

void AShooterCharacter::AddWeaponRecoil(float Recoil)
{
	// apply the recoil as pitch input
	AddControllerPitchInput(Recoil);
}

void AShooterCharacter::UpdateWeaponHUD(int32 CurrentAmmo, int32 MagazineSize)
{
	OnBulletCountUpdated.Broadcast(MagazineSize, CurrentAmmo);
}

FVector AShooterCharacter::GetWeaponTargetLocation()
{
	// trace ahead from the camera viewpoint
	FHitResult OutHit;

	const FVector Start = GetFirstPersonCameraComponent()->GetComponentLocation();
	const FVector End = Start + (GetFirstPersonCameraComponent()->GetForwardVector() * MaxAimDistance);

	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(this);

	GetWorld()->LineTraceSingleByChannel(OutHit, Start, End, ECC_Visibility, QueryParams);

	// return either the impact point or the trace end
	return OutHit.bBlockingHit ? OutHit.ImpactPoint : OutHit.TraceEnd;
}

void AShooterCharacter::AddWeaponClass(const TSubclassOf<AShooterWeapon>& WeaponClass)
{
	if(HasAuthority())
	{
		// do we already own this weapon?
		AShooterWeapon* OwnedWeapon = FindWeaponOfType(WeaponClass);

		if (!OwnedWeapon)
		{
			// spawn the new weapon
			FActorSpawnParameters SpawnParams;
			SpawnParams.Owner = this;
			SpawnParams.Instigator = this;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.TransformScaleMethod = ESpawnActorScaleMethod::MultiplyWithRoot;

			AShooterWeapon* AddedWeapon = GetWorld()->SpawnActor<AShooterWeapon>(WeaponClass, GetActorTransform(), SpawnParams);

			if (AddedWeapon)
			{
				// add the weapon to the owned list
				OwnedWeapons.Add(AddedWeapon);

				// if we have an existing weapon, deactivate it
				if (CurrentWeapon)
				{
					CurrentWeapon->DeactivateWeapon();
				}

				// switch to the new weapon
				CurrentWeapon = AddedWeapon;
				CurrentWeapon->ActivateWeapon();
			}
		}
	}
}

void AShooterCharacter::OnWeaponActivated(AShooterWeapon* Weapon)
{
	// update the bullet counter
	OnBulletCountUpdated.Broadcast(Weapon->GetMagazineSize(), Weapon->GetBulletCount());

	// set the character mesh AnimInstances
	GetFirstPersonMesh()->SetAnimInstanceClass(Weapon->GetFirstPersonAnimInstanceClass());
	GetMesh()->SetAnimInstanceClass(Weapon->GetThirdPersonAnimInstanceClass());
}

void AShooterCharacter::OnWeaponDeactivated(AShooterWeapon* Weapon)
{
	// unused
}

void AShooterCharacter::OnSemiWeaponRefire()
{
	// unused
}

AShooterWeapon* AShooterCharacter::FindWeaponOfType(TSubclassOf<AShooterWeapon> WeaponClass) const
{
	// check each owned weapon
	for (AShooterWeapon* Weapon : OwnedWeapons)
	{
		if (Weapon->IsA(WeaponClass))
		{
			return Weapon;
		}
	}

	// weapon not found
	return nullptr;

}
