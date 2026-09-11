// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "ViGEmInterface.h"

#pragma comment(lib, "setupapi.lib")

ViGEmTarget360::ViGEmTarget360(std::shared_ptr<ViGEmClient> Client)
	: mClient(std::move(Client))
	, mTarget(vigem_target_x360_alloc())
{}

bool ViGEmTarget360::Connect()
{
	if (mTarget == nullptr)
	{
		return false;
	}

	if (!VIGEM_SUCCESS(vigem_target_add(mClient->GetHandle(), mTarget)))
	{
		// Leave the pad unplugged rather than carrying on with a target the bus
		// never accepted: its serial number is meaningless, and subscribing to
		// notifications for it would put a worker thread on a request the driver
		// rejects every time.
		return false;
	}

	mConnected = true;

	// Not fatal on failure; the pad just never reports rumble back upstream.
	vigem_target_x360_register_notification(mClient->GetHandle(), mTarget, &StaticControllerNotification, this);

	return true;
}

ViGEmTarget360::~ViGEmTarget360()
{
	// No lock held here on purpose. Unregistering blocks until the notification
	// worker has exited, and that worker takes mMutex in
	// StaticControllerNotification, so holding it across this call deadlocks the
	// two against each other.
	if (mConnected)
	{
		vigem_target_x360_unregister_notification(mTarget);
		vigem_target_remove(mClient->GetHandle(), mTarget);
		mConnected = false;
	}

	if (mTarget != nullptr)
	{
		// Only safe because unregistering above waited for the worker: it reads
		// through this pointer for as long as it is alive.
		vigem_target_free(mTarget);
		mTarget = nullptr;
	}
}

void ViGEmTarget360::SetGamepadState(const XINPUT_GAMEPAD& Gamepad)
{
	if (!mConnected)
	{
		return;
	}

	XUSB_REPORT report;
	report.wButtons = Gamepad.wButtons;
	report.bLeftTrigger = Gamepad.bLeftTrigger;
	report.bRightTrigger = Gamepad.bRightTrigger;
	report.sThumbLX = Gamepad.sThumbLX;
	report.sThumbLY = Gamepad.sThumbLY;
	report.sThumbRX = Gamepad.sThumbRX;
	report.sThumbRY = Gamepad.sThumbRY;
	vigem_target_x360_update(mClient->GetHandle(), mTarget, report);
}

bool ViGEmTarget360::GetVibration(XINPUT_VIBRATION& OutVibration)
{
	std::unique_lock<std::mutex> lock(mMutex);
	if (mHasPendingVibration)
	{
		OutVibration = mPendingVibration;
		mHasPendingVibration = false;
		return true;
	}
	return false;
}

void ViGEmTarget360::StaticControllerNotification(PVIGEM_CLIENT Client, PVIGEM_TARGET Target, UCHAR LargeMotor, UCHAR SmallMotor, UCHAR LedNumber, LPVOID Context)
{
	auto pThis = static_cast<ViGEmTarget360*>(Context);

	std::unique_lock<std::mutex> lock(pThis->mMutex);
	pThis->mPendingVibration.wLeftMotorSpeed = LargeMotor << 8;
	pThis->mPendingVibration.wRightMotorSpeed = SmallMotor << 8;
	pThis->mHasPendingVibration = true;
}

ViGEmClient::ViGEmClient()
	: mClient(vigem_alloc())
{
	if (mClient != nullptr)
	{
		mConnected = VIGEM_SUCCESS(vigem_connect(mClient));
	}
}

ViGEmClient::~ViGEmClient()
{
	if (mClient == nullptr)
	{
		return;
	}

	// vigem_free only calls free(); vigem_disconnect is what closes the handle
	// on the bus device. Without it the driver keeps this process's connection
	// open for as long as the receiver runs and never gets the chance to reclaim
	// whatever was left plugged in.
	if (mConnected)
	{
		vigem_disconnect(mClient);
		mConnected = false;
	}

	vigem_free(mClient);
	mClient = nullptr;
}

std::unique_ptr<ViGEmTarget360> ViGEmClient::CreateController()
{
	if (!mConnected)
	{
		return nullptr;
	}

	std::unique_ptr<ViGEmTarget360> Controller{new ViGEmTarget360{shared_from_this()}};
	if (!Controller->Connect())
	{
		return nullptr;
	}

	return Controller;
}
