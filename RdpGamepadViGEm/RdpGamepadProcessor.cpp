// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "RdpGamepadProcessor.h"

#include "ViGEmInterface.h"
#include <RdpGamepadProtocol.h>

RdpGamepadProcessor::RdpGamepadProcessor(std::function<void()> ConnectionStateCallback)
	: mConnectionStateCallback(std::move(ConnectionStateCallback))
	, mRdpGamepadChannel(new RdpGamepad::RdpGamepadVirtualChannel())
	, mViGEmClient(std::make_shared<ViGEmClient>())
{}

RdpGamepadProcessor::~RdpGamepadProcessor()
{
	Stop();
}

void RdpGamepadProcessor::Start()
{
	mKeepRunning = true;
	mThread = std::thread(&RdpGamepadProcessor::Run, this);
}

void RdpGamepadProcessor::Stop()
{
	{
		std::unique_lock<std::mutex> lock{mMutex};
		mKeepRunning = false;
	}

	// Idempotent: joinable() is false once the thread has already been joined,
	// which matters because session end and normal teardown can both get here.
	if (mThread.joinable())
	{
		mThread.join();
	}

	// The only place the pad is ever unplugged and the bus connection dropped:
	// the pad stays plugged in across reconnects, so nothing earlier does it.
	// This has to happen here rather than in the destructor because when Windows
	// ends the session the process is terminated where it stands and no
	// destructor runs, and a bus still holding a plugged-in pad and this
	// process's connection is a bus that cannot power down.
	std::unique_lock<std::mutex> lock{mMutex};
	mViGEmTarget360 = nullptr;
	mViGEmClient = nullptr;
}

bool RdpGamepadProcessor::IsConnected() const
{
	std::unique_lock<std::mutex> lock{mMutex};
	return mRdpGamepadConnected;
}

void RdpGamepadProcessor::Run()
{
	static constexpr int PollFrequency = 16; // ms

	HANDLE TimerEvent;
	LARGE_INTEGER DueTime;
	DueTime.QuadPart = -1;

	TimerEvent = CreateWaitableTimerEx(NULL, NULL, 0, TIMER_ALL_ACCESS);
	if (TimerEvent == nullptr)
	{
		RdpGamepadTidy();
		return;
	}

	SetWaitableTimer(TimerEvent, &DueTime, PollFrequency, NULL, NULL, false);

	std::unique_lock<std::mutex> lock{mMutex};
	while (mKeepRunning)
	{
		mMutex.unlock();
		TriggerConnectionStateCallback();
		const unsigned long WaitResult = WaitForSingleObject(TimerEvent, PollFrequency * 2);
		mMutex.lock();

		if (WaitResult == 0)
		{
			RdpGamepadProcess();
		}
	}

	CancelWaitableTimer(TimerEvent);
	CloseHandle(TimerEvent);
	RdpGamepadTidy();
}

void RdpGamepadProcessor::RdpGamepadTidy()
{
	bool bWasConnected = mRdpGamepadConnected;

	// The pad deliberately stays plugged in rather than being unplugged and
	// rebuilt around every reconnect, so a flaky session cannot churn it. Each
	// plug/unplug cycle costs more on the bus driver side than it looks: the
	// queue holding that pad's notification requests is owned by the bus itself
	// rather than by the pad, and nothing releases it when the pad goes away, so
	// every cycle strands one more of them. Neutralise the pad instead and let
	// Stop() do the single teardown this process ever needs.
	if (mViGEmTarget360 != nullptr)
	{
		mViGEmTarget360->SetGamepadState(XINPUT_GAMEPAD{0});
	}

	mRdpGamepadChannel->Close();
	mRdpGamepadConnected = false;
	mRdpGamepadPollTicks = 0;
	if (bWasConnected)
	{
		mConnectionStateCallbackPending = true;
	}
}

void RdpGamepadProcessor::RdpGamepadProcess()
{
	++mRdpGamepadPollTicks;

	// Try to open the channel if we don't have a channel open already
	if (!mRdpGamepadChannel->IsOpen())
	{
		// Only try to reconnect every few seconds instead of every tick as WTSVirtualChannelOpen can take a bit long.
		// I would probably be best if we called that outside of the critical section lock but for now this should
		// really make things much better.
		if (mRdpGamepadOpenRetry == 0)
		{
			if (!mRdpGamepadChannel->Open())
			{
				mRdpGamepadOpenRetry = 35; // Retry about every second
				RdpGamepadTidy();
				return;
			}
		}
		else
		{
			--mRdpGamepadOpenRetry;
			RdpGamepadTidy();
			return;
		}
	}

	//assert(mRdpGamepadChannel->IsOpen());
	if (!mRdpGamepadConnected)
	{
		// Built on the first session that needs it and kept from then on, so a
		// reconnect reuses the pad that is already plugged in.
		if (mViGEmTarget360 == nullptr)
		{
			mViGEmTarget360 = mViGEmClient->CreateController();
			if (mViGEmTarget360 == nullptr)
			{
				// No virtual pad, so there is nothing to forward input to. Back
				// off and retry instead of reporting a connection and then
				// dereferencing a pad that was never created.
				mRdpGamepadOpenRetry = 35;
				RdpGamepadTidy();
				return;
			}
		}

		mRdpGamepadConnected = true;
		mConnectionStateCallbackPending = true;
	}

	// Request controller state and update vibration
	if (!mRdpGamepadChannel->Send(RdpGamepad::RdpGetStateRequest::MakeRequest(0)))
	{
		RdpGamepadTidy();
		return;
	}

	XINPUT_VIBRATION PendingVibes;
	if (mViGEmTarget360->GetVibration(PendingVibes))
	{
		if (!mRdpGamepadChannel->Send(RdpGamepad::RdpSetStateRequest::MakeRequest(0, PendingVibes)))
		{
			RdpGamepadTidy();
			return;
		}
	}

	// Read all the pending messages
	RdpGamepad::RdpProtocolPacket packet;
	while (mRdpGamepadChannel->Receive(&packet))
	{
		// Handle controller state
		if (packet.mHeader.mMessageType == RdpGamepad::RdpMessageType::GetStateResponse)
		{
			if (packet.mGetStateResponse.mUserIndex == 0)
			{
				if (packet.mGetStateResponse.mResult == 0)
				{
					mViGEmTarget360->SetGamepadState(packet.mGetStateResponse.mState.Gamepad);
				}
				else
				{
					mViGEmTarget360->SetGamepadState(XINPUT_GAMEPAD{0});
				}
				mLastGetStateResponseTicks = mRdpGamepadPollTicks;
			}
		}
	}

	// Remove stale controller data
	if (mRdpGamepadPollTicks < mLastGetStateResponseTicks || (mRdpGamepadPollTicks - mLastGetStateResponseTicks) > 120) // Timeout in about 2 seconds
	{
		mViGEmTarget360->SetGamepadState(XINPUT_GAMEPAD{0});
	}

	// Check connection state
	if (!mRdpGamepadChannel->IsOpen())
	{
		RdpGamepadTidy();
	}
}

void RdpGamepadProcessor::TriggerConnectionStateCallback()
{
	if (mConnectionStateCallbackPending && mConnectionStateCallback)
	{
		mConnectionStateCallback();
		mConnectionStateCallbackPending = false;
	}
}
