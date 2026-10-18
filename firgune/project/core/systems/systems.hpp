#pragma once

#include <filesystem>
#include <thread>
#include <utilities/proto/proto.hpp>
#include <core/settings.hpp>

namespace systems {

	class schemas
	{
	public:
		[[nodiscard]] static std::uint32_t lookup( const char* class_name, std::uint32_t field_hash );
	};

	class materials
	{
	public:
		enum class clone_type : int { translucent, ignorez };

		[[nodiscard]] static bool initialize( );
		[[nodiscard]] static std::uintptr_t find( settings::esp::cham_ids id );
		[[nodiscard]] static const char* get_texture_path( std::uintptr_t entry );
		[[nodiscard]] static std::string emit_translucent_kv( std::uintptr_t src_mat );
		[[nodiscard]] static std::string emit_ignorez_kv( std::uintptr_t src_mat );
		[[nodiscard]] static std::uintptr_t load( const char* vmat_data, const char* name );

		[[nodiscard]] static std::uintptr_t get_or_create_clone( std::uintptr_t src_mat, clone_type type = clone_type::translucent );
		static void clear_clones( );
		static void set_material_vec3( std::uintptr_t mat, const char* param_name, float x, float y, float z );

	private:
		static inline std::array<std::uintptr_t, static_cast< std::size_t >( settings::esp::cham_ids::count )> m_loaded{};
		static inline std::unordered_map<std::uint64_t, std::uintptr_t> m_map{};
		static inline std::vector<cstypes::strong_handle> m_handles{};
		static inline std::mutex m_mtx{};
	};

	class events
	{
	public:
		using handler_fn = void( __fastcall* )( void* event );

		static bool initialize( );
		static void shutdown( );

		static bool register_listener( const char* event_name, handler_fn handler );
		static void unregister_listener( const char* event_name );

	private:
		struct listener
		{
			void** vtable;
			int debug_id;
		};

		struct entry
		{
			listener listener;
			void* vtable_data[ 3 ];
			handler_fn handler;
			const char* name;
			bool registered;
		};

		static void* __fastcall fire_event( void* self, void* event );
		static int __fastcall get_debug_id( void* self );
		inline static std::vector<std::unique_ptr<entry>> m_listeners{};
	};

	class input
	{
	public:
		struct in_button_state
		{
			void* vtable;
			std::uintptr_t value;
			std::uintptr_t value_changed;
			std::uintptr_t value_scroll;
		};

		struct usercmd
		{
			void* vtable;
			std::int32_t command_number;
			char command_number_padding[ 0x4 ];
			void* proto_vtable;
			void* proto_arena;
			proto::csgo_usercmd_pb csgo_user_cmd;
			in_button_state buttons;
			char pad1[ 0x8 ];
			double last_server_time;
			int flag;
			bool has_been_predicted;
			char prediction_padding[ 0x3 ];
			int previous_command_type;
			int command_type;
		};

		static_assert( sizeof( usercmd ) == 0x98 );
		static_assert( offsetof( usercmd, command_number ) == 0x8 );
		static_assert( offsetof( usercmd, csgo_user_cmd ) == 0x20 );
		static_assert( offsetof( usercmd, buttons ) == 0x58 );
		static_assert( offsetof( usercmd, last_server_time ) == 0x80 );
		static_assert( offsetof( usercmd, flag ) == 0x88 );
		static_assert( offsetof( usercmd, has_been_predicted ) == 0x8c );
		static_assert( offsetof( usercmd, previous_command_type ) == 0x90 );
		static_assert( offsetof( usercmd, command_type ) == 0x94 );

		struct input_history_params
		{
			math::vector3 view_angles{};
			math::vector3 shoot_position{};
			int render_tick{};
			float render_frac{};
			int player_tick{};
			float player_frac{};
			int frame_number{};
			int target_ent_index{ -1 };

			float cl_interp_frac{};
			int sv_interp0_src{};
			int sv_interp0_dst{};
			float sv_interp0_frac{};
			int sv_interp1_src{};
			int sv_interp1_dst{};
			float sv_interp1_frac{};
			int player_interp_src{};
			int player_interp_dst{};
			float player_interp_frac{};

			math::vector3 target_head_pos{};
			math::vector3 target_abs_pos{};
			math::vector3 target_abs_ang{};
			bool fill_cheat_check_data{};
		};

		void update( usercmd* command );
		void prepare_subtick_movement( );
		void apply( );

		[[nodiscard]] usercmd* get( ) const { return this->m_current_cmd; }
		[[nodiscard]] proto::subtick_move_step* acquire_subtick_step( proto::repeated_ptr_field<proto::subtick_move_step>* subtick_moves ) const;
		[[nodiscard]] math::vector3 get_view_angles( ) const;
		[[nodiscard]] proto::input_history_entry* push_input_history( usercmd* cmd, const input_history_params& params ) const;

		void set_view_angles( const math::vector3& angles ) const;
		void desubtick( usercmd* cmd ) const;

		[[nodiscard]] usercmd* get_command_by_sequence( std::uintptr_t local_controller, int sequence ) const;

	private:
		usercmd* m_current_cmd{};
		proto::base_usercmd_pb m_backup{};

		bool calculate_crc( proto::base_usercmd_pb* base ) const;
	};

	class legit_input
	{
	public:
		void add_mouse_delta( float pitch_degrees, float yaw_degrees );
		void on_process_input_event( std::uintptr_t csgo_input, int slot );

	private:
		float m_pending_pitch{};
		float m_pending_yaw{};
	};

	class entities
	{
	public:
		enum class type : std::uint8_t
		{
			unknown,
			player,
			item,
			projectile
		};

		struct cached
		{
			std::uintptr_t ptr{};
			std::uint32_t schema_hash{};
			std::int16_t index{};
			type type{};
		};

		void on_add_entity( std::uintptr_t entity, std::uint32_t handle );
		void on_remove_entity( std::uintptr_t entity, std::uint32_t handle );
		void force_update( );

		// 快照语义：写端（实体增删钩子）在锁内 copy-on-write，
		// 读端取一次 shared_ptr 后无锁遍历，免去每帧按类型拷贝分配。
		using snapshot_t = std::shared_ptr<const std::vector<cached>>;

		[[nodiscard]] snapshot_t snapshot( ) const;
		[[nodiscard]] bool find_by_ptr( std::uintptr_t entity, cached& out ) const;
		[[nodiscard]] bool exists( std::uintptr_t entity ) const;
		[[nodiscard]] const char* get_schema_name( std::uintptr_t entity ) const;
		[[nodiscard]] std::uintptr_t get_by_index( int index );
		[[nodiscard]] std::uintptr_t lookup( std::uint32_t handle ) const;
		[[nodiscard]] std::vector<cached> get_by_type( type type ) const;

		[[nodiscard]] bool is_empty( ) const;

	private:
		[[nodiscard]] type classify_entity( std::uint32_t schema_hash ) const;

		snapshot_t m_cached{ std::make_shared<const std::vector<cached>>( ) };
		mutable std::shared_mutex m_cache_mtx{};
		std::array<std::uintptr_t, 32> m_cached_list_entries{};
		std::uintptr_t m_cached_entity_list{};
	};

	class local
	{
	public:
		struct snapshot
		{
			std::uintptr_t controller{};
			std::uintptr_t pawn{};
			std::uintptr_t observer_pawn{};
			std::uintptr_t observer_controller{};
			int team{};
			int view_team{};
			bool is_alive{};
			bool is_team_mode{};

			[[nodiscard]] std::uintptr_t view_controller( ) const { return this->is_alive ? this->controller : this->observer_controller; }
			[[nodiscard]] std::uintptr_t view_pawn( ) const { return this->is_alive ? this->pawn : this->observer_pawn; }
			[[nodiscard]] bool is_valid( ) const { return this->pawn != 0 || this->observer_pawn != 0; }
			[[nodiscard]] bool is_this_other_team( int other_team ) const { return !this->is_team_mode || this->view_team != other_team; }
		};

		void update( );

		[[nodiscard]] snapshot get( ) const
		{
			std::shared_lock lock( this->m_mtx );
			return this->m_snapshot;
		}

		[[nodiscard]] bool is_in_cinematic( ) const { return this->m_is_in_cinematic.load( ); }
		[[nodiscard]] bool is_in_time_freeze( ) const { return this->m_is_in_time_freeze.load( ); }
		[[nodiscard]] bool is_in_deathmatch( ) const { return this->m_is_deathmatch.load( ); }

		// made this public, to be called by level_shutdown
		void reset( );

	private:
		snapshot m_snapshot{};
		mutable std::shared_mutex m_mtx{};

		std::atomic<bool> m_is_deathmatch{};
		std::atomic<bool> m_is_in_cinematic{};
		std::atomic<bool> m_is_in_time_freeze{};
	};

	class prediction
	{
	public:
		struct state
		{
			std::uint32_t flags{};
			math::vector3 networked_velocity{};
			math::vector3 velocity{};
			math::vector3 origin{};
			math::vector3 networked_origin{};
			math::vector3 last_movement_impulses{};
			math::vector3 last_movement_forward{};
			math::vector3 last_movement_left{};
			std::uint32_t last_command_number_processed{};
			float surface_friction{};
			float stamina{};
		};

		void capture_prestate( std::uintptr_t local_pawn, std::uintptr_t movement_services );
		bool simulate( input::usercmd* cmd, const systems::local::snapshot& local, const std::function<void( )>& fn );

		[[nodiscard]] const state& pre( ) const { return this->m_prestate; }
		[[nodiscard]] bool is_simulating( ) const { return m_simulating; }
		[[nodiscard]] bool landing_observation_ready( ) const { return this->m_landing_observation_ready.load( std::memory_order_acquire ); }
		[[nodiscard]] bool landing_observed( ) const { return m_trial_landed; }
		[[nodiscard]] bool landing_duck_held( ) const { return m_trial_duck_held; }
		[[nodiscard]] bool is_simulating_pawn( std::uintptr_t pawn ) const { return m_simulating && pawn == m_trial_pawn; }
		void observe_landing( std::uintptr_t pawn, bool duck_held )
		{
			if ( !this->is_simulating_pawn( pawn ) ) return;
			m_trial_landed = true;
			m_trial_duck_held |= duck_held;
		}
		void set_landing_observation_ready( bool ready ) { this->m_landing_observation_ready.store( ready, std::memory_order_release ); }
		void set_animation_isolation_ready( bool ready )
		{
			this->m_animation_isolation_ready.store( ready, std::memory_order_release );
		}

	private:
		state m_prestate{};
		std::mutex m_simulation_mtx{};
		inline static thread_local bool m_simulating{};
		inline static thread_local std::uintptr_t m_trial_pawn{};
		inline static thread_local bool m_trial_landed{};
		inline static thread_local bool m_trial_duck_held{};
		std::atomic_bool m_landing_observation_ready{};
		std::atomic_bool m_animation_isolation_ready{};
	};

	class view
	{
	public:
		struct projection
		{
			math::vector2 screen{};
			float w{};
			bool on_screen{};
		};

		void update( std::uintptr_t view );
		void update_matrix( );

		[[nodiscard]] math::vector2 project( const math::vector3& world_pos );
		[[nodiscard]] projection project_full( const math::vector3& world_pos ) const;
		[[nodiscard]] bool projection_valid( const math::vector2& screen_pos ) { return screen_pos.x != this->k_invalid && screen_pos.y != this->k_invalid; }

		[[nodiscard]] bool has_camera( ) const { return this->m_matrix_valid.load( std::memory_order_acquire ); }
		// Read-only snapshot for clipping world-space overlay geometry.
		[[nodiscard]] math::matrix4x4 matrix( ) const;
		[[nodiscard]] math::vector3 origin( ) const { return this->m_origin; }
		[[nodiscard]] math::vector3 angles( ) const { return this->m_angles; }
		[[nodiscard]] float fov( ) const { return this->m_fov; }

	private:
		static constexpr auto k_invalid{ 0xdead };

		// 无锁双槽矩阵：写端（stage 12）写非活跃槽后原子换槽，
		// 读端（渲染线程）按 acquire 序取槽拷贝，全程无互斥锁。
		std::array<math::matrix4x4, 2> m_matrix_slots{};
		std::atomic<std::uint32_t> m_matrix_slot{ 0 };
		std::atomic<bool> m_matrix_valid{};
		math::vector3 m_origin{};
		math::vector3 m_angles{};
		float m_fov{ k_invalid };
	};

	class bones
	{
	public:
		struct data
		{
			math::vector3 position{};
			float scale{};
			math::quaternion rotation{};
		};

		[[nodiscard]] data get( std::uintptr_t entity, std::uint32_t bone_id );
		[[nodiscard]] std::array<data, 27> get_skeleton( std::uintptr_t entity );

	private:
		[[nodiscard]] std::uintptr_t get_bone_cache( std::uintptr_t entity, int* out_count = nullptr );
	};

	class bounds
	{
	public:
		struct data
		{
			math::vector2 min{};
			math::vector2 max{};
			bool valid{};

			[[nodiscard]] float width( ) const { return this->max.x - this->min.x; }
			[[nodiscard]] float height( ) const { return this->max.y - this->min.y; }
			[[nodiscard]] math::vector2 center( ) const { return { this->min.x + this->width( ) * 0.5f, this->min.y + this->height( ) * 0.5f }; }
		};

		[[nodiscard]] data get( std::uintptr_t entity );
	};

	class hitboxes
	{
	public:
		struct entry
		{
			int index{ -1 };
			int bone{ -1 };
			math::vector3 mins{};
			math::vector3 maxs{};
			float radius{};
			std::uint8_t shape_type{};
			bool translation_only{};
		};

		struct set
		{
			std::array<entry, 20> entries{};
			int count{};

			[[nodiscard]] const entry* begin( ) const { return this->entries.data( ); }
			[[nodiscard]] const entry* end( ) const { return this->entries.data( ) + this->count; }
		};

		[[nodiscard]] set query( std::uintptr_t game_scene_node, bool seh = false );
		[[nodiscard]] int hitgroup_from_hitbox( int hitbox );
		[[nodiscard]] const char* hitgroup_to_name( int hitgroup );
	};

	class tracing
	{
	public:
		struct filter
		{
			std::uintptr_t vtable;
			std::uintptr_t mask;
			std::array<std::int64_t, 2> v1;
			std::array<int, 4> skip_handles;
			std::array<std::int16_t, 2> collisions;
			std::int16_t v2;
			std::uint8_t layer;
			std::uint8_t flags;
			std::uint8_t v5;
			std::uint8_t v6;
			std::byte pad0[ 0x6 ];
			char v7;
		};

		struct ray
		{
			math::vector3 mins;
			math::vector3 maxs;
			std::byte pad0[ 0x10 ];
			std::uint8_t type;
			std::byte pad1[ 0x7 ];
		};

		struct result
		{
			void* surface;
			std::uintptr_t hit_entity;
			void* hitbox_data;
			std::byte pad0[ 0x38 ];
			std::uint32_t contents;
			std::byte pad1[ 0x24 ];
			math::vector3 start_pos;
			math::vector3 end_pos;
			math::vector3 normal;
			math::vector3 position;
			std::byte pad2[ 0x4 ];
			float fraction;
			std::byte pad3[ 0x6 ];
			bool all_solid;
			std::byte pad4[ 0x4d ];
		};

		struct trace_array_element
		{
			std::byte pad0[ 0x38 ];
		};

		struct trace_data
		{
			int unknown1;
			float unknown2{ 52.0f };
			void* array_pointer;
			int unknown3{ 128 };
			int unknown4{ static_cast< int >( 0x80000000 ) };
			std::array<trace_array_element, 0x80> elements;
			std::byte pad0[ 0x8 ];
			int num_hits;
			int unknown5;
			void* hit_array_pointer;
			int hit_capacity{ 8 };
			int hit_flags{ static_cast< int >( 0x80000000 ) };
			std::byte hit_elements[ 0xc0 ];
			math::vector3 start;
			math::vector3 direction;
			float fraction{ 1.0f };
			bool unknown6;
			std::byte pad1[ 0x4b ];
		};

		static_assert( offsetof( trace_data, elements ) == 0x18 );
		static_assert( offsetof( trace_data, num_hits ) == 0x1c20 );
		static_assert( offsetof( trace_data, hit_array_pointer ) == 0x1c28 );
		static_assert( offsetof( trace_data, start ) == 0x1cf8 );
		static_assert( sizeof( trace_data ) == 0x1d60 );

		struct bbox_collision
		{
			math::vector3 mins;
			math::vector3 maxs;
		};

		[[nodiscard]] bool is_visible( const math::vector3& start, const math::vector3& end, std::uintptr_t target_entity, std::uintptr_t skip_entity, std::uintptr_t mask = 0x1c3003 ) const;

		[[nodiscard]] result trace( const math::vector3& start, const math::vector3& end, std::uintptr_t skip_entity, std::uintptr_t mask = 0x1c3003, std::uint8_t layer = 4 ) const;
		[[nodiscard]] result trace( const math::vector3& start, const math::vector3& end, const filter& filter ) const;
		[[nodiscard]] result trace_hull( const math::vector3& start, const math::vector3& end, const math::vector3& mins, const math::vector3& maxs, std::uintptr_t skip_entity, std::uintptr_t mask = 0x1c3003, std::uint8_t layer = 4 ) const;
		[[nodiscard]] result trace_hull( const math::vector3& start, const math::vector3& end, const math::vector3& mins, const math::vector3& maxs, const filter& filter ) const;
		[[nodiscard]] result trace_sphere( const math::vector3& start, const math::vector3& end, float radius, const filter& filter ) const;
		[[nodiscard]] result trace_to_entity( const math::vector3& start, const math::vector3& end, std::uintptr_t target_entity, std::uintptr_t skip_entity, std::uintptr_t mask = 0x1c3003, std::uint8_t layer = 4 ) const;
		[[nodiscard]] result trace_to_entity( const math::vector3& start, const math::vector3& end, std::uintptr_t target_entity, const filter& filter ) const;
		[[nodiscard]] filter make_filter( std::uintptr_t skip_entity, std::uintptr_t mask, std::uint8_t layer, int type ) const;
		[[nodiscard]] filter make_filter( std::uintptr_t skip_entity, std::uintptr_t mask, std::uint8_t layer ) const;
		[[nodiscard]] tracing::result trace_player_bbox( const math::vector3& start, const math::vector3& end, const bbox_collision& bbox, const filter& filter ) const;

		void setup_trace( trace_data* trace_data, const math::vector3& start, const math::vector3& delta, const filter& filter, int penetration_count, bool trace_world = false ) const;
		void init_result( result* trace_result ) const;
		void finalize_trace( trace_data* trace_data, result* trace_result, float unknown_float, void* unknown ) const;
	};

	class frame_data
	{
	public:
		void update( );

		[[nodiscard]] math::vector3 origin( ) const { return this->m_origin; }
		[[nodiscard]] bool valid( ) const { return this->m_valid; }

	private:
		void reset( );

		math::vector3 m_origin{};
		bool m_valid{};
	};

	class icons
	{
	public:
		struct icon
		{
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture{};
			int width{};
			int height{};
		};

		bool initialize( );
		void shutdown( );

		[[nodiscard]] const icon* get( const std::string& name, float scale = 1.0f );
		[[nodiscard]] const icon* get( std::uint32_t schema_hash, float scale = 1.0f );

	private:
		bool load_vpk_directory( const std::filesystem::path& path );
		bool cache_svg_bytes( const std::filesystem::path& vpk_path, const std::string& icon_name, std::uint32_t entry_offset, std::uint32_t entry_length );
		std::vector<std::byte> decompile_vsvg( std::span<const std::byte> data ) const;

		[[nodiscard]] const icon* get_impl( std::uint32_t hash, const std::string& name, float scale );

		// 键以哈希替代字符串：图标名在游戏会话内不变，
		// 每帧查找不再构造/拷贝 std::string 键。
		struct icon_key
		{
			std::uint32_t hash{};
			std::uint32_t scale_bits{};

			bool operator==( const icon_key& o ) const noexcept { return this->scale_bits == o.scale_bits && this->hash == o.hash; }
		};

		struct icon_key_hash
		{
			std::size_t operator()( const icon_key& k ) const noexcept { return std::hash<std::uint32_t>{}( k.hash ) ^ ( std::hash<std::uint32_t>{}( k.scale_bits ) << 1 ); }
		};

		std::unordered_map<icon_key, icon, icon_key_hash> m_icons{};
		std::unordered_map<std::uint32_t, std::string> m_hash_to_name{};
		std::unordered_map<std::string, std::vector<std::byte>> m_pending_svgs{};
	};

	// 账号数据：注入器经共享内存交付（用户名/机器码/access_token），
	// 后台线程周期复验 user_status.php 刷新订阅并探测服务器失联。
	// 失联（含启动即无映射）仅供显示层降级，不做任何功能门禁。
	class account
	{
	public:
		enum class state : int { uninitialized, linked, lost };

		struct snapshot
		{
			state status{ state::uninitialized };
			std::string username{};
			std::string sub_until{};
		};

		bool initialize( );
		void shutdown( );

		[[nodiscard]] snapshot get( ) const;

	private:
		void worker( );
		[[nodiscard]] bool revalidate( );
		bool on_failure( );

		bool m_channel{ false };
		std::string m_username{};
		std::string m_hwid{};
		std::string m_access_token{};

		mutable std::mutex m_mtx{};
		snapshot m_snap{};
		std::thread m_thread{};
		std::atomic<bool> m_stop{ false };
		int m_failures{ 0 };
	};

	inline input g_input{};
	inline legit_input g_legit_input{};
	inline entities g_entities{};
	inline local g_local{};
	inline prediction g_prediction{};
	inline account g_account{};
	inline view g_view{};
	inline bones g_bones{};
	inline bounds g_bounds{};
	inline hitboxes g_hitboxes{};
	inline tracing g_tracing{};
	inline frame_data g_frame_data{};
	inline icons g_icons{};

} // namespace systems

#define SCHEMA( class_name, field_hash ) \
	[]( ) -> int { \
		static const auto val = systems::schemas::lookup( class_name, field_hash ); \
		return val; \
	}( )
