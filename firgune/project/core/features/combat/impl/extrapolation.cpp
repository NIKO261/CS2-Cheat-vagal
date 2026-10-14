#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	namespace {

		[[nodiscard]] constexpr bool can_use_linear_extrapolation( float velocity_2d, std::uint32_t flags ) noexcept
		{
			return ( flags & cstypes::entity_flags::on_ground ) && velocity_2d < 200.0f;
		}

		[[nodiscard]] math::vector3 predict_linear( const math::vector3& origin, const math::vector3& velocity,
		                                            int delta_ticks, float& out_sim_time ) noexcept
		{
			const auto dt = static_cast<float>( delta_ticks ) * cstypes::tick_interval;
			out_sim_time += dt;
			return origin + velocity * dt;
		}

	} // namespace

	void shared::lagcomp::predict_movement( extrapolation_data& data, std::uintptr_t skip_entity, float gravity ) const
	{
		if ( !addresses::globals::game_trace_manager ) return;

		auto remaining = cstypes::tick_interval;
		for ( auto bump = 0; bump < 2 && remaining > 0.0001f; ++bump )
		{
			const auto destination = data.origin + data.velocity * remaining;
			const auto trace = systems::g_tracing.trace_hull( data.origin, destination, data.obb_mins, data.obb_maxs,
				skip_entity, 0x1c3003, 4 );
			if ( !std::isfinite( trace.fraction ) ) break;
			const auto fraction = std::clamp( trace.fraction, 0.0f, 1.0f );
			data.origin = fraction >= 1.0f ? destination : trace.end_pos;
			if ( fraction >= 1.0f || trace.all_solid ) break;
			remaining *= 1.0f - fraction;
			const auto into_surface = data.velocity.dot( trace.normal );
			if ( into_surface < 0.0f ) data.velocity -= trace.normal * into_surface;
			if ( trace.normal.z > 0.7f )
			{
				data.flags |= cstypes::entity_flags::on_ground;
				data.velocity.z = 0.0f;
			}
		}

		const auto ground = systems::g_tracing.trace_hull( data.origin, data.origin + math::vector3{ 0.0f, 0.0f, -2.0f },
			data.obb_mins, data.obb_maxs, skip_entity, 0x1c3003, 4 );
		if ( std::isfinite( ground.fraction ) && ground.fraction < 1.0f && !ground.all_solid && ground.normal.z > 0.7f )
		{
			data.flags |= cstypes::entity_flags::on_ground;
			data.velocity.z = 0.0f;
		}
		else
		{
			data.flags &= ~cstypes::entity_flags::on_ground;
		}
	}

	std::optional<shared::lagcomp::record> shared::lagcomp::extrapolate( std::uintptr_t pawn )
	{
		// ---- cheap guards first ------------------------------------------------
		if ( !settings::g_combat.m_lagcomp.extrapolation.value || !pawn || !addresses::globals::game_trace_manager )
			return std::nullopt;

		// ---- snapshot the latest record ---------------------------------------
		record latest{};
		std::optional<record> previous{};
		{
			std::shared_lock records_lock( this->m_records_mtx );
			const auto it = this->m_records.find( pawn );
			if ( it == this->m_records.end( ) || it->second.empty( ) || !it->second.front( ).valid )
				return std::nullopt;
			latest = it->second.front( );
			latest.is_applied = false;
			latest.bone_cache = 0;
			if ( it->second.size( ) > 1 && it->second[ 1 ].valid )
				previous = it->second[ 1 ];
		}

		// ---- compute delta ticks ----------------------------------------------
		const auto net_client = addresses::globals::network_client_service;
		if ( !net_client ) return std::nullopt;
		const auto tick_state = memory::call_vfunc<std::uintptr_t>( net_client, 23 );
		if ( !tick_state ) return std::nullopt;
		const auto server_tick = memory::read<int>( tick_state + 892 );
		const auto delta_ticks = server_tick - latest.tick;
		if ( delta_ticks <= 0 ) return std::nullopt;

		const auto max_extrap = std::clamp( settings::g_combat.m_lagcomp.max_extrapolate_ticks.value, 1, 16 );
		if ( delta_ticks > max_extrap ) return std::nullopt;

		// ---- read pawn state --------------------------------------------------
		auto velocity = memory::read<math::vector3>( pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
		if ( previous )
		{
			const auto dt = latest.simulation_time - previous->simulation_time;
			if ( dt > 0.0f && dt <= 0.25f ) velocity = ( latest.origin - previous->origin ) / dt;
		}

		const auto velocity_length_2d = velocity.length_2d( );
		if ( !std::isfinite( velocity.x ) || !std::isfinite( velocity.y ) || !std::isfinite( velocity.z ) ||
			 velocity_length_2d < 0.1f || velocity_length_2d > 400.0f )
			return std::nullopt;

		const auto collision = pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		const auto obb_mins = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		const auto obb_maxs = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );
		if ( !std::isfinite( obb_mins.x ) || !std::isfinite( obb_mins.y ) || !std::isfinite( obb_mins.z ) ||
			 !std::isfinite( obb_maxs.x ) || !std::isfinite( obb_maxs.y ) || !std::isfinite( obb_maxs.z ) )
			return std::nullopt;

		const auto flags = memory::read<std::uint32_t>( pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );

		const auto gravity = CONVAR( "sv_gravity" )->get<float>( );

		// ---- fast path: analytical extrapolation ------------------------------
		// When the player is on ground and moving slowly, or when delta_ticks
		// is 1, we can skip the iterative bump loop entirely.  The linear
		// estimate is exact for straight-line ground movement and is within
		// one tick of the true result for the common case.
		extrapolation_data data{};
		data.origin = latest.origin;
		data.velocity = velocity;
		data.sim_time = latest.simulation_time;

		const auto use_linear = can_use_linear_extrapolation( velocity_length_2d, flags );

		if ( delta_ticks == 1 )
		{
			data.obb_mins = obb_mins;
			data.obb_maxs = obb_maxs;
			data.flags = flags;
			data.sim_time += cstypes::tick_interval;
			this->predict_movement( data, pawn, gravity );
		}
		else if ( use_linear )
		{
			// Straight-line estimate: zero trace calls, no collision data needed.
			data.origin = predict_linear( latest.origin, velocity, delta_ticks, data.sim_time );
		}
		else
		{
			// Full iterative simulation, but with adaptive bumps.
			data.obb_mins = obb_mins;
			data.obb_maxs = obb_maxs;
			data.flags = flags;
			for ( auto i = 0; i < delta_ticks; ++i )
			{
				data.sim_time += cstypes::tick_interval;
				this->predict_movement( data, pawn, gravity );
			}
		}

		// ---- validate movement -------------------------------------------------
		const auto origin_delta = data.origin - latest.origin;
		if ( origin_delta.length_sqr( ) < 0.01f ) return std::nullopt;

		// ---- build extrapolated record -----------------------------------------
		record extrap_record = latest;
		extrap_record.origin = data.origin;
		extrap_record.simulation_time = data.sim_time;
		extrap_record.tick = cstypes::time_to_ticks( data.sim_time );
		extrap_record.extrapolated = true;
		extrap_record.is_applied = false;
		extrap_record.bone_cache = 0;

		// Bone adjustment: early exit when the shift is smaller than the
		// typical rendered hitbox resolution (a few units).
		if ( origin_delta.length_sqr( ) >= 1.0f )
		{
			for ( auto i = 0; i < extrap_record.bone_count && i < 128; ++i )
			{
				extrap_record.bones[ i ].position.x += origin_delta.x;
				extrap_record.bones[ i ].position.y += origin_delta.y;
				extrap_record.bones[ i ].position.z += origin_delta.z;
			}
		}

		return extrap_record;
	}

} // namespace features::combat
