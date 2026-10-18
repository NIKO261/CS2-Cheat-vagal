#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/features/features.hpp>
#include <limits>

namespace features::combat {

	namespace {
		// Lower is better: head > chest > stomach > arms > legs > feet.
		[[nodiscard]] int hitbox_priority( int hitbox_index ) noexcept
		{
			if ( hitbox_index < 0 ) return 6;
			if ( hitbox_index <= 1 ) return 0;
			if ( hitbox_index >= 4 && hitbox_index <= 6 ) return 1;
			if ( hitbox_index == 2 || hitbox_index == 3 ) return 2;
			if ( hitbox_index >= 13 && hitbox_index <= 18 ) return 3;
			if ( hitbox_index >= 7 && hitbox_index <= 10 ) return 4;
			if ( hitbox_index == 11 || hitbox_index == 12 ) return 5;
			return 6;
		}
	}

	float rage::required_hitchance( const settings::combat::ragebot::weapon_group& config ) const
	{
		return config.hitchance_override.value
			? static_cast< float >( config.hitchance_override_value ) / 100.0f
			: static_cast< float >( config.hitchance ) / 100.0f;
	}

	void rage::advance_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy, shared::hitchance_result& result, int target_samples ) const
	{
		if ( !hit.record || !hit.record->valid || hit.bone_index < 0 || hit.bone_index >= hit.record->bone_count )
		{
			result.planned = std::max( result.planned, target_samples );
			result.evaluated = std::max( result.evaluated, target_samples );
			result.hits = 0;
			result.complete = result.evaluated >= result.planned;
			return;
		}
		g_shared.advance_hitchance( hit.source_eye.position, hit.aim_angle, hit.hitbox,
			hit.record->bones[ hit.bone_index ], inaccuracy, ctx.spread, result, target_samples );
	}

	// True only when the point can no longer reach the threshold AND cannot
	// overtake the incumbent even if every remaining sample were a hit.
	bool rage::cannot_precede( const scan_hit& hit, const shared::hitchance_result& chance, const target& incumbent, int required_percent ) const
	{
		if ( !incumbent.valid ) return false;
		if ( chance.can_reach( required_percent ) ) return false;
		// A definite failure never precedes a passing incumbent (the first key).
		if ( incumbent.passes_hitchance ) return true;

		target upper{};
		upper.hit = hit;
		upper.hitchance = chance.upper_bound( );
		upper.expected_damage = hit.damage * upper.hitchance;
		upper.passes_hitchance = false;
		upper.valid = true;
		return !this->target_precedes( upper, incumbent );
	}

	// Single final ordering for normal and precision modes:
	// threshold -> lethal -> hitbox priority -> expected damage -> no wall ->
	// center -> newer history -> smaller FOV -> stable target/record/eye/point id.
	bool rage::target_precedes( const target& lhs, const target& rhs ) const
	{
		if ( !rhs.valid ) return lhs.valid;
		if ( !lhs.valid ) return false;
		const auto compare = [ ]( auto left, auto right )
		{
			if ( left == right ) return 0;
			return left > right ? 1 : -1;
		};
		if ( const auto v = compare( lhs.passes_hitchance, rhs.passes_hitchance ) ) return v > 0;
		if ( const auto v = compare( lhs.is_lethal( ), rhs.is_lethal( ) ) ) return v > 0;
		if ( const auto v = compare( -hitbox_priority( lhs.hit.hitbox_index ), -hitbox_priority( rhs.hit.hitbox_index ) ) ) return v > 0;
		if ( const auto v = compare( lhs.expected_damage, rhs.expected_damage ) ) return v > 0;
		if ( const auto v = compare( !lhs.hit.penetrated, !rhs.hit.penetrated ) ) return v > 0;
		if ( const auto v = compare( lhs.hit.is_center, rhs.hit.is_center ) ) return v > 0;
		const auto lhs_tick = lhs.hit.record ? lhs.hit.record->tick : std::numeric_limits<int>::min( );
		const auto rhs_tick = rhs.hit.record ? rhs.hit.record->tick : std::numeric_limits<int>::min( );
		if ( const auto v = compare( lhs_tick, rhs_tick ) ) return v > 0;
		if ( const auto v = compare( -lhs.hit.fov, -rhs.hit.fov ) ) return v > 0;
		if ( lhs.hit.candidate_ordinal != rhs.hit.candidate_ordinal ) return lhs.hit.candidate_ordinal < rhs.hit.candidate_ordinal;
		if ( lhs.hit.record_ordinal != rhs.hit.record_ordinal ) return lhs.hit.record_ordinal < rhs.hit.record_ordinal;
		if ( lhs.hit.source_eye_index != rhs.hit.source_eye_index ) return lhs.hit.source_eye_index < rhs.hit.source_eye_index;
		if ( lhs.hit.point_ordinal != rhs.hit.point_ordinal ) return lhs.hit.point_ordinal < rhs.hit.point_ordinal;
		return lhs.hit.hitbox_index < rhs.hit.hitbox_index;
	}

	rage::target rage::select_best( const aim_context& ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy, rage_stats* stats ) const
	{
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto required_percent = static_cast<int>( config.hitchance_override.value ? config.hitchance_override_value : config.hitchance );
		const auto no_spread = config.no_spread.value;
		const auto precision = settings::g_combat.m_ragebot.precision.value && !no_spread;
		const auto performance = settings::g_combat.m_ragebot.precision_mode.value
			== settings::combat::ragebot::precision_mode::performance;
		// Final verification budget per mode. The coarse pass below only orders the
		// work; it never decides the winner.
		const auto full_samples = precision ? ( performance ? 1024 : 512 ) : 512;
		const auto coarse_samples = performance ? 256 : 128;

		struct ranked_hit
		{
			const scan_hit* hit{};
			shared::hitchance_result chance{};
		};

		std::vector<ranked_hit> ranked{};
		ranked.reserve( hits.size( ) );
		for ( const auto& hit : hits )
		{
			if ( !hit.record || !hit.record->valid || hit.damage <= 0.0f ) continue;
			ranked.push_back( { &hit, {} } );
		}
		if ( ranked.empty( ) ) return {};

		const auto materialize = [ & ]( const ranked_hit& entry )
		{
			target out{};
			out.hit = *entry.hit;
			out.hitchance = no_spread ? 1.0f : std::clamp( entry.chance.value( ), 0.0f, 1.0f );
			// 实际伤害 × 实际命中率；零命中率不会被抬成正值。
			out.expected_damage = entry.hit->damage * out.hitchance;
			out.passes_hitchance = no_spread || entry.chance.reaches( required_percent );
			out.reason = no_spread ? fire_reason::no_spread
				: ( out.passes_hitchance ? fire_reason::hitchance : fire_reason::none );
			out.valid = true;
			return out;
		};

		if ( no_spread )
		{
			// NoSpread keeps its existing path: fire_gun derives the angle and the
			// spread cone is not sampled for selection.
			target best{};
			for ( const auto& entry : ranked )
			{
				const auto current = materialize( entry );
				if ( !best.valid || this->target_precedes( current, best ) ) best = current;
			}
			return best;
		}

		// Coarse pass: order the full verification by the same comparator using
		// the partial estimate. It cannot promote or reject a point by itself.
		if ( precision )
		{
			for ( auto& entry : ranked )
			{
				const auto before = entry.chance.evaluated;
				this->advance_hitchance( *entry.hit, ctx, eval_inaccuracy, entry.chance, coarse_samples );
				if ( stats )
				{
					++stats->work( ).hitchance_points;
					stats->work( ).hitchance_samples += entry.chance.evaluated - before;
				}
			}
			std::vector<target> estimates{};
			estimates.reserve( ranked.size( ) );
			for ( const auto& entry : ranked ) estimates.push_back( materialize( entry ) );
			std::vector<std::size_t> order( ranked.size( ) );
			std::iota( order.begin( ), order.end( ), std::size_t{} );
			std::stable_sort( order.begin( ), order.end( ), [ & ]( std::size_t lhs, std::size_t rhs )
				{
					return this->target_precedes( estimates[ lhs ], estimates[ rhs ] );
				} );
			std::vector<ranked_hit> sorted{};
			sorted.reserve( ranked.size( ) );
			for ( const auto index : order ) sorted.push_back( std::move( ranked[ index ] ) );
			ranked = std::move( sorted );
		}

		const auto begin_ns = k_rage_diagnostics ? steady_now_ns( ) : 0;
		constexpr auto advance_chunk{ 128 };
		target best{};
		for ( auto& entry : ranked )
		{
			// Cheap comparator-exact pre-check: a passing, lethal incumbent with a
			// higher hitbox priority (or the same priority and more raw damage than
			// the incumbent's expected damage) cannot be overtaken by this hit.
			if ( best.valid && best.passes_hitchance && best.is_lethal( )
				&& ( hitbox_priority( entry.hit->hitbox_index ) > hitbox_priority( best.hit.hitbox_index )
					|| ( hitbox_priority( entry.hit->hitbox_index ) == hitbox_priority( best.hit.hitbox_index )
						&& entry.hit->damage < best.expected_damage ) ) )
			{
				continue;
			}

			// Fix the plan at the mode's full budget first; each step only appends
			// the missing suffix, so a coarse result is reused instead of redone.
			if ( stats && !precision ) ++stats->work( ).hitchance_points;
			entry.chance.planned = full_samples;
			entry.chance.complete = entry.chance.evaluated >= full_samples;
			while ( !entry.chance.complete )
			{
				const auto before = entry.chance.evaluated;
				const auto target_samples = std::min( before + advance_chunk, full_samples );
				this->advance_hitchance( *entry.hit, ctx, eval_inaccuracy, entry.chance, target_samples );
				if ( stats ) stats->work( ).hitchance_samples += entry.chance.evaluated - before;
				if ( entry.chance.complete ) break;
				if ( this->cannot_precede( *entry.hit, entry.chance, best, required_percent ) ) break;
			}

			// Points stopped by a proof are skipped; the winner always has the
			// mode's complete verification.
			if ( !entry.chance.complete ) continue;
			const auto current = materialize( entry );
			if ( !best.valid || this->target_precedes( current, best ) ) best = current;
		}
		if constexpr ( k_rage_diagnostics )
		{
			if ( stats ) stats->time( ).hitchance_ns += steady_now_ns( ) - begin_ns;
		}
		return best;
	}
}
