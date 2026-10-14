#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		constexpr auto k_min_speed = 20.0f;

		constexpr auto k_movement_mask = static_cast<std::uintptr_t>(
			cstypes::command_buttons::in_forward
			| cstypes::command_buttons::in_back
			| cstypes::command_buttons::in_moveleft
			| cstypes::command_buttons::in_moveright );

		// 移动按钮位与模拟量保持一致（与 slowwalk / movement_fix 的同名逻辑同语义）。
		void sync_movement_buttons( systems::input::usercmd* cmd, float forward_move, float side_move )
		{
			constexpr auto epsilon{ 1.0e-4f };
			auto buttons = cmd->buttons.value & ~k_movement_mask;
			if ( forward_move > epsilon )
			{
				buttons |= cstypes::command_buttons::in_forward;
			}
			else if ( forward_move < -epsilon )
			{
				buttons |= cstypes::command_buttons::in_back;
			}

			if ( side_move > epsilon )
			{
				buttons |= cstypes::command_buttons::in_moveleft;
			}
			else if ( side_move < -epsilon )
			{
				buttons |= cstypes::command_buttons::in_moveright;
			}

			const auto changed = ( cmd->buttons.value ^ buttons ) & k_movement_mask;
			cmd->buttons.value = buttons;
			cmd->buttons.value_changed = ( cmd->buttons.value_changed & ~k_movement_mask ) | changed;
			cmd->buttons.value_scroll = ( cmd->buttons.value_scroll & ~k_movement_mask )
				| ( buttons & changed & k_movement_mask );
		}

	} // namespace

	// 松开方向键后接管滑行：写满幅反向移动（counter-strafe）替代引擎摩擦
	// 的长尾减速。有输入的 tick 不接管，与 slowwalk / 玩家转向互不抢输入。
	void fast_stop::on_create_move( systems::input::usercmd* cmd ) const
	{
		if ( !settings::g_movement.fast_stop.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn || !cmd || !cmd->csgo_user_cmd.mutable_base( ) )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( !( prestate.flags & cstypes::entity_flags::on_ground ) )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		// 起跳 tick 仍在地面，但起跳后无地面摩擦，反向输入只会歪跳向；
		// bhop / edgejump 连跳期间持续按住跳，这里一并让路。
		if ( cmd->buttons.value & cstypes::command_buttons::in_jump )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( base->forwardmove( ) != 0.0f || base->leftmove( ) != 0.0f )
		{
			return;
		}

		// m_vecAbsVelocity（networked 副本滞后 ~2 tick，autostop 同源结论）。
		const auto& velocity = prestate.velocity;
		const auto speed = velocity.length_2d( );

		// 阈值以下交给引擎摩擦收尾（drop 下限 sv_stopspeed*friction/tickrate
		// ≈ 6.5 u/s/tick，几 tick 归零）；满幅反向输入在此速度下会反向过冲。
		if ( speed <= k_min_speed )
		{
			return;
		}

		// 与 autostop 的 counter_input 同策略：世界系反方向旋转到 source yaw
		// 基准（本阶段 base 一律相对源视角表达，movement_fix::finish 统一
		// reframe 到最终 AA 基准）。
		const auto world_x = -velocity.x / speed;
		const auto world_y = -velocity.y / speed;
		const auto yaw = math::helpers::deg_to_rad( g_movement_fix.source_yaw( ) );
		const auto sine = std::sinf( yaw );
		const auto cosine = std::cosf( yaw );
		auto move = math::vector2{
			std::clamp( world_x * cosine + world_y * sine, -1.0f, 1.0f ),
			std::clamp( ( world_x * sine - world_y * cosine ) * -1.0f, -1.0f, 1.0f )
		};

		// 量化服务器（server.dll 已核实）SetupMove 对 base forward/side 逐项
		// roundf：分数模拟量分量 <0.5 归零、方向偏斜，刹车弱且歪。吸附到最近
		// 8 向全幅数字状态（±1 整数对，roundf 不动点），模拟与下发逐字节一致
		// （方向误差 ≤22.5°，与 autostop 同一取舍）。
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

		base->set_forwardmove( move.x );
		base->set_leftmove( move.y );
		sync_movement_buttons( cmd, move.x, move.y );
	}

} // namespace features::movement
