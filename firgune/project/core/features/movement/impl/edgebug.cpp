#include <pch/pch.hpp>
#include <array>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>
#include "landing.hpp"

namespace features::movement {

	void edgebug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;
		this->m_prediction_pending = false;

		if ( !cmd || !settings::g_movement.landing_assist.value )
		{
			return;
		}

		const auto passes = std::clamp( settings::g_movement.edgebug_passes.value, 1, 5 );

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

		const auto holding_duck = ( cmd->buttons.value & cstypes::command_buttons::in_duck ) != 0;
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

		// Keep this unpadded: the fraction also seeds a within-tick trial time.

		const auto bbox = systems::tracing::bbox_collision{ mins, maxs };
		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, bbox, filter );
		const auto valid_trace = ( !result.all_solid && std::isfinite( result.fraction ) && result.fraction >= 0.0f && result.fraction < 1.0f );

		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			return;
		}

		// Sideways hull probes rejected front/back edges too. Geometry only
		// schedules trials; final airborne contact/velocity determines success.
		this->m_hint_when = result.fraction;
		this->m_passes = passes;
		this->m_needed = passes * 2 + 1; // Duck cycles plus a jump release.
		this->m_prediction_command = cmd->command_number;
		this->m_prediction_pawn = local.pawn;
		this->m_prediction_pending = true;
	}

	void edgebug::finish_prediction( systems::input::usercmd* cmd )
	{
		if ( !this->m_prediction_pending ) return;
		const auto number = this->m_prediction_command;
		const auto pawn = this->m_prediction_pawn;
		this->m_prediction_pending = false;
		const auto local = systems::g_local.get( );
		if ( !cmd || cmd->command_number != number || local.pawn != pawn || !local.controller || !local.is_alive
			|| !settings::g_movement.landing_assist.value || features::combat::g_rage.should_stop( )
			|| ( cmd->buttons.value & landing::duck ) ) return;
		systems::g_input.prepare_subtick_movement( );
		const auto baseline = landing::predict( cmd, local );
		if ( !baseline.valid ) return;
		landing::transaction candidate{ cmd, this->m_needed };
		if ( !candidate.valid( ) ) return;
		// A fleeting edge contact need not leave a last-landed record. The ray
		// hint seeds trials only; airborne contact/velocity are verified below.
		const auto contact_when = baseline.landed ? baseline.landing_when : this->m_hint_when;
		const auto gravity = CONVAR( "sv_gravity" );
		if ( !gravity ) return;
		const auto scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );
		const auto gravity_step = gravity->get<float>( ) * scale * cstypes::tick_interval;
		if ( !std::isfinite( gravity_step ) || gravity_step <= 0.0f ) return;
		const auto apply = [ & ]( float when )
		{
			candidate.reset( );
			const auto seg = when / static_cast<float>( this->m_passes );
			int index = 0;
			for ( int p = 0; p < this->m_passes; ++p )
			{
				landing::set_event( candidate.event( index++ ), landing::duck, true, seg * p );
				landing::set_event( candidate.event( index++ ), landing::duck, false, seg * ( p + 1 ) );
			}
			// Edge trials never jump; jumpbug owns the fallback if no edge works.
			landing::set_event( candidate.event( index ), landing::jump, false, 0.0f );
			cmd->buttons.value &= ~( landing::duck | landing::jump );
			cmd->buttons.value_changed |= landing::duck | landing::jump;
			cmd->buttons.value_scroll &= ~( landing::duck | landing::jump );
			candidate.encode_angles( );
		};
		float best_when = -1.0f;
		float best_speed = -1.0f;
		for ( const float offset : { 0.0f, landing::time_step, -landing::time_step } )
		{
			const auto when = std::clamp( contact_when + offset, landing::time_step, 1.0f - landing::time_step );
			apply( when );
			const auto trial = landing::predict( cmd, local );
			if ( !trial.valid || ( trial.flags & cstypes::entity_flags::on_ground )
				|| trial.speed + landing::speed_tolerance < baseline.speed ) continue;
			// A real contact interrupts the downward trajectory. Accept the
			// remaining sub-frame gravity range, not a magic fixed -6.25 Z.
			if ( trial.jumped || trial.velocity.z > 0.0f || trial.velocity.z < -gravity_step - 1.0f
				|| trial.velocity.z <= systems::g_prediction.pre( ).velocity.z + 1.0f ) continue;
			if ( trial.speed > best_speed ) { best_when = when; best_speed = trial.speed; }
		}
		if ( best_when < 0.0f ) return;
		apply( best_when );
		candidate.commit( );
		this->m_active_this_tick = true;
	}

	void edgebug::on_render( xdraw::draw_list& draw_list )
	{
		( void )draw_list;

		if ( !settings::g_movement.landing_assist.value )
		{
			return;
		}
	}

} // namespace features::movement
