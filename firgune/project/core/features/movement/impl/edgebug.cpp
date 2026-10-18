#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include "landing.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		[[nodiscard]] bool side_has_standable_ground( const math::vector3& at_xy_z_reference, float offset_x, float offset_y, const systems::tracing::bbox_collision& bbox, const systems::tracing::filter& filter, float sv_standable_normal )
		{
			const auto start = math::vector3{ at_xy_z_reference.x + offset_x, at_xy_z_reference.y + offset_y, at_xy_z_reference.z + 2.0f };
			const auto end = math::vector3{ at_xy_z_reference.x + offset_x, at_xy_z_reference.y + offset_y, at_xy_z_reference.z - 4.0f };

			const auto r = systems::g_tracing.trace_player_bbox( start, end, bbox, filter );
			return r.fraction > 0.0f && r.fraction < 1.0f && r.normal.z >= sv_standable_normal;
		}

		[[nodiscard]] bool mode_allows( int mode, bool holding_jump, float vel2d, float vz )
		{
			switch ( mode )
			{
			case 0:
				return true;
			case 1:
				return true;
			case 2:
				return !holding_jump;
			case 3:
				return vel2d > 15.0f;
			case 4:
				return vel2d > 25.0f && vz < -100.0f;
			default:
				return true;
			}
		}

		[[nodiscard]] bool edge_geometry_ok( int mode, bool left_ok, bool right_ok )
		{
			if ( mode == 0 )
			{
				return true;
			}

			if ( left_ok && right_ok )
			{
				return false;
			}

			return true;
		}

	} // namespace

	void edgebug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;
		this->m_prediction_pending = false;

		if ( !cmd || !settings::g_movement.landing_assist.value )
		{
			return;
		}

		const auto mode = std::clamp( settings::g_movement.edgebug_mode.value, 0, 4 );
		const auto passes = std::clamp( settings::g_movement.edgebug_passes.value, 1, 5 );

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

		const auto holding_jump = ( cmd->buttons.value & cstypes::command_buttons::in_jump ) != 0;
		const auto vel2d = prestate.networked_velocity.length_2d( );

		if ( !mode_allows( mode, holding_jump, vel2d, prestate.networked_velocity.z ) )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
		const auto holding_duck = ( cmd->buttons.value & cstypes::command_buttons::in_duck ) != 0;
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

		const auto bbox = systems::tracing::bbox_collision{ mins, maxs };
		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, bbox, filter );
		const auto valid_trace = ( result.fraction > 0.0f && result.fraction < 1.0f );

		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			return;
		}

		const auto dot = velocity.x * result.normal.x + velocity.y * result.normal.y;
		if ( !( result.normal.z >= 0.98f || dot >= 0.0f ) )
		{
			return;
		}

		const auto vel2d_len = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
		float perp_x{};
		float perp_y{};
		if ( vel2d_len > 0.01f )
		{
			perp_x = -velocity.y / vel2d_len;
			perp_y = velocity.x / vel2d_len;
		}
		else
		{
			perp_x = 1.0f;
			perp_y = 0.0f;
		}

		const auto side_dist = std::max( 14.0f, std::max( std::fabsf( maxs.x ), std::fabsf( maxs.y ) ) * 1.25f );
		const math::vector3 ground_ref{ result.position.x, result.position.y, trace_origin.z };

		const auto left_ok = side_has_standable_ground( ground_ref, -perp_x * side_dist, -perp_y * side_dist, bbox, filter, sv_standable_normal );
		const auto right_ok = side_has_standable_ground( ground_ref, perp_x * side_dist, perp_y * side_dist, bbox, filter, sv_standable_normal );

		if ( !edge_geometry_ok( mode, left_ok, right_ok ) )
		{
			return;
		}

		this->m_landing_fraction = std::clamp( result.fraction, 0.001f, 0.99f );
		this->m_passes = passes;
		this->m_include_jump = settings::g_movement.edgebug_include_jump_steps.value && holding_jump;
		this->m_needed = passes * 2 + ( this->m_include_jump ? 2 : 0 );
		this->m_prediction_command = cmd->command_number;
		this->m_prediction_pawn = local.pawn;
		this->m_prediction_pending = true;
	}

	void edgebug::finish_prediction( systems::input::usercmd* cmd )
	{
		if ( !this->m_prediction_pending ) return;
		this->m_prediction_pending = false;
		const auto local = systems::g_local.get( );
		if ( !cmd || cmd->command_number != this->m_prediction_command || local.pawn != this->m_prediction_pawn
			|| !local.controller || !local.is_alive || !settings::g_movement.landing_assist.value
			|| features::combat::g_rage.should_stop( ) ) return;
		const auto baseline = landing::predict( cmd, local );
		if ( !baseline.valid ) return;
		const auto& prestate = systems::g_prediction.pre( );
		const auto minimum_speed = std::max( prestate.velocity.length_2d( ), baseline.speed );
		const auto gravity = CONVAR( "sv_gravity" );
		if ( !gravity ) return;
		const auto gravity_scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );
		const auto gravity_step = gravity->get<float>( ) * gravity_scale * cstypes::tick_interval;
		if ( !std::isfinite( gravity_step ) || gravity_step <= 0.0f ) return;
		landing::transaction candidate{ cmd, this->m_needed };
		if ( !candidate.valid( ) ) return;
		const auto apply = [ & ]( float when )
		{
			candidate.reset( );
			const auto seg = when / static_cast<float>( this->m_passes );
			for ( int p = 0; p < this->m_passes; ++p )
			{
				landing::set_event( candidate.event( p * 2 ), landing::duck, true, seg * static_cast<float>( p ) );
				landing::set_event( candidate.event( p * 2 + 1 ), landing::duck, false, seg * static_cast<float>( p + 1 ) );
			}
			if ( this->m_include_jump )
			{
				landing::set_event( candidate.event( this->m_passes * 2 ), landing::jump, false, when );
				landing::set_event( candidate.event( this->m_passes * 2 + 1 ), landing::jump, true, when );
			}
			candidate.encode_angles( );
		};
		const auto accept = [ & ]( const landing::result& trial, float when )
		{
			return ( this->m_include_jump && landing::clean_jump( trial, when, minimum_speed ) )
				|| landing::clean_edge( trial, minimum_speed, prestate.velocity.z, gravity_step );
		};
		if ( !landing::find_timing( cmd, local, this->m_landing_fraction, this->m_passes == 1, apply, accept ) ) return;
		candidate.commit( );
		this->m_active_this_tick = true;
	}

	void edgebug::on_render( xdraw::draw_list& draw_list )
	{
		( void )draw_list;
	}

} // namespace features::movement
