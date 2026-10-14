#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/addresses/commands.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

//如果你是接手这个项目的人，请务必看下面的话
//1.如果你是想修复或者改进这个垃圾ragebot就别想了，因为这个连fable5都改进修复不了，直接删了重写得了
//2.如果你是想删了重写那么我劝你直接删除项目吧，因为这样你会报错999+
//
//
//
//
//
namespace features::combat {
	namespace {
		[[nodiscard]] std::int64_t visual_time_ms( ) noexcept
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( );
		}
	}

	void rage::on_create_move( systems::input::usercmd* cmd )
	{
		const auto local = systems::g_local.get( );
		auto& weapon = g_shared.ctx( );
		this->update_penetration_crosshair( local );

		// Always update DT so weapon switches and unsupported weapons clear a
		// queued follow-up deterministically. The module rejects R8 internally.
		this->m_double_tap.on_create_move( cmd );

		const auto landing_pending = this->m_landing_stop_active;
		const auto previous_stop_mode = this->m_stop_mode;
		this->m_stop_mode = autostop_mode::none;
		this->m_landing_stop_active = false;
		this->m_firing_this_tick = false;
		this->m_release_duck_for_shot = false;
		this->m_no_spread_state_pending = false;
		this->m_knife_attack = 0;
		const auto drop_taser = this->m_zeus_fired;
		this->m_zeus_fired = false;

		if ( !settings::g_combat.m_duckpeek.enabled.value ) this->m_duckpeek_reduck = false;

		if ( drop_taser && weapon.weapon_type == cstypes::weapon_type::taser &&
			settings::g_combat.m_zeusbot.drop_after && !systems::g_local.is_in_deathmatch( ) )
		{
			addresses::client_command( "drop" );
			return;
		}

		if ( !cmd || !weapon.valid || !local.is_alive || !local.pawn || !local.controller )
		{
			this->m_no_spread_accuracy.reset( );
			this->m_no_spread_state_prediction = false;
			this->m_revolver_cock_ticks = 0;
			this->m_settle_ticks = 0;
			this->clear_visual_hit( );
			return;
		}

		const auto is_knife = weapon.weapon_type == cstypes::weapon_type::knife;
		const auto is_taser = weapon.weapon_type == cstypes::weapon_type::taser;
		const auto is_gun = weapon.weapon_type >= cstypes::weapon_type::pistol && weapon.weapon_type <= cstypes::weapon_type::lmg;
		if ( !is_knife && !is_taser && !is_gun )
		{
			this->m_no_spread_accuracy.reset( );
			this->m_no_spread_state_prediction = false;
			this->m_revolver_cock_ticks = 0;
			this->m_settle_ticks = 0;
			return;
		}
		if ( !is_gun || !settings::g_combat.m_ragebot.enabled ||
			!settings::g_combat.m_ragebot.get_group( weapon.weapon_type ).no_spread.value )
		{
			this->m_no_spread_accuracy.reset( );
			this->m_no_spread_state_prediction = false;
		}

		// early-stop 结算对齐：counter 输入由管线后段的 autostop 写入，而
		// settle 预测在 build_context 的 simulate 里跑——模拟看到的永远是
		// 玩家原始输入，刹车期间门禁看不到减速（白等 tick，目标跑掉/回溯
		// 记录过期）。上一 tick 已决定 ground/air 刹车时，先把本 tick 的
		// counter 注入命令：模拟与实际下发的是同一份刹停输入，开火门禁与
		// 服务端弹道散布一致，急停到出弹不再多等。
		if ( settings::g_combat.m_ragebot.enabled && !is_knife && !is_taser
			&& ( previous_stop_mode == autostop_mode::ground || ( previous_stop_mode == autostop_mode::air
				&& settings::g_combat.m_ragebot.get_group( weapon.weapon_type ).autostop.value
				&& settings::g_combat.m_ragebot.get_group( weapon.weapon_type ).autostop_in_air.value ) ) )
		{
			g_misc.autostop( ).write_brake_input( cmd, previous_stop_mode );
		}

		auto aim = this->build_context( cmd, local );
		this->m_landing_stop_active = landing_pending;
		if ( is_knife )
		{
			this->m_landing_stop_active = false;
			this->m_revolver_cock_ticks = 0;
			this->m_settle_ticks = 0;
			if ( !g_shared.can_shoot( cmd, local.controller ) )
			{
				this->m_double_tap.on_shot_intent( cmd, false );
				return;
			}
			this->run_knife( cmd, aim, local );
			this->m_double_tap.on_shot_intent( cmd, this->m_firing_this_tick );
			return;
		}
		if ( is_taser )
		{
			this->m_landing_stop_active = false;
			this->m_revolver_cock_ticks = 0;
			this->m_settle_ticks = 0;
			if ( g_shared.can_shoot( cmd, local.controller ) ) this->run_taser( cmd, aim, local );
			return;
		}

		if ( weapon.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver )
		{
			this->auto_revolver( cmd, aim, local );
			return;
		}

		this->m_revolver_cock_ticks = 0;

		// The first shot must pass the normal weapon cooldown. Only an already
		// committed pair may bypass it while waiting to submit its follow-up.
		const auto can_fire = g_shared.can_shoot( cmd, local.controller,
			!this->m_double_tap.bypass_cooldown( ) );
		if ( !can_fire )
		{
			this->m_landing_stop_active = false;
			this->m_double_tap.on_shot_intent( cmd, false );
			if ( settings::g_esp.m_player.m_chams.hittable.enabled.value )
			{
				this->run_gun( cmd, aim, local, false, true );
			}
			return;
		}
		this->run_gun( cmd, aim, local );
		this->m_double_tap.on_shot_intent( cmd, this->m_firing_this_tick );
	}

	rage::aim_context rage::build_context( systems::input::usercmd* cmd, const systems::local::snapshot& local ) const
	{
		const auto& pre = systems::g_prediction.pre( );
		const auto& weapon = g_shared.ctx( );
		aim_context out{};
		out.view_angles = systems::g_input.get_view_angles( );
		out.velocity = pre.velocity;
		out.spread = g_shared.get_spread( );
		out.predicted_inaccuracy = g_shared.get_inaccuracy( true );
		out.on_ground = ( pre.flags & cstypes::entity_flags::on_ground ) != 0;
		out.is_scoped = weapon.is_scoped;
		out.weapon_max_speed = weapon.weapon_max_speed;
		out.accurate_threshold = weapon.weapon_max_speed * 0.34f;

		{
			// Brake inputs are still in movement_fix's source frame here. The
			// final command is reframed later; only this probe uses the source yaw.
			// Do not change the late, already-reframed NoSpread prediction path.
			auto* base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
			auto* angles = base && g_misc.antiaim( ).is_active( ) ? base->mutable_viewangles( ) : nullptr;
			struct scoped_prediction_angles
			{
				proto::msg_qangle* target;
				proto::msg_qangle saved;
				~scoped_prediction_angles( ) { if ( target ) *target = saved; }
			} restore_angles{ angles, angles ? *angles : proto::msg_qangle{} };
			if ( angles ) angles->set_y( features::movement::g_movement_fix.source_yaw( ) );

			out.prediction_valid = systems::g_prediction.simulate( cmd, local, [ & ]
				{
					g_shared.sh( ).snapshot( local.pawn, weapon.weapon_services );
					out.velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
					out.spread = g_shared.get_spread( );
					// Compare actual and zero-speed accuracy in the same predicted
					// tick/stance/turning state, before simulate restores the pawn.
					out.predicted_inaccuracy = g_shared.get_inaccuracy( true );
					out.standing_inaccuracy = g_shared.get_inaccuracy_at_velocity( local.pawn, {} );
				} );
		}

		out.eyes = this->resolve_eye_candidates( );
		if ( !std::isfinite( out.standing_inaccuracy ) || out.standing_inaccuracy <= 0.0f )
		{
			out.standing_inaccuracy = out.predicted_inaccuracy;
		}

		g_shared.ctx( ).spread = out.spread;
		g_shared.ctx( ).inaccuracy = out.predicted_inaccuracy;
		return out;
	}

	shared::shoot_history::eye_candidates rage::resolve_eye_candidates( ) const
	{
		auto eyes = g_shared.sh( ).get_candidates( );
		if ( eyes.count == 0 )
		{
			// Ring window empty: the server still resolves the shot from the
			// interpolated (lerp-rewound) shoot position. Aim from THAT
			// position, not the live one - a live-position aim is offset by
			// the full lerp window and misses on fast peeks.
			const auto local = systems::g_local.get( );
			eyes.entries[ 0 ].position = g_shared.get_interpolated_shoot_position( local.pawn, false );
			if ( eyes.entries[ 0 ].position.length_sqr( ) < 1.0f )
			{
				eyes.entries[ 0 ].position = g_shared.get_shoot_position( );
			}
			if ( eyes.entries[ 0 ].position.length_sqr( ) < 1.0f )
			{
				eyes.entries[ 0 ].position = g_shared.get_eye_position( local.pawn );
			}
			eyes.entries[ 0 ].is_uninterpolated = true;
			eyes.count = 1;
		}
		return eyes;
	}

	void rage::publish_visual_hits( const std::vector<scan_hit>& hits )
	{
		std::vector<visual_hit_point> points{};
		points.reserve( hits.size( ) );
		for ( const auto& hit : hits )
		{
			const auto duplicate = std::any_of( points.begin( ), points.end( ), [ & ]( const visual_hit_point& point )
				{
					return point.hitbox_index == hit.hitbox_index && point.is_center == hit.is_center &&
						( point.position - hit.position ).length_sqr( ) < 1.0f;
				} );
			if ( duplicate ) continue;
			const auto extents = ( hit.hitbox.maxs - hit.hitbox.mins ) * 0.5f;
			const auto radius = hit.hitbox.radius > 0.0f ? hit.hitbox.radius :
				std::max( { std::fabsf( extents.x ), std::fabsf( extents.y ), std::fabsf( extents.z ) } );
			points.push_back( { hit.position, radius, hit.hitbox_index, hit.is_center } );
		}
		std::unique_lock lock( this->m_visual_hit_mtx );
		this->m_visual_hit_points = std::move( points );
		this->m_visual_hit_expiry_ms = visual_time_ms( ) + 120;
	}

	void rage::clear_visual_hit( ) noexcept
	{
		std::unique_lock lock( this->m_visual_hit_mtx );
		this->m_visual_hit_points.clear( );
		this->m_visual_hit_expiry_ms = 0;
	}

	void rage::on_render( xdraw::draw_list& draw_list )
	{
		if ( g_shared.ctx( ).item_def_idx != cstypes::item_definition_index::weapon_r8_revolver )
		{
			this->m_double_tap.on_render( draw_list );
		}

		this->draw_penetration_crosshair( draw_list );
		if ( settings::g_esp.m_player.m_chams.hittable.enabled.value )
		{
			std::vector<visual_hit_point> points{};
			{
				std::shared_lock lock( this->m_visual_hit_mtx );
				if ( visual_time_ms( ) <= this->m_visual_hit_expiry_ms ) points = this->m_visual_hit_points;
			}
			const auto color = settings::g_esp.m_player.m_chams.hittable.primary.color.value;
			for ( const auto& point : points )
			{
				const auto screen = systems::g_view.project( point.position );
				if ( !systems::g_view.projection_valid( screen ) ) continue;
				const auto radius = point.is_center ? 5.0f : 3.25f;
				draw_list.circle_filled( screen.x, screen.y, radius, point.is_center ? color.alpha( static_cast<std::uint8_t>( color.a * 0.38f ) ) : color, 16 );
			}
		}

		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		if ( !config.debug_multipoints.value ) return;
		std::lock_guard lock( this->m_debug_mtx );
		for ( const auto& point : this->m_debug_points )
		{
			const auto screen = systems::g_view.project( point.position );
			if ( !systems::g_view.projection_valid( screen ) ) continue;
			const auto color = point.is_center ? xdraw::color{ 255, 120, 90, 255 } : xdraw::color{ 120, 180, 255, 180 };
			draw_list.circle_filled( screen.x, screen.y, point.is_center ? 3.5f : 2.0f, color );
		}
	}

	void rage::update_penetration_crosshair( const systems::local::snapshot& local )
	{
		const auto& config = settings::g_combat.m_penetration_crosshair;
		const auto& weapon = g_shared.ctx( );
		if ( !config.enabled.value || !weapon.valid || !local.is_alive || !local.pawn ||
			weapon.weapon_type < cstypes::weapon_type::pistol || weapon.weapon_type > cstypes::weapon_type::lmg )
		{
			this->m_penetration_crosshair_state.store( penetration_crosshair_state::unavailable, std::memory_order_relaxed );
			return;
		}
		auto angles = systems::g_input.get_view_angles( );
		const auto punch = g_shared.get_aim_punch( local.pawn );
		angles.x += punch.x;
		angles.y += punch.y;
		math::vector3 forward{};
		math::helpers::angle_vectors_left( angles, &forward );
		auto damage{ 0.0f };
		const auto penetrable = g_shared.pen( ).can( g_shared.get_eye_position( local.pawn ), forward, damage, local );
		this->m_penetration_crosshair_state.store( penetrable ? penetration_crosshair_state::penetrable : penetration_crosshair_state::blocked, std::memory_order_relaxed );
	}

	void rage::draw_penetration_crosshair( xdraw::draw_list& draw_list ) const
	{
		const auto& config = settings::g_combat.m_penetration_crosshair;
		if ( !config.enabled.value ) return;
		const auto state = this->m_penetration_crosshair_state.load( std::memory_order_relaxed );
		if ( state == penetration_crosshair_state::unavailable || systems::g_local.is_in_cinematic( ) ) return;
		const auto can_penetrate = state == penetration_crosshair_state::penetrable;
		const auto& fill = can_penetrate ? config.can_penetrate_fill : config.blocked_fill;
		const auto& outline = can_penetrate ? config.can_penetrate_outline : config.blocked_outline;
		const auto [ width, height ] = xdraw::viewport_size( );
		const auto x = std::floorf( static_cast<float>( width ) * 0.5f );
		const auto y = std::floorf( static_cast<float>( height ) * 0.5f );
		if ( config.glow )
		{
			const auto alpha = static_cast<std::uint8_t>( static_cast<float>( outline.value.a ) * config.glow_strength );
			xdraw::get_glow( ).rect_filled( x - 4.0f, y - 4.0f, 8.0f, 8.0f,
				xdraw::color{ outline.value.r, outline.value.g, outline.value.b, alpha } );
		}
		draw_list.rect_filled( x - 4.0f, y - 4.0f, 8.0f, 8.0f, outline );
		draw_list.rect_filled( x - 3.0f, y - 3.0f, 6.0f, 6.0f, fill );
	}
} // namespace features::combat
