#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <climits>
// AA_FIRE_DIAGNOSTICS_BEGIN
#include <utilities/diag.hpp>
// AA_FIRE_DIAGNOSTICS_END

namespace features::combat {
	namespace {
		// Diagnostics are aggregated into bounded histograms; no per-command
		// history is kept and disabled builds compile the whole path out.
		constexpr auto k_stats_report_interval{ 128 };
		constexpr auto k_stats_detail_interval{ 31 };
		constexpr auto k_stats_histogram_buckets{ 64 };
		constexpr auto k_stats_phase_count{ 6 };
		constexpr auto k_stats_reason_count{ 12 };

		struct stats_aggregate
		{
			unsigned commands{};
			unsigned reasons[ k_stats_reason_count ]{};
			unsigned submitted{};
			unsigned advanced{};
			unsigned fired[ 5 ]{};
			unsigned brake_repredictions{};
			unsigned rescans{};
			rage_stats::counters work{};
			unsigned long long sums[ k_stats_phase_count ]{};
			unsigned histogram[ k_stats_phase_count ][ k_stats_histogram_buckets ]{};
			int last_command{ -1 };
			std::uint32_t last_mode{};
			std::uint32_t last_config{};
		};

		[[nodiscard]] stats_aggregate& aggregate_state( )
		{
			static stats_aggregate instance{};
			return instance;
		}

		[[nodiscard]] int histogram_bucket( std::uint64_t value ) noexcept
		{
			auto bucket = 0;
			while ( value > 1 && bucket + 1 < k_stats_histogram_buckets )
			{
				value >>= 1;
				++bucket;
			}
			return bucket;
		}

		[[nodiscard]] double histogram_percentile( const unsigned* buckets, unsigned count, double percentile )
		{
			if ( count == 0 ) return 0.0;
			const auto target = std::max( 1u, static_cast< unsigned >( std::ceil( static_cast< double >( count ) * percentile ) ) );
			unsigned seen{};
			for ( auto i = 0; i < k_stats_histogram_buckets; ++i )
			{
				seen += buckets[ i ];
				if ( seen >= target )
				{
					const auto bits = std::min( i + 1, 62 );
					return static_cast< double >( 1ull << bits ) / 1000.0;
				}
			}
			return 0.0;
		}

		void report_stats( const stats_aggregate& aggregate )
		{
			const auto phase = [ & ]( int index, double percentile )
			{
				return histogram_percentile( aggregate.histogram[ index ], aggregate.commands, percentile );
			};
			diag::writef( diag::level::info,
				"[rage-stats:v1] cmds=%u cmd=%d mode=%x cfg=%08x "
				"total_us=%.1f/%.1f/%.1f pred=%.1f/%.1f/%.1f prep=%.1f/%.1f/%.1f "
				"scan=%.1f/%.1f/%.1f hc=%.1f/%.1f/%.1f sel=%.1f/%.1f/%.1f "
				"enemies=%d records=%d eyes=%d points=%d pen=%d hcpoints=%d hcsamples=%d "
				"brake=%d rescan=%d submitted=%u advanced=%u fired=hc:%u,force:%u,dt:%u,ns:%u "
				"reasons=disabled:%u,no_candidates:%u,no_hits:%u,no_best:%u,cooldown:%u,pred_failed:%u,"
				"not_settled:%u,low_hc:%u,visual:%u,scope:%u,submit_failed:%u",
				aggregate.commands, aggregate.last_command, aggregate.last_mode, aggregate.last_config,
				phase( 5, 0.5 ), phase( 5, 0.95 ), phase( 5, 0.99 ),
				phase( 0, 0.5 ), phase( 0, 0.95 ), phase( 0, 0.99 ),
				phase( 1, 0.5 ), phase( 1, 0.95 ), phase( 1, 0.99 ),
				phase( 2, 0.5 ), phase( 2, 0.95 ), phase( 2, 0.99 ),
				phase( 3, 0.5 ), phase( 3, 0.95 ), phase( 3, 0.99 ),
				phase( 4, 0.5 ), phase( 4, 0.95 ), phase( 4, 0.99 ),
				aggregate.work.enemies, aggregate.work.records, aggregate.work.eyes, aggregate.work.points,
				aggregate.work.penetration_calls, aggregate.work.hitchance_points, aggregate.work.hitchance_samples,
				static_cast<int>( aggregate.brake_repredictions ), static_cast<int>( aggregate.rescans ), aggregate.submitted, aggregate.advanced,
				aggregate.fired[ 1 ], aggregate.fired[ 2 ], aggregate.fired[ 3 ], aggregate.fired[ 4 ],
				aggregate.reasons[ 1 ], aggregate.reasons[ 2 ], aggregate.reasons[ 3 ], aggregate.reasons[ 4 ],
				aggregate.reasons[ 5 ], aggregate.reasons[ 6 ], aggregate.reasons[ 7 ], aggregate.reasons[ 8 ],
				aggregate.reasons[ 9 ], aggregate.reasons[ 10 ], aggregate.reasons[ 11 ] );
		}

		[[nodiscard]] std::uint32_t weapon_config_hash( const settings::combat::ragebot::weapon_group& config )
		{
			auto hash = 2166136261u;
			const auto mix = [ & ]( std::uint32_t value ) { hash = ( hash ^ value ) * 16777619u; };
			mix( static_cast< std::uint32_t >( config.hitchance.value ) );
			mix( config.hitchance_override.value ? 1u : 0u );
			mix( static_cast< std::uint32_t >( config.hitchance_override_value ) );
			mix( static_cast< std::uint32_t >( config.min_damage ) );
			mix( config.min_damage_override.value ? 1u : 0u );
			mix( static_cast< std::uint32_t >( config.min_damage_override_value ) );
			mix( static_cast< std::uint32_t >( config.pointscale.value * 100.0f ) );
			mix( static_cast< std::uint32_t >( config.max_fov.value ) );
			mix( config.no_spread.value ? 1u : 0u );
			mix( config.body_aim.value ? 1u : 0u );
			mix( config.force_shot.value ? 1u : 0u );
			mix( config.force_shot_air.value ? 1u : 0u );
			mix( config.dynamic_pointscale.value ? 1u : 0u );
			for ( auto i = 0; i < 6; ++i )
			{
				mix( config.hitboxes.values[ static_cast<std::size_t>( i ) ] ? 1u : 0u );
				mix( config.multipoints.values[ static_cast<std::size_t>( i ) ] ? 1u : 0u );
			}
			return hash;
		}

		[[nodiscard]] std::uint32_t diagnostic_mode( const settings::combat::ragebot::weapon_group& config )
		{
			const auto& weapon = g_shared.ctx( );
			auto mode = static_cast< std::uint32_t >( weapon.weapon_type ) & 0x1fu;
			if ( settings::g_combat.m_ragebot.precision.value ) mode |= 0x100u;
			if ( settings::g_combat.m_ragebot.precision_mode.value == settings::combat::ragebot::precision_mode::performance ) mode |= 0x200u;
			if ( config.no_spread.value ) mode |= 0x400u;
			if ( config.force_shot.value ) mode |= 0x800u;
			if ( config.force_shot_air.value ) mode |= 0x1000u;
			if ( weapon.is_scoped ) mode |= 0x2000u;
			if ( g_rage.dt( ).can_pair( ) ) mode |= 0x4000u;
			return mode;
		}
	}

	void rage_stats::begin( int command_number )
	{
		this->m_command = command_number;
		this->m_mode = 0;
		this->m_config_hash = 0;
		this->m_work = {};
		this->m_time = {};
		this->m_brake_repredictions = 0;
		this->m_rescans = 0;
		this->m_reason = no_fire_reason::none;
		this->m_fire_reason = fire_reason::none;
		this->m_submitted = false;
		if constexpr ( k_rage_diagnostics ) this->m_start_ns = steady_now_ns( );
	}

	void rage_stats::add( const counters& other ) noexcept
	{
		this->m_work.enemies += other.enemies;
		this->m_work.records += other.records;
		this->m_work.eyes += other.eyes;
		this->m_work.points += other.points;
		this->m_work.penetration_calls += other.penetration_calls;
		this->m_work.hitchance_points += other.hitchance_points;
		this->m_work.hitchance_samples += other.hitchance_samples;
	}

	void rage_stats::submit( fire_reason reason )
	{
		this->m_submitted = true;
		this->m_fire_reason = reason;
		this->m_reason = no_fire_reason::none;
	}

	void rage_stats::finish( )
	{
		if constexpr ( !k_rage_diagnostics ) return;

		// Submission and actual weapon discharge are separate facts: the second
		// one is only observed when the live weapon claims or consumes the shot.
		struct pending_shot
		{
			bool valid{};
			int command{ -1 };
			std::uintptr_t weapon{};
			int next_attack_tick{};
			int clip{};
		};
		static pending_shot pending{};
		const auto& weapon = g_shared.ctx( );
		auto& aggregate = aggregate_state( );
		if ( pending.valid )
		{
			const auto advanced = weapon.valid && weapon.weapon == pending.weapon &&
				( weapon.clip < pending.clip || weapon.next_primary_attack_tick > pending.next_attack_tick );
			if ( advanced ) ++aggregate.advanced;
			pending.valid = false;
		}
		if ( this->m_submitted )
		{
			++aggregate.submitted;
			++aggregate.fired[ static_cast<std::size_t>( this->m_fire_reason ) ];
			pending = { true, this->m_command, weapon.weapon, weapon.next_primary_attack_tick, weapon.clip };
		}
		if ( this->m_command < 0 ) return;

		this->m_time.total_ns = steady_now_ns( ) - this->m_start_ns;
		++aggregate.commands;
		const auto reason_index = static_cast< std::size_t >( this->m_reason );
		if ( reason_index < k_stats_reason_count ) ++aggregate.reasons[ reason_index ];
		aggregate.brake_repredictions += static_cast<unsigned>( this->m_brake_repredictions );
		aggregate.rescans += static_cast<unsigned>( this->m_rescans );
		aggregate.work.enemies += this->m_work.enemies;
		aggregate.work.records += this->m_work.records;
		aggregate.work.eyes += this->m_work.eyes;
		aggregate.work.points += this->m_work.points;
		aggregate.work.penetration_calls += this->m_work.penetration_calls;
		aggregate.work.hitchance_points += this->m_work.hitchance_points;
		aggregate.work.hitchance_samples += this->m_work.hitchance_samples;

		const std::uint64_t* phases[ k_stats_phase_count ]{
			&this->m_time.prediction_ns, &this->m_time.prepare_ns, &this->m_time.scan_ns,
			&this->m_time.hitchance_ns, &this->m_time.select_ns, &this->m_time.total_ns };
		for ( auto i = 0; i < k_stats_phase_count; ++i )
		{
			aggregate.sums[ i ] += *phases[ i ];
			++aggregate.histogram[ i ][ histogram_bucket( *phases[ i ] ) ];
		}
		aggregate.last_command = this->m_command;
		aggregate.last_mode = this->m_mode;
		aggregate.last_config = this->m_config_hash;

		if ( aggregate.commands % k_stats_report_interval == 0 )
		{
			report_stats( aggregate );
		}
		if ( this->m_command % k_stats_detail_interval == 0 )
		{
			diag::writef( diag::level::info,
				"[rage-stats-cmd:v1] cmd=%d mode=%x cfg=%08x reason=%u eyes=%d points=%d pen=%d hcpoints=%d hcsamples=%d "
				"brake=%d rescan=%d submitted=%d fire=%u pred_us=%.1f prep_us=%.1f scan_us=%.1f hc_us=%.1f sel_us=%.1f total_us=%.1f",
				this->m_command, this->m_mode, this->m_config_hash, static_cast<unsigned>( this->m_reason ),
				this->m_work.eyes, this->m_work.points, this->m_work.penetration_calls,
				this->m_work.hitchance_points, this->m_work.hitchance_samples,
				this->m_brake_repredictions, this->m_rescans, static_cast< int >( this->m_submitted ), static_cast<unsigned>( this->m_fire_reason ),
				static_cast< double >( this->m_time.prediction_ns ) / 1000.0,
				static_cast< double >( this->m_time.prepare_ns ) / 1000.0,
				static_cast< double >( this->m_time.scan_ns ) / 1000.0,
				static_cast< double >( this->m_time.hitchance_ns ) / 1000.0,
				static_cast< double >( this->m_time.select_ns ) / 1000.0,
				static_cast< double >( this->m_time.total_ns ) / 1000.0 );
		}
	}

	bool rage::is_physically_settled( const aim_context& ctx ) const
	{
		// The bullet resolves on the predicted tick (tick_base + 1). Judge
		// the settle state against the post-simulation values
		// (ctx.velocity / ctx.predicted_inaccuracy), not the pre-simulation
		// snapshot: the brake input written this tick is applied inside
		// simulate, so the post values are what the server samples the
		// spread cone from when the shot leaves.
		if ( !ctx.prediction_valid ) return false;
		if ( !std::isfinite( ctx.predicted_inaccuracy ) || !std::isfinite( ctx.standing_inaccuracy ) ) return false;

		// Inaccuracy is the direct source of spread; speed is only its
		// driver. Lead with the predicted post-brake inaccuracy and let
		// the speed gate act as a sanity check against a stale weapon read.
		// The 1% tolerance absorbs floating-point drift in the standing
		// probe (get_inaccuracy_at_velocity(0)) without reopening the gap
		// that produced the impact-mismatch misses.
		const auto inaccuracy_tolerance = std::max( ctx.standing_inaccuracy * 0.01f, 0.0001f );
		if ( ctx.predicted_inaccuracy > ctx.standing_inaccuracy + inaccuracy_tolerance ) return false;

		if ( ctx.on_ground )
		{
			// Horizontal speed only: the vertical component (stairs, a
			// one-unit micro-fall) is folded into the inaccuracy the
			// engine computes, so a 3D length gate would falsely reject a
			// player who has stopped sliding but is momentarily off the
			// floor.
			return ctx.velocity.length_2d( ) <= ctx.accurate_threshold + 0.5f;
		}
		// Air: the brake cancels the horizontal slide; the vertical
		// component lives inside the inaccuracy, which hitchance already
		// evaluates - so a cancelled horizontal slide is settled.
		return ctx.velocity.length_2d( ) <= ctx.accurate_threshold;
	}

	bool rage::weapon_ready_to_fire( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const
	{
		// Use the same command clock and committed DT exception as submission.
		// TickBase alone can lag the client tick and postpone the brake.
		return cmd && local.controller && g_shared.ctx( ).valid &&
			g_shared.can_shoot( cmd, local.controller, !this->m_double_tap.bypass_cooldown( ) );
	}

	rage::stop_decision rage::decide_stop( systems::input::usercmd* cmd, const aim_context& ctx, const scan_config& scan, const target& best,
		std::vector<candidate>& candidates, const systems::local::snapshot& local,
		bool allow_fire, bool landing_pending ) const
	{
		stop_decision result{};
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto required = this->required_hitchance( config );
		const auto precision = settings::g_combat.m_ragebot.precision.value && !config.no_spread.value;
		// Double tap fires a tight pair: a hitchance sitting a few percent
		// under the threshold would otherwise stall the alternating beat
		// and delay the second bullet.
		const auto dt_hc_boost = this->m_double_tap.can_pair( ) && !precision ? 0.03f : 0.0f;
		const auto space = ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0 ||
			( cmd && ( cmd->buttons.value & cstypes::command_buttons::in_jump ) );
		// Post-simulation horizontal speed: the brake input written this
		// tick is applied inside simulate, so ctx.velocity is the speed
		// the shot leaves at. The pre-simulation networked_velocity
		// lagged by ~2 ticks would report "still moving" even after the
		// brake already cancelled the slide, and the stop-mode gate below
		// would keep grinding the brake past the settle point.
		const auto speed = ctx.velocity.length_2d( );
		const auto physically_settled = ctx.physically_settled;
		const auto settled = config.no_spread.value || physically_settled;
		// This command's native prediction owns the accuracy vote. A previous
		// command (or a second invocation of the same command) adds no evidence.
		const auto accurate = best.valid && ( best.passes_hitchance || best.hitchance >= required - dt_hc_boost ) && settled;
		const auto maximum_accuracy = g_shared.is_max_accuracy( ctx.predicted_inaccuracy );
		// Force shot skips the hitchance vote, never the physical stop:
		// a bullet fired mid-slide only feeds the wall.
		result.forced = best.valid && physically_settled && maximum_accuracy &&
			( ctx.on_ground ? config.force_shot.value : config.force_shot_air.value );

		// Weapon state gate: reloading / empty clip / fire cooldown means
		// no shot can come out of the brake, so none of the stop modes run.
		const auto weapon_ready = this->weapon_ready_to_fire( cmd, local );

		// Landing stop: a qualifying hit already exists while airborne, so
		// the landing tick itself is braked (see autostop.cpp). An airborne
		// shot that is already settled and passes hitchance fires first -
		// the landing reservation is only for shots that must wait.
		const auto air_shot_ready = best.valid && settled && best.passes_hitchance;
		if ( weapon_ready && config.autostop.value && config.autostop_on_landing.value && allow_fire && !space && !air_shot_ready )
		{
			if ( !ctx.on_ground && best.valid && g_misc.autostop( ).request_landing( cmd, local ) )
			{
				result.mode = autostop_mode::landing;
				result.landing_follow_through = true;
				return result;
			}
			// Follow-through: the landing happened last tick, finish the
			// brake on the ground.
			if ( ctx.on_ground && landing_pending )
			{
				result.mode = autostop_mode::ground;
				result.landing_follow_through = true;
				result.ready_to_fire = best.valid && best.passes_hitchance && settled;
				return result;
			}
		}

		if ( weapon_ready && config.autostop.value )
		{
			if ( ctx.on_ground )
			{
				// Early ground planning already ran once, before prediction and HC.
				if ( best.valid && ( speed > ctx.accurate_threshold || !settled ) )
					result.mode = autostop_mode::ground;
			}
			else if ( config.autostop_in_air.value )
			{
				// Air stop: counter input cancels horizontal slide both for
				// an immediate hit and for the predicted landing spot.
				if ( best.valid || ( !space && this->hittable_after_landing( ctx, scan, local, candidates ) ) )
					result.mode = autostop_mode::air;
			}
		}

		result.ready_to_fire = allow_fire && best.valid && ( accurate || result.forced );
		return result;
	}

	bool rage::duckpeek_stand_viable( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
	{
		if ( candidates.empty( ) ) return false;

		// Pair the current eye with the current duck amount. Historical shot
		// eyes can still be crouched after the live pawn has started standing.
		auto eye = g_shared.get_eye_position( local.pawn );
		if ( eye.length_sqr( ) < 1.0f ) return false;

		// Probe standing geometry only; real shots retain their scanned eye.
		// Keep the first-person fake-standing view's existing height delta.
		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( movement_services )
		{
			const auto duck_amount = std::clamp( memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) ), 0.0f, 1.0f );
			eye.z += 18.0f * duck_amount;
		}

		return this->any_damage_from_eye( eye, ctx, scan, candidates, local );
	}

	void rage::run_gun( systems::input::usercmd* cmd, const aim_context& initial_ctx, const systems::local::snapshot& local, bool allow_fire, bool visual_only )
	{
		if ( !cmd || !cmd->csgo_user_cmd.base( ) ) return;
		auto ctx = initial_ctx;
		const auto landing_pending = this->m_landing_stop_active;
		this->m_landing_stop_active = false;
		if ( !settings::g_combat.m_ragebot.enabled )
		{
			this->m_stats.set_reason( rage_stats::no_fire_reason::disabled );
			return;
		}
		const auto& weapon = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( weapon.weapon_type );
		this->m_stats.set_mode( diagnostic_mode( config ) );
		this->m_stats.set_config_hash( weapon_config_hash( config ) );
		// AA_FIRE_DIAGNOSTICS_BEGIN
		// Same command cadence as the pipeline trace; never re-run scan/stop decisions.
		static thread_local int diagnostic_aa_mode{ -1 };
		static thread_local unsigned diagnostic_reports{};
		const auto diagnostic_aa = static_cast<int>( settings::g_combat.m_antiaim.enabled.value );
		if ( diagnostic_aa_mode != diagnostic_aa )
		{
			diagnostic_aa_mode = diagnostic_aa;
			diagnostic_reports = 0;
		}
		const auto trace_gate = cmd && cmd->command_number % 31 == 0 && diagnostic_reports < 240;
		if ( trace_gate ) ++diagnostic_reports;
		const auto trace_decision = [ & ]( const char* reason, float hitchance = -1.0f, bool ready = false )
		{
			if ( !trace_gate ) return;
			diag::writef( diag::level::info,
				"[aa-fire-gate:v1] cmd=%d aa=%d reason=%s pred=%d ground=%d speed=%.5f threshold=%.5f "
				"inacc=%.7f standing=%.7f eyes=%d settled=%d ns=%d precision=%d allow=%d visual=%d hc=%.5f required=%.5f ready=%d",
				cmd->command_number, diagnostic_aa, reason, static_cast<int>( ctx.prediction_valid ), static_cast<int>( ctx.on_ground ),
				ctx.velocity.length_2d( ), ctx.accurate_threshold, ctx.predicted_inaccuracy, ctx.standing_inaccuracy,
				ctx.eyes.count, static_cast<int>( ctx.physically_settled ), static_cast<int>( config.no_spread.value ),
				static_cast<int>( settings::g_combat.m_ragebot.precision.value ), static_cast<int>( allow_fire ), static_cast<int>( visual_only ),
				hitchance, static_cast<float>( config.hitchance_override.value ? config.hitchance_override_value : config.hitchance ) / 100.0f,
				static_cast<int>( ready ) );
		};
		// AA_FIRE_DIAGNOSTICS_END

		const auto scan = this->make_scan_config( config );
		std::vector<candidate> candidates{};
		{
			stats_timer timer( this->m_stats.time( ).prepare_ns );
			candidates = gather_candidates( local );
			if ( !candidates.empty( ) ) this->prepare_scan_targets( candidates );
		}
		this->m_stats.work( ).enemies = static_cast<int>( candidates.size( ) );
		{
			auto records = 0;
			for ( const auto& cand : candidates ) records += cand.record_count;
			this->m_stats.work( ).records = records;
		}
		{
			std::lock_guard lock( m_debug_mtx );
			m_debug_points.clear( );
		}

		// Every exit records why no shot left; the submission path clears it.
		auto no_fire = rage_stats::no_fire_reason::no_best;
		struct reason_scope
		{
			rage_stats& stats;
			rage_stats::no_fire_reason& reason;
			~reason_scope( ) { stats.set_reason( reason ); }
		} reason_writer{ this->m_stats, no_fire };

		if ( candidates.empty( ) )
		{
			no_fire = rage_stats::no_fire_reason::no_candidates;
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-candidates" );
			// AA_FIRE_DIAGNOSTICS_END
			return;
		}

		auto inaccuracy = config.no_spread.value ? g_shared.get_inaccuracy( false ) : ctx.predicted_inaccuracy;
		auto& hits = this->m_scan_hits;
		const auto scan_current_context = [ & ]
		{
			hits.clear( );
			const auto begin = k_rage_diagnostics ? steady_now_ns( ) : 0;
			this->m_stats.work( ).eyes = ctx.eyes.count;
			for ( auto eye_index = 0; eye_index < ctx.eyes.count; ++eye_index )
			{
				this->scan_players( ctx.eyes.entries[ eye_index ].position, eye_index, inaccuracy, ctx, scan, candidates, local, this->m_view_hits, &this->m_stats );
				for ( auto& hit : this->m_view_hits )
				{
					hit.source_eye = ctx.eyes.entries[ eye_index ];
					hits.push_back( hit );
				}
			}
			if constexpr ( k_rage_diagnostics ) this->m_stats.time( ).scan_ns += steady_now_ns( ) - begin;
		};
		scan_current_context( );

		// Damage establishes stop intent; hitchance must see the brake, not the
		// original movement input. At most one independent re-prediction runs,
		// followed by a fresh geometry scan from its actual eye/spread state.
		// No HC sampling is spent on the superseded, unbraked context.
		auto ground_brake_applied = false;
		const auto space = ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0 ||
			( cmd && ( cmd->buttons.value & cstypes::command_buttons::in_jump ) );
		if ( !visual_only && config.autostop.value && ctx.on_ground && !space &&
			this->weapon_ready_to_fire( cmd, local ) &&
			( ( !hits.empty( ) && ( ctx.velocity.length_2d( ) > ctx.accurate_threshold ||
					( !config.no_spread.value && !ctx.physically_settled ) ) ) ||
				( hits.empty( ) && config.autostop_early.value && this->hittable_after_brake( ctx, scan, local, candidates ) ) ) )
		{
			auto* base = cmd->csgo_user_cmd.mutable_base( );
			const auto forward = base->forwardmove( );
			const auto left = base->leftmove( );
			const auto buttons = cmd->buttons;
			g_misc.autostop( ).write_brake_input( cmd, autostop_mode::ground );
			ground_brake_applied = true;
			this->m_stop_mode = autostop_mode::ground;
			if ( forward != base->forwardmove( ) || left != base->leftmove( ) ||
				buttons.value != cmd->buttons.value || buttons.value_changed != cmd->buttons.value_changed ||
				buttons.value_scroll != cmd->buttons.value_scroll )
			{
				this->m_stats.repredicted( );
				{
					stats_timer timer( this->m_stats.time( ).prediction_ns );
					ctx = this->build_context( cmd, local );
				}
				inaccuracy = config.no_spread.value ? g_shared.get_inaccuracy( false ) : ctx.predicted_inaccuracy;
				{
					std::lock_guard lock( m_debug_mtx );
					m_debug_points.clear( );
				}
				this->m_stats.rescanned( );
				scan_current_context( );
			}
		}

		// Geometry owns posture, not hitchance, shot submission or cooldown.
		// Keep this before visual_only so cooldown scans make the same vote.
		if ( settings::g_combat.m_duckpeek.enabled.value && ctx.on_ground )
		{
			this->m_release_duck_for_shot = !hits.empty( ) || this->duckpeek_stand_viable( ctx, scan, local, candidates );
		}

		if ( hits.empty( ) )
		{
			no_fire = rage_stats::no_fire_reason::no_hits;
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-hits" );
			// AA_FIRE_DIAGNOSTICS_END
			// Keep this command's brake even if its updated eye loses the shot.
			return;
		}

		if ( settings::g_esp.m_player.m_chams.hittable.enabled.value ) publish_visual_hits( hits );
		if ( visual_only )
		{
			no_fire = rage_stats::no_fire_reason::visual_only;
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "visual-only" );
			// AA_FIRE_DIAGNOSTICS_END
			return;
		}

		if ( settings::g_combat.m_autos.scope.value && weapon.weapon_type == cstypes::weapon_type::sniper
			&& !weapon.is_scoped && !this->m_double_tap.second_pending( ) )
		{
			no_fire = rage_stats::no_fire_reason::scope;
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "auto-scope" );
			// AA_FIRE_DIAGNOSTICS_END
			cmd->buttons.value |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_second_attack;
			return;
		}

		target best{};
		{
			stats_timer timer( this->m_stats.time( ).select_ns );
			best = this->select_best( ctx, hits, inaccuracy, &this->m_stats );
		}
		if ( !best.valid )
		{
			no_fire = rage_stats::no_fire_reason::no_best;
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-best" );
			// AA_FIRE_DIAGNOSTICS_END
			return;
		}

		const auto decision = this->decide_stop( cmd, ctx, scan, best, candidates, local, allow_fire, landing_pending );
		this->m_stop_mode = ground_brake_applied ? autostop_mode::ground : decision.mode;
		this->m_landing_stop_active = decision.landing_follow_through;

		// Shot one owns the settle/hitchance vote. Once its pulse is committed,
		// shot two must use the current scan immediately; post-shot inaccuracy
		// cannot re-qualify inside the short pair window. The live weapon claim
		// remains authoritative for the follow-up pulse.
		const auto dt_second_ready = this->m_double_tap.second_ready( );
		const auto precision = settings::g_combat.m_ragebot.precision.value && !config.no_spread.value;
		// AA_FIRE_DIAGNOSTICS_BEGIN
		trace_decision( "decision", best.hitchance, decision.ready_to_fire || ( dt_second_ready && best.valid && !precision ) );
		// AA_FIRE_DIAGNOSTICS_END
		if ( !decision.ready_to_fire && !( dt_second_ready && best.valid && !precision ) )
		{
			no_fire = !ctx.prediction_valid ? rage_stats::no_fire_reason::prediction_failed
				: ( !ctx.physically_settled && !config.no_spread.value ? rage_stats::no_fire_reason::not_settled
					: rage_stats::no_fire_reason::low_hitchance );
			return;
		}

		// Record why this shot is allowed separately from the measured threshold.
		auto release = fire_reason::none;
		if ( config.no_spread.value ) release = fire_reason::no_spread;
		else if ( decision.forced && !best.passes_hitchance ) release = fire_reason::force;
		else if ( best.passes_hitchance ) release = fire_reason::hitchance;
		else if ( dt_second_ready ) release = fire_reason::double_tap;
		else release = fire_reason::hitchance;
		auto fired = best;
		fired.reason = release;
		if ( fire_gun( cmd, fired, decision.forced && !best.passes_hitchance, best.hit.source_eye.position, local ) )
		{
			this->m_landing_stop_active = false;
			no_fire = rage_stats::no_fire_reason::none;
			this->m_stats.submit( release );
		}
		else
		{
			no_fire = rage_stats::no_fire_reason::submit_failed;
		}
		// AA_FIRE_DIAGNOSTICS_BEGIN
		trace_decision( this->m_firing_this_tick ? "submitted" : "fire-rejected", best.hitchance );
		// AA_FIRE_DIAGNOSTICS_END
	}

	bool rage::revolver_ready_to_fire( const systems::input::usercmd* cmd ) const
	{
		const auto& weapon = g_shared.ctx( );
		const auto base = cmd ? cmd->csgo_user_cmd.base( ) : nullptr;
		if ( !base || !weapon.valid || !weapon.weapon ) return false;
		const auto ready_offset = SCHEMA( "C_CSWeaponBase", "m_nPostponeFireReadyTicks"_hash );
		if ( ready_offset <= 0 ) return false;
		// Only a live tick that has already been reached counts as "hammer back".
		// A cancelled wind-up leaves INT_MAX behind, and a spent shot leaves the
		// previous tick behind: neither may be read as cocked. The hauled-back
		// flag is deliberately not consulted — it is not a dependable cocked
		// signal on the client copy of the weapon.
		const auto ready_tick = memory::read<int>( weapon.weapon + ready_offset );
		if ( ready_tick <= 0 || ready_tick >= INT_MAX ) return false;
		return base->client_tick( ) >= ready_tick;
	}

	void rage::auto_revolver( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		const auto was_cocking = this->m_revolver_cock_ticks > 0;
		this->m_revolver_cock_ticks = 0;
		if ( !settings::g_combat.m_ragebot.enabled || !settings::g_combat.m_autos.revolver.value )
		{
			if ( settings::g_esp.m_player.m_chams.hittable.enabled.value ) run_gun( cmd, ctx, local, false, true );
			return;
		}
		// The R8 wind-up and its shot ride two different attack channels: primary
		// hauls the hammer back, secondary releases it. This module owns both so a
		// rejected shot cannot leak a raw press into the command.
		const auto attacks = static_cast<std::uintptr_t>( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
		cmd->buttons.value &= ~attacks;
		cmd->buttons.value_changed |= attacks;
		cmd->buttons.value_scroll &= ~attacks;
		cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
		cmd->csgo_user_cmd.set_attack2_start_history_index( -1 );
		if ( SCHEMA( "C_CSWeaponBase", "m_nPostponeFireReadyTicks"_hash ) <= 0
			|| !g_shared.can_shoot( cmd, local.controller ) )
		{
			if ( settings::g_esp.m_player.m_chams.hittable.enabled.value ) run_gun( cmd, ctx, local, false, true );
			return;
		}
		// A ready tick left behind by the previous shot must not fire on its own:
		// the hammer has to have been hauled back on the preceding command.
		if ( was_cocking && this->revolver_ready_to_fire( cmd ) )
		{
			run_gun( cmd, ctx, local );
			return;
		}
		// Still hauling the hammer back: hold primary, keep the scan alive so the
		// shot is taken on the exact tick the engine reports the weapon ready.
		run_gun( cmd, ctx, local, false );
		cmd->buttons.value |= cstypes::command_buttons::in_attack;
		if ( was_cocking ) cmd->buttons.value_changed &= ~cstypes::command_buttons::in_attack;
		else cmd->buttons.value_scroll |= cstypes::command_buttons::in_attack;
		if ( cmd->csgo_user_cmd.input_history_size( ) > 0 ) cmd->csgo_user_cmd.set_attack1_start_history_index( cmd->csgo_user_cmd.input_history_size( ) - 1 );
		this->m_revolver_cock_ticks = 1;
	}

	float rage::get_min_damage( const settings::combat::ragebot::weapon_group& config, int health, bool override_active ) const
	{
		if ( override_active ) return static_cast< float >( config.min_damage_override_value );
		const auto configured = static_cast< float >( config.min_damage );
		if ( configured >= 101.0f ) return std::min( static_cast< float >( health ), 101.0f );
		return health < configured ? static_cast< float >( health ) + 1.0f : configured;
	}
}
