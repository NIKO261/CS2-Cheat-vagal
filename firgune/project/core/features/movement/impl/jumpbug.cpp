#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>
#include "landing.hpp"

namespace features::movement {

	void jumpbug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;
		this->m_prediction_pending = false;

		if ( features::combat::g_rage.should_stop( ) )
		{
			return;
		}

		if ( !cmd || !settings::g_movement.landing_assist.value )
		{
			return;
		}

		auto& buttons = cmd->buttons;
		// The shared setting/bind is the activation intent; do not require Space.

		const auto local = systems::g_local.get( );
		if ( !local.pawn || !local.controller || !local.is_alive )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip || move_type == cstypes::move_type::observer )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( !std::isfinite( prestate.velocity.z ) || prestate.velocity.z >= 0.0f )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto holding_duck = ( buttons.value & cstypes::command_buttons::in_duck ) != 0;
		if ( holding_duck ) return;
		const auto mins = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		const auto maxs = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

		const auto trace_origin = prestate.networked_origin;

		// Movement queries use the pawn's collision attributes, not the shot mask.
		// Keep the verified generic filter/TraceShape ABI; do not copy stale helpers.
		const auto collision_offset = SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		const auto attributes_offset = SCHEMA( "CCollisionProperty", "m_collisionAttribute"_hash );
		const auto interacts_offset = SCHEMA( "VPhysicsCollisionAttribute_t", "m_nInteractsWith"_hash );
		if ( !collision_offset || !attributes_offset || !interacts_offset ) return;
		auto mask = memory::read<std::uint64_t>( local.pawn + collision_offset + attributes_offset + interacts_offset );
		if ( prestate.flags & 0x10u ) mask |= 0x20ull;
		if ( !mask ) return;
		const auto filter = systems::g_tracing.make_filter( local.pawn, mask, 11 );
		const auto gravity = CONVAR( "sv_gravity" );
		const auto standable = CONVAR( "sv_standable_normal" );
		if ( !gravity || !standable ) return;
		const auto sv_gravity = gravity->get<float>( );
		const auto sv_standable_normal = standable->get<float>( );
		const auto gravity_scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );

		auto velocity = prestate.velocity;
		velocity.z -= ( gravity_scale * sv_gravity * cstypes::tick_interval ) * 0.5f;

		math::vector3 trace_start = trace_origin;
		math::vector3 trace_end{};

		trace_end.x = trace_origin.x + velocity.x * cstypes::tick_interval;
		trace_end.y = trace_origin.y + velocity.y * cstypes::tick_interval;
		trace_end.z = trace_origin.z + velocity.z * cstypes::tick_interval;

		trace_end.z -= 2.0f;

		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, { mins, maxs }, filter );
		const auto valid_trace = ( !result.all_solid && std::isfinite( result.fraction ) && result.fraction >= 0.0f && result.fraction < 1.0f );

		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			return;
		}

		// Trace is an eligibility/capacity hint, never the submitted landing time.
		this->m_prediction_command = cmd->command_number;
		this->m_prediction_pawn = local.pawn;
		this->m_prediction_pending = true;
	}

	void jumpbug::finish_prediction( systems::input::usercmd* cmd )
	{
		if ( !this->m_prediction_pending ) return;
		const auto number = this->m_prediction_command;
		const auto pawn = this->m_prediction_pawn;
		this->m_prediction_pending = false;
		const auto local = systems::g_local.get( );
		if ( !cmd || cmd->command_number != number || local.pawn != pawn || !local.controller || !local.is_alive
			|| !settings::g_movement.landing_assist.value || g_edgebug.active_this_tick( )
			|| features::combat::g_rage.should_stop( ) || ( cmd->buttons.value & landing::duck ) ) return;
		systems::g_input.prepare_subtick_movement( );
		const auto baseline = landing::predict( cmd, local );
		if ( !baseline.valid || !baseline.landed ) return;
		const auto pre_speed = systems::g_prediction.pre( ).velocity.length_2d( );
		if ( !std::isfinite( pre_speed ) ) return;
		// A damaged landing is not a speed target: preserve the incoming speed too.
		const auto minimum_speed = std::max( baseline.speed, pre_speed );
		landing::transaction candidate{ cmd, 4 };
		if ( !candidate.valid( ) ) return;

		// Stand while the ducked hull is still above the floor. Waiting for the
		// crouched landing record can already include impact damage and slowdown.
		const auto contact_when = baseline.landing_when;

		float best_when = -1.0f;
		float best_speed = -1.0f;
		const auto apply = [ & ]( float when )
		{
			candidate.reset( );
			landing::set_event( candidate.event( 0 ), landing::duck, true, 0.0f );
			// Release jump throughout the crouched approach, not only at takeoff.
			landing::set_event( candidate.event( 1 ), landing::jump, false, 0.0f );
			landing::set_event( candidate.event( 2 ), landing::duck, false, when );
			landing::set_event( candidate.event( 3 ), landing::jump, true, when );
			// The final button snapshot must agree with the last jump-down event.
			cmd->buttons.value = ( cmd->buttons.value & ~landing::duck ) | landing::jump;
			cmd->buttons.value_changed |= landing::duck | landing::jump;
			cmd->buttons.value_scroll = ( cmd->buttons.value_scroll & ~landing::duck ) | landing::jump;
			candidate.encode_angles( );
		};
		for ( const float offset : { 0.0f, landing::time_step, -landing::time_step } )
		{
			const auto when = std::clamp( contact_when + offset, landing::time_step, 1.0f - landing::time_step );
			apply( when );
			const auto trial = landing::predict( cmd, local );
			// A fresh landed record means CheckFalling ran. Jumping afterwards is
			// an ordinary bhop, not a jumpbug, even if the final state is airborne.
			if ( !trial.valid || !trial.jumped || trial.landed || std::fabsf( trial.jump_when - when ) > landing::time_step
				|| trial.speed + landing::speed_tolerance < minimum_speed ) continue;
			if ( trial.speed > best_speed ) { best_when = when; best_speed = trial.speed; }
		}
		if ( best_when < 0.0f ) return;
		apply( best_when );
		candidate.commit( );
		this->m_active_this_tick = true;
	}

} // namespace features::movement
