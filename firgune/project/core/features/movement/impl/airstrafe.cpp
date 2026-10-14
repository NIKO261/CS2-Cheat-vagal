#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include "landing.hpp"

namespace features::movement {

	void airstrafe::on_create_move( systems::input::usercmd* cmd )
	{
		const auto rage_stop_mode = features::combat::g_rage.stop_mode( );
		if ( rage_stop_mode == features::combat::autostop_mode::air || rage_stop_mode == features::combat::autostop_mode::landing )
		{
			// Autostop owns this command's brake, including its low-speed tail.
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder
			|| move_type == cstypes::move_type::noclip
			|| move_type == cstypes::move_type::observer )
		{
			return;
		}

		auto current_buttons = cmd->buttons.value;
		const auto movement_buttons = static_cast<std::uintptr_t>(
			cstypes::command_buttons::in_forward |
			cstypes::command_buttons::in_back |
			cstypes::command_buttons::in_moveleft |
			cstypes::command_buttons::in_moveright );
		const auto command_movement = current_buttons & movement_buttons;
		auto physical_movement = static_cast<std::uintptr_t>( 0 );
		if ( GetAsyncKeyState( 'W' ) & 0x8000 ) physical_movement |= cstypes::command_buttons::in_forward;
		if ( GetAsyncKeyState( 'S' ) & 0x8000 ) physical_movement |= cstypes::command_buttons::in_back;
		if ( GetAsyncKeyState( 'A' ) & 0x8000 ) physical_movement |= cstypes::command_buttons::in_moveleft;
		if ( GetAsyncKeyState( 'D' ) & 0x8000 ) physical_movement |= cstypes::command_buttons::in_moveright;

		// The command movement bits are not guaranteed to be populated at this
		// point when AA is disabled. Physical keys are authoritative while held;
		// command bits preserve bind/controller input when no WASD key is down.
		const auto directional_input = physical_movement != 0
			? physical_movement
			: command_movement;
		current_buttons = ( current_buttons & ~movement_buttons ) | directional_input;

		if ( settings::g_combat.m_antiaim.enabled.value )
		{
			cmd->buttons.value = ( cmd->buttons.value & ~movement_buttons ) | ( current_buttons & movement_buttons );
			cmd->buttons.value_changed |= movement_buttons;
		}

		const auto wants_stop = features::misc::g_projectile_trajectory.should_stop( );

		// 早期退出：menu 关闭且没有任何 wants_stop 触发，整段逻辑都不该跑
		// （包括 shift-hold 段——它之前是无门禁的，会在按住 shift 空中时
		// 无视 menu 改写 base move 与 subtick，导致"开启后变慢/出现
		// autostrafer 效果"）。
		if ( !settings::g_movement.airstrafe.value && !wants_stop )
		{
			return;
		}

		const bool shift_held = current_buttons & static_cast< std::uintptr_t >( cstypes::command_buttons::in_sprint );

		const auto& prestate = systems::g_prediction.pre( );
		const bool in_air = !( prestate.flags & cstypes::entity_flags::on_ground );

		if ( shift_held && in_air )
		{
			const auto& vel = prestate.networked_velocity;
			float forward_move = 0.0f;
			float left_move = 0.0f;

			if ( vel.length_2d( ) > 10.0f )
			{
				const auto vel_yaw = std::atan2f( vel.y, vel.x )
					* ( 180.0f / std::numbers::pi_v<float> );

				const auto view_yaw = features::movement::g_movement_fix.source_yaw( );
				const auto relative_yaw = ( vel_yaw - view_yaw )
					* ( std::numbers::pi_v<float> / 180.0f );

				const auto fwd_x = std::cosf( relative_yaw );
				const auto fwd_y = std::sinf( relative_yaw );

				forward_move = std::clamp( -fwd_x, -1.0f, 1.0f );
				left_move = std::clamp( -fwd_y, -1.0f, 1.0f ); // negated
			}

			base->set_forwardmove( forward_move );
			base->set_leftmove( left_move );

			// write subtick entries so the engine respects the input precisely
			const auto subtick_moves = base->mutable_subtick_moves( );
			if ( subtick_moves && landing::available_steps( subtick_moves ) > 0 )
			{
				const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
				if ( step )
				{
					step->set_button( 0 );
					step->set_pressed( false );
					step->set_when( 0.0f );
					const auto& source_impulses = features::movement::g_movement_fix.source_impulses( );
					step->set_analog_forward_delta( forward_move - source_impulses.x );
					step->set_analog_left_delta( left_move - source_impulses.y );
				}
			}
			return;
		}

		if ( !wants_stop && settings::g_movement.m_test_strafer.enabled.value && features::movement::g_test_strafer.handled_this_tick( ) )
		{
			return;
		}

		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( current_buttons & static_cast< std::uintptr_t >( cstypes::command_buttons::in_sprint ) )
		{
			return;
		}

		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( !subtick_moves )
		{
			return;
		}

		if ( !wants_stop )
		{
			this->check_button( current_buttons, cstypes::command_buttons::in_moveleft );
			this->check_button( current_buttons, cstypes::command_buttons::in_moveright );
			this->check_button( current_buttons, cstypes::command_buttons::in_forward );
			this->check_button( current_buttons, cstypes::command_buttons::in_back );
			this->m_last_buttons = current_buttons;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto sv_airaccelerate = CONVAR ("sv_airaccelerate")->get<float>( );
		const auto sv_air_max_wishspeed = CONVAR ("sv_air_max_wishspeed")->get<float>( );
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_staminarecoveryrate = CONVAR ("sv_staminarecoveryrate")->get<float>( );

		const auto view_angles = this->m_angles;
		const auto cmd_move_backup = math::vector3{ base->forwardmove( ), base->leftmove( ), 0.0f };
		const auto effective_maxspeed = memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );

		const auto subtick_count = landing::available_steps( subtick_moves );
		if ( subtick_count <= 0 ) return;
		const auto frame_time = cstypes::tick_interval / static_cast< float >( subtick_count );

		auto yaw_offset{ 0.0f };

		if ( !wants_stop )
		{
			if ( this->m_last_pressed & cstypes::command_buttons::in_moveleft )
			{
				yaw_offset += 90.0f;
			}

			if ( this->m_last_pressed & cstypes::command_buttons::in_moveright )
			{
				yaw_offset -= 90.0f;
			}

			if ( this->m_last_pressed & cstypes::command_buttons::in_forward )
			{
				yaw_offset *= 0.5f;
			}
			else if ( this->m_last_pressed & cstypes::command_buttons::in_back )
			{
				yaw_offset = -yaw_offset * 0.5f + 180.0f;
			}
		}

		const auto has_direction_input = ( this->m_last_pressed & cstypes::command_buttons::in_moveleft ) || ( this->m_last_pressed & cstypes::command_buttons::in_moveright ) || ( this->m_last_pressed & cstypes::command_buttons::in_forward ) || ( this->m_last_pressed & cstypes::command_buttons::in_back );
		if ( settings::g_movement.bhop_wasd_steering.value && !wants_stop && !has_direction_input )
		{
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			return;
		}

		const auto effective_wants_stop = wants_stop || ( settings::g_movement.airstrafe_fully_directional.value && !has_direction_input );

		auto velocity = prestate.networked_velocity;
		const auto& source_impulses = features::movement::g_movement_fix.source_impulses( );
		auto last_impulses = math::vector3{ source_impulses.x, source_impulses.y, prestate.last_movement_impulses.z };
		auto stamina = prestate.stamina;
		const auto surface_friction = prestate.surface_friction;

		if ( last_impulses.y < 0.0f )
		{
			this->m_side_switch = false;
		}
		else if ( last_impulses.y > 0.0f )
		{
			this->m_side_switch = true;
		}

		for ( auto i = 0; i < subtick_count; ++i )
		{
			const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
			if ( !step )
			{
				break;
			}
			base->set_forwardmove( cmd_move_backup.x );
			base->set_leftmove( cmd_move_backup.y );

			auto speed_2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );

			if ( stamina > 0.0f )
			{
				const auto speed_scale = std::clamp( 1.0f - ( stamina / 100.0f ), 0.0f, 1.0f );
				speed_2d *= speed_scale * speed_scale;
				stamina = std::fmaxf( stamina - ( frame_time * sv_staminarecoveryrate ), 0.0f );
			}

			velocity.z -= ( sv_gravity * frame_time );

			if ( speed_2d > 0.0001f )
			{
				math::vector3 forward_dir{}, right_dir{};
				math::helpers::angle_vectors_2d( features::movement::g_movement_fix.source_yaw( ), forward_dir, right_dir );

				math::vector3 wish_dir
				{
					( forward_dir.x * last_impulses.x * effective_maxspeed ) + ( right_dir.x * last_impulses.y * effective_maxspeed ),
					( forward_dir.y * last_impulses.x * effective_maxspeed ) + ( right_dir.y * last_impulses.y * effective_maxspeed ),
					0.0f
				};

				auto wish_speed = std::sqrtf( wish_dir.x * wish_dir.x + wish_dir.y * wish_dir.y );
				if ( wish_speed > 0.0001f )
				{
					wish_dir.x /= wish_speed;
					wish_dir.y /= wish_speed;
					wish_dir.z = 0.0f;
				}

				wish_speed = std::fminf( wish_speed, effective_maxspeed );

				const auto capped_wish = std::fminf( wish_speed, sv_air_max_wishspeed );
				const auto current_speed = velocity.x * wish_dir.x + velocity.y * wish_dir.y;
				const auto add_speed = capped_wish - current_speed;

				if ( add_speed > 0.0f )
				{
					const auto accel_speed = sv_airaccelerate * effective_maxspeed * frame_time * surface_friction;
					const auto gain = std::fminf( accel_speed, add_speed );

					velocity.x += wish_dir.x * gain;
					velocity.y += wish_dir.y * gain;
				}
			}

			velocity.z += ( sv_gravity * frame_time ) * 0.5f;
			speed_2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );

			if ( speed_2d >= 10.0f || effective_wants_stop )
			{
				base->set_forwardmove( 0.0f );
				base->set_leftmove( 0.0f );

				if ( effective_wants_stop )
				{
					this->rotate_to_stop( base, velocity );
				}
				else
				{
					// 临界角选向：cos(theta*) = (wish_cap - accel) / speed 时 add_speed
					// 恰好等于引擎每子帧的 accel 上限，速度模长增益最大（低速退化为 0 度沿速度直推）。
					const auto velocity_angle = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / std::numbers::pi_v<float> );
					const auto accel_speed = sv_airaccelerate * effective_maxspeed * frame_time * surface_friction;
					const auto ideal_cos = accel_speed >= sv_air_max_wishspeed
						? sv_air_max_wishspeed / ( 2.0f * speed_2d )
						: ( sv_air_max_wishspeed - accel_speed ) / speed_2d;
					const auto ideal_angle = std::acosf( std::clamp( ideal_cos, -1.0f, 1.0f ) ) * ( 180.0f / std::numbers::pi_v<float> );

					auto target_yaw = view_angles.y + yaw_offset;
					math::helpers::normalize_angle( target_yaw );

					const auto velocity_delta = std::remainderf( target_yaw - velocity_angle, 360.0f );

					if ( std::fabsf( velocity_delta ) > 2.0f )
					{
						// 未对齐：朝目标一侧偏临界角，速度每子帧向 wish 折转并逐步逼近目标方向。
						target_yaw = velocity_angle + ( velocity_delta > 0.0f ? ideal_angle : -ideal_angle );
					}
					else
					{
						// 已对齐：左右交替偏临界角，净方向保持在目标线上。
						target_yaw = velocity_angle + ( this->m_side_switch ? ideal_angle : -ideal_angle );
					}

					math::helpers::normalize_angle( target_yaw );

					base->set_forwardmove( 1.0f );
					base->set_leftmove( 0.0f );

					this->rotate_movement( base, target_yaw, features::movement::g_movement_fix.source_yaw( ) );
				}
			}

			step->set_button( 0 );
			step->set_pressed( false );
			step->set_when( static_cast< float >( i ) / static_cast< float >( subtick_count ) );
			step->set_analog_forward_delta( base->forwardmove( ) - last_impulses.x );
			step->set_analog_left_delta( base->leftmove( ) - last_impulses.y );

			last_impulses.x += base->forwardmove( ) - last_impulses.x;
			last_impulses.y += base->leftmove( ) - last_impulses.y;

			if ( !effective_wants_stop )
			{
				this->m_side_switch = !this->m_side_switch;
			}
		}

		if ( wants_stop )
		{
			cmd->buttons.value &= ~movement_buttons;
			cmd->buttons.value_changed |= movement_buttons;
			cmd->buttons.value_scroll &= ~movement_buttons;

			if ( base->forwardmove( ) > 0.0f )
			{
				cmd->buttons.value |= cstypes::command_buttons::in_forward;
			}
			else if ( base->forwardmove( ) < 0.0f )
			{
				cmd->buttons.value |= cstypes::command_buttons::in_back;
			}

			if ( base->leftmove( ) > 0.0f )
			{
				cmd->buttons.value |= cstypes::command_buttons::in_moveleft;
			}
			else if ( base->leftmove( ) < 0.0f )
			{
				cmd->buttons.value |= cstypes::command_buttons::in_moveright;
			}
		}
	}

	void airstrafe::store_angles( )
	{
		this->m_angles = systems::g_input.get_view_angles( );
		this->m_angles.y = features::movement::g_movement_fix.source_yaw( );
	}

	void airstrafe::check_button( std::uintptr_t current_buttons, std::uintptr_t button )
	{
		constexpr auto moveleft = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveleft );
		constexpr auto moveright = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveright );
		constexpr auto forward = static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward );
		constexpr auto back = static_cast< std::uintptr_t >( cstypes::command_buttons::in_back );

		if ( current_buttons & button && ( !( this->m_last_buttons & button ) || ( button & moveleft && !( this->m_last_pressed & moveright ) ) || ( button & moveright && !( this->m_last_pressed & moveleft ) ) || ( button & forward && !( this->m_last_pressed & back ) ) || ( button & back && !( this->m_last_pressed & forward ) ) ) )
		{
			if ( button & moveleft )
			{
				this->m_last_pressed &= ~moveright;
			}
			else if ( button & moveright )
			{
				this->m_last_pressed &= ~moveleft;
			}
			else if ( button & forward )
			{
				this->m_last_pressed &= ~back;
			}
			else if ( button & back )
			{
				this->m_last_pressed &= ~forward;
			}

			this->m_last_pressed |= button;
		}
		else if ( !( current_buttons & button ) )
		{
			this->m_last_pressed &= ~button;
		}
	}

	void airstrafe::rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const
	{
		const auto forward_move = base->forwardmove( );
		const auto side_move = base->leftmove( );

		math::vector3 target_forward{}, target_right{};
		math::helpers::angle_vectors_2d( target_yaw, target_forward, target_right );

		// angle_vectors_2d 的 right 是几何左（逆时针 +90 度）；
		// 引擎 leftmove 为正 = 右移，因此目标世界向量对 side 分量取反。
		// 旧实现缺少该取反且输出整体取负，实际 wish 被镜像成速度反侧，
		// 空中接管（如 test strafer 开火让路）时表现为持续反向刹车。
		const auto world = target_forward * forward_move - target_right * side_move;

		math::vector3 view_forward{}, view_right{};
		math::helpers::angle_vectors_2d( view_yaw, view_forward, view_right );

		base->set_forwardmove( std::clamp( view_forward.dot( world ), -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( -view_right.dot( world ), -1.0f, 1.0f ) );
	}

	void airstrafe::rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const
	{
		const auto speed = velocity.length_2d( );
		const auto wish_yaw = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / std::numbers::pi_v<float> ) + 180.0f;

		{
			const auto& ctx = features::combat::g_shared.ctx( );
			const auto max_speed = ( ctx.valid && ctx.weapon_vdata ) ? memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) ) : 250.0f;

			base->set_forwardmove( std::clamp( speed / max_speed, 0.0f, 1.0f ) );
			base->set_leftmove( 0.0f );
		}

		const auto rotation = ( features::movement::g_movement_fix.source_yaw( ) - wish_yaw ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto fwd = base->forwardmove( );
		const auto side = base->leftmove( );

		base->set_forwardmove( std::clamp( std::cosf( rotation ) * fwd - std::sinf( rotation ) * side, -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( ( std::sinf( rotation ) * fwd + std::cosf( rotation ) * side ) * -1.0f, -1.0f, 1.0f ) );
	}

} // namespace features::movement
