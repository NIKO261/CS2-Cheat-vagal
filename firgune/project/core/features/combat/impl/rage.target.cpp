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

	float rage::evaluate_hitchance( const scan_hit& hit, const aim_context& ctx, float inaccuracy, int samples ) const
	{
		if ( !hit.record || !hit.record->valid || hit.bone_index < 0 || hit.bone_index >= hit.record->bone_count ) return 0.0f;
		return g_shared.calculate_hitchance( hit.source_eye.position, hit.aim_angle, hit.hitbox,
			hit.record->bones[ hit.bone_index ], inaccuracy, ctx.spread, samples );
	}

	bool rage::target_precedes( const target& lhs, const target& rhs ) const
	{
		if ( !rhs.valid ) return lhs.valid;
		if ( !lhs.valid ) return false;
		const auto compare = [ ]( auto left, auto right )
		{
			if ( left == right ) return 0;
			return left > right ? 1 : -1;
		};
		if ( const auto v = compare( lhs.passes_hitchance, rhs.passes_hitchance ) )
		{
			// 期望伤害豁免：未过门槛的更高优先级命中（典型是小 hitbox 的头，
			// 残余精度下 hitchance 差几个百分点）只要期望伤害不低于已过门槛
			// 的低优先级命中，就按优先级取——大 hitbox 的 hitchance 优势不该
			// 抢走期望伤害更高的头。门槛本身仍是硬闸：没有任何过门槛候选时
			// 不会放行纯低 hitchance 的射击。
			const auto lhs_priority = hitbox_priority( lhs.hit.hitbox_index );
			const auto rhs_priority = hitbox_priority( rhs.hit.hitbox_index );
			if ( lhs_priority != rhs_priority )
			{
				const auto higher_is_lhs = lhs_priority < rhs_priority;
				const auto& higher = higher_is_lhs ? lhs : rhs;
				const auto& lower = higher_is_lhs ? rhs : lhs;
				if ( higher.expected_damage >= lower.expected_damage )
				{
					return higher_is_lhs;
				}
			}
			return v > 0;
		}
		if ( const auto v = compare( lhs.is_lethal( ), rhs.is_lethal( ) ) ) return v > 0;
		if ( const auto v = compare( -hitbox_priority( lhs.hit.hitbox_index ), -hitbox_priority( rhs.hit.hitbox_index ) ) ) return v > 0;
		if ( const auto v = compare( lhs.expected_damage, rhs.expected_damage ) ) return v > 0;
		if ( const auto v = compare( !lhs.hit.penetrated, !rhs.hit.penetrated ) ) return v > 0;
		if ( const auto v = compare( lhs.hit.is_center, rhs.hit.is_center ) ) return v > 0;
		const auto lhs_tick = lhs.hit.record ? lhs.hit.record->tick : std::numeric_limits<int>::min( );
		const auto rhs_tick = rhs.hit.record ? rhs.hit.record->tick : std::numeric_limits<int>::min( );
		if ( const auto v = compare( lhs_tick, rhs_tick ) ) return v > 0;
		if ( const auto v = compare( -lhs.hit.fov, -rhs.hit.fov ) ) return v > 0;
		if ( const auto v = compare( -static_cast<std::int64_t>( lhs.hit.candidate_ordinal ), -static_cast<std::int64_t>( rhs.hit.candidate_ordinal ) ) ) return v > 0;
		return lhs.hit.hitbox_index < rhs.hit.hitbox_index;
	}

	rage::target rage::select_best( const aim_context& ctx, const std::vector<scan_hit>& hits, float eval_inaccuracy ) const
	{
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto required = config.hitchance_override.value
			? static_cast<float>( config.hitchance_override_value ) / 100.0f
			: static_cast<float>( config.hitchance ) / 100.0f;
		const auto precision = settings::g_combat.m_ragebot.precision.value && !config.no_spread.value;
		if ( precision )
		{
			// First rank every viable point with a small deterministic sample.
			// Only the first likely winner pays for the full verification in the
			// common case; failed estimates fall through to the next point, so a
			// coarse false negative cannot hide a genuinely valid shot.
			const auto performance = settings::g_combat.m_ragebot.precision_mode.value
				== settings::combat::ragebot::precision_mode::performance;
			const auto coarse_samples = performance ? 256 : 128;
			const auto full_samples = performance ? 1024 : 512;
			struct ranked_hit
			{
				const scan_hit* hit{};
				float hitchance{};
				float expected_damage{};
				bool passes{};
			};

			std::vector<ranked_hit> ranked{};
			ranked.reserve( hits.size( ) );
			for ( const auto& hit : hits )
			{
				if ( !hit.record || !hit.record->valid || hit.damage <= 0.0f ) continue;
				const auto hc = std::clamp( this->evaluate_hitchance( hit, ctx, eval_inaccuracy, coarse_samples ), 0.0f, 1.0f );
				ranked.push_back( { &hit, hc, hit.damage * std::max( hc, 0.01f ), hc >= required } );
			}

			const auto compare = [ ]( auto left, auto right )
			{
				if ( left == right ) return 0;
				return left > right ? 1 : -1;
			};
			std::stable_sort( ranked.begin( ), ranked.end( ), [ & ]( const ranked_hit& lhs, const ranked_hit& rhs )
				{
					if ( const auto v = compare( lhs.passes, rhs.passes ) ) return v > 0;
					const auto lhs_lethal = lhs.hit->damage >= static_cast<float>( lhs.hit->health );
					const auto rhs_lethal = rhs.hit->damage >= static_cast<float>( rhs.hit->health );
					if ( const auto v = compare( lhs_lethal, rhs_lethal ) ) return v > 0;
					if ( const auto v = compare( -hitbox_priority( lhs.hit->hitbox_index ), -hitbox_priority( rhs.hit->hitbox_index ) ) ) return v > 0;
					if ( const auto v = compare( lhs.expected_damage, rhs.expected_damage ) ) return v > 0;
					if ( const auto v = compare( !lhs.hit->penetrated, !rhs.hit->penetrated ) ) return v > 0;
					if ( const auto v = compare( lhs.hit->is_center, rhs.hit->is_center ) ) return v > 0;
					const auto lhs_tick = lhs.hit->record ? lhs.hit->record->tick : std::numeric_limits<int>::min( );
					const auto rhs_tick = rhs.hit->record ? rhs.hit->record->tick : std::numeric_limits<int>::min( );
					if ( const auto v = compare( lhs_tick, rhs_tick ) ) return v > 0;
					if ( const auto v = compare( -lhs.hit->fov, -rhs.hit->fov ) ) return v > 0;
					if ( lhs.hit->candidate_ordinal != rhs.hit->candidate_ordinal ) return lhs.hit->candidate_ordinal < rhs.hit->candidate_ordinal;
					return lhs.hit->hitbox_index < rhs.hit->hitbox_index;
				} );

			target fallback{};
			for ( const auto& entry : ranked )
			{
				const auto hc = std::clamp( this->evaluate_hitchance( *entry.hit, ctx, eval_inaccuracy, full_samples ), 0.0f, 1.0f );
				target current{};
				current.hit = *entry.hit;
				current.hitchance = hc;
				current.expected_damage = entry.hit->damage * std::max( hc, 0.01f );
				current.passes_hitchance = hc >= required;
				current.valid = true;

				// The full-sample threshold is authoritative. Precision mode has
				// no expected-damage exemption and never promotes a failed point.
				if ( current.passes_hitchance ) return current;
				if ( !fallback.valid || current.hitchance > fallback.hitchance ||
					( current.hitchance == fallback.hitchance && current.expected_damage > fallback.expected_damage ) )
				{
					fallback = current;
				}
			}
			return fallback;
		}

		target best{};
		// 过门槛最优：comparator-exact 剪枝的锚点。豁免仲裁只会让更高
		// 优先级的命中翻盘，因此"更差优先级 / 同优先级更低伤害"的剪枝
		// 锚到它依然精确，512 采样 hitchance 的性能护栏不变。
		target best_pass{};
		// 低帧率护栏：候选多时减半蒙特卡洛采样（256 样本 ±3% 噪声，
		// 门槛容差内），避免"扫描越重帧越低、帧越低扫描越晚"的螺旋。
		const auto hc_samples = hits.size( ) > 12 ? 256 : 512;
		for ( const auto& hit : hits )
		{
			if ( !hit.record || !hit.record->valid || hit.damage <= 0.0f ) continue;

			// Comparator-exact prune: evaluate_hitchance runs a 512-sample
			// spread simulation per hit on the main thread, which dominates
			// the tick with many players and multipoints on. Once the
			// incumbent passes hitchance, is lethal and sits at a higher
			// hitbox priority, no later hit can precede it (both are
			// comparator keys that do not depend on hitchance); at equal
			// priority a hit with strictly lower raw damage loses the
			// expected-damage key even at a perfect hitchance, since
			// expected_damage = damage * clamp(hc) <= damage.
			if ( best_pass.valid && best_pass.passes_hitchance && best_pass.is_lethal( )
				&& ( hitbox_priority( hit.hitbox_index ) > hitbox_priority( best_pass.hit.hitbox_index )
					|| ( hitbox_priority( hit.hitbox_index ) == hitbox_priority( best_pass.hit.hitbox_index )
						&& hit.damage < best_pass.expected_damage ) ) )
			{
				continue;
			}

			const auto hc = config.no_spread.value ? 1.0f : this->evaluate_hitchance( hit, ctx, eval_inaccuracy, hc_samples );
			target current{};
			current.hit = hit;
			current.hitchance = std::clamp( hc, 0.0f, 1.0f );
			current.expected_damage = hit.damage * std::max( current.hitchance, 0.01f );
			current.passes_hitchance = config.no_spread.value || current.hitchance >= required;
			current.valid = true;
			if ( current.passes_hitchance && ( !best_pass.valid || this->target_precedes( current, best_pass ) ) )
			{
				best_pass = current;
			}
			if ( !best.valid || this->target_precedes( current, best ) ) best = current;
		}

		// 物化豁免仲裁：最终 best 未过门槛但按完整 comparator 赢过了过门槛
		// 候选（期望伤害豁免 / 致命优先），视为可开火——否则 decide_stop 的
		// accurate 门禁会把仲裁结果再拦回去，选择等于白选。
		if ( best.valid && !best.passes_hitchance && best_pass.valid
			&& this->target_precedes( best, best_pass ) )
		{
			best.passes_hitchance = true;
		}
		return best;
	}

}
