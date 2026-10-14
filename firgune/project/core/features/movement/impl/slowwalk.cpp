#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		constexpr auto k_movement_mask = static_cast<std::uintptr_t>(
			cstypes::command_buttons::in_forward
			| cstypes::command_buttons::in_back
			| cstypes::command_buttons::in_moveleft
			| cstypes::command_buttons::in_moveright );

		// 移动按钮位与模拟量保持一致（与 movement_fix 的同名逻辑同语义）。
		// 引擎 wish = forward * forwardmove + left * leftmove，left 为玩家左侧，
		// 因此 leftmove 正 = 左（IN_MOVELEFT），负 = 右（IN_MOVERIGHT）。
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

	void slowwalk::on_create_move( systems::input::usercmd* cmd ) const
	{
		if ( !settings::g_movement.slowwalk.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( !( prestate.flags & cstypes::entity_flags::on_ground ) )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto forward_move = base->forwardmove( );
		const auto side_move = base->leftmove( );

		if ( forward_move == 0.0f && side_move == 0.0f )
		{
			return;
		}

		const auto move_length = std::sqrtf( forward_move * forward_move + side_move * side_move );

		if ( move_length < 0.001f )
		{
			return;
		}

		const auto target_ratio = settings::g_movement.slowwalk_speed / 100.0f;

		auto quantized{ true };
		if ( const auto cvar = CONVAR( "sv_quantize_movement_input" ) )
		{
			quantized = cvar->get<bool>( );
		}

		// 量化服务器（server.dll 已核实）：SetupMove 对 forward/side/up move
		// 逐项 roundf——分数 base 模拟量 <0.5 变 0、≥0.5 变满速，缩放路径
		// 必然失效。数字速度只能用速度控制器逼近目标：低于目标保留原始
		// 满速输入，高于目标反向全速输入制动（counter-strafe，同时加快
		// 换向），模拟量与按钮位同步翻转。
		if ( quantized )
		{
			const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			if ( !movement_services )
			{
				return;
			}

			const auto max_speed = memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );
			const auto speed = prestate.networked_velocity.length_2d( );

			if ( speed > target_ratio * max_speed )
			{
				base->set_forwardmove( -forward_move );
				base->set_leftmove( -side_move );
				sync_movement_buttons( cmd, -forward_move, -side_move );
			}

			return;
		}

		const auto scaled = std::fminf( move_length, target_ratio );

		base->set_forwardmove( ( forward_move / move_length ) * scaled );
		base->set_leftmove( ( side_move / move_length ) * scaled );
	}

} // namespace features::movement