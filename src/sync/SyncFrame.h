#pragma once

#include "protocol/PlayerState.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <string>

// Short-lived views for one main-loop synchronization pass. They borrow state
// owned by SyncCoordinator and must not outlive the pass. Cross-feature results
// are passed to each stage explicitly rather than through these views.

struct LocalFrame
{
	RE::PlayerCharacter& player;
};

struct RemoteFrame
{
	const std::string& playerID;
	// The newest accepted checkpoint and its sequence.
	const Network::PlayerTransform& checkpoint;
	std::uint64_t lastSequence;
};

// A newer checkpoint accepted from the network, before it replaces the previous
// one. inserted marks the first message ever received for the player.
struct CheckpointUpdate
{
	const std::string& playerID;
	bool inserted;
	const Network::PlayerTransform& previous;
	std::uint64_t previousSequence;
	const Network::PlayerTransform& checkpoint;
};
