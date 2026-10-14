#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
#include <limits>

// Autostop execution. The rage side decides WHEN to stop (damage test in
// rage.scan.cpp / rage.gun.cpp); this file only expresses the brake:
// a full-strength counter input opposite to the horizontal velocity.
// Ground: the engine resolves it as a counter-strafe. Air: the air
// acceleration path bleeds horizontal speed. Landing: on the predicted
// landing tick the player's own subtick analog is stripped so it cannot
// fight the brake while the ground friction kicks in mid-tick.
namespace features::combat {
	namespace {
		constexpr auto landing_lookahead = 8;
		constexpr auto minimum_fraction = 0.002f;
		constexpr auto maximum_fraction = 0.99f;

		[[nodiscard]] bool finite( const math::vector3& value )
		{
			return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
		}

		// Exact opposite of the current horizontal velocity, rotated into
		// view-relative movement space at full magnitude.
		[[nodiscard]] math::vector2 counter_input( const math::vector3& velocity )
		{
			const auto speed = velocity.length_2d( );
			if ( speed <= 0.01f ) return {};
			const auto world_x = -velocity.x / speed;
			const auto world_y = -velocity.y / speed;
			const auto yaw = math::helpers::deg_to_rad( features::movement::g_movement_fix.source_yaw( ) );
			const auto sine = std::sinf( yaw );
			const auto cosine = std::cosf( yaw );
			auto move = math::vector2{
				std::clamp( world_x * cosine + world_y * sine, -1.0f, 1.0f ),
				std::clamp( ( world_x * sine - world_y * cosine ) * -1.0f, -1.0f, 1.0f )
			};

			// 量化服务器（server.dll 已核实）在 SetupMove 里对 base forward/side
			// 逐项 roundf：分数模拟量的刹车被逐分量取整——分量 <0.5 直接归零、
			// 方向偏到最近基准斜向，服务端减速既弱且歪；同一份输入喂给 simulate
			// 客户端却按精确模拟量刹车，两边运动分叉，开火门禁在服务器仍在滑行
			// 时放行（MISS hc=100 的移动 spread）。把刹车吸附到最近的 8 向全幅
			// 数字状态（±1 整数对，roundf 不动点，45° 整数倍与 movement_fix 时间
			// 分割的退化单状态逐位一致）：模拟与下发逐字节相同，门禁恢复诚实，
			// 每 tick 满幅无分量损失。非量化服消费精确模拟量，保持原样。
			if ( const auto cvar = CONVAR( "sv_quantize_movement_input" ); cvar && cvar->get<bool>( ) )
			{
				const std::array<math::vector2, 8> states{ {
					{ 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f },
					{ 1.0f, 1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { -1.0f, -1.0f } } };
				auto best_alignment = -1.0f;
				auto best_state = move;
				for ( const auto& state : states )
				{
					const auto length = std::sqrtf( state.x * state.x + state.y * state.y );
					const auto alignment = ( move.x * state.x + move.y * state.y ) / length;
					if ( alignment > best_alignment )
					{
						best_alignment = alignment;
						best_state = state;
					}
				}
				move = best_state;
			}
			return move;
		}

		void clear_jump( systems::input::usercmd* cmd )
		{
			const auto jump = static_cast<std::uintptr_t>( cstypes::command_buttons::in_jump );
			cmd->buttons.value &= ~jump;
			cmd->buttons.value_changed &= ~jump;
			cmd->buttons.value_scroll &= ~jump;
		}

		// Replaces the movement buttons and the analog move with the counter
		// input for the whole tick.
		void write_counter_move( systems::input::usercmd* cmd, const math::vector2& move )
		{
			const auto mask = static_cast<std::uintptr_t>( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back |
				cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );
			cmd->buttons.value &= ~mask;
			cmd->buttons.value_changed |= mask;
			cmd->buttons.value_scroll &= ~mask;
			if ( move.x > 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_forward;
			else if ( move.x < 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_back;
			if ( move.y > 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_moveleft;
			else if ( move.y < 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_moveright;
			if ( auto* base = cmd->csgo_user_cmd.mutable_base( ) )
			{
				base->set_forwardmove( move.x );
				base->set_leftmove( move.y );
			}
		}
	}

	std::optional<autostop_landing_prediction> misc::autostop::predict_landing( std::uintptr_t pawn )
	{
		// Cache check: avoid recalculating landing prediction multiple times per frame.
		// Use a stable frame identifier; systems::g_local snapshot may lag.
		const auto current_command = g_shared.ctx( ).current_tick;
		if ( current_command == this->m_cache_command && this->m_cached_prediction.has_value( ) )
			return this->m_cached_prediction;
		this->m_cache_command = current_command;

		const auto& pre = systems::g_prediction.pre( );
		if ( !pawn || !finite( pre.networked_origin ) || !finite( pre.velocity ) || pre.velocity.z > 0.0f )
		{
			this->m_cached_prediction.reset( );
			return std::nullopt;
		}
		const auto services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !services )
		{
			this->m_cached_prediction.reset( );
			return std::nullopt;
		}
		const auto collision = pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		const auto mins = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		const auto maxs = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );
		if ( !finite( mins ) || !finite( maxs ) )
		{
			this->m_cached_prediction.reset( );
			return std::nullopt;
		}
		const auto gravity = CONVAR( "sv_gravity" )->get<float>( ) * memory::read<float>( pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );
		const auto filter = systems::g_tracing.make_filter( pawn, 0x1c3003, 11 );
		const auto standable = CONVAR( "sv_standable_normal" )->get<float>( );
		auto origin = pre.networked_origin;
		auto velocity = pre.velocity;
		for ( auto tick = 0; tick < landing_lookahead; ++tick )
		{
			velocity.z -= gravity * cstypes::tick_interval * 0.5f;
			const auto end = origin + velocity * cstypes::tick_interval;
			const auto trace = systems::g_tracing.trace_player_bbox( origin, end, { mins, maxs }, filter );
			if ( !trace.all_solid && std::isfinite( trace.fraction ) && trace.fraction >= 0.0f && trace.fraction < 1.0f && trace.normal.z >= standable )
			{
				const auto fraction = std::clamp( trace.fraction, minimum_fraction, maximum_fraction );
				const auto hit = origin + ( end - origin ) * fraction;
				this->m_cached_prediction = autostop_landing_prediction{ tick, fraction, hit };
				return this->m_cached_prediction;
			}
			velocity.z -= gravity * cstypes::tick_interval * 0.5f;
			origin = end;
		}
		this->m_cached_prediction.reset( );
		return std::nullopt;
	}

	bool misc::autostop::request_landing( systems::input::usercmd* cmd, const systems::local::snapshot& local )
	{
		this->m_landing_prediction.reset( );
		this->m_landing_command = -1;
		if ( !cmd ) return false;
		this->m_landing_prediction = this->predict_landing( local.pawn );
		if ( !this->m_landing_prediction ) return false;
		this->m_landing_command = cmd->command_number;
		return true;
	}

	void misc::autostop::write_brake_input( systems::input::usercmd* cmd, autostop_mode mode )
	{
		// Prediction and the final air command must use the same live brake.
		if ( !cmd || !cmd->csgo_user_cmd.mutable_base( ) ) return;
		if ( mode == autostop_mode::air )
		{
			const auto& pre = systems::g_prediction.pre( );
			const auto& velocity = pre.velocity;
			const auto local = systems::g_local.get( );
			if ( !local.pawn || !finite( velocity ) || ( pre.flags & cstypes::entity_flags::on_ground ) ) return;
			const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
			if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip || move_type == cstypes::move_type::observer ) return;

			// Space is jump intent, not an airborne brake veto. Only replace
			// horizontal input; leave jump, duck and vertical physics intact.
			write_counter_move( cmd, velocity.length_2d( ) <= 0.1f ? math::vector2{} : counter_input( velocity ) );
			return;
		}
		if ( ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0 || ( cmd->buttons.value & cstypes::command_buttons::in_jump ) ) return;
		const auto& velocity = systems::g_prediction.pre( ).velocity;
		if ( velocity.length_2d( ) <= 0.1f ) return;
		clear_jump( cmd );
		write_counter_move( cmd, counter_input( velocity ) );
	}

	void misc::autostop::on_create_move( systems::input::usercmd* cmd )
	{
		if ( !cmd ) return;
		const auto mode = g_rage.stop_mode( );
		if ( mode == autostop_mode::none ) return;
		const auto local = systems::g_local.get( );
		if ( !local.pawn || !cmd->csgo_user_cmd.mutable_base( ) ) return;
		if ( mode == autostop_mode::air )
		{
			this->write_brake_input( cmd, mode );
			return;
		}
		if ( ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0 || ( cmd->buttons.value & cstypes::command_buttons::in_jump ) ) return;

		// Abs velocity, not the networked copy (m_vecVelocity trails ~2 ticks):
		// at peek start the lagged read under-reports and the brake engages late;
		// after the true stop it still reads moving and keeps pushing the player
		// backward, stalling the settle gate. The abs vector is what the engine
		// resolves the counter-strafe from this tick.
		const auto& velocity = systems::g_prediction.pre( ).velocity;
		if ( velocity.length_2d( ) <= 0.1f ) return;

		clear_jump( cmd );
		const auto move = counter_input( velocity );

		if ( mode == autostop_mode::ground )
		{
			write_counter_move( cmd, move );
			return;
		}

		if ( this->m_landing_command != cmd->command_number || !this->m_landing_prediction ) return;
		if ( this->m_landing_prediction->ticks > 0 )
		{
			// Landing still a few ticks out: bleed horizontal speed in air.
			write_counter_move( cmd, move );
			return;
		}

		// Landing inside this tick: strip the player's subtick analog so the
		// raw impulses cannot fight the brake, then counter for the whole
		// tick - air accel before the landing fraction, ground counter-strafe
		// after it.
		auto* base = cmd->csgo_user_cmd.mutable_base( );
		auto* moves = base->mutable_subtick_moves( );
		if ( !moves ) return;
		moves->clear( );
		base->m_cached_size = 0;
		write_counter_move( cmd, move );
	}
}
