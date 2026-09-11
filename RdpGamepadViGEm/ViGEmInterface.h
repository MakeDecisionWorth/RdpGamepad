// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include <ViGEm/Client.h>
#include <Xinput.h>

class ViGEmClient : public std::enable_shared_from_this<ViGEmClient>
{
public:
	ViGEmClient();
	~ViGEmClient();

	// False when the ViGEm bus driver is missing or could not be opened, in
	// which case CreateController always fails.
	bool IsConnected() const { return mConnected; }

	// Returns nullptr if the bus refused to hand out a virtual pad.
	std::unique_ptr<class ViGEmTarget360> CreateController();

	const PVIGEM_CLIENT GetHandle() const { return mClient; }

private:
	PVIGEM_CLIENT mClient = nullptr;
	bool mConnected = false;
};

class ViGEmTarget360
{
	friend class ViGEmClient;

private:
	explicit ViGEmTarget360(std::shared_ptr<ViGEmClient> Client);

public:
	~ViGEmTarget360();

	void SetGamepadState(const XINPUT_GAMEPAD& Gamepad);
	bool GetVibration(XINPUT_VIBRATION& OutVibration);

private:
	// Plugs the pad into the bus and subscribes to rumble notifications.
	bool Connect();

	std::shared_ptr<ViGEmClient> mClient;
	PVIGEM_TARGET mTarget = nullptr;
	bool mConnected = false;

	// Guards the vibration fields below, which are the only state the
	// notification worker touches. Deliberately not held across any ViGEm call:
	// unregistering a notification waits for that worker to exit, and the worker
	// takes this lock, so the two would deadlock.
	std::mutex mMutex;
	XINPUT_VIBRATION mPendingVibration{0};
	bool mHasPendingVibration = false;

	static void CALLBACK StaticControllerNotification(PVIGEM_CLIENT Client, PVIGEM_TARGET Target, UCHAR LargeMotor, UCHAR SmallMotor, UCHAR LedNumber, LPVOID Context);
};
