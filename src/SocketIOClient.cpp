#include "SocketIOClient.h"

#include <sio_client.h>

#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Network
{
	namespace
	{
		constexpr auto kDefaultServerEndpoint = "http://172.22.53.101:3000";
		constexpr auto kServerEndpointPath =
			"Data/SKSE/Plugins/skyrim-mp-server.txt";
		std::atomic_bool connected{ false };
		std::mutex remoteChangesLock;
		std::unordered_map<std::string, RemotePlayerTransform> pendingTransforms;
		std::unordered_map<std::string, PlayerTransform> latestReceivedTransforms;
		std::deque<AnimationAction> pendingAnimationActions;
		std::unordered_map<std::string, std::uint64_t> latestReceivedActionSequences;
		std::unordered_set<std::string> pendingDisconnects;
		std::uint64_t localActionSequence = 0;

		const std::string& GetServerEndpoint()
		{
			static const std::string endpoint = [] {
				std::ifstream input{ kServerEndpointPath };
				std::string configuredEndpoint;
				if (!std::getline(input, configuredEndpoint)) {
					return std::string{ kDefaultServerEndpoint };
				}

				const auto first = configuredEndpoint.find_first_not_of(" \t\r\n");
				const auto last = configuredEndpoint.find_last_not_of(" \t\r\n");
				if (first == std::string::npos) {
					return std::string{ kDefaultServerEndpoint };
				}
				configuredEndpoint = configuredEndpoint.substr(first, last - first + 1);

				if (!configuredEndpoint.starts_with("http://") &&
					!configuredEndpoint.starts_with("https://")) {
					logs::warn(
						"Ignoring invalid endpoint in {}; using {}",
						kServerEndpointPath,
						kDefaultServerEndpoint);
					return std::string{ kDefaultServerEndpoint };
				}

				return configuredEndpoint;
			}();

			return endpoint;
		}

		sio::client& GetClient()
		{
			// Keep the Socket.IO client alive for the lifetime of the SKSE DLL.
			static sio::client client;
			return client;
		}

		bool ReadString(
			const std::map<std::string, sio::message::ptr>& a_values,
			const std::string& a_key,
			std::string& a_result)
		{
			const auto value = a_values.find(a_key);
			if (value == a_values.end() || !value->second ||
				value->second->get_flag() != sio::message::flag_string) {
				return false;
			}

			a_result = value->second->get_string();
			return !a_result.empty();
		}

		bool ReadNumber(
			const std::map<std::string, sio::message::ptr>& a_values,
			const std::string& a_key,
			double& a_result)
		{
			const auto value = a_values.find(a_key);
			if (value == a_values.end() || !value->second) {
				return false;
			}

			switch (value->second->get_flag()) {
			case sio::message::flag_double:
				a_result = value->second->get_double();
				break;
			case sio::message::flag_integer:
				a_result = static_cast<double>(value->second->get_int());
				break;
			default:
				return false;
			}

			return std::isfinite(a_result);
		}

		bool ReadMovementState(
			const std::map<std::string, sio::message::ptr>& a_values,
			Multiplayer::MovementState& a_result)
		{
			std::string state;
			if (!ReadString(a_values, "movementState", state)) {
				return false;
			}

			if (state == "idle") {
				a_result = Multiplayer::MovementState::kIdle;
			} else if (state == "walking") {
				a_result = Multiplayer::MovementState::kWalking;
			} else if (state == "running") {
				a_result = Multiplayer::MovementState::kRunning;
			} else if (state == "sprinting") {
				a_result = Multiplayer::MovementState::kSprinting;
			} else if (state == "sneakingIdle") {
				a_result = Multiplayer::MovementState::kSneakingIdle;
			} else if (state == "sneakingWalking") {
				a_result = Multiplayer::MovementState::kSneakingWalking;
			} else if (state == "sneakingRunning") {
				a_result = Multiplayer::MovementState::kSneakingRunning;
			} else {
				return false;
			}

			return true;
		}

		bool ReadBool(
			const std::map<std::string, sio::message::ptr>& a_values,
			const std::string& a_key,
			bool& a_result)
		{
			const auto value = a_values.find(a_key);
			if (value == a_values.end() || !value->second ||
				value->second->get_flag() != sio::message::flag_boolean) {
				return false;
			}

			a_result = value->second->get_bool();
			return true;
		}

		bool ReadUnsignedInteger(
			const std::map<std::string, sio::message::ptr>& a_values,
			const std::string& a_key,
			std::uint64_t a_maximum,
			std::uint64_t& a_result)
		{
			const auto value = a_values.find(a_key);
			if (value == a_values.end() || !value->second ||
				value->second->get_flag() != sio::message::flag_integer) {
				return false;
			}

			const auto integer = value->second->get_int();
			if (integer < 0 || static_cast<std::uint64_t>(integer) > a_maximum) {
				return false;
			}

			a_result = static_cast<std::uint64_t>(integer);
			return true;
		}

		bool ParseRemoteTransform(
			const sio::message::ptr& a_data,
			RemotePlayerTransform& a_result)
		{
			if (!a_data || a_data->get_flag() != sio::message::flag_object) {
				return false;
			}

			const auto& values = a_data->get_map();
			double x = 0.0;
			double y = 0.0;
			double z = 0.0;
			double heading = 0.0;
			double speed = 0.0;
			Multiplayer::MovementState movementState{
				Multiplayer::MovementState::kIdle
			};
			bool jumping = false;
			bool weaponDrawn = false;
			std::uint64_t cellID = 0;
			std::uint64_t worldspaceID = 0;
			std::uint64_t furnitureRefID = 0;
			std::uint64_t graphEpoch = 0;
			std::uint64_t sequence = 0;
			if (!ReadString(values, "playerId", a_result.playerID) ||
				!ReadNumber(values, "x", x) ||
				!ReadNumber(values, "y", y) ||
				!ReadNumber(values, "z", z) ||
				!ReadNumber(values, "heading", heading) ||
				!ReadNumber(values, "speed", speed) ||
				speed < 0.0 ||
				speed > (std::numeric_limits<float>::max)() ||
				!ReadMovementState(values, movementState) ||
				!ReadBool(values, "jumping", jumping) ||
				!ReadBool(values, "weaponDrawn", weaponDrawn) ||
				!ReadUnsignedInteger(
					values,
					"cellId",
					(std::numeric_limits<std::uint32_t>::max)(),
					cellID) ||
				!ReadUnsignedInteger(
					values,
					"worldspaceId",
					(std::numeric_limits<std::uint32_t>::max)(),
					worldspaceID) ||
				!ReadUnsignedInteger(
					values,
					"furnitureRefId",
					(std::numeric_limits<std::uint32_t>::max)(),
					furnitureRefID) ||
				!ReadUnsignedInteger(
					values,
					"graphEpoch",
					(std::numeric_limits<std::int64_t>::max)(),
					graphEpoch) ||
				graphEpoch == 0 ||
				!ReadUnsignedInteger(
					values,
					"sequence",
					(std::numeric_limits<std::int64_t>::max)(),
					sequence)) {
				return false;
			}

			a_result.transform = PlayerTransform{
				.x = static_cast<float>(x),
				.y = static_cast<float>(y),
				.z = static_cast<float>(z),
				.heading = static_cast<float>(heading),
				.speed = static_cast<float>(speed),
				.animationState = Multiplayer::AnimationState{
					.movementState = movementState,
					.jumping = jumping,
					.weaponDrawn = weaponDrawn
				},
				.cellID = static_cast<std::uint32_t>(cellID),
				.worldspaceID = static_cast<std::uint32_t>(worldspaceID),
				.furnitureRefID = static_cast<std::uint32_t>(furnitureRefID),
				.graphEpoch = graphEpoch,
				.sequence = sequence
			};
			return true;
		}

		bool ParseRemoteAnimationAction(
			const sio::message::ptr& a_data,
			AnimationAction& a_result)
		{
			if (!a_data || a_data->get_flag() != sio::message::flag_object) {
				return false;
			}

			const auto& values = a_data->get_map();
			std::string family;
			std::string action;
			std::uint64_t actionSequence = 0;
			std::uint64_t graphEpoch = 0;
			std::uint64_t checkpointSequence = 0;
			if (!ReadString(values, "playerId", a_result.playerID) ||
				!ReadString(values, "family", family) || family != "attack" ||
				!ReadString(values, "action", action) || action != "normal" ||
				!ReadUnsignedInteger(
					values,
					"actionSequence",
					(std::numeric_limits<std::int64_t>::max)(),
					actionSequence) ||
				actionSequence == 0 ||
				!ReadUnsignedInteger(
					values,
					"graphEpoch",
					(std::numeric_limits<std::int64_t>::max)(),
					graphEpoch) ||
				graphEpoch == 0 ||
				!ReadUnsignedInteger(
					values,
					"checkpointSequence",
					(std::numeric_limits<std::int64_t>::max)(),
					checkpointSequence) ||
				checkpointSequence == 0) {
				return false;
			}

			const auto prerequisites = values.find("prerequisites");
			if (prerequisites == values.end() || !prerequisites->second ||
				prerequisites->second->get_flag() != sio::message::flag_object ||
				prerequisites->second->get_map().size() != 1) {
				return false;
			}
			bool weaponDrawn = false;
			if (!ReadBool(
					prerequisites->second->get_map(),
					"weaponDrawn",
					weaponDrawn) ||
				!weaponDrawn) {
				return false;
			}

			a_result.actionSequence = actionSequence;
			a_result.graphEpoch = graphEpoch;
			a_result.checkpointSequence = checkpointSequence;
			return true;
		}

		void OnRemoteTransform(
			std::string const&,
			sio::message::ptr const& a_data,
			bool,
			sio::message::list&)
		{
			RemotePlayerTransform update;
			if (!ParseRemoteTransform(a_data, update)) {
				logs::warn("Ignored malformed Socket.IO player:state payload");
				return;
			}

			const auto playerID = update.playerID;
			std::scoped_lock lock{ remoteChangesLock };
			pendingDisconnects.erase(playerID);

			const auto latest = latestReceivedTransforms.find(playerID);
			if (latest != latestReceivedTransforms.end() &&
				(update.transform.sequence <= latest->second.sequence ||
					update.transform.graphEpoch < latest->second.graphEpoch)) {
				logs::info(
					"Animation sync ignored stale network snapshot: player={} sequence={} state={} newestSequence={} newestState={}",
					playerID,
					static_cast<unsigned long long>(update.transform.sequence),
					Multiplayer::GetMovementStateName(
						update.transform.animationState.movementState),
					static_cast<unsigned long long>(latest->second.sequence),
					Multiplayer::GetMovementStateName(
						latest->second.animationState.movementState));
				return;
			}
			// Transform, locomotion, jump state, and combat stance are one latest-wins
			// checkpoint. Transient graph events are consequences of changing between
			// checkpoints; retaining them separately can replay obsolete transitions.
			latestReceivedTransforms.insert_or_assign(playerID, update.transform);
			pendingTransforms.insert_or_assign(playerID, std::move(update));
		}

		void OnRemoteAnimationAction(
			std::string const&,
			sio::message::ptr const& a_data,
			bool,
			sio::message::list&)
		{
			AnimationAction action;
			if (!ParseRemoteAnimationAction(a_data, action)) {
				logs::warn("Ignored malformed Socket.IO player:action payload");
				return;
			}

			std::scoped_lock lock{ remoteChangesLock };
			pendingDisconnects.erase(action.playerID);
			const auto latest = latestReceivedActionSequences.find(action.playerID);
			if (latest != latestReceivedActionSequences.end()) {
				if (action.actionSequence <= latest->second) {
					logs::info(
						"Animation sync ignored stale action: player={} actionSequence={} newestActionSequence={}",
						action.playerID,
						static_cast<unsigned long long>(action.actionSequence),
						static_cast<unsigned long long>(latest->second));
					return;
				}
				if (action.actionSequence != latest->second + 1) {
					// Socket.IO is reliable and ordered within a connection. A gap means
					// continuity was lost; reject rather than playing actions out of order.
					logs::warn(
						"Animation sync rejected action sequence gap: player={} expected={} received={}",
						action.playerID,
						static_cast<unsigned long long>(latest->second + 1),
						static_cast<unsigned long long>(action.actionSequence));
					return;
				}
			}

			latestReceivedActionSequences.insert_or_assign(
				action.playerID,
				action.actionSequence);
			logs::info(
				"Animation sync received normal attack: player={} actionSequence={} graphEpoch={} checkpointSequence={}",
				action.playerID,
				static_cast<unsigned long long>(action.actionSequence),
				static_cast<unsigned long long>(action.graphEpoch),
				static_cast<unsigned long long>(action.checkpointSequence));
			pendingAnimationActions.push_back(std::move(action));
		}

		void OnRemoteDisconnect(
			std::string const&,
			sio::message::ptr const& a_data,
			bool,
			sio::message::list&)
		{
			if (!a_data || a_data->get_flag() != sio::message::flag_object) {
				return;
			}

			std::string playerID;
			if (!ReadString(a_data->get_map(), "playerId", playerID)) {
				return;
			}

			std::scoped_lock lock{ remoteChangesLock };
			pendingTransforms.erase(playerID);
			latestReceivedTransforms.erase(playerID);
			latestReceivedActionSequences.erase(playerID);
			std::erase_if(
				pendingAnimationActions,
				[&playerID](const AnimationAction& a_action) {
					return a_action.playerID == playerID;
				});
			pendingDisconnects.insert(std::move(playerID));
		}
	}

	void Initialize()
	{
		static std::once_flag connectOnce;

		std::call_once(connectOnce, [] {
			auto& client = GetClient();
			auto socket = client.socket();

			socket->on("player:state", sio::socket::event_listener_aux(OnRemoteTransform));
			socket->on(
				"player:action",
				sio::socket::event_listener_aux(OnRemoteAnimationAction));
			socket->on("player:left", sio::socket::event_listener_aux(OnRemoteDisconnect));

			client.set_open_listener([] {
				RE::ConsoleLog::GetSingleton()->Print("MP: Connected.");
				logs::info("Socket.IO transport connected to {}", GetServerEndpoint());
			});
			client.set_socket_open_listener([](std::string const&) {
				connected.store(true, std::memory_order_release);
				logs::info("Socket.IO default namespace ready");
			});
			client.set_socket_close_listener([](std::string const&) {
				connected.store(false, std::memory_order_release);
				// A local transport interruption does not prove that any remote
				// player left. Keep their puppets until the server explicitly emits
				// player:left, allowing automatic Socket.IO reconnects to be seamless.
				RE::ConsoleLog::GetSingleton()->Print("MP: Connection closed.");
				logs::info("Socket.IO namespace closed; retaining remote puppets");
			});
			client.set_fail_listener([] {
				connected.store(false, std::memory_order_release);
				logs::warn("Socket.IO could not connect to {}", GetServerEndpoint());
			});
			client.set_close_listener([](sio::client::close_reason const&) {
				connected.store(false, std::memory_order_release);
				logs::info("Socket.IO connection closed; retaining remote puppets");
			});

			logs::info("Opening Socket.IO connection to {}", GetServerEndpoint());
			client.connect(GetServerEndpoint());
		});
	}

	bool IsConnected()
	{
		return connected.load(std::memory_order_acquire);
	}

	void PublishLocalPlayerTransform(const PlayerTransform& a_transform)
	{
		if (!IsConnected()) {
			return;
		}

		auto payload = sio::object_message::create();
		auto& values = payload->get_map();
		values["x"] = sio::double_message::create(a_transform.x);
		values["y"] = sio::double_message::create(a_transform.y);
		values["z"] = sio::double_message::create(a_transform.z);
		values["heading"] = sio::double_message::create(a_transform.heading);
		values["speed"] = sio::double_message::create(a_transform.speed);
		values["movementState"] = sio::string_message::create(
			Multiplayer::GetMovementStateName(
				a_transform.animationState.movementState));
		values["jumping"] = sio::bool_message::create(
			a_transform.animationState.jumping);
		values["weaponDrawn"] = sio::bool_message::create(
			a_transform.animationState.weaponDrawn);
		values["cellId"] = sio::int_message::create(a_transform.cellID);
		values["worldspaceId"] = sio::int_message::create(a_transform.worldspaceID);
		values["furnitureRefId"] = sio::int_message::create(
			a_transform.furnitureRefID);
		values["graphEpoch"] = sio::int_message::create(
			static_cast<std::int64_t>(a_transform.graphEpoch));
		values["sequence"] = sio::int_message::create(
			static_cast<std::int64_t>(a_transform.sequence));

		GetClient().socket()->emit("player:update", payload);
	}

	void PublishLocalNormalAttack(
		std::uint64_t a_graphEpoch,
		std::uint64_t a_checkpointSequence)
	{
		if (!IsConnected() || a_graphEpoch == 0 || a_checkpointSequence == 0) {
			return;
		}

		if (localActionSequence >= static_cast<std::uint64_t>(
				(std::numeric_limits<std::int64_t>::max)())) {
			localActionSequence = 0;
		}
		++localActionSequence;

		auto prerequisites = sio::object_message::create();
		prerequisites->get_map()["weaponDrawn"] = sio::bool_message::create(true);
		auto payload = sio::object_message::create();
		auto& values = payload->get_map();
		values["actionSequence"] = sio::int_message::create(
			static_cast<std::int64_t>(localActionSequence));
		values["graphEpoch"] = sio::int_message::create(
			static_cast<std::int64_t>(a_graphEpoch));
		values["checkpointSequence"] = sio::int_message::create(
			static_cast<std::int64_t>(a_checkpointSequence));
		values["family"] = sio::string_message::create("attack");
		values["action"] = sio::string_message::create("normal");
		values["prerequisites"] = std::move(prerequisites);

		logs::info(
			"Animation sync publishing normal attack: actionSequence={} graphEpoch={} checkpointSequence={}",
			static_cast<unsigned long long>(localActionSequence),
			static_cast<unsigned long long>(a_graphEpoch),
			static_cast<unsigned long long>(a_checkpointSequence));
		GetClient().socket()->emit("animation:action", payload);
	}

	RemotePlayerChanges DrainRemotePlayerChanges()
	{
		RemotePlayerChanges changes;
		std::scoped_lock lock{ remoteChangesLock };

		changes.transforms.reserve(pendingTransforms.size());
		for (auto& entry : pendingTransforms) {
			changes.transforms.push_back(std::move(entry.second));
		}
		pendingTransforms.clear();

		changes.animationActions.reserve(pendingAnimationActions.size());
		while (!pendingAnimationActions.empty()) {
			changes.animationActions.push_back(
				std::move(pendingAnimationActions.front()));
			pendingAnimationActions.pop_front();
		}

		changes.disconnectedPlayerIDs.reserve(pendingDisconnects.size());
		for (auto& playerID : pendingDisconnects) {
			changes.disconnectedPlayerIDs.push_back(std::move(playerID));
		}
		pendingDisconnects.clear();

		return changes;
	}
}
