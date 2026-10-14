#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>

namespace features::combat {
	namespace {
		class scoped_record
		{
		public:
			explicit scoped_record( shared::lagcomp::record* record ) : m_record( record )
			{
				if ( this->m_record ) this->m_record->apply( );
			}

			~scoped_record( )
			{
				if ( this->m_record ) this->m_record->restore( );
			}

			[[nodiscard]] bool applied( ) const noexcept
			{
				return this->m_record && this->m_record->is_applied;
			}

		private:
			shared::lagcomp::record* m_record{};
		};
	}

	std::vector<rage::scan_hit> rage::scan_taser( const math::vector3& eye, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		std::vector<scan_hit> result{};
		const auto& weapon = g_shared.ctx( );
		for ( auto& candidate : candidates )
		{
			const auto scene = memory::read<std::uintptr_t>( candidate.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( !scene ) continue;
			const auto hitboxes = systems::g_hitboxes.query( scene );
			if ( hitboxes.count <= 0 ) continue;
			for ( auto ordinal = 0; ordinal < candidate.record_count; ++ordinal )
			{
				auto* record = candidate.records[ ordinal ];
				if ( !record || !record->valid ) continue;
				scoped_record applied{ record };
				if ( !applied.applied( ) ) continue;
				for ( const auto& hitbox : hitboxes )
				{
					if ( hitbox.bone < 0 || hitbox.bone >= record->bone_count ) continue;
					const auto& bone = record->bones[ hitbox.bone ];
					const auto center = bone.rotation.rotate_vector( ( hitbox.mins + hitbox.maxs ) * 0.5f ) + bone.position;
					if ( center.length_sqr( ) < 1.0f ) continue;
					const auto angle = math::helpers::calculate_angle( eye, center );
					if ( math::helpers::angle_distance( ctx.view_angles, angle ) > settings::g_combat.m_zeusbot.max_fov ) continue;
					math::vector3 forward{};
					math::helpers::angle_vectors_left( angle, &forward );
					if ( trace_taser_hit( eye, forward, weapon.range * 0.85f, candidate.pawn, local.pawn ).hit_entity != candidate.pawn ) continue;
					result.push_back( scan_hit{ .position = center, .aim_angle = angle, .damage = 500.0f,
						.fov = math::helpers::angle_distance( ctx.view_angles, angle ), .hitbox_index = hitbox.index,
						.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hitbox.index ), .bone_index = hitbox.bone,
						.hitbox = hitbox, .is_center = true, .penetrated = false, .pawn = candidate.pawn, .health = candidate.health,
						.record = record, .candidate_ordinal = candidate.ordinal,
						.record_ordinal = static_cast<std::uint8_t>( ordinal ) } );
				}
			}
		}
		return result;
	}

	void rage::run_taser( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_zeusbot.enabled ) return;
		auto candidates = gather_candidates( local );
		if ( candidates.empty( ) ) return;
		const auto eyes = ctx.eyes.count > 0 ? ctx.eyes : resolve_eye_candidates( );
		target best{};
		for ( auto i = 0; i < eyes.count; ++i )
		{
			auto hits = scan_taser( eyes.entries[ i ].position, ctx, candidates, local );
			for ( auto& hit : hits )
			{
				hit.source_eye = eyes.entries[ i ];
				target current{};
				current.hit = hit;
				current.hitchance = 1.0f;
				current.expected_damage = hit.damage;
				current.passes_hitchance = true;
				current.valid = true;
				if ( target_precedes( current, best ) ) best = current;
			}
		}
		if ( best.valid && fire_melee( cmd, best, local ) ) this->m_zeus_fired = true;
	}

	void rage::run_knife( systems::input::usercmd* cmd, const aim_context& ctx, const systems::local::snapshot& local )
	{
		if ( !settings::g_combat.m_knifebot.enabled ) return;
		const auto info = get_knife_info( local );
		if ( !info.can_slash && !info.can_stab ) return;
		auto candidates = gather_candidates( local, 150.0f * 150.0f );
		if ( candidates.empty( ) ) return;
		const auto eyes = ctx.eyes.count > 0 ? ctx.eyes : resolve_eye_candidates( );
		target best{};
		target backstab{};
		for ( auto i = 0; i < eyes.count; ++i )
		{
			auto hits = scan_knife( eyes.entries[ i ].position, ctx, info, candidates, local );
			for ( auto& hit : hits )
			{
				hit.source_eye = eyes.entries[ i ];
				target current{};
				current.hit = hit;
				current.hitchance = 1.0f;
				current.expected_damage = hit.damage;
				current.passes_hitchance = true;
				current.valid = true;
				if ( hit.is_backstab ? target_precedes( current, backstab ) : target_precedes( current, best ) )
				{
					if ( hit.is_backstab ) backstab = current;
					else best = current;
				}
			}
		}
		const auto& selected = backstab.valid ? backstab : best;
		if ( selected.valid && fire_melee( cmd, selected, local ) )
		{
			this->m_knife_attack = static_cast<std::uint8_t>( selected.hit.attack_type );
		}
	}

	rage::knife_info rage::get_knife_info( const systems::local::snapshot& local ) const
	{
		const auto& weapon = g_shared.ctx( );
		const auto tick = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto now = static_cast<float>( tick ) * cstypes::tick_interval;
		const auto last = memory::read<float>( weapon.weapon + SCHEMA( "C_CSWeaponBase", "m_fLastShotTime"_hash ) );
		return { .can_slash = tick >= weapon.next_primary_attack_tick, .can_stab = tick >= weapon.next_secondary_attack_tick,
			.charged = now - last > 0.4f, .armor_ratio = weapon.weapon_vdata ? memory::read<float>( weapon.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flArmorRatio"_hash ) ) : 0.5f };
	}

	std::vector<rage::scan_hit> rage::scan_knife( const math::vector3& eye, const aim_context& ctx, const knife_info& info, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		constexpr auto stab_range = 50.0f;
		constexpr auto slash_range = 66.0f;
		std::vector<scan_hit> result{};
		for ( auto& candidate : candidates )
		{
			const auto scene = memory::read<std::uintptr_t>( candidate.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( !scene ) continue;
			const auto hitboxes = systems::g_hitboxes.query( scene );
			if ( hitboxes.count <= 0 ) continue;
			const auto enemy_angles = memory::read<math::vector3>( candidate.pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
			const auto frontal_slash = get_knife_damage( info.charged ? 40.0f : 25.0f, candidate.armor, info.armor_ratio );
			const auto frontal_stab = get_knife_damage( 65.0f, candidate.armor, info.armor_ratio );
			const auto frontal_lethal = ( info.can_slash && frontal_slash >= candidate.health ) || ( info.can_stab && frontal_stab >= candidate.health );
			for ( auto ordinal = 0; ordinal < candidate.record_count; ++ordinal )
			{
				auto* record = candidate.records[ ordinal ];
				if ( !record || !record->valid ) continue;
				scoped_record applied{ record };
				if ( !applied.applied( ) ) continue;
				const auto delta = record->origin - systems::g_prediction.pre( ).origin;
				const auto distance_2d = delta.length_2d( );
				math::vector3 body_forward{}, enemy_forward{};
				math::helpers::angle_vectors_left( record->rotation, &body_forward );
				math::helpers::angle_vectors_left( enemy_angles, &enemy_forward );
				const auto direction_x = distance_2d > 0.01f ? delta.x / distance_2d : 0.0f;
				const auto direction_y = distance_2d > 0.01f ? delta.y / distance_2d : 0.0f;
				const auto backstab = distance_2d > 0.01f &&
					( direction_x * body_forward.x + direction_y * body_forward.y > 0.475f ||
						direction_x * enemy_forward.x + direction_y * enemy_forward.y > 0.475f );
				for ( const auto& hitbox : hitboxes )
				{
					if ( hitbox.bone < 0 || hitbox.bone >= record->bone_count ) continue;
					const auto& bone = record->bones[ hitbox.bone ];
					const auto center = bone.rotation.rotate_vector( ( hitbox.mins + hitbox.maxs ) * 0.5f ) + bone.position;
					const auto distance = ( center - eye ).length( );
					const auto aim = math::helpers::calculate_angle( eye, center );
					if ( distance <= 0.01f || math::helpers::angle_distance( ctx.view_angles, aim ) > settings::g_combat.m_knifebot.max_fov ) continue;
					for ( const auto stab : { true, false } )
					{
						if ( stab ? !info.can_stab : !info.can_slash ) continue;
						const auto reach = stab ? stab_range : slash_range;
						if ( distance > reach || ( backstab && !frontal_lethal && !stab ) ) continue;
						const auto raw = stab ? ( backstab ? 180.0f : 65.0f ) : ( backstab ? 90.0f : ( info.charged ? 40.0f : 25.0f ) );
						const auto damage = get_knife_damage( raw, candidate.armor, info.armor_ratio );
						math::vector3 forward{};
						math::helpers::angle_vectors_left( aim, &forward );
						if ( trace_knife_hit( eye, forward, reach, candidate.pawn, local.pawn ).hit_entity != candidate.pawn ) continue;
						result.push_back( scan_hit{ .position = center, .aim_angle = aim, .damage = damage, .fov = math::helpers::angle_distance( ctx.view_angles, aim ),
							.hitbox_index = hitbox.index, .hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hitbox.index ), .bone_index = hitbox.bone,
							.hitbox = hitbox, .is_center = true, .is_backstab = backstab, .attack_type = stab ? 1 : 0, .pawn = candidate.pawn,
							.health = candidate.health, .record = record, .candidate_ordinal = candidate.ordinal,
							.record_ordinal = static_cast<std::uint8_t>( ordinal ) } );
						break;
					}
				}
			}
		}
		return result;
	}

	float rage::get_knife_damage( float raw, int armor, float armor_ratio ) const
	{
		if ( armor <= 0 ) return raw;
		const auto health_damage = raw * armor_ratio * 0.5f;
		const auto armor_damage = ( raw - health_damage ) * 0.5f;
		return std::max( 0.0f, std::floorf( armor_damage > static_cast<float>( armor ) ? raw - armor * 2.0f : health_damage ) );
	}

	systems::tracing::result rage::trace_taser_hit( const math::vector3& origin, const math::vector3& forward, float range, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * range;
		const auto filter = systems::g_tracing.make_filter( local_pawn, 0x001c1003, 4 );
		auto result = systems::g_tracing.trace( origin, end, filter );
		if ( result.hit_entity == target_pawn ) return result;
		return systems::g_tracing.trace_sphere( origin, end - forward * 3.0f, 3.0f, filter );
	}

	systems::tracing::result rage::trace_knife_hit( const math::vector3& origin, const math::vector3& forward, float reach, std::uintptr_t target_pawn, std::uintptr_t local_pawn ) const
	{
		const auto end = origin + forward * reach;
		auto result = systems::g_tracing.trace( origin, end, systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4 ) );
		if ( result.hit_entity == target_pawn ) return result;
		return systems::g_tracing.trace_sphere( origin, end - forward * 10.0f, 10.0f, systems::g_tracing.make_filter( local_pawn, 0x0c3001, 4, 15 ) );
	}
}
