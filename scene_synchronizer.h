#pragma once

#include "core/object/object.h"
#include "modules/network_synchronizer/core/core.h"

#include "core/network_interface.h"
#include "core/object_data.h"
#include "core/templates/local_vector.h"
#include "core/templates/oa_hash_map.h"
#include "data_buffer.h"
#include "modules/network_synchronizer/core/object_data_storage.h"
#include "modules/network_synchronizer/core/processor.h"
#include "modules/network_synchronizer/core/var_data.h"
#include "net_utilities.h"
#include "snapshot.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class SceneDiff;

NS_NAMESPACE_BEGIN

class NetworkedControllerBase;
struct PlayerController;
class Synchronizer;

class SynchronizerManager {
public:
	virtual ~SynchronizerManager() {}

	virtual void on_init_synchronizer(bool p_was_generating_ids) {}
	virtual void on_uninit_synchronizer() {}

#ifdef DEBUG_ENABLED
	virtual void debug_only_validate_nodes() {}
#endif

	/// Add node data and generates the `NetNodeId` if allowed.
	virtual void on_add_object_data(NS::ObjectData &p_object_data) {}
	virtual void on_drop_object_data(NS::ObjectData &p_object_data) {}

	virtual void on_sync_group_created(SyncGroupId p_group_id) {}

	/// This function is always executed on the server before anything else
	/// and it's here that you want to update the node relevancy.
	virtual void update_nodes_relevancy() {}

	virtual bool snapshot_get_custom_data(const NS::SyncGroup *p_group, NS::VarData &r_custom_data) { return false; }
	virtual void snapshot_set_custom_data(const NS::VarData &r_custom_data) {}

	virtual ObjectHandle fetch_app_object(const std::string &p_object_name) = 0;
	virtual uint64_t get_object_id(ObjectHandle p_app_object_handle) const = 0;
	virtual std::string get_object_name(ObjectHandle p_app_object_handle) const = 0;
	virtual void setup_synchronizer_for(ObjectHandle p_app_object_handle, ObjectLocalId p_id) = 0;
	virtual void set_variable(ObjectHandle p_app_object_handle, const char *p_var_name, const Variant &p_val) = 0;
	virtual bool get_variable(ObjectHandle p_app_object_handle, const char *p_var_name, Variant &p_val) const = 0;

	virtual NetworkedControllerBase *extract_network_controller(ObjectHandle p_app_object_handle) = 0;
	virtual const NetworkedControllerBase *extract_network_controller(ObjectHandle p_app_object_handle) const = 0;
};

/// # SceneSynchronizer
///
/// NOTICE: Do not instantiate this class directly, please use `SceneSynchronizer<>` instead.
///
/// The `SceneSynchronizer` is responsible to keep the scene of all peers in sync.
/// Usually each peer has it istantiated, and depending if it's istantiated in
/// the server or in the client, it does a different thing.
///
/// ## The `Player` is playing the game on the server.
///
/// The server is authoritative and it can't never be wrong. For this reason
/// the `SceneSynchronizer` on the server sends at a fixed interval (defined by
/// `server_notify_state_interval`) a snapshot to all peers.
///
/// The clients receives the server snapshot, so it compares with the local
/// snapshot and if it's necessary perform the recovery.
///
/// ## Variable traking
///
/// The `SceneSynchronizer` is able to track any node variable. It's possible to specify
/// the variables to track using the function `register_variable`.
///
/// ## NetworkedController
/// The `NetworkedController` is able to aquire the `Player` input and perform
/// operation in sync with other peers. When a discrepancy is found by the
/// `SceneSynchronizer`, it will drive the `NetworkedController` so to recover that
/// missalignment.
///
///
/// ## Processing function
/// Some objects, that are not direclty controlled by a `Player`, may need to be
/// in sync between peers; since those are not controlled by a `Player` is
/// not necessary use the `NetworkedController`.
///
/// It's possible to specify some process functions using `register_process`.
/// The `SceneSynchronizer` will call these functions each frame, in sync with the
/// other peers.
///
/// As example object we may think about a moving platform, or a bridge that
/// opens and close, or even a simple timer to track the match time.
/// An example implementation would be:
/// ```
/// var time := 0.0
///
/// func _ready():
/// 	# Make sure this never go out of sync.
/// 	SceneSynchronizer.register_variable(self, "time")
///
/// 	# Make sure to call this in sync with other peers.
/// 	SceneSynchronizer.register_process(self, "in_sync_process")
///
/// func in_sync_process(delta: float):
/// 	time += delta
/// ```
/// In the above code the variable `time` will always be in sync.
///
//
// # Implementation details.
//
// The entry point of the above mechanism is the function `SceneSynchronizer::process()`.
// The server `SceneSynchronizer` code is inside the class `ServerSynchronizer`.
// The client `SceneSynchronizer` code is inside the class `ClientSynchronizer`.
// The no networking `SceneSynchronizer` code is inside the class `NoNetSynchronizer`.
class SceneSynchronizerBase {
	template <class C, class NI>
	friend class SceneSynchronizer;
	friend class Synchronizer;
	friend class ServerSynchronizer;
	friend class ClientSynchronizer;
	friend class NoNetSynchronizer;
	friend SceneDiff;

public:
	enum SynchronizerType {
		SYNCHRONIZER_TYPE_NULL,
		SYNCHRONIZER_TYPE_NONETWORK,
		SYNCHRONIZER_TYPE_CLIENT,
		SYNCHRONIZER_TYPE_SERVER
	};

	/// This SyncGroup contains ALL the registered NodeData.
	static const SyncGroupId GLOBAL_SYNC_GROUP_ID;

private:
	class NetworkInterface *network_interface = nullptr;
	SynchronizerManager *synchronizer_manager = nullptr;

	RpcHandle<DataBuffer &> rpc_handler_state;
	RpcHandle<> rpc_handler_notify_need_full_snapshot;
	RpcHandle<bool> rpc_handler_set_network_enabled;
	RpcHandle<bool> rpc_handler_notify_peer_status;
	RpcHandle<const Vector<uint8_t> &> rpc_handler_deferred_sync_data;

	int max_deferred_nodes_per_update = 30;
	real_t server_notify_state_interval = 1.0;
	/// Can be 0.0 to update the relevancy each frame.
	real_t nodes_relevancy_update_time = 0.5;

	SynchronizerType synchronizer_type = SYNCHRONIZER_TYPE_NULL;
	Synchronizer *synchronizer = nullptr;
	bool recover_in_progress = false;
	bool reset_in_progress = false;
	bool rewinding_in_progress = false;
	bool end_sync = false;

	bool peer_dirty = false;
	std::map<int, NS::PeerData> peer_data;

	bool generate_id = false;

	ObjectDataStorage objects_data_storage;

	int event_flag = 0;
	std::vector<ChangesListener *> changes_listeners;

	bool cached_process_functions_valid = false;
	Processor<float> cached_process_functions[PROCESSPHASE_COUNT];

	// Set at runtime by the constructor by reading the project settings.
	bool debug_rewindings_enabled = false;

public: // -------------------------------------------------------------- Events
	Processor<> event_sync_started;
	Processor<> event_sync_paused;
	Processor<const NS::ObjectData * /*p_object_data*/, int /*p_peer*/, bool /*p_connected*/, bool /*p_enabled*/> event_peer_status_updated;
	Processor<uint32_t /*p_input_id*/> event_state_validated;
	Processor<uint32_t /*p_input_id*/, int /*p_index*/, int /*p_count*/> event_rewind_frame_begin;
	Processor<uint32_t /*p_input_id*/, ObjectHandle /*p_app_object_handle*/, const std::vector<std::string> & /*p_var_names*/, const std::vector<Variant> & /*p_client_values*/, const std::vector<Variant> & /*p_server_values*/> event_desync_detected;

private:
	// This is private so this class can be created only from
	// `SceneSynchronizer<BaseClass>` and the user is forced to define a base class.
	SceneSynchronizerBase(NetworkInterface *p_network_interface);

public:
	~SceneSynchronizerBase();

public: // -------------------------------------------------------- Manager APIs
	/// Setup the synchronizer
	void setup(
			SynchronizerManager &p_synchronizer_manager);

	/// Prepare the synchronizer for destruction.
	void conclude();

	/// Process the SceneSync.
	void process();

	/// Call this function when a networked app object is destroyed.
	void on_app_object_removed(ObjectHandle p_app_object_handle);

public:
	NS::NetworkInterface &get_network_interface() {
		return *network_interface;
	}
	const NS::NetworkInterface &get_network_interface() const {
		return *network_interface;
	}

	NS::SynchronizerManager &get_synchronizer_manager() {
		return *synchronizer_manager;
	}
	const NS::SynchronizerManager &get_synchronizer_manager() const {
		return *synchronizer_manager;
	}

	void set_max_deferred_nodes_per_update(int p_rate);
	int get_max_deferred_nodes_per_update() const;

	void set_server_notify_state_interval(real_t p_interval);
	real_t get_server_notify_state_interval() const;

	void set_nodes_relevancy_update_time(real_t p_time);
	real_t get_nodes_relevancy_update_time() const;

	bool is_variable_registered(ObjectLocalId p_id, const StringName &p_variable) const;

public: // ---------------------------------------------------------------- RPCs
	void rpc_receive_state(DataBuffer &p_snapshot);
	void rpc__notify_need_full_snapshot();
	void rpc_set_network_enabled(bool p_enabled);
	void rpc_notify_peer_status(bool p_enabled);
	void rpc_deferred_sync_data(const Vector<uint8_t> &p_data);

public: // ---------------------------------------------------------------- APIs
	/// Register a new node and returns its `NodeData`.
	void register_app_object(ObjectHandle p_app_object_handle, ObjectLocalId *out_id = nullptr);
	void unregister_app_object(ObjectLocalId p_id);
	void register_variable(ObjectLocalId p_id, const StringName &p_variable);
	void unregister_variable(ObjectLocalId p_id, const StringName &p_variable);

	ObjectNetId get_app_object_net_id(ObjectHandle p_app_object_handle) const;

	ObjectHandle get_app_object_from_id(ObjectNetId p_id, bool p_expected = true);
	ObjectHandle get_app_object_from_id_const(ObjectNetId p_id, bool p_expected = true) const;

	const std::vector<ObjectData *> &get_all_object_data() const;

	/// Returns the variable ID relative to the `Node`.
	/// This may return `UINT32_MAX` in various cases:
	/// - The node is not registered.
	/// - The variable is not registered.
	/// - The client doesn't know the ID yet.
	VarId get_variable_id(ObjectLocalId p_id, const StringName &p_variable);

	void set_skip_rewinding(ObjectLocalId p_id, const StringName &p_variable, bool p_skip_rewinding);

	ListenerHandle track_variable_changes(
			ObjectLocalId p_id,
			const StringName &p_variable,
			std::function<void(const std::vector<Variant> &p_old_values)> p_listener_func,
			NetEventFlag p_flags = NetEventFlag::DEFAULT);

	ListenerHandle track_variables_changes(
			const std::vector<ObjectLocalId> &p_object_ids,
			const std::vector<StringName> &p_variables,
			std::function<void(const std::vector<Variant> &p_old_values)> p_listener_func,
			NetEventFlag p_flags = NetEventFlag::DEFAULT);

	void untrack_variable_changes(ListenerHandle p_handle);

	/// You can use the macro `callable_mp()` to register custom C++ function.
	NS::PHandler register_process(ObjectLocalId p_id, ProcessPhase p_phase, std::function<void(float)> p_func);
	void unregister_process(ObjectLocalId p_id, ProcessPhase p_phase, NS::PHandler p_func_handler);

	/// Setup the deferred sync method for this specific node.
	/// The deferred-sync is different from the realtime-sync because the data
	/// is streamed and not simulated.
	void setup_deferred_sync(ObjectLocalId p_id, const Callable &p_collect_epoch_func, const Callable &p_apply_epoch_func);

	/// Creates a realtime sync group containing a list of nodes.
	/// The Peers listening to this group will receive the updates only
	/// from the nodes within this group.
	SyncGroupId sync_group_create();
	const NS::SyncGroup *sync_group_get(SyncGroupId p_group_id) const;

	void sync_group_add_node_by_id(ObjectNetId p_node_id, SyncGroupId p_group_id, bool p_realtime);
	void sync_group_add_node(NS::ObjectData *p_object_data, SyncGroupId p_group_id, bool p_realtime);
	void sync_group_remove_node_by_id(ObjectNetId p_node_id, SyncGroupId p_group_id);
	void sync_group_remove_node(NS::ObjectData *p_object_data, SyncGroupId p_group_id);

	/// Use `std::move()` to transfer `p_new_realtime_nodes` and `p_new_deferred_nodes`.
	void sync_group_replace_nodes(SyncGroupId p_group_id, LocalVector<NS::SyncGroup::RealtimeNodeInfo> &&p_new_realtime_nodes, LocalVector<NS::SyncGroup::DeferredNodeInfo> &&p_new_deferred_nodes);

	void sync_group_remove_all_nodes(SyncGroupId p_group_id);
	void sync_group_move_peer_to(int p_peer_id, SyncGroupId p_group_id);
	SyncGroupId sync_group_get_peer_group(int p_peer_id) const;
	const LocalVector<int> *sync_group_get_peers(SyncGroupId p_group_id) const;

	void sync_group_set_deferred_update_rate(ObjectLocalId p_node_id, SyncGroupId p_group_id, real_t p_update_rate);
	void sync_group_set_deferred_update_rate(ObjectNetId p_node_id, SyncGroupId p_group_id, real_t p_update_rate);
	real_t sync_group_get_deferred_update_rate(ObjectLocalId p_node_id, SyncGroupId p_group_id) const;
	real_t sync_group_get_deferred_update_rate(ObjectNetId p_node_id, SyncGroupId p_group_id) const;

	void sync_group_set_user_data(SyncGroupId p_group_id, uint64_t p_user_ptr);
	uint64_t sync_group_get_user_data(SyncGroupId p_group_id) const;

	void start_tracking_scene_changes(Object *p_diff_handle) const;
	void stop_tracking_scene_changes(Object *p_diff_handle) const;
	Variant pop_scene_changes(Object *p_diff_handle) const;
	void apply_scene_changes(DataBuffer &p_sync_data);

	bool is_recovered() const;
	bool is_resetted() const;
	bool is_rewinding() const;
	bool is_end_sync() const;

	/// This function works only on server.
	void force_state_notify(SyncGroupId p_sync_group_id);
	void force_state_notify_all();
	/// Make peers as dirty, so they will be reloaded next frame.
	void dirty_peers();

	void set_enabled(bool p_enable);
	bool is_enabled() const;

	void set_peer_networking_enable(int p_peer, bool p_enable);
	bool is_peer_networking_enable(int p_peer) const;

	void on_peer_connected(int p_peer);
	void on_peer_disconnected(int p_peer);

	void init_synchronizer(bool p_was_generating_ids);
	void uninit_synchronizer();
	void reset_synchronizer_mode();
	void clear();

	void notify_controller_control_mode_changed(NetworkedControllerBase *controller);

	void update_peers();
	void clear_peers();

	void detect_and_signal_changed_variables(int p_flags);

	void change_events_begin(int p_flag);
	void change_event_add(NS::ObjectData *p_object_data, VarId p_var_id, const Variant &p_old);
	void change_events_flush();

public: // ------------------------------------------------------------ INTERNAL
	void update_nodes_relevancy();

	void process_functions__clear();
	void process_functions__execute(const double p_delta);

	ObjectLocalId find_object_local_id(ObjectHandle p_app_object) const;
	ObjectLocalId find_object_local_id(const NetworkedControllerBase &p_controller) const;

	ObjectData *get_object_data(ObjectLocalId p_id, bool p_expected = true);
	const ObjectData *get_object_data(ObjectLocalId p_id, bool p_expected = true) const;

	ObjectData *get_object_data(ObjectNetId p_id, bool p_expected = true);
	const ObjectData *get_object_data(ObjectNetId p_id, bool p_expected = true) const;

	NetworkedControllerBase *get_controller_for_peer(int p_peer, bool p_expected = true);
	const NetworkedControllerBase *get_controller_for_peer(int p_peer, bool p_expected = true) const;

	PeerData *get_peer_for_controller(const NetworkedControllerBase &p_controller, bool p_expected = true);
	const PeerData *get_peer_for_controller(const NetworkedControllerBase &p_controller, bool p_expected = true) const;

	/// Returns the latest generated `NetNodeId`.
	ObjectNetId get_biggest_node_id() const;

	void reset_controllers();
	void reset_controller(NS::ObjectData *p_controller);

	real_t get_pretended_delta() const;

	/// Read the node variables and store the value if is different from the
	/// previous one and emits a signal.
	void pull_node_changes(NS::ObjectData *p_object_data);

	void drop_object_data(NS::ObjectData &p_object_data);

	void notify_object_data_net_id_changed(ObjectData &p_object_data);

	NetworkedControllerBase *fetch_controller_by_peer(int peer);

public:
	/// Returns true if this peer is server.
	bool is_server() const;
	/// Returns true if this peer is client.
	bool is_client() const;
	/// Returns true if there is no network.
	bool is_no_network() const;
	/// Returns true if network is enabled.
	bool is_networked() const;
};

class Synchronizer {
protected:
	SceneSynchronizerBase *scene_synchronizer;

public:
	Synchronizer(SceneSynchronizerBase *p_node);
	virtual ~Synchronizer() = default;

	virtual void clear() = 0;

	virtual void process() = 0;
	virtual void on_peer_connected(int p_peer_id) {}
	virtual void on_peer_disconnected(int p_peer_id) {}
	virtual void on_object_data_added(NS::ObjectData *p_object_data) {}
	virtual void on_object_data_removed(NS::ObjectData &p_object_data) {}
	virtual void on_variable_added(NS::ObjectData *p_object_data, const StringName &p_var_name) {}
	virtual void on_variable_changed(NS::ObjectData *p_object_data, VarId p_var_id, const Variant &p_old_value, int p_flag) {}
	virtual void on_controller_reset(NS::ObjectData *p_object_data) {}
};

class NoNetSynchronizer : public Synchronizer {
	friend class SceneSynchronizerBase;

	bool enabled = true;
	uint32_t frame_count = 0;

public:
	NoNetSynchronizer(SceneSynchronizerBase *p_node);

	virtual void clear() override;
	virtual void process() override;

	void set_enabled(bool p_enabled);
	bool is_enabled() const;
};

class ServerSynchronizer : public Synchronizer {
	friend class SceneSynchronizerBase;

	real_t nodes_relevancy_update_timer = 0.0;
	uint32_t epoch = 0;
	/// This array contains a map between the peers and the relevant nodes.
	LocalVector<NS::SyncGroup> sync_groups;

	enum SnapshotGenerationMode {
		/// The shanpshot will include The NodeId or NodePath and allthe changed variables.
		SNAPSHOT_GENERATION_MODE_NORMAL,
		/// The snapshot will include The NodePath only.
		SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY,
		/// The snapshot will contains everything no matter what.
		SNAPSHOT_GENERATION_MODE_FORCE_FULL,
	};

public:
	ServerSynchronizer(SceneSynchronizerBase *p_node);

	virtual void clear() override;
	virtual void process() override;
	virtual void on_peer_connected(int p_peer_id) override;
	virtual void on_peer_disconnected(int p_peer_id) override;
	virtual void on_object_data_added(NS::ObjectData *p_object_data) override;
	virtual void on_object_data_removed(NS::ObjectData &p_object_data) override;
	virtual void on_variable_added(NS::ObjectData *p_object_data, const StringName &p_var_name) override;
	virtual void on_variable_changed(NS::ObjectData *p_object_data, VarId p_var_id, const Variant &p_old_value, int p_flag) override;

	SyncGroupId sync_group_create();
	const NS::SyncGroup *sync_group_get(SyncGroupId p_group_id) const;
	void sync_group_add_node(NS::ObjectData *p_object_data, SyncGroupId p_group_id, bool p_realtime);
	void sync_group_remove_node(NS::ObjectData *p_object_data, SyncGroupId p_group_id);
	void sync_group_replace_nodes(SyncGroupId p_group_id, LocalVector<NS::SyncGroup::RealtimeNodeInfo> &&p_new_realtime_nodes, LocalVector<NS::SyncGroup::DeferredNodeInfo> &&p_new_deferred_nodes);
	void sync_group_remove_all_nodes(SyncGroupId p_group_id);
	void sync_group_move_peer_to(int p_peer_id, SyncGroupId p_group_id);
	const LocalVector<int> *sync_group_get_peers(SyncGroupId p_group_id) const;

	void sync_group_set_deferred_update_rate(NS::ObjectData *p_object_data, SyncGroupId p_group_id, real_t p_update_rate);
	real_t sync_group_get_deferred_update_rate(const NS::ObjectData *p_object_data, SyncGroupId p_group_id) const;

	void sync_group_set_user_data(SyncGroupId p_group_id, uint64_t p_user_ptr);
	uint64_t sync_group_get_user_data(SyncGroupId p_group_id) const;

	void sync_group_debug_print();

	void process_snapshot_notificator(real_t p_delta);

	void generate_snapshot(
			bool p_force_full_snapshot,
			const NS::SyncGroup &p_group,
			DataBuffer &r_snapshot_db) const;

	void generate_snapshot_object_data(
			const NS::ObjectData *p_object_data,
			SnapshotGenerationMode p_mode,
			const NS::SyncGroup::Change &p_change,
			DataBuffer &r_snapshot_db) const;

	void process_deferred_sync(real_t p_delta);
};

class ClientSynchronizer : public Synchronizer {
	friend class SceneSynchronizerBase;

	NS::ObjectData *player_controller_node_data = nullptr;
	std::map<ObjectNetId, std::string> objects_names;

	NS::Snapshot last_received_snapshot;
	std::deque<NS::Snapshot> client_snapshots;
	std::deque<NS::Snapshot> server_snapshots;
	uint32_t last_checked_input = 0;
	bool enabled = true;
	bool want_to_enable = false;

	bool need_full_snapshot_notified = false;

	struct EndSyncEvent {
		NS::ObjectData *node_data;
		VarId var_id;
		Variant old_value;

		bool operator<(const EndSyncEvent &p_other) const {
			if (node_data->get_net_id() == p_other.node_data->get_net_id()) {
				return var_id < p_other.var_id;
			} else {
				return node_data->get_net_id().id < p_other.node_data->get_net_id().id;
			}
		}
	};

	RBSet<EndSyncEvent> sync_end_events;

	struct DeferredSyncInterpolationData {
		NS::ObjectData *nd = nullptr;
		DataBuffer past_epoch_buffer;
		DataBuffer future_epoch_buffer;

		uint32_t past_epoch = UINT32_MAX;
		uint32_t future_epoch = UINT32_MAX;
		real_t alpha_advacing_per_epoch = 1.0;
		real_t alpha = 0.0;

		DeferredSyncInterpolationData() = default;
		DeferredSyncInterpolationData(const DeferredSyncInterpolationData &p_dss) :
				nd(p_dss.nd),
				past_epoch(p_dss.past_epoch),
				future_epoch(p_dss.future_epoch),
				alpha_advacing_per_epoch(p_dss.alpha_advacing_per_epoch),
				alpha(p_dss.alpha) {
			past_epoch_buffer.copy(p_dss.past_epoch_buffer);
			future_epoch_buffer.copy(p_dss.future_epoch_buffer);
		}
		DeferredSyncInterpolationData &operator=(const DeferredSyncInterpolationData &p_dss) {
			nd = p_dss.nd;
			past_epoch_buffer.copy(p_dss.past_epoch_buffer);
			future_epoch_buffer.copy(p_dss.future_epoch_buffer);
			past_epoch = p_dss.past_epoch;
			future_epoch = p_dss.future_epoch;
			alpha_advacing_per_epoch = p_dss.alpha_advacing_per_epoch;
			alpha = p_dss.alpha;
			return *this;
		}

		DeferredSyncInterpolationData(
				NS::ObjectData *p_nd) :
				nd(p_nd) {}
		DeferredSyncInterpolationData(
				NS::ObjectData *p_nd,
				DataBuffer p_past_epoch_buffer,
				DataBuffer p_future_epoch_buffer) :
				nd(p_nd),
				past_epoch_buffer(p_past_epoch_buffer),
				future_epoch_buffer(p_future_epoch_buffer) {}
		bool operator==(const DeferredSyncInterpolationData &o) const { return nd == o.nd; }
	};
	LocalVector<DeferredSyncInterpolationData> deferred_sync_array;

public:
	ClientSynchronizer(SceneSynchronizerBase *p_node);

	virtual void clear() override;

	virtual void process() override;
	virtual void on_object_data_added(NS::ObjectData *p_object_data) override;
	virtual void on_object_data_removed(NS::ObjectData &p_object_data) override;
	virtual void on_variable_changed(NS::ObjectData *p_object_data, VarId p_var_id, const Variant &p_old_value, int p_flag) override;
	void signal_end_sync_changed_variables_events();
	virtual void on_controller_reset(NS::ObjectData *p_object_data) override;

	void receive_snapshot(DataBuffer &p_snapshot);
	bool parse_sync_data(
			DataBuffer &p_snapshot,
			void *p_user_pointer,
			void (*p_custom_data_parse)(void *p_user_pointer, VarData &&p_custom_data),
			void (*p_ode_parse)(void *p_user_pointer, NS::ObjectData *p_object_data),
			void (*p_input_id_parse)(void *p_user_pointer, uint32_t p_input_id),
			void (*p_controller_parse)(void *p_user_pointer, NS::ObjectData *p_object_data),
			void (*p_variable_parse)(void *p_user_pointer, NS::ObjectData *p_object_data, VarId p_var_id, const Variant &p_value),
			void (*p_node_activation_parse)(void *p_user_pointer, NS::ObjectData *p_object_data, bool p_is_active));

	void set_enabled(bool p_enabled);

	void receive_deferred_sync_data(const Vector<uint8_t> &p_data);
	void process_received_deferred_sync_data(real_t p_delta);

	void remove_node_from_deferred_sync(NS::ObjectData *p_object_data);

private:
	/// Store node data organized per controller.
	void store_snapshot();

	void store_controllers_snapshot(
			const NS::Snapshot &p_snapshot,
			std::deque<NS::Snapshot> &r_snapshot_storage);

	void process_simulation(real_t p_delta, real_t p_physics_ticks_per_second);

	void process_received_server_state(real_t p_delta);

	bool __pcr__fetch_recovery_info(
			const uint32_t p_input_id,
			NS::Snapshot &r_no_rewind_recover);

	void __pcr__sync__rewind();

	void __pcr__rewind(
			real_t p_delta,
			const uint32_t p_checkable_input_id,
			NS::ObjectData *p_local_controller_node,
			NetworkedControllerBase *p_controller,
			PlayerController *p_player_controller);

	void __pcr__sync__no_rewind(
			const NS::Snapshot &p_postponed_recover);

	void __pcr__no_rewind(
			const uint32_t p_checkable_input_id,
			PlayerController *p_player_controller);

	void process_paused_controller_recovery(real_t p_delta);
	bool parse_snapshot(DataBuffer &p_snapshot);

	void notify_server_full_snapshot_is_needed();

	void update_client_snapshot(NS::Snapshot &p_snapshot);
	void apply_snapshot(
			const NS::Snapshot &p_snapshot,
			int p_flag,
			LocalVector<String> *r_applied_data_info,
			bool p_skip_custom_data = false);
};

/// This is used to make sure we can safely convert any `BaseType` defined by
// the user to `void*`.
template <class BaseType, class NetInterfaceClass>
class SceneSynchronizer : public SceneSynchronizerBase {
	NetInterfaceClass custom_network_interface;

public:
	SceneSynchronizer() :
			SceneSynchronizerBase(&custom_network_interface) {}

	NetInterfaceClass &get_network_interface() {
		return custom_network_interface;
	}

	const NetInterfaceClass &get_network_interface() const {
		return custom_network_interface;
	}

	static ObjectHandle to_handle(const BaseType *p_app_object) {
		return { reinterpret_cast<std::intptr_t>(p_app_object) };
	}

	static BaseType *from_handle(ObjectHandle p_app_object_handle) {
		return reinterpret_cast<BaseType *>(p_app_object_handle.id);
	}
};

NS_NAMESPACE_END
