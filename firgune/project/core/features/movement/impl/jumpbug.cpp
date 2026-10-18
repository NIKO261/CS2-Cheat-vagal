#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include "landing.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	void jumpbug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;
		this->m_prediction_pending = false;

		if ( !cmd || !settings::g_movement.landing_assist.value )
		{
			return;
		}

		if ( features::movement::g_edgebug.active_this_tick( ) )
		{
			return;
		}

		auto& buttons = cmd->buttons;
		if ( !( buttons.value & cstypes::command_buttons::in_jump ) )
		{
			if ( !g_bhop.suppressed_jump( cmd ) ) return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn || !local.controller || !local.is_alive )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( prestate.networked_velocity.z > 0.0f )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
		const auto holding_duck = ( buttons.value & cstypes::command_buttons::in_duck ) != 0;
		const auto mins = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		auto maxs = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

		auto trace_origin = prestate.networked_origin;
		{
			if ( holding_duck && duck_amount > 0.0f )
			{
				const auto standing_height{ 72.0f };
				const auto duck_hull_diff = standing_height - maxs.z;
				trace_origin.z -= duck_hull_diff * 0.5f;
				maxs.z = standing_height;
			}
		}

		const auto collision_offset = SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		const auto attributes_offset = SCHEMA( "CCollisionProperty", "m_collisionAttribute"_hash );
		const auto interacts_offset = SCHEMA( "VPhysicsCollisionAttribute_t", "m_nInteractsWith"_hash );
		if ( !collision_offset || !attributes_offset || !interacts_offset ) return;
		auto mask = memory::read<std::uint64_t>( local.pawn + collision_offset + attributes_offset + interacts_offset );
		if ( prestate.flags & 0x10u ) mask |= 0x20ull;
		if ( !mask ) return;
		// Use this project's trace ABI, not the reference's cached-context offset.
		const auto filter = systems::g_tracing.make_filter( local.pawn, mask, 11 );
		const auto gravity = CONVAR( "sv_gravity" );
		const auto standable = CONVAR( "sv_standable_normal" );
		if ( !gravity || !standable ) return;
		const auto sv_gravity = gravity->get<float>( );
		const auto sv_standable_normal = standable->get<float>( );
		const auto gravity_scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );

		auto velocity = prestate.networked_velocity;
		velocity.z -= ( gravity_scale * sv_gravity * cstypes::tick_interval ) * 0.5f;

		math::vector3 trace_start = trace_origin;
		math::vector3 trace_end{};

		trace_end.x = trace_origin.x + velocity.x * cstypes::tick_interval;
		trace_end.y = trace_origin.y + velocity.y * cstypes::tick_interval;
		trace_end.z = trace_origin.z + velocity.z * cstypes::tick_interval;

		trace_end.z -= 2.0f;

		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, { mins, maxs }, filter );
		const auto valid_trace = ( result.fraction > 0.0f && result.fraction < 1.0f );

		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			return;
		}

		const auto dot = velocity.x * result.normal.x + velocity.y * result.normal.y;
		this->m_candidate_active = ( result.normal.z >= 0.98f || dot >= 0.0f ) && result.normal.z >= 0.985f;
		this->m_landing_fraction = std::clamp( result.fraction, 0.001f, 0.99f );
		this->m_prediction_command = cmd->command_number;
		this->m_prediction_pawn = local.pawn;
		this->m_prediction_pending = true;
	}

	void jumpbug::finish_prediction( systems::input::usercmd* cmd )
	{
		if ( !this->m_prediction_pending ) return;
		this->m_prediction_pending = false;
		const auto local = systems::g_local.get( );
		if ( !cmd || cmd->command_number != this->m_prediction_command || local.pawn != this->m_prediction_pawn
			|| !local.controller || !local.is_alive || !settings::g_movement.landing_assist.value
			|| features::combat::g_rage.should_stop( ) || g_edgebug.active_this_tick( )
			|| ( !( cmd->buttons.value & landing::jump ) && !g_bhop.suppressed_jump( cmd ) ) ) return;
		const auto restore_jump = !( cmd->buttons.value & landing::jump ) && g_bhop.suppressed_jump( cmd );
		const auto baseline = landing::predict( cmd, local );
		if ( !baseline.valid ) return;
		const auto minimum_speed = std::max( systems::g_prediction.pre( ).velocity.length_2d( ), baseline.speed );
		landing::transaction candidate{ cmd, 4 };
		if ( !candidate.valid( ) ) return;
		const auto apply = [ & ]( float when )
		{
			candidate.reset( );
			if ( restore_jump )
			{
				// Match the endpoint of the accepted jump pulse. The transaction
				// restores bhop's cleared channels whenever a trial is rejected.
				cmd->buttons.value |= landing::jump;
				cmd->buttons.value_changed |= landing::jump;
				cmd->buttons.value_scroll |= landing::jump;
			}
			landing::set_event( candidate.event( 0 ), landing::duck, true, 0.0f );
			landing::set_event( candidate.event( 1 ), landing::duck, false, when );
			landing::set_event( candidate.event( 2 ), landing::jump, false, when );
			landing::set_event( candidate.event( 3 ), landing::jump, true, when );
			candidate.encode_angles( );
		};
		const auto accept = [ & ]( const landing::result& trial, float when )
		{
			return landing::clean_jump( trial, when, minimum_speed );
		};
		// The coarse collision fraction is a search seed, not proof of success.
		if ( !landing::find_timing( cmd, local, this->m_landing_fraction, true, apply, accept ) ) return;
		candidate.commit( );
		this->m_active_this_tick = this->m_candidate_active;
	}

} // namespace features::movement
