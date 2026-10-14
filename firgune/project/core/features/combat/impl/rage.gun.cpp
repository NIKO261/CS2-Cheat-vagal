#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <climits>
// AA_FIRE_DIAGNOSTICS_BEGIN
#include <utilities/diag.hpp>
// AA_FIRE_DIAGNOSTICS_END

namespace features::combat {

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

	bool rage::weapon_ready_to_fire( ) const
	{
		const auto& weapon = g_shared.ctx( );
		if ( !weapon.valid || weapon.in_reload || weapon.clip <= 0 ) return false;
		const auto controller = systems::g_local.get( ).controller;
		if ( !controller ) return false;
		const auto tick_base = memory::read<int>( controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		return tick_base >= weapon.next_primary_attack_tick;
	}

	bool rage::should_brake_early( const aim_context& ctx, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
	{
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( !config.autostop.value || !config.autostop_early.value || !ctx.on_ground ) return false;
		if ( !this->weapon_ready_to_fire( ) ) return false;
		if ( ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0 ) return false;
		return this->hittable_after_brake( ctx, local, candidates );
	}

	rage::stop_decision rage::decide_stop( systems::input::usercmd* cmd, const aim_context& ctx, const target& best,
		std::vector<candidate>& candidates, const systems::local::snapshot& local,
		bool allow_fire, bool landing_pending ) const
	{
		stop_decision result{};
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto required = config.hitchance_override.value
			? static_cast<float>( config.hitchance_override_value ) / 100.0f
			: static_cast<float>( config.hitchance ) / 100.0f;
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
		const auto physically_settled = this->is_physically_settled( ctx );
		const auto settled = config.no_spread.value || physically_settled;
		this->m_settle_ticks = physically_settled
			? std::min( this->m_settle_ticks + 1, k_settle_ticks_required )
			: 0;
		// Precision trusts the engine-simulated state for this exact command.
		// Once that state is physically settled, waiting for a second command
		// cannot make the current server shot more accurate and only adds latency.
		const auto fire_ready = config.no_spread.value || precision || this->m_settle_ticks >= k_settle_ticks_required;
		const auto accurate = best.valid && ( best.passes_hitchance || best.hitchance >= required - dt_hc_boost ) && settled && fire_ready;
		const auto maximum_accuracy = g_shared.is_max_accuracy( ctx.predicted_inaccuracy );
		// Force shot skips the hitchance vote, never the physical stop:
		// a bullet fired mid-slide only feeds the wall.
		result.forced = best.valid && physically_settled && maximum_accuracy &&
			( ctx.on_ground ? config.force_shot.value : config.force_shot_air.value );

		// Weapon state gate: reloading / empty clip / fire cooldown means
		// no shot can come out of the brake, so none of the stop modes run.
		const auto weapon_ready = this->weapon_ready_to_fire( );

		// Landing stop: a qualifying hit already exists while airborne, so
		// the landing tick itself is braked (see autostop.cpp). An airborne
		// shot that is already settled and passes hitchance fires first -
		// the landing reservation is only for shots that must wait.
		const auto air_shot_ready = best.valid && settled && fire_ready && best.passes_hitchance;
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
				result.ready_to_fire = best.valid && best.passes_hitchance && settled && fire_ready;
				return result;
			}
		}

		if ( weapon_ready && config.autostop.value )
		{
			if ( ctx.on_ground )
			{
				// Stop when the scan already produced a qualifying hit
				// (direct or through a wall), or - with early stop - when
				// braking right now lands on a spot that can produce one.
				// Short-circuit: only evaluate early brake if no current hit exists.
				if ( best.valid || ( config.autostop_early.value && !space && this->hittable_after_brake( ctx, local, candidates ) ) )
				{
					if ( speed > ctx.accurate_threshold || !settled )
						result.mode = autostop_mode::ground;
				}
			}
			else if ( config.autostop_in_air.value )
			{
				// Air stop: counter input cancels horizontal slide both for
				// an immediate hit and for the predicted landing spot.
				if ( best.valid || ( !space && this->hittable_after_landing( ctx, local, candidates ) ) )
					result.mode = autostop_mode::air;
			}
		}

		result.ready_to_fire = allow_fire && best.valid && ( accurate || result.forced );
		return result;
	}

	bool rage::duckpeek_stand_viable( const aim_context& ctx, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
	{
		if ( candidates.empty( ) ) return false;

		auto eye = ctx.eyes.count > 0 ? ctx.eyes.entries[ 0 ].position : g_shared.get_shoot_position( );
		if ( eye.length_sqr( ) < 1.0f ) eye = g_shared.get_eye_position( local.pawn );
		if ( eye.length_sqr( ) < 1.0f ) return false;

		// Mirror the fake-stand camera (misc::duckpeek::on_override_view):
		// the assist already shows the standing view while the body is still
		// crouched, so probe the shot from that same raised eye. If it can
		// deal damage, stand the body up and let the real standing-eye scan
		// take over next tick.
		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( movement_services )
		{
			const auto duck_amount = std::clamp( memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) ), 0.0f, 1.0f );
			eye.z += 18.0f * duck_amount;
		}

		return this->any_damage_from_eye( eye, ctx, candidates, local );
	}

	void rage::run_gun( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local, bool allow_fire, bool visual_only )
	{
		const auto landing_pending = this->m_landing_stop_active;
		this->m_landing_stop_active = false;
		if ( !settings::g_combat.m_ragebot.enabled ) return;
		const auto& weapon = g_shared.ctx( );
		const auto& config = settings::g_combat.m_ragebot.get_group( weapon.weapon_type );
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
				ctx.eyes.count, this->m_settle_ticks, static_cast<int>( config.no_spread.value ),
				static_cast<int>( settings::g_combat.m_ragebot.precision.value ), static_cast<int>( allow_fire ), static_cast<int>( visual_only ),
				hitchance, static_cast<float>( config.hitchance_override.value ? config.hitchance_override_value : config.hitchance ) / 100.0f,
				static_cast<int>( ready ) );
		};
		// AA_FIRE_DIAGNOSTICS_END
		auto candidates = gather_candidates( local );
		{
			std::lock_guard lock( m_debug_mtx );
			m_debug_points.clear( );
		}
		if ( candidates.empty( ) )
		{
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-candidates" );
			// AA_FIRE_DIAGNOSTICS_END
			this->m_settle_ticks = 0;
			return;
		}

		const auto inaccuracy = config.no_spread.value ? g_shared.get_inaccuracy( false ) : ctx.predicted_inaccuracy;
		std::vector<scan_hit> hits{};
		for ( auto eye_index = 0; eye_index < ctx.eyes.count; ++eye_index )
		{
			auto eye_hits = scan_players( ctx.eyes.entries[ eye_index ].position, inaccuracy, ctx, candidates, local );
			for ( auto& hit : eye_hits ) { hit.source_eye = ctx.eyes.entries[ eye_index ]; hits.push_back( std::move( hit ) ); }
		}
		if ( hits.empty( ) )
		{
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-hits" );
			// AA_FIRE_DIAGNOSTICS_END
			// No current shot: anticipate the peek. Braking now settles the
			// player at a spot that can still produce a qualifying hit.
			// settle 计数不复位：物理急停状态与目标可见性无关，扫描闪烁
			// 清零会让开门后的射击多等两个确认 tick。
			if ( !visual_only && this->should_brake_early( ctx, local, candidates ) )
			{
				this->m_stop_mode = autostop_mode::ground;
			}
			// Duck peek: the crouched eye cannot hit yet, but if the raised
			// (standing) eye can, stand up now so the next scan fires from the
			// standing eye. Without this the assist stays crouched forever
			// whenever the shot only opens while standing.
			if ( !visual_only && settings::g_combat.m_duckpeek.enabled.value && ctx.on_ground && allow_fire &&
				this->duckpeek_stand_viable( ctx, local, candidates ) )
			{
				this->m_release_duck_for_shot = true;
			}
			return;
		}
		const auto best = select_best( ctx, hits, inaccuracy );

		if ( settings::g_esp.m_player.m_chams.hittable.enabled.value ) publish_visual_hits( hits );
		if ( visual_only )
		{
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "visual-only" );
			// AA_FIRE_DIAGNOSTICS_END
			this->m_settle_ticks = 0;
			return;
		}
		if ( !best.valid )
		{
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "no-best" );
			// AA_FIRE_DIAGNOSTICS_END
			// settle 计数不复位（同上：扫描闪烁不改物理状态）。
			if ( this->should_brake_early( ctx, local, candidates ) )
			{
				this->m_stop_mode = autostop_mode::ground;
			}
			return;
		}

		if ( settings::g_combat.m_autos.scope.value && weapon.weapon_type == cstypes::weapon_type::sniper
			&& !weapon.is_scoped && !this->m_double_tap.second_pending( ) )
		{
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_decision( "auto-scope", best.hitchance );
			// AA_FIRE_DIAGNOSTICS_END
			cmd->buttons.value |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_changed |= cstypes::command_buttons::in_second_attack;
			cmd->buttons.value_scroll |= cstypes::command_buttons::in_second_attack;
			return;
		}

		const auto decision = decide_stop( cmd, ctx, best, candidates, local, allow_fire, landing_pending );
		this->m_stop_mode = decision.mode;
		this->m_landing_stop_active = decision.landing_follow_through;
		const auto duckpeek = settings::g_combat.m_duckpeek.enabled.value && ctx.on_ground;
		if ( duckpeek && allow_fire ) this->m_release_duck_for_shot = !decision.ready_to_fire;

		// Shot one owns the settle/hitchance vote. Once its pulse is committed,
		// shot two must use the current scan immediately; post-shot inaccuracy
		// cannot re-qualify inside the short pair window. The live weapon claim
		// remains authoritative for the follow-up pulse.
		const auto dt_second_ready = this->m_double_tap.second_ready( );
		const auto precision = settings::g_combat.m_ragebot.precision.value && !config.no_spread.value;
		// AA_FIRE_DIAGNOSTICS_BEGIN
		trace_decision( "decision", best.hitchance, decision.ready_to_fire || ( dt_second_ready && best.valid && !precision ) );
		// AA_FIRE_DIAGNOSTICS_END
		if ( !decision.ready_to_fire && !( dt_second_ready && best.valid && !precision ) ) return;

		if ( fire_gun( cmd, best, decision.forced && !best.passes_hitchance, best.hit.source_eye.position, local ) )
		{
			this->m_landing_stop_active = false;
			if ( duckpeek ) { this->m_duckpeek_reduck = true; this->m_release_duck_for_shot = false; }
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
		if ( override_active ) return static_cast<float>( config.min_damage_override_value );
		const auto configured = static_cast<float>( config.min_damage );
		if ( configured >= 101.0f ) return std::min( static_cast<float>( health ), 101.0f );
		return health < configured ? static_cast<float>( health ) + 1.0f : configured;
	}
}
