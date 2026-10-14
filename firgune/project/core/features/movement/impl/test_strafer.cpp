#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include "landing.hpp"

namespace features::movement {

	namespace {

		constexpr auto k_max_subtick_moves{ 32 };
		constexpr auto k_min_strafe_speed{ 6.0f };
		constexpr auto k_max_subtick_when{ 0.9f };
		constexpr auto k_resume_when_threshold{ 0.99f };

		// ---------------------------------------------------------------
		// 引擎空气加速模型（Source 2 AirAccelerate 物理）。
		// 参数一次读取并校验，非法值回退默认，避免循环内重复查 convar。
		// ---------------------------------------------------------------

		struct air_model
		{
			float air_accel{ 100.0f };
			float max_speed{ 5000.0f };
			float air_max_wishspeed{ 100.0f };

			[[nodiscard]] static air_model read( )
			{
				auto model = air_model{};

				if ( const auto cvar = CONVAR ("sv_airaccelerate") )
				{
					const auto value = cvar->get<float>( );
					if ( std::isfinite( value ) && value > 0.0f )
					{
						model.air_accel = value;
					}
				}

				if ( const auto cvar = CONVAR ("sv_maxspeed") )
				{
					const auto value = cvar->get<float>( );
					if ( std::isfinite( value ) && value > 0.0f )
					{
						model.max_speed = value;
					}
				}

				if ( const auto cvar = CONVAR ("sv_air_max_wishspeed") )
				{
					const auto value = cvar->get<float>( );
					if ( std::isfinite( value ) && value > 0.0f )
					{
						model.air_max_wishspeed = value;
					}
				}

				return model;
			}
		};

		// 当前速度下让空气加速增益最大的夹角（度）。
		// 由 add_speed = cap - dot(vel, wish) 与 accel_speed = maxspeed * airaccel * dt * friction
		// 联立解得 cos(theta) 的最优值。surface_friction 乘入 accel_speed 以匹配积分时的实际增益。
		[[nodiscard]] float ideal_angle( float speed, float frame_time, const air_model& model, float surface_friction )
		{
			if ( speed < 1.0f )
			{
				return 15.0f;
			}

			const auto accel_speed = model.max_speed * model.air_accel * frame_time * surface_friction;
			const auto cosine = accel_speed >= model.air_max_wishspeed
				? model.air_max_wishspeed / ( 2.0f * speed )
				: ( model.air_max_wishspeed - accel_speed ) / speed;

			return std::fmaxf(
				std::acosf( std::clamp( cosine, -1.0f, 1.0f ) )
					* ( 180.0f / std::numbers::pi_v<float> ),
				1.0f );
		}

		// 本子帧的期望移动方向：速度矢量朝目标 yaw 折转 ideal_angle；低速直飞目标。
		[[nodiscard]] float strafe_wish_yaw(
			float velocity_x,
			float velocity_y,
			float target_yaw,
			float frame_time,
			bool side,
			const air_model& model,
			float surface_friction )
		{
			const auto speed = std::hypotf( velocity_x, velocity_y );
			if ( speed < 15.0f )
			{
				return target_yaw;
			}

			const auto theta = ideal_angle( speed, frame_time, model, surface_friction );
			const auto velocity_yaw = std::atan2f( velocity_y, velocity_x )
				* ( 180.0f / std::numbers::pi_v<float> );
			const auto delta = std::remainderf( target_yaw - velocity_yaw, 360.0f );

			if ( std::fabsf( delta ) > 2.0f )
			{
				return std::remainderf( velocity_yaw + ( delta > 0.0f ? theta : -theta ), 360.0f );
			}

			return std::remainderf( velocity_yaw + ( side ? theta : -theta ), 360.0f );
		}

		// 一个子帧的空气加速：向 wish 方向施加 min(accel_speed, add_speed) 增益。
		void integrate_air_accel(
			float& velocity_x,
			float& velocity_y,
			float wish_yaw,
			float frame_time,
			float surface_friction,
			const air_model& model )
		{
			const auto radians = wish_yaw * ( std::numbers::pi_v<float> / 180.0f );
			const auto wish_x = std::cosf( radians );
			const auto wish_y = std::sinf( radians );

			const auto current_speed = velocity_x * wish_x + velocity_y * wish_y;
			const auto add_speed = model.air_max_wishspeed - current_speed;
			if ( add_speed <= 0.0f )
			{
				return;
			}

			const auto accel_speed = model.max_speed * model.air_accel * surface_friction * frame_time;
			const auto gain = std::fminf( accel_speed, add_speed );
			velocity_x += wish_x * gain;
			velocity_y += wish_y * gain;
		}

		// ---------------------------------------------------------------
		// subtick 调度：从当前最后时刻起，把剩余区间均分给可用槽位。
		// ---------------------------------------------------------------

		struct subtick_schedule
		{
			float start_when{};
			float when_step{};
			int count{};
		};

		[[nodiscard]] bool build_schedule(
			proto::base_usercmd_pb* base,
			int requested,
			subtick_schedule& schedule )
		{
			const auto moves = base ? base->mutable_subtick_moves( ) : nullptr;
			if ( !moves || moves->size( ) > k_max_subtick_moves )
			{
				return false;
			}

			auto max_when{ 0.0f };
			for ( auto i = 0; i < moves->size( ); ++i )
			{
				const auto step = moves->mutable_at( i );
				const auto when = step && step->m_has_bits.test( 0x4u ) && std::isfinite( step->when( ) )
					? std::clamp( step->when( ), 0.0f, 1.0f )
					: 0.0f;
				max_when = std::fmaxf( max_when, when );
			}

			if ( max_when >= k_resume_when_threshold )
			{
				return false;
			}

			const auto available = landing::available_steps( moves );
			schedule.count = std::min( std::clamp( requested, 1, k_max_subtick_moves ), available );
			if ( schedule.count <= 0 )
			{
				return false;
			}

			schedule.start_when = max_when;
			schedule.when_step = ( 1.0f - schedule.start_when ) / static_cast< float >( schedule.count + 1 );
			return true;
		}

		[[nodiscard]] float schedule_when( const subtick_schedule& schedule, int index )
		{
			return std::clamp(
				schedule.start_when + static_cast< float >( index + 1 ) * schedule.when_step,
				0.0f,
				k_max_subtick_when );
		}

	} // namespace

	[[nodiscard]] bool test_strafer::is_active( ) const
	{
		// 不再以 sv_quantize_movement_input 为门槛：非量化服务器上
		// movement_fix 会把 yaw 扫掠折叠成 base 模拟量（经典逐 tick
		// 平移输入），量化服务器上保持原扫掠编码。
		return settings::g_movement.m_test_strafer.enabled.value;
	}

	math::vector2 test_strafer::movement_from_buttons( std::uintptr_t pressed )
	{
		auto forward_move{ 0.0f };
		auto left_move{ 0.0f };

		if ( pressed & cstypes::command_buttons::in_forward )
		{
			forward_move = 1.0f;
		}
		else if ( pressed & cstypes::command_buttons::in_back )
		{
			forward_move = -1.0f;
		}

		if ( pressed & cstypes::command_buttons::in_moveleft )
		{
			left_move = -1.0f;
		}
		else if ( pressed & cstypes::command_buttons::in_moveright )
		{
			left_move = 1.0f;
		}

		return { forward_move, left_move };
	}

	void test_strafer::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_handled_this_tick = false;
		const auto stop_mode = features::combat::g_rage.stop_mode( );
		if ( stop_mode == features::combat::autostop_mode::air || stop_mode == features::combat::autostop_mode::landing )
		{
			return;
		}

		// menu 关闭时不进入任何状态/写入路径。
		// 早期 is_active 还会以 sv_quantize_movement_input 为门，
		// 2026-08-16 归一化后已解除——这里只跟 menu 挂钩即可。
		if ( !this->is_active( ) )
		{
			return;
		}

		const auto current_buttons = cmd->buttons.value;
		if ( current_buttons & static_cast< std::uintptr_t >( cstypes::command_buttons::in_sprint ) )
		{
			return;
		}

		// 理想角模型是纯空气加速物理：地面上 wish 侧向分量逐子帧左右抵消，
		// 摩擦却照常扣速，yaw 扫掠只会拖慢行走，因此仅在空中接管。
		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		this->check_button( current_buttons, cstypes::command_buttons::in_moveleft );
		this->check_button( current_buttons, cstypes::command_buttons::in_moveright );
		this->check_button( current_buttons, cstypes::command_buttons::in_forward );
		this->check_button( current_buttons, cstypes::command_buttons::in_back );
		this->m_last_buttons = current_buttons;

		const auto player_move = movement_from_buttons( this->m_last_pressed );
		if ( player_move.x == 0.0f && player_move.y == 0.0f )
		{
			return;
		}

		const auto velocity = prestate.networked_velocity;
		if ( velocity.length_2d( ) < k_min_strafe_speed )
		{
			return;
		}

		const auto command_yaw = features::movement::g_movement_fix.source_yaw( );
		const auto base_yaw_offset = std::atan2f( -player_move.y, player_move.x )
			* ( 180.0f / std::numbers::pi_v<float> );
		const auto target_yaw = std::remainderf( command_yaw + base_yaw_offset, 360.0f );

		auto model = air_model::read( );
		const auto surface_friction = prestate.surface_friction;

		// 取玩家实际 m_flMaxspeed（可能低于 sv_maxspeed）与 sv_maxspeed 的较高值，
		// 确保规划不因武器移动上限而提前收敛（与 airstrafe 路径一致）。
		if ( const auto local_pawn = systems::g_local.get( ).pawn )
		{
			const auto movement_services = memory::read<std::uintptr_t>( local_pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			if ( movement_services )
			{
				const auto player_maxspeed = memory::read<float>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );
				model.max_speed = std::fmaxf( model.max_speed, player_maxspeed );
			}
		}

		// 平滑度（0-100%）= 转向占每 tick 的占比。引擎空中加速增益只有在
		// wish 与速度夹角处于理想角附近时才近似垂直于速度，直接缩放注入角
		// 会把增益转成前向加速而不是更慢的转向；因此低平滑度走周期化转向
		// 窗口：每周期以完整理想角转向 s·slot 时长，其余时间中和 wish 惯性
		// 滑行，转向速率与增速随占比精确缩放。100% 保持原逐子帧编码不变。
		const auto smoothness = std::clamp( settings::g_movement.m_test_strafer.smoothness.value, 0, 100 ) / 100.0f;
		if ( smoothness <= 0.0f )
		{
			return;
		}

		auto accumulated_yaw = command_yaw;
		auto simulated_x = velocity.x;
		auto simulated_y = velocity.y;
		auto injected{ 0 };

		if ( smoothness >= 1.0f )
		{
			subtick_schedule schedule{};
			if ( !build_schedule( base, k_max_subtick_moves, schedule ) )
			{
				return;
			}

			const auto sub_frame = cstypes::tick_interval * schedule.when_step;

			for ( auto i = 0; i < schedule.count; ++i )
			{
				const auto entry_side = ( ( this->m_substep_counter + i ) % 2 ) == 0;
				const auto wish_yaw = strafe_wish_yaw(
					simulated_x, simulated_y, target_yaw, sub_frame, entry_side, model, surface_friction );

				const auto target_view_yaw = std::remainderf( wish_yaw - base_yaw_offset, 360.0f );
				const auto yaw_delta = std::remainderf( target_view_yaw - accumulated_yaw, 360.0f );

				if ( !this->apply_yaw_subtick( base, schedule_when( schedule, i ), yaw_delta ) )
				{
					break;
				}

				accumulated_yaw = target_view_yaw;
				integrate_air_accel( simulated_x, simulated_y, wish_yaw, sub_frame, surface_friction, model );
				++injected;
			}
		}
		else
		{
			// 周期化转向：区间 [max_when, 0.9] 均分为 cycles 个周期，每周期
			// 先中和 wish 滑行 (1-s)·slot，再以完整理想角转向 s·slot。
			const auto moves = base->mutable_subtick_moves( );
			if ( !moves )
			{
				return;
			}

			auto max_when{ 0.0f };
			for ( auto i = 0; i < moves->size( ); ++i )
			{
				const auto step = moves->mutable_at( i );
				const auto when = step && step->m_has_bits.test( 0x4u ) && std::isfinite( step->when( ) )
					? std::clamp( step->when( ), 0.0f, 1.0f )
					: 0.0f;
				max_when = std::fmaxf( max_when, when );
			}

			if ( max_when >= k_resume_when_threshold )
			{
				return;
			}

			// 每周期 2 步（中和 + 转向）加收尾中和 1 步，再留 1 槽给失败恢复。
			const auto cycles = std::min( 15, ( landing::available_steps( moves ) - 2 ) / 2 );
			if ( cycles <= 0 )
			{
				return;
			}

			const auto slot = ( k_max_subtick_when - max_when ) / static_cast< float >( cycles );
			if ( slot <= 0.0f )
			{
				return;
			}

			const auto window_dt = slot * smoothness * cstypes::tick_interval;
			const math::vector2 base_move{ base->forwardmove( ), base->leftmove( ) };
			auto analog_live = true;

			for ( auto j = 0; j < cycles; ++j )
			{
				const auto coast_when = std::clamp( max_when + j * slot, 0.0f, k_max_subtick_when );
				if ( analog_live && !this->apply_analog_subtick( base, coast_when, -base_move.x, -base_move.y ) )
				{
					break;
				}
				analog_live = false;

				const auto entry_side = ( ( this->m_substep_counter + j ) % 2 ) == 0;
				const auto wish_yaw = strafe_wish_yaw(
					simulated_x, simulated_y, target_yaw, window_dt, entry_side, model, surface_friction );
				const auto target_view_yaw = std::remainderf( wish_yaw - base_yaw_offset, 360.0f );
				const auto yaw_delta = std::remainderf( target_view_yaw - accumulated_yaw, 360.0f );

				const auto steer_when = std::clamp( coast_when + ( 1.0f - smoothness ) * slot, 0.0f, k_max_subtick_when );
				if ( !this->apply_yaw_subtick( base, steer_when, yaw_delta,
					analog_live ? 0.0f : base_move.x, analog_live ? 0.0f : base_move.y ) )
				{
					// 转向步失败：恢复模拟量，避免输入链断在滑行段。
					if ( !analog_live )
					{
						this->apply_analog_subtick( base, coast_when, base_move.x, base_move.y );
						analog_live = true;
					}
					break;
				}
				analog_live = true;

				accumulated_yaw = target_view_yaw;
				integrate_air_accel( simulated_x, simulated_y, wish_yaw, window_dt, surface_friction, model );
				++injected;
			}

			// 收尾中和：最后一个转向窗口结束后 wish 保持归零到 tick 末尾。
			if ( injected > 0 )
			{
				this->apply_analog_subtick( base,
					std::clamp( max_when + cycles * slot, 0.0f, k_max_subtick_when ), -base_move.x, -base_move.y );
			}
		}

		if ( injected > 0 )
		{
			this->m_handled_this_tick = true;
			++this->m_substep_counter;
		}
	}

	bool test_strafer::apply_yaw_subtick( proto::base_usercmd_pb* base, float when, float yaw_delta, float forward_delta, float left_delta ) const
	{
		math::helpers::normalize_angle( yaw_delta );

		if ( std::fabsf( yaw_delta ) <= 0.01f )
		{
			return false;
		}

		const auto subtick_moves = base->mutable_subtick_moves( );
		if ( !subtick_moves )
		{
			return false;
		}

		const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
		if ( !step )
		{
			return false;
		}

		step->set_when( when );
		step->set_button( 0 );
		step->set_pressed( false );
		step->set_analog_forward_delta( forward_delta );
		step->set_analog_left_delta( left_delta );
		step->set_yaw_delta( yaw_delta );
		step->set_pitch_delta( 0.0f );
		return true;
	}

	bool test_strafer::apply_analog_subtick( proto::base_usercmd_pb* base, float when, float forward_delta, float left_delta ) const
	{
		const auto subtick_moves = base ? base->mutable_subtick_moves( ) : nullptr;
		if ( !subtick_moves )
		{
			return false;
		}

		const auto step = systems::g_input.acquire_subtick_step( subtick_moves );
		if ( !step )
		{
			return false;
		}

		step->set_when( when );
		step->set_button( 0 );
		step->set_pressed( false );
		step->set_analog_forward_delta( forward_delta );
		step->set_analog_left_delta( left_delta );
		step->set_yaw_delta( 0.0f );
		step->set_pitch_delta( 0.0f );
		return true;
	}

	void test_strafer::check_button( std::uintptr_t current_buttons, std::uintptr_t button )
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

} // namespace features::movement
