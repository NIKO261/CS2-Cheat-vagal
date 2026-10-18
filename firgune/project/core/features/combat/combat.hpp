#pragma once

#include <core/systems/systems.hpp>

namespace features::combat {

	// Per-command ragebot diagnostics. Disabled by default: flip this constant,
	// rebuild Development|x64 and the aggregate is written to vagal_init.log.
	// It adds no menu item, config key or Lua surface, and compiles out when off.
	inline constexpr bool k_rage_diagnostics{ false };

	// Why a shot was released. Selection keeps the measured threshold result
	// separate; the stop/fire path owns this reason.
	enum class fire_reason : std::uint8_t
	{
		none,
		hitchance,
		force,
		double_tap,
		no_spread
	};

	// Aggregated over a bounded histogram instead of keeping per-command history.
	// Workers touch only their own counter slot; the caller merges in a fixed
	// order after the parallel scan finishes.
	struct rage_stats
	{
		enum class no_fire_reason : std::uint8_t
		{
			none,
			disabled,
			no_candidates,
			no_hits,
			no_best,
			cooldown,
			prediction_failed,
			not_settled,
			low_hitchance,
			visual_only,
			scope,
			submit_failed
		};

		struct counters
		{
			int enemies{};
			int records{};
			int eyes{};
			int points{};
			int penetration_calls{};
			int hitchance_points{};
			int hitchance_samples{};
		};

		struct timings
		{
			std::uint64_t prediction_ns{};
			std::uint64_t prepare_ns{};
			std::uint64_t scan_ns{};
			std::uint64_t hitchance_ns{};
			std::uint64_t select_ns{};
			std::uint64_t total_ns{};
		};

		void begin( int command_number );
		void finish( );
		void submit( fire_reason reason );
		void set_mode( std::uint32_t mode ) noexcept { this->m_mode = mode; }
		void set_config_hash( std::uint32_t hash ) noexcept { this->m_config_hash = hash; }
		void repredicted( ) noexcept { ++this->m_brake_repredictions; }
		void rescanned( ) noexcept { ++this->m_rescans; }
		void set_reason( no_fire_reason reason ) noexcept { this->m_reason = reason; }

		[[nodiscard]] counters& work( ) noexcept { return this->m_work; }
		[[nodiscard]] timings& time( ) noexcept { return this->m_time; }
		void add( const counters& other ) noexcept;

	private:
		int m_command{ -1 };
		std::uint32_t m_mode{};
		std::uint32_t m_config_hash{};
		counters m_work{};
		timings m_time{};
		int m_brake_repredictions{};
		int m_rescans{};
		no_fire_reason m_reason{ no_fire_reason::none };
		fire_reason m_fire_reason{ fire_reason::none };
		bool m_submitted{};
		std::uint64_t m_start_ns{};
	};

	inline std::uint64_t steady_now_ns( )
	{
		return static_cast<std::uint64_t>( std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( ) );
	}

	// Adds the elapsed time to one phase bucket; elided when diagnostics are off.
	class stats_timer
	{
	public:
		explicit stats_timer( std::uint64_t& target ) : m_target( target )
		{
			if constexpr ( k_rage_diagnostics ) this->m_start = steady_now_ns( );
		}
		~stats_timer( )
		{
			if constexpr ( k_rage_diagnostics ) this->m_target += steady_now_ns( ) - this->m_start;
		}
		stats_timer( const stats_timer& ) = delete;
		stats_timer& operator=( const stats_timer& ) = delete;

	private:
		std::uint64_t& m_target;
		std::uint64_t m_start{};
	};

	enum class autostop_mode : std::uint8_t
	{
		none,
		ground,
		air,
		landing
	};

	struct autostop_landing_prediction
	{
		int ticks{};
		float fraction{};
		math::vector3 origin{};
	};

	class shared
	{
	public:
		class lagcomp
		{
		public:
			struct record
			{
				std::uintptr_t pawn{};
				std::uintptr_t game_scene_node{};
				std::uintptr_t bone_cache{};
				int bone_count{};

				systems::bones::data bones[ 128 ]{};
				systems::bones::data bones_backup[ 128 ]{};

				math::vector3 origin{};
				math::vector3 rotation{};

				float simulation_time{};
				int tick{};
				bool valid{};
				bool was_valid{};
				bool is_applied{};
				bool extrapolated{};

				bool setup( std::uintptr_t pawn );
				[[nodiscard]] bool is_valid( ) const;

				void apply( );
				void restore( );
			};

			struct visual_record
			{
				math::vector3 origin{};
				std::array<systems::bones::data, 27> bones{};
			};

			struct extrapolation_data
			{
				math::vector3 origin{};
				math::vector3 velocity{};
				math::vector3 obb_mins{};
				math::vector3 obb_maxs{};
				std::uint32_t flags{};
				float sim_time{};
				float direction{};
			};

			void run( );

			[[nodiscard]] record* get_oldest_valid( std::uintptr_t pawn );
			[[nodiscard]] record* get_oldest_was_valid( std::uintptr_t pawn );
			[[nodiscard]] std::optional<visual_record> get_oldest_was_valid_visual( std::uintptr_t pawn ) const;
			[[nodiscard]] std::vector<record*> get_valid_records( std::uintptr_t pawn );
			[[nodiscard]] std::vector<record> get_valid_record_copies( std::uintptr_t pawn ) const;
			[[nodiscard]] std::array<systems::bones::data, 27> get_skeleton( const record& record ) const;

			[[nodiscard]] std::optional<record> extrapolate( std::uintptr_t pawn );


		private:
			void predict_movement( extrapolation_data& data, std::uintptr_t skip_entity, float gravity ) const;

			std::unordered_map<std::uintptr_t, std::deque<record>> m_records{};
			mutable std::shared_mutex m_records_mtx{};
		};

		class penetration
		{
		public:
			struct weapon_data
			{
				float damage;
				float penetration;
				float range_modifier;
				float range;
				float armor_ratio;
				float headshot_multiplier;
			};

			struct damage_scales
			{
				float ct_head;
				float t_head;
				float ct_body;
				float t_body;
			};

			// World-space hitbox geometry precomputed once per target record:
			// the scan evaluates up to ~95 rays per record and re-deriving
			// capsule endpoints / oriented-box frames per ray dominated the
			// per-point CPU cost with many players.
			struct world_hitbox
			{
				int index{ -1 };
				float radius{};
				math::vector3 capsule_start{};
				math::vector3 capsule_end{};
				math::quaternion rotation{};
				math::vector3 position{};
				math::vector3 mins{};
				math::vector3 maxs{};
				bool capsule{};
			};

			struct run_context
			{
				std::uintptr_t target_pawn{};
				int target_armor{};
				int target_team{};
				bool has_helmet{};
				damage_scales scales{};
				float armor_ratio{};
				float headshot_multiplier{};
				systems::hitboxes::set hitboxes{};
				std::array<world_hitbox, 20> world{};
				int world_count{};
				lagcomp::record* record{};
			};

			struct result
			{
				float damage{};
				int hitbox{ -1 };
				int hitgroup{ -1 };
				bool penetrated{};
			};

			void prepare( std::uintptr_t weapon_vdata, std::uintptr_t weapon );

			[[nodiscard]] run_context prepare_target( std::uintptr_t target_pawn, lagcomp::record* record ) const;
			[[nodiscard]] bool run( const math::vector3& start, const math::vector3& end, const run_context& ctx, std::uintptr_t local_pawn, int local_team, result& out ) const;
			// Exact scan pre-filter: resolves the ray against the precomputed
			// world hitboxes and returns the maximum damage the resolved
			// hitgroup could produce. The multipoint scan skips the engine
			// trace for points whose resolved hitbox cannot reach the damage
			// floor - run() would discard those points anyway.
			[[nodiscard]] float max_damage_for_ray( const math::vector3& start, const math::vector3& end, const run_context& ctx ) const;
			[[nodiscard]] bool can( const math::vector3& start, const math::vector3& direction, float& out_damage, const systems::local::snapshot& local ) const;
			[[nodiscard]] float get_max_damage( int hitgroup, int target_armor, bool has_helmet, int target_team ) const;
			[[nodiscard]] const weapon_data& get_weapon_data( ) const { return this->m_weapon_data; }

		private:
			struct trace_snapshot;

			[[nodiscard]] bool has_valid_weapon_data( ) const;
			[[nodiscard]] bool simulate_trace( const math::vector3& start, const math::vector3& direction, std::uintptr_t local_pawn, int local_team, lagcomp::record* record, trace_snapshot& out, float max_distance = 0.0f ) const;
			[[nodiscard]] bool record_hits_target( const trace_snapshot& trace, int record_index, std::uintptr_t target_pawn ) const;
			[[nodiscard]] int resolve_hitbox( const math::vector3& start, const math::vector3& ray_delta, const run_context& ctx ) const;
			void apply_player_damage( int hitgroup, int armor, bool has_helmet, int team, float armor_ratio, float headshot_multiplier, const damage_scales& scales, float& damage ) const;
			weapon_data m_weapon_data{};
			// Read once per tick in prepare() (main thread) - prepare_target
			// runs on worker threads and must not do convar lookups.
			damage_scales m_damage_scales{};
		};

		class shoot_history
		{
		public:
			struct ring_entry
			{
				int tick{};
				float fraction{};
				math::vector3 position{};
			};

			struct eye_candidate
			{
				math::vector3 position{};
				int player_tick{};
				float player_frac{};
				int lerp_ticks_int{};
				float lerp_ticks_frac{};
				bool is_uninterpolated{};
			};

			struct eye_candidates
			{
				eye_candidate entries[ 2 ]{};
				int count{};
			};

			void snapshot( std::uintptr_t local_pawn, std::uintptr_t weapon_services );
			[[nodiscard]] eye_candidates get_candidates( ) const;
			[[nodiscard]] int client_tick( ) const { return this->m_client_tick; }
			[[nodiscard]] int server_tick( ) const { return this->m_server_tick; }
			[[nodiscard]] int lerp_ticks_int( ) const { return this->m_lerp_ticks_int; }
			[[nodiscard]] float lerp_ticks_frac( ) const { return this->m_lerp_ticks_frac; }
			[[nodiscard]] int count( ) const { return this->m_count; }
			[[nodiscard]] int newest_tick( ) const { return this->m_count > 0 ? this->m_entries[ this->m_count - 1 ].tick : -1; }

		private:
			ring_entry m_entries[ 32 ]{};
			int m_count{};
			int m_client_tick{};
			float m_client_tick_frac{};
			int m_server_tick{};
			int m_lerp_ticks_int{};
			float m_lerp_ticks_frac{};
		};

		struct context
		{
			std::uintptr_t weapon{};
			std::uintptr_t weapon_services{};
			std::uintptr_t weapon_vdata{};
			std::uint32_t weapon_type{};
			std::uint16_t item_def_idx{};
			int num_bullets{};
			int weapon_mode{};
			int clip{};
			int next_primary_attack_tick{};
			int next_secondary_attack_tick{};
			float recoil_index{};
			float cycle_time{};
			int current_tick{};
			int ticks_since_land{};
			float current_time{};
			float weapon_max_speed{};
			float range{};
			bool is_jump_scouting{};
			bool is_scoped{};
			bool in_reload{};
			bool valid{};

			float inaccuracy{};
			float spread{};
		};

		struct shot_spread_state
		{
			float inaccuracy{};
			float spread{};
			float recoil_index{};
			int item_def_idx{};
			int num_bullets{};
			int weapon_mode{};
		};

		void update( );
		void invalidate_if_needed( );

		[[nodiscard]] context& ctx( ) { return this->m_ctx; }
		[[nodiscard]] penetration& pen( ) { return this->m_pen; }
		[[nodiscard]] lagcomp& lc( ) { return this->m_lc; }
		[[nodiscard]] shoot_history& sh( ) { return this->m_sh; }

		[[nodiscard]] bool autowalling( ) const { return this->m_autowalling; }
		[[nodiscard]] lagcomp::record* current_autowall_record( ) const { return this->m_current_autowall_record; }

		[[nodiscard]] int& last_shoot_tick( ) { return this->m_last_shoot_tick; }

		[[nodiscard]] std::uint32_t get_spread_seed( const math::vector3& angles, int tick ) const;
		[[nodiscard]] shot_spread_state capture_shot_spread_state( ) const;
		[[nodiscard]] math::vector2 calculate_spread( std::uint32_t seed, float accuracy, float spread, float recoil_index, int item_def_idx, int num_bullets, int weapon_mode = 0 ) const;
		[[nodiscard]] math::vector3 get_aim_punch( std::uintptr_t local_pawn ) const;
		// Incremental hit-chance evaluation. `planned` fixes the sample budget for
		// one point in one spread context; extending it only appends samples, so
		// the coarse ordering pass can be reused by the full verification.
		struct hitchance_result
		{
			int hits{};
			int evaluated{};
			int planned{};
			bool complete{};

			[[nodiscard]] float value( ) const noexcept
			{
				return this->planned > 0 ? static_cast<float>( this->hits ) / static_cast<float>( this->planned ) : 0.0f;
			}
			// All remaining samples missing/hit. These are hard bounds: a point
			// whose upper bound cannot reach the threshold is a definite failure.
			[[nodiscard]] float lower_bound( ) const noexcept { return this->value( ); }
			[[nodiscard]] float upper_bound( ) const noexcept
			{
				return this->planned > 0
					? static_cast<float>( this->hits + ( this->planned - this->evaluated ) ) / static_cast<float>( this->planned )
					: 0.0f;
			}
			[[nodiscard]] bool reaches( int percent ) const noexcept
			{
				return this->planned > 0 && this->hits * 100 >= percent * this->planned;
			}
			[[nodiscard]] bool can_reach( int percent ) const noexcept
			{
				return this->planned > 0 &&
					( this->hits + ( this->planned - this->evaluated ) ) * 100 >= percent * this->planned;
			}
		};

		// Extends `result` to `target_samples`, evaluating only the missing suffix.
		// The per-thread spread table is reused for every point in the same context.
		void advance_hitchance( const math::vector3& shoot_position, const math::vector3& aim_angle, const systems::hitboxes::entry& hitbox, const systems::bones::data& bone, float inaccuracy, float spread, hitchance_result& result, int target_samples ) const;
		[[nodiscard]] float calculate_hitchance( const math::vector3& shoot_position, const math::vector3& aim_angle, const systems::hitboxes::entry& hitbox, const systems::bones::data& bone, float inaccuracy, float spread, int samples = 256 ) const;
		[[nodiscard]] std::optional<math::vector3> find_spread_correction( const math::vector3& aim_angle, int tick, const math::vector3& punch, const shot_spread_state& state ) const;
		[[nodiscard]] math::vector3 get_eye_position( std::uintptr_t local_pawn ) const;
		[[nodiscard]] math::vector3 get_shoot_position( ) const;
		[[nodiscard]] math::vector3 get_interpolated_shoot_position( std::uintptr_t local_pawn, bool newest = false ) const;
		[[nodiscard]] float get_spread( ) const;
		[[nodiscard]] float get_inaccuracy( bool update_accuracy_penalty ) const;
		[[nodiscard]] float get_inaccuracy_at_velocity( std::uintptr_t local_pawn, const math::vector3& velocity ) const;
		[[nodiscard]] bool can_shoot( systems::input::usercmd* cmd, std::uintptr_t local_controller, bool check_next_attack = true ) const;
		[[nodiscard]] bool is_max_accuracy( float inaccuracy ) const;

		bool ray_vs_capsule( const math::vector3& ray_origin, const math::vector3& ray_dir, const math::vector3& capsule_a, const math::vector3& capsule_b, float radius, float& out_fraction ) const;

	private:
		context m_ctx{};
		penetration m_pen{};
		lagcomp m_lc{};
		shoot_history m_sh{};

		// Parallel rage workers must not overwrite each other's trace record.
		inline static thread_local bool m_autowalling{};
		inline static thread_local lagcomp::record* m_current_autowall_record{ nullptr };

		int m_last_shoot_tick{};
	};

	class misc
	{
	private:
		class antiaim
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void on_render( xdraw::draw_list& draw_list );

			[[nodiscard]] const math::vector3& get_modified_angles( ) const { return this->m_modified_angles; }
			[[nodiscard]] bool is_active( ) const { return this->m_antiaim_active; }

			// Brackets the engine input call: the first half captures the camera,
			// the second consumes its mouse delta and restores the captured view.
			void begin_process_input_event( std::uintptr_t csgo_input, int slot );
			void end_process_input_event( std::uintptr_t csgo_input, int slot );
			[[nodiscard]] bool get_steering_view( math::vector3& angles ) const;

		private:
			[[nodiscard]] float get_pitch( float view_pitch );
			[[nodiscard]] float get_yaw( const math::vector3& view_angles, const systems::local::snapshot& local );
			[[nodiscard]] float get_steering_yaw( );
			[[nodiscard]] bool steering_requested( ) const;
			void push_steering_trail_point( const math::vector2& cursor );
			void correct_movement( systems::input::usercmd* cmd );

			struct steering_trail_point
			{
				math::vector2 cursor{};
				float age{};
			};

			static constexpr std::size_t steering_trail_capacity{ 24 };

			math::vector3 m_old_angles{};
			math::vector3 m_modified_angles{};

			int m_yaw_side{};
			bool m_should_correct{};
			bool m_antiaim_active{};
			float m_indicator_yaw{};

			// Mouse steering keeps the active hold separate from the retained
			// world-space direction so releasing the key does not reset the yaw.
			math::vector2 m_steering_cursor{};
			float m_steering_frozen_pitch{};
			float m_steering_frozen_yaw{};
			float m_steering_last_yaw_offset{ 180.0f };
			float m_steering_yaw{};
			float m_steering_hold_travel{};
			std::array<steering_trail_point, steering_trail_capacity> m_steering_trail{};
			std::size_t m_steering_trail_count{};
			bool m_steering_engaged{};
			bool m_steering_hold_active{};
			bool m_steering_moved_this_hold{};
			bool m_steering_has_direction{};
		};

		class duckpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void on_override_view( std::uintptr_t view_setup );
		};

		class quickpeek
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			void reset_if_needed( );

		private:
			static constexpr std::uint32_t invalid_effect_index{ static_cast<std::uint32_t>( -1 ) };

			void create_particle( );
			void update_particle( );
			void release_particle( );
			void reset( );

			math::vector3 m_saved_origin{};
			bool m_should_retrack{};
			bool m_fired{};
			bool m_active{};
			std::uint32_t m_particle_effect{ invalid_effect_index };
			bool m_particle_loaded{};
			std::uintptr_t m_prev_movement_bits{};
		};

		class autostop
		{
		public:
			void on_create_move( systems::input::usercmd* cmd );
			// Shared input writer for this command's ground prediction, the air
			// pre-pass and final execution. All three use the same live velocity.
			void write_brake_input( systems::input::usercmd* cmd, autostop_mode mode );
			[[nodiscard]] bool request_landing( systems::input::usercmd* cmd, const systems::local::snapshot& local );
			[[nodiscard]] std::optional<autostop_landing_prediction> predict_landing( std::uintptr_t pawn );

		private:
			std::optional<autostop_landing_prediction> m_landing_prediction{};
			std::intptr_t m_landing_command{ -1 };
			// Cache to avoid recalculating landing prediction multiple times per frame
			std::optional<autostop_landing_prediction> m_cached_prediction{};
			std::intptr_t m_cache_command{ -1 };
		};

		antiaim m_antiaim{};
		duckpeek m_duckpeek{};
		quickpeek m_quickpeek{};
		autostop m_autostop{};

	public:
		[[nodiscard]] antiaim& antiaim( ) { return this->m_antiaim; }
		[[nodiscard]] duckpeek& duckpeek( ) { return this->m_duckpeek; }
		[[nodiscard]] quickpeek& quickpeek( ) { return this->m_quickpeek; }
		[[nodiscard]] autostop& autostop( ) { return this->m_autostop; }
	};

	class rage
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		// Runs after Double Tap finalized the command's subtick attack pulse.
		void finalize_no_spread_state( systems::input::usercmd* cmd, const systems::local::snapshot& local );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool should_stop( ) const noexcept { return this->m_stop_mode != autostop_mode::none; }
		[[nodiscard]] autostop_mode stop_mode( ) const noexcept { return this->m_stop_mode; }
		[[nodiscard]] bool is_firing_this_tick( ) const noexcept { return this->m_firing_this_tick; }
		[[nodiscard]] bool is_no_spread_state_prediction( ) const noexcept { return this->m_no_spread_state_prediction; }
		[[nodiscard]] bool is_cocking_revolver( ) const noexcept { return this->m_revolver_cock_ticks > 0; }
		[[nodiscard]] bool should_release_duck_for_shot( ) const noexcept { return this->m_release_duck_for_shot; }
		[[nodiscard]] bool landing_stop_active( ) const noexcept { return this->m_landing_stop_active; }

		static constexpr auto k_max_lagcomp_records{ 16 };
		static constexpr auto k_max_scan_records{ 5 };

		// The first shot keeps fire_gun's ordinary attack history. Only a later
		// command, after live ammo/cooldown advance, may submit the DT follow-up.
		// Rebuilding either command must not count as another discharged bullet.
		class double_tap
		{
		public:
			enum class charge_state : std::uint8_t
			{
				charging,
				ready,
				failed
			};

			enum class fail_reason : std::uint8_t
			{
				none,
				reloading,
				empty_clip,
				unavailable
			};

			void on_create_move( systems::input::usercmd* cmd );
			void on_shot_intent( systems::input::usercmd* cmd, bool fired );
			void finalize_command( systems::input::usercmd* cmd );
			void on_render( xdraw::draw_list& draw_list ) const;

			[[nodiscard]] charge_state state( ) const noexcept { return this->m_state; }
			[[nodiscard]] float progress( ) const noexcept { return this->m_progress; }
			[[nodiscard]] fail_reason last_fail( ) const noexcept { return this->m_fail_reason; }
			[[nodiscard]] bool second_pending( ) const noexcept
			{
				return ( this->m_phase == pair_phase::second_queued && this->m_command > this->m_first_command )
					|| ( this->m_phase == pair_phase::second_submitted && this->m_command == this->m_second_command );
			}
			[[nodiscard]] bool second_ready( ) const noexcept
			{
				return this->second_pending( ) && this->m_claim_valid && this->m_first_observed;
			}
			[[nodiscard]] bool bypass_cooldown( ) const noexcept { return this->second_ready( ); }
			[[nodiscard]] int attack_claim_tick( ) const noexcept { return this->second_ready( ) ? this->m_claim_tick : -1; }
			[[nodiscard]] bool can_pair( ) const noexcept
			{
				return this->m_supported && this->m_claim_valid &&
					( this->second_ready( ) || ( this->m_phase == pair_phase::idle && this->m_natural_ready && this->m_clip >= 2 )
						|| ( this->m_phase == pair_phase::second_queued && this->m_command == this->m_first_command ) );
			}

		private:
			enum class pair_phase : std::uint8_t
			{
				idle,
				second_queued,
				second_submitted
			};

			void reset( ) noexcept;
			void reset_pair( ) noexcept;
			[[nodiscard]] bool commit_pulse( systems::input::usercmd* cmd, int claim_tick ) const;

			charge_state m_state{ charge_state::charging };
			fail_reason m_fail_reason{ fail_reason::none };
			pair_phase m_phase{ pair_phase::idle };
			std::uintptr_t m_weapon{};
			std::intptr_t m_command{ -1 };
			std::intptr_t m_first_command{ -1 };
			std::intptr_t m_second_command{ -1 };
			std::intptr_t m_intent_command{ -1 };
			int m_tick_base{ -1 };
			int m_claim_tick{ -1 };
			int m_first_claim_tick{ -1 };
			int m_second_claim_tick{ -1 };
			int m_first_clip{};
			int m_pair_deadline{ -1 };
			int m_cycle_ticks{ 1 };
			int m_clip{};
			bool m_supported{};
			bool m_claim_valid{};
			bool m_natural_ready{};
			bool m_first_observed{};
			bool m_intent{};
			bool m_intent_is_second{};
			float m_progress{};
		} m_double_tap{};

		[[nodiscard]] double_tap& dt( ) noexcept { return this->m_double_tap; }
		[[nodiscard]] const double_tap& dt( ) const noexcept { return this->m_double_tap; }

	private:
		struct aim_context
		{
			math::vector3 view_angles{};
			math::vector3 velocity{};
			shared::shoot_history::eye_candidates eyes{};
			float predicted_inaccuracy{};
			float standing_inaccuracy{};
			float spread{};
			float weapon_max_speed{};
			float accurate_threshold{};
			bool on_ground{};
			bool is_scoped{};
			bool prediction_valid{};
			bool physically_settled{};
		};

		struct debug_point
		{
			math::vector3 position{};
			int hitbox_index{};
			bool is_center{};
		};

		// Read-only command snapshot built on the main thread. Workers read it
		// and never touch settings or target state again.
		struct scan_config
		{
			std::array<int, 19> hitbox_order{};
			std::array<bool, 6> multipoints{};
			int hitbox_count{};
			float pointscale{};
			float max_fov{};
			bool dynamic_pointscale{};
			bool debug_points{};
		};

		// View-independent part of one historical record: hitbox set plus the
		// world-space capsule/box geometry. Identical for every eye of the same
		// command, so it is derived once instead of once per viewpoint.
		struct prepared_record
		{
			shared::lagcomp::record* record{};
			shared::penetration::run_context penetration{};
			std::array<const systems::hitboxes::entry*, 19> hitboxes{};
			std::uint8_t ordinal{};
		};

		struct candidate
		{
			std::uintptr_t pawn{};
			int health{};
			int armor{};
			float min_damage{};
			std::array<shared::lagcomp::record*, k_max_scan_records> records{};
			std::array<prepared_record*, k_max_scan_records> prepared{};
			int record_count{};
			std::size_t ordinal{};
		};

		struct scan_hit
		{
			math::vector3 position{};
			math::vector3 aim_angle{};
			float damage{};
			float fov{};
			int hitbox_index{};
			int hitgroup{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool is_center{};
			bool penetrated{};
			bool is_backstab{};
			int attack_type{};
			shared::shoot_history::eye_candidate source_eye{};

			std::uintptr_t pawn{};
			int health{};
			shared::lagcomp::record* record{};
			std::size_t candidate_ordinal{};
			std::uint8_t record_ordinal{};
			int source_eye_index{};
			std::size_t point_ordinal{};
		};

		struct target
		{
			scan_hit hit{};
			float hitchance{};
			float expected_damage{};
			// Measured threshold result only. Force/DT/exemption decisions live in
			// fire_reason and the stop decision, never in this flag.
			bool passes_hitchance{};
			fire_reason reason{ fire_reason::none };
			bool valid{};

			[[nodiscard]] bool is_lethal( ) const noexcept
			{
				return this->hit.damage >= static_cast< float >( this->hit.health );
			}
		};

		struct stop_decision
		{
			autostop_mode mode{ autostop_mode::none };
			bool landing_follow_through{};
			bool ready_to_fire{};
			bool forced{};
		};

		struct knife_info
		{
			bool can_slash{};
			bool can_stab{};
			bool charged{};
			float armor_ratio{};
		};

		[[nodiscard]] aim_context build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const;
		[[nodiscard]] bool weapon_ready_to_fire( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const;
		[[nodiscard]] scan_config make_scan_config( const settings::combat::ragebot::weapon_group& config ) const;
		void prepare_scan_targets( std::vector<candidate>& candidates );
		[[nodiscard]] bool hittable_after_brake( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const;
		[[nodiscard]] bool hittable_after_landing( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const;
		[[nodiscard]] bool any_damage_from_eye( const math::vector3& eye, const aim_context& ctx, const scan_config& scan, const std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;
		[[nodiscard]] bool duckpeek_stand_viable( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const;
		[[nodiscard]] std::vector<candidate> gather_candidates( const systems::local::snapshot& local, float max_distance_sq = 0.0f ) const;
		[[nodiscard]] shared::shoot_history::eye_candidates resolve_eye_candidates( ) const;

		void run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire = true, bool visual_only = false );
		void run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		void auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local );
		[[nodiscard]] bool revolver_ready_to_fire( const systems::input::usercmd* cmd ) const;

		void scan_players( const math::vector3& eye, int eye_index, float inaccuracy, const aim_context& ctx, const scan_config& scan, std::vector<candidate>& candidates, const systems::local::snapshot& local, std::vector<scan_hit>& out, rage_stats* stats );
		void scan_player( const math::vector3& eye, int eye_index, float inaccuracy, const aim_context& ctx, const scan_config& scan, const candidate& cand, const prepared_record& prepared, const systems::local::snapshot& local, std::vector<scan_hit>& out, rage_stats::counters* counters, std::vector<debug_point>& debug ) const;
		[[nodiscard]] target select_best( const aim_context& aim_ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy, rage_stats* stats ) const;
		void advance_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy, shared::hitchance_result& result, int target_samples ) const;
		[[nodiscard]] bool cannot_precede( const scan_hit& hit, const shared::hitchance_result& chance, const target& incumbent, int required_percent ) const;
		[[nodiscard]] bool target_precedes( const target& lhs, const target& rhs ) const;
		[[nodiscard]] float required_hitchance( const settings::combat::ragebot::weapon_group& config ) const;
		[[nodiscard]] stop_decision decide_stop( systems::input::usercmd* cmd, const aim_context& ctx, const scan_config& scan, const target& best, std::vector<candidate>& candidates, const systems::local::snapshot& local, bool allow_fire, bool landing_pending ) const;

		[[nodiscard]] std::vector<scan_hit> scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;
		[[nodiscard]] bool is_physically_settled( const aim_context& ctx ) const;

		[[nodiscard]] knife_info get_knife_info( const systems::local::snapshot& local ) const;
		[[nodiscard]] std::vector<scan_hit> scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const;

		[[nodiscard]] bool fire_gun( systems::input::usercmd* cmd, const target& tgt, bool was_forced, const math::vector3& shoot_eye, const systems::local::snapshot& local );
		[[nodiscard]] bool fire_melee( systems::input::usercmd* cmd, const target& tgt, const systems::local::snapshot& local );
		void stamp_history( systems::input::usercmd* cmd, const scan_hit& hit );
		[[nodiscard]] shared::shot_spread_state capture_no_spread_state( );
		void advance_no_spread_state( systems::input::usercmd* cmd, const systems::local::snapshot& local );

		[[nodiscard]] std::array<math::vector3, 4> generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& bone_rot, float pointscale, const math::vector3& shoot_pos, float inaccuracy, float spread, bool dynamic_pointscale ) const;
		[[nodiscard]] float get_min_damage( const settings::combat::ragebot::weapon_group& config, int target_health, bool override_active ) const;
		[[nodiscard]] float get_knife_damage( float raw, int armor, float armor_ratio ) const;
		[[nodiscard]] systems::tracing::result trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;
		[[nodiscard]] systems::tracing::result trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const;

		enum class penetration_crosshair_state : std::uint8_t
		{
			unavailable,
			blocked,
			penetrable
		};

		void update_penetration_crosshair( const systems::local::snapshot& local );
		void draw_penetration_crosshair( xdraw::draw_list& draw_list ) const;
		void publish_visual_hits( const std::vector<scan_hit>& hits );
		void clear_visual_hit( ) noexcept;

		struct visual_hit_point
		{
			math::vector3 position{};
			float world_radius{};
			int hitbox_index{};
			bool is_center{};
		};

		// The generic prediction pass deliberately rolls weapon state back. Rage
		// injects attack after that pass, so automatic fire needs its own private
		// post-shot accuracy state for the next compensation solve.
		struct no_spread_accuracy_shadow
		{
			std::uintptr_t weapon{};
			int offset{};
			std::vector<std::uint8_t> baseline{};
			std::vector<std::uint8_t> predicted{};
			bool valid{};

			void reset( ) noexcept
			{
				this->weapon = 0;
				this->offset = 0;
				this->baseline.clear( );
				this->predicted.clear( );
				this->valid = false;
			}
		};

		autostop_mode m_stop_mode{ autostop_mode::none };
		bool m_landing_stop_active{};
		bool m_firing_this_tick{};
		bool m_release_duck_for_shot{};
		bool m_no_spread_state_pending{};
		bool m_no_spread_state_prediction{};
		no_spread_accuracy_shadow m_no_spread_accuracy{};
		mutable std::shared_mutex m_visual_hit_mtx{};
		std::vector<visual_hit_point> m_visual_hit_points{};
		std::int64_t m_visual_hit_expiry_ms{};

		std::uint8_t m_knife_attack{};
		bool m_zeus_fired{};

		int m_revolver_cock_ticks{};
		std::atomic<penetration_crosshair_state> m_penetration_crosshair_state{ penetration_crosshair_state::unavailable };

		// Per-command scratch. Capacity is kept between commands; contents are
		// rebuilt before the scan and stay immutable while workers run.
		std::vector<shared::lagcomp::record> m_scan_records{};
		std::vector<prepared_record> m_prepared_records{};
		std::vector<std::vector<scan_hit>> m_scan_slots{};
		std::vector<std::vector<debug_point>> m_debug_slots{};
		std::vector<rage_stats::counters> m_scan_counters{};
		std::vector<scan_hit> m_scan_hits{};
		std::vector<scan_hit> m_view_hits{};
		rage_stats m_stats{};

		mutable std::vector<debug_point> m_debug_points{};
		mutable std::mutex m_debug_mtx{};
	};

	class legit
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );
		void invalidate_if_needed( );

		[[nodiscard]] bool has_target( ) const noexcept { return this->m_target.has_target( ); }

	private:
		struct scan_point
		{
			math::vector3 position{};
			float damage{};
			float fov{};
			int hitgroup{};
			std::size_t cfg_index{};
			int bone_index{};
			systems::hitboxes::entry hitbox{};
			bool visible{};
			bool is_center{};
			bool valid{};
		};

		struct target_result
		{
			std::uintptr_t pawn{};
			scan_point best_point{};
			math::vector3 aim_angle{};
			float hitchance{};
			float score{};
			float fov{};
			int health{};
			shared::lagcomp::record* record{};

			[[nodiscard]] bool has_target( ) const noexcept { return this->best_point.valid; }
		};

		[[nodiscard]] target_result find_target( const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const;
		[[nodiscard]] scan_point scan_player( std::uintptr_t pawn, shared::lagcomp::record* record, const math::vector3& shoot_position, const math::vector3& view_angles, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local ) const;

		void apply_aimbot( systems::input::usercmd* cmd, const target_result& tgt, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );
		void apply_triggerbot( systems::input::usercmd* cmd, const math::vector3& shoot_position, const math::vector3& view_angles, const math::vector3& aim_punch, const settings::combat::legitbot::weapon_group& config, const systems::local::snapshot& local );
		void apply_rcs( math::vector3& aim_angle, const math::vector3& aim_punch, int rand_min, int rand_max ) const;

		void update_standalone_rcs( const math::vector3& view_angles, const math::vector3& aim_punch, int amount, int rand_min, int rand_max, bool apply, const systems::local::snapshot& local );
		[[nodiscard]] float compute_rcs_factor( int rand_min, int rand_max ) const;

		void draw_fov( xdraw::draw_list& draw_list, const math::vector3& view_angles, const math::vector3& aim_punch, float fov_degrees, const config::col& color, bool rcs_active ) const;

		[[nodiscard]] static int hitgroup_to_cfg( int hitgroup );

		target_result m_target{};
		math::vector3 m_old_punch{};
		mutable float m_last_significant_punch_time{};

		float m_remainder_x{};
		float m_remainder_y{};

		float m_bezier_t{ 1.0f };
		float m_bezier_prev_delta_x{};
		float m_bezier_prev_delta_y{};

		float m_trigger_delay_start{};
		float m_trigger_release_time{};
		std::uintptr_t m_trigger_pending_pawn{};

		math::vector3 m_cached_view_angles{};
		math::vector3 m_cached_aim_punch{};
	};

} // namespace features::combat
