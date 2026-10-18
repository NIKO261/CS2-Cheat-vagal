#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include "landing.hpp"

namespace features::movement {

	namespace {

		constexpr auto move_mask = static_cast<std::uintptr_t>( cstypes::command_buttons::in_forward
			| cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );
		constexpr auto forward_button = static_cast<std::uintptr_t>( cstypes::command_buttons::in_forward );
		constexpr auto degrees = 180.0f / std::numbers::pi_v<float>;

		[[nodiscard]] std::uintptr_t axis_button( std::uintptr_t held, std::uintptr_t previous,
			std::uintptr_t selected, std::uintptr_t positive, std::uintptr_t negative )
		{
			const auto axis = held & ( positive | negative );
			if ( axis != ( positive | negative ) ) return axis;
			const auto pressed = axis & ~previous;
			if ( pressed & negative ) return negative;
			if ( pressed & positive ) return positive;
			return selected & axis ? selected & axis : negative;
		}

		struct air_model
		{
			float rate{};
			float cap{};

			// Use the player's actual wishspeed, not max(sv_maxspeed, player).
			[[nodiscard]] bool read( std::uintptr_t pawn, float friction )
			{
				const auto services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
				const auto accel = CONVAR( "sv_airaccelerate" );
				const auto wish_cap = CONVAR( "sv_air_max_wishspeed" );
				if ( !services || !accel || !wish_cap ) return false;
				const auto max_speed = memory::read<float>( services + SCHEMA( "CPlayer_MovementServices", "m_flMaxspeed"_hash ) );
				const auto acceleration = accel->get<float>( );
				const auto limit = wish_cap->get<float>( );
				if ( !std::isfinite( max_speed ) || max_speed <= 0.0f
					|| !std::isfinite( acceleration ) || acceleration <= 0.0f
					|| !std::isfinite( limit ) || limit <= 0.0f
					|| !std::isfinite( friction ) || friction <= 0.0f ) return false;
				rate = acceleration * max_speed * friction;
				cap = std::min( max_speed, limit );
				return std::isfinite( rate ) && rate > 0.0f;
			}

			[[nodiscard]] math::vector2 advance( const math::vector2& velocity, float yaw, float dt ) const
			{
				const auto angle = yaw / degrees;
				const math::vector2 wish{ std::cosf( angle ), std::sinf( angle ) };
				const auto projection = velocity.x * wish.x + velocity.y * wish.y;
				const auto gain = std::max( 0.0f, std::min( rate * dt, cap - projection ) );
				return velocity + wish * gain;
			}

			[[nodiscard]] float wish_yaw( const math::vector2& velocity, float target_yaw, float dt, bool side ) const
			{
				const auto speed = std::hypotf( velocity.x, velocity.y );
				if ( speed <= 0.0001f ) return target_yaw;
				// Maximize |v + min(A, C-v.u)u|. For A >= C the optimum
				// projection is ZERO; C/2 loses a quarter of the energy gain.
				const auto projection = std::min( speed, std::max( 0.0f, cap - rate * dt ) );
				const auto theta = std::acosf( std::clamp( projection / speed, 0.0f, 1.0f ) ) * degrees;
				const auto heading = std::atan2f( velocity.y, velocity.x ) * degrees;
				const auto left_yaw = heading + theta;
				const auto right_yaw = heading - theta;
				const auto left = advance( velocity, left_yaw, dt );
				const auto right = advance( velocity, right_yaw, dt );
				const math::vector2 target{ std::cosf( target_yaw / degrees ), std::sinf( target_yaw / degrees ) };
				const auto score = ( left.x - right.x ) * target.x + ( left.y - right.y ) * target.y;
				return score > 0.0001f ? left_yaw : score < -0.0001f ? right_yaw : side ? left_yaw : right_yaw;
			}
		};

		[[nodiscard]] bool compatible_steps( proto::repeated_ptr_field<proto::subtick_move_step>* moves )
		{
			if ( !moves || moves->size( ) < 0 || moves->size( ) > landing::max_steps
				|| ( moves->size( ) && !moves->m_rep ) ) return false;
			for ( auto i = 0; i < moves->size( ); ++i )
			{
				const auto step = moves->mutable_at( i );
				// Leave any existing movement/aim trajectory to its writer.
				if ( !step || !std::isfinite( step->when( ) ) || step->when( ) < 0.0f || step->when( ) > 1.0f
					|| ( step->button( ) & move_mask ) || step->analog_forward_delta( ) != 0.0f
					|| step->analog_left_delta( ) != 0.0f || step->yaw_delta( ) != 0.0f || step->pitch_delta( ) != 0.0f ) return false;
			}
			return true;
		}

	} // namespace

	void airstrafe::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_pending = false;
		const auto stop_mode = features::combat::g_rage.stop_mode( );
		if ( stop_mode == features::combat::autostop_mode::air || stop_mode == features::combat::autostop_mode::landing ) return;
		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		const auto local = systems::g_local.get( );
		if ( !base || !local.pawn ) return;
		const auto wants_stop = features::misc::g_projectile_trajectory.should_stop( );
		if ( !settings::g_movement.airstrafe.value && !wants_stop ) return;
		const auto& prestate = systems::g_prediction.pre( );
		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( ( prestate.flags & cstypes::entity_flags::on_ground ) || move_type == cstypes::move_type::ladder
			|| move_type == cstypes::move_type::noclip || move_type == cstypes::move_type::observer ) return;
		if ( !std::isfinite( prestate.velocity.x ) || !std::isfinite( prestate.velocity.y ) ) return;

		auto held = cmd->buttons.value & move_mask;
		auto physical = static_cast<std::uintptr_t>( 0 );
		if ( GetAsyncKeyState( 'W' ) & 0x8000 ) physical |= cstypes::command_buttons::in_forward;
		if ( GetAsyncKeyState( 'S' ) & 0x8000 ) physical |= cstypes::command_buttons::in_back;
		if ( GetAsyncKeyState( 'A' ) & 0x8000 ) physical |= cstypes::command_buttons::in_moveleft;
		if ( GetAsyncKeyState( 'D' ) & 0x8000 ) physical |= cstypes::command_buttons::in_moveright;
		if ( physical ) held = physical;
		this->m_last_pressed = axis_button( held, this->m_last_buttons, this->m_last_pressed,
			cstypes::command_buttons::in_forward, cstypes::command_buttons::in_back )
			| axis_button( held, this->m_last_buttons, this->m_last_pressed,
				cstypes::command_buttons::in_moveleft, cstypes::command_buttons::in_moveright );
		this->m_last_buttons = held;

		const auto shift = ( cmd->buttons.value & cstypes::command_buttons::in_sprint ) != 0;
		const auto no_direction = this->m_last_pressed == 0;
		if ( shift )
		{
			// Keep the existing full-strength Shift brake. Scaling it by weapon
			// speed would round to zero on quantized servers at moderate speed.
			const auto speed = prestate.velocity.length_2d( );
			const auto relative = ( std::atan2f( -prestate.velocity.y, -prestate.velocity.x ) * degrees
				- g_movement_fix.source_yaw( ) ) / degrees;
			base->set_forwardmove( speed > 10.0f ? std::cosf( relative ) : 0.0f );
			base->set_leftmove( speed > 10.0f ? std::sinf( relative ) : 0.0f );
			return;
		}
		if ( !wants_stop && no_direction && settings::g_movement.bhop_wasd_steering.value )
		{
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			return;
		}
		if ( wants_stop || ( no_direction && settings::g_movement.airstrafe_fully_directional.value ) )
		{
			this->rotate_to_stop( base, prestate.velocity );
			if ( wants_stop )
			{
				cmd->buttons.value &= ~move_mask;
				cmd->buttons.value_changed |= move_mask;
				cmd->buttons.value_scroll &= ~move_mask;
				if ( base->forwardmove( ) > 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_forward;
				else if ( base->forwardmove( ) < 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_back;
				if ( base->leftmove( ) > 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_moveleft;
				else if ( base->leftmove( ) < 0.0f ) cmd->buttons.value |= cstypes::command_buttons::in_moveright;
			}
			return;
		}
		const auto moves = base->mutable_subtick_moves( );
		if ( !compatible_steps( moves ) ) return;

		const auto forward = ( this->m_last_pressed & cstypes::command_buttons::in_forward ) ? 1.0f
			: ( this->m_last_pressed & cstypes::command_buttons::in_back ) ? -1.0f : 0.0f;
		const auto left = ( this->m_last_pressed & cstypes::command_buttons::in_moveleft ) ? 1.0f
			: ( this->m_last_pressed & cstypes::command_buttons::in_moveright ) ? -1.0f : 0.0f;
		this->m_target_yaw = std::remainderf( g_movement_fix.source_yaw( )
			+ ( no_direction ? 0.0f : std::atan2f( left, forward ) * degrees ), 360.0f );
		this->m_command = cmd;
		this->m_command_number = cmd->command_number;
		this->m_pawn = local.pawn;
		this->m_expected_move = { base->forwardmove( ), base->leftmove( ) };
		this->m_expected_buttons = cmd->buttons.value & move_mask;
		this->m_expected_steps = moves->size( );
		this->m_pending = true;
	}

	bool airstrafe::finish( systems::input::usercmd* cmd )
	{
		const auto pending = this->m_pending;
		this->m_pending = false;
		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		const auto angles = base ? base->viewangles( ) : nullptr;
		const auto moves = base ? base->mutable_subtick_moves( ) : nullptr;
		const auto local = systems::g_local.get( );
		// Later writers retain priority. Consume only this command's unchanged intent.
		if ( !pending || !settings::g_movement.airstrafe.value || cmd != this->m_command || cmd->command_number != this->m_command_number
			|| local.pawn != this->m_pawn || !base || !angles || !std::isfinite( angles->y( ) )
			|| base->forwardmove( ) != this->m_expected_move.x || base->leftmove( ) != this->m_expected_move.y
			|| ( cmd->buttons.value & move_mask ) != this->m_expected_buttons
			|| !compatible_steps( moves ) || moves->size( ) != this->m_expected_steps ) return false;
		const auto& prestate = systems::g_prediction.pre( );
		air_model model{};
		if ( !model.read( local.pawn, prestate.surface_friction ) ) return false;
		const auto persisted = prestate.last_movement_impulses;
		if ( !std::isfinite( persisted.x ) || !std::isfinite( persisted.y ) ) return false;

		auto previous_buttons = this->m_expected_buttons;
		const auto previous = cmd->command_number > 0 && local.controller
			? systems::g_input.get_command_by_sequence( local.controller, static_cast<int>( cmd->command_number - 1 ) ) : nullptr;
		if ( previous && previous != cmd && previous->command_number == cmd->command_number - 1 )
			previous_buttons = previous->buttons.value & move_mask;
		const auto changed = previous_buttons ^ forward_button;
		auto changes = changed;
		auto start_count = 0;
		while ( changes ) { ++start_count; changes &= changes - 1; }
		start_count = std::max( 1, start_count );
		const auto available = landing::available_steps( moves );
		const auto segments = available - start_count;
		if ( segments <= 0 ) return false;

		// Preallocate before touching live input. Failure restores the active pool prefix.
		const auto original_count = moves->size( );
		std::array<proto::subtick_move_step*, landing::max_steps> added{};
		for ( auto i = 0; i < available; ++i )
		{
			added[ i ] = systems::g_input.acquire_subtick_step( moves );
			if ( !added[ i ] || !moves->m_rep || moves->size( ) != original_count + i + 1 )
			{
				moves->m_current_size = original_count;
				return false;
			}
		}
		changes = changed;
		for ( auto i = 0; i < start_count; ++i )
		{
			const auto bit = changes & ( ~changes + 1 );
			added[ i ]->set_when( 0.0f );
			added[ i ]->set_button( bit );
			added[ i ]->set_pressed( ( forward_button & bit ) != 0 );
			changes &= ~bit;
		}
		added[ 0 ]->set_analog_forward_delta( 1.0f - persisted.x );
		added[ 0 ]->set_analog_left_delta( -persisted.y );
		for ( auto i = 0; i < segments; ++i )
			added[ start_count + i ]->set_when( static_cast<float>( i + 1 ) / static_cast<float>( segments ) );

		auto elements = moves->m_rep->elements;
		std::stable_sort( elements, elements + moves->size( ), []( void* a, void* b )
			{ return proto::impl_ptr<proto::subtick_move_step>( a )->when( ) < proto::impl_ptr<proto::subtick_move_step>( b )->when( ); } );
		math::vector2 velocity{ prestate.velocity.x, prestate.velocity.y };
		const auto heading = std::hypotf( velocity.x, velocity.y ) > 0.0001f
			? std::atan2f( velocity.y, velocity.x ) * degrees : this->m_target_yaw;
		const auto steering = static_cast<float>( std::clamp( settings::g_movement.autostrafe_smoothness.value, 0, 100 ) ) / 100.0f;
		// Smoothness changes the steering target, NEVER the acceleration duty.
		const auto target_yaw = heading + std::remainderf( this->m_target_yaw - heading, 360.0f ) * steering;
		auto previous_when = 0.0f;
		auto accumulated_yaw = 0.0f;
		auto wish_yaw = target_yaw;
		for ( auto i = 0; i < moves->size( ); ++i )
		{
			const auto step = moves->mutable_at( i );
			const auto duration = step->when( ) - previous_when;
			if ( duration > 0.0f )
			{
				const auto dt = cstypes::tick_interval * duration;
				wish_yaw = model.wish_yaw( velocity, target_yaw, dt, this->m_side_switch );
				velocity = model.advance( velocity, wish_yaw, dt );
				this->m_side_switch = !this->m_side_switch;
			}
			// Record yaw controls the interval ENDING here; its input events apply
			// afterwards. when=1 prevents the tail snapping back to command yaw.
			const auto relative = std::remainderf( wish_yaw - angles->y( ), 360.0f );
			step->set_yaw_delta( std::remainderf( relative - accumulated_yaw, 360.0f ) );
			accumulated_yaw = relative;
			previous_when = step->when( );
		}
		base->set_forwardmove( 1.0f );
		base->set_leftmove( 0.0f );
		cmd->buttons.value = ( cmd->buttons.value & ~move_mask ) | forward_button;
		cmd->buttons.value_changed = ( cmd->buttons.value_changed & ~move_mask ) | changed;
		cmd->buttons.value_scroll = ( cmd->buttons.value_scroll & ~move_mask ) | ( forward_button & changed );
		base->m_cached_size = 0;
		return true;
	}

	void airstrafe::rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const
	{
		const auto& ctx = features::combat::g_shared.ctx( );
		const auto weapon_speed = ( ctx.valid && ctx.weapon_vdata )
			? memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) ) : 250.0f;
		const auto speed = velocity.length_2d( );
		const auto strength = speed <= 0.1f ? 0.0f : std::clamp( speed / std::max( weapon_speed, 1.0f ), 0.0f, 1.0f );
		const auto relative = ( std::atan2f( -velocity.y, -velocity.x ) * degrees - g_movement_fix.source_yaw( ) ) / degrees;
		base->set_forwardmove( std::cosf( relative ) * strength );
		base->set_leftmove( std::sinf( relative ) * strength );
	}

} // namespace features::movement
