#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/EquipmentState.h"
#include "protocol/PlayerState.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Network
{
	struct RemotePlayerTransform
	{
		std::string playerID;
		PlayerTransform transform;
		// Server-clock milliseconds when the source emitted this checkpoint.
		// Empty when the source had no clock estimate yet.
		std::optional<double> sentAtServerMilliseconds;
	};

	struct RemotePlayerAppearance
	{
		std::string playerID;
		Multiplayer::PlayerAppearance appearance;
	};

	struct RemotePlayerEquipment
	{
		std::string playerID;
		Multiplayer::PlayerEquipment equipment;
	};

	struct RemotePlayerChanges
	{
		std::vector<RemotePlayerTransform> transforms;
		// Only the newest pending appearance revision per player.
		std::vector<RemotePlayerAppearance> appearances;
		// Only the newest pending equipment revision per player.
		std::vector<RemotePlayerEquipment> equipment;
		std::vector<AnimationAction> animationActions;
		std::vector<std::string> disconnectedPlayerIDs;
	};

	// Starts the Socket.IO client. Safe to call more than once.
	void Initialize();

	[[nodiscard]] bool IsConnected();

	// Advances every time the default namespace opens, including automatic
	// reconnects. The server forgets per-connection state, so latest-wins
	// publishers republish whenever this changes.
	[[nodiscard]] std::uint64_t GetConnectionGeneration();

	// Advances on every acknowledgement the server returns. Socket.IO can keep
	// reporting a dead transport as connected for a long time, so an unchanged
	// count while probes are outstanding is the reliable sign of a lost server.
	[[nodiscard]] std::uint64_t GetServerResponseCount();

	// Called from the game thread. socket.io-client-cpp queues the emit for its
	// transport thread; no Skyrim objects cross the network boundary.
	void PublishLocalPlayerTransform(const PlayerTransform& a_transform);

	// Publishes the latest-wins appearance state. The caller owns revisions.
	void PublishLocalAppearance(const Multiplayer::PlayerAppearance& a_appearance);

	// Publishes the latest-wins equipped items. The caller owns revisions.
	void PublishLocalEquipment(const Multiplayer::PlayerEquipment& a_equipment);

	// Publishes one item in the ordered semantic action stream. Power attacks are
	// intentionally normalized to this same normal-attack vocabulary by capture.
	void PublishLocalNormalAttack(
		std::uint64_t a_graphEpoch,
		std::uint64_t a_checkpointSequence);

	// Emits latency:probe. a_onAcknowledged runs on the transport thread when the
	// server acknowledges it, with the server clock in milliseconds when the ack
	// carried one; it never runs if the connection drops first.
	void EmitLatencyProbe(
		std::function<void(std::optional<double> a_serverTimeMilliseconds)> a_onAcknowledged);

	// Drops every pending change and per-player receive history, as if each
	// remote player had left. The server resends appearance and equipment with
	// their existing revisions on connect, which would otherwise be rejected.
	void ForgetRemotePlayers();

	// Transfers network-thread updates to the caller. The authoritative
	// main-loop frame hook is the only consumer.
	[[nodiscard]] RemotePlayerChanges DrainRemotePlayerChanges();
}
