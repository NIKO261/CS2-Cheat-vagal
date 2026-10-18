#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/threadpool/threadpool.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
#include <limits>

namespace features::combat {
	namespace {
		// Hitbox scan order from the weapon group config: head, chest,
		// stomach, arms, legs, feet. Falls back to head + body when no
		// group is enabled. Force b-aim removes the head group.
		[[nodiscard]] int build_hitbox_order( const settings::combat::ragebot::weapon_group& config, std::array<int, 19>& order )
		{
			int count = 0;
			const auto add = [ & ]( std::initializer_list<int> ids, int group, bool allow )
			{
				if ( !allow || ( group == 0 && config.body_aim.value ) ) return;
				for ( const auto id : ids ) if ( count < static_cast< int >( order.size( ) ) ) order[ count++ ] = id;
			};
			add( { 0 }, 0, config.hitboxes.values[ 0 ] );
			add( { 4, 5, 6 }, 1, config.hitboxes.values[ 1 ] );
			add( { 3, 2 }, 2, config.hitboxes.values[ 2 ] );
			add( { 13, 14, 15, 16, 17, 18 }, 3, config.hitboxes.values[ 3 ] );
			add( { 7, 8, 9, 10 }, 4, config.hitboxes.values[ 4 ] );
			add( { 11, 12 }, 5, config.hitboxes.values[ 5 ] );
			if ( count == 0 )
			{
				if ( !config.body_aim.value ) order[ count++ ] = 0;
				for ( const auto id : { 4, 5, 6, 3, 2 } ) order[ count++ ] = id;
			}
			return count;
		}

		// Closed-form braking distance under a full counter input:
		// while the brake bleeds the speed the body keeps travelling
		// ALONG +v for v^2 / (2a) - the stop position is AHEAD of the
		// current one, never behind it. The constant-deceleration model
		// evaluates friction at the current (maximum) speed, so the true
		// stop lands ~20% further out; the 1.2 factor compensates and
		// biases the predicted stop slightly PAST the real one, which
		// favours engaging the brake before the peek fully opens.
		// 1.2 -> 1.5：更激进的提前急停 - 预测停点再往 peek 前方偏 25%，
		// 刹车在更早、掩体更深处介入；进入击杀区后由 best.valid 接管完成刹停。
		[[nodiscard]] math::vector3 brake_displacement( const math::vector3& velocity )
		{
			const auto speed = velocity.length_2d( );
			if ( speed <= 0.01f ) return {};
			const auto surface = std::max( systems::g_prediction.pre( ).surface_friction, 0.0f );
			const auto wish_speed = std::max( g_shared.ctx( ).weapon_max_speed, 1.0f );
			const auto accel_term = CONVAR( "sv_accelerate" )->get<float>( ) * wish_speed * surface;
			const auto friction_term = std::max( speed, CONVAR( "sv_stopspeed" )->get<float>( ) ) * CONVAR( "sv_friction" )->get<float>( ) * surface;
			const auto decel = accel_term + friction_term;
			if ( decel <= 1.0f ) return {};
			const auto distance = speed * speed / ( 2.0f * decel ) * 1.5f;
			return { velocity.x / speed * distance, velocity.y / speed * distance, 0.0f };
		}
	}

	rage::scan_config rage::make_scan_config( const settings::combat::ragebot::weapon_group& config ) const
	{
		scan_config out{};
		out.hitbox_count = build_hitbox_order( config, out.hitbox_order );
		for ( auto i = 0; i < 6; ++i ) out.multipoints[ static_cast<std::size_t>( i ) ] = config.multipoints.values[ static_cast<std::size_t>( i ) ];
		out.pointscale = config.pointscale.value;
		out.max_fov = config.max_fov.value;
		out.dynamic_pointscale = config.dynamic_pointscale.value;
		out.debug_points = config.debug_multipoints.value;
		return out;
	}

	std::vector<rage::candidate> rage::gather_candidates( const systems::local::snapshot& local, float max_distance_sq ) const
	{
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );
		const auto origin = systems::g_prediction.pre( ).origin;
		auto* self = const_cast< rage* >( this );
		self->m_scan_records.clear( );
		self->m_scan_records.reserve( players.size( ) * k_max_scan_records );

		std::vector<candidate> result{};
		result.reserve( players.size( ) );
		std::vector<std::size_t> starts{};
		starts.reserve( players.size( ) );

		const auto weapon_type = g_shared.ctx( ).weapon_type;
		const auto is_gun = weapon_type >= cstypes::weapon_type::pistol && weapon_type <= cstypes::weapon_type::lmg;
		const auto& weapon_config = settings::g_combat.m_ragebot.get_group( weapon_type );
		const auto precision = is_gun && settings::g_combat.m_ragebot.precision.value && !weapon_config.no_spread.value;
		const auto precision_performance = precision && settings::g_combat.m_ragebot.precision_mode.value
			== settings::combat::ragebot::precision_mode::performance;

		for ( const auto& controller : players )
		{
			if ( !controller.ptr || controller.ptr == local.controller ||
				!memory::read<bool>( controller.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) ) continue;

			const auto handle = memory::read<std::uint32_t>( controller.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
			const auto pawn = systems::g_entities.lookup( handle );
			if ( !pawn || pawn == local.pawn ) continue;
			const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) || memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 ||
				memory::read<bool>( pawn + SCHEMA( "C_CSPlayerPawn", "m_bGunGameImmunity"_hash ) ) ) continue;

			const auto health = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			const auto records = g_shared.lc( ).get_valid_record_copies( pawn );
			std::array<shared::lagcomp::record, k_max_scan_records> selected{};
			auto selected_count = 0;
			if ( !records.empty( ) )
			{
				// Precision never invents a pose between network updates: the
				// command can only rewind the server to records that actually
				// existed. Spread the limited scan budget across those records so
				// the newest and oldest endpoints are always retained.
				const auto record_budget = precision ? ( precision_performance ? k_max_scan_records : 3 ) : 2;
				const auto record_count = std::min( static_cast<int>( records.size( ) ), record_budget );
				for ( auto slot = 0; slot < record_count; ++slot )
				{
					const auto index = record_count == 1 ? std::size_t{} :
						static_cast<std::size_t>( slot ) * ( records.size( ) - 1 ) / static_cast<std::size_t>( record_count - 1 );
					selected[ selected_count++ ] = records[ index ];
				}
			}
			else if ( const auto extrapolated = g_shared.lc( ).extrapolate( pawn ) )
			{
				selected[ selected_count++ ] = *extrapolated;
			}
			if ( selected_count == 0 ) continue;

			if ( max_distance_sq > 0.0f )
			{
				auto nearest = std::numeric_limits<float>::max( );
				for ( auto i = 0; i < selected_count; ++i ) nearest = std::min( nearest, selected[ i ].origin.distance_sqr( origin ) );
				if ( nearest > max_distance_sq ) continue;
			}

			// Copy every record first; the stable references are attached only
			// after the collection loop so no container growth can invalidate a
			// pointer while the scan snapshot is alive.
			const auto start = self->m_scan_records.size( );
			for ( auto i = 0; i < selected_count; ++i )
			{
				if ( selected[ i ].valid ) self->m_scan_records.push_back( std::move( selected[ i ] ) );
			}
			const auto record_count = self->m_scan_records.size( ) - start;
			if ( record_count == 0 ) continue;

			candidate out{};
			out.pawn = pawn;
			out.health = health;
			out.armor = memory::read<int>( pawn + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) );
			if ( is_gun )
			{
				const auto& config = settings::g_combat.m_ragebot.get_group( weapon_type );
				out.min_damage = this->get_min_damage( config, health, config.min_damage_override.value );
			}
			out.record_count = static_cast<int>( record_count );
			out.ordinal = result.size( );
			result.push_back( out );
			starts.push_back( start );
		}

		for ( std::size_t i = 0; i < result.size( ); ++i )
		{
			for ( auto r = 0; r < result[ i ].record_count; ++r )
			{
				result[ i ].records[ r ] = &self->m_scan_records[ starts[ i ] + static_cast<std::size_t>( r ) ];
			}
		}

		// Double tap kill efficiency: with a single visible target the pair
		// kills it together - each bullet only needs half its HP, which
		// opens up body shots instead of waiting for a headshot-only lethal
		// point. Multiple targets keep the per-bullet lethal floor.
		if ( is_gun && this->m_double_tap.can_pair( ) && result.size( ) == 1 )
		{
			const auto& config = settings::g_combat.m_ragebot.get_group( weapon_type );
			if ( !config.min_damage_override.value && config.min_damage >= 101 )
			{
				auto& cand = result.front( );
				cand.min_damage = std::max( 1.0f, std::ceil( static_cast< float >( cand.health ) * 0.5f ) );
			}
		}

		return result;
	}

	void rage::prepare_scan_targets( std::vector<candidate>& candidates )
	{
		std::size_t total{};
		for ( const auto& cand : candidates ) total += static_cast<std::size_t>( cand.record_count );

		this->m_prepared_records.clear( );
		if ( total == 0 ) return;
		// Exact size first: no reallocation may move a prepared record while the
		// scan (or a late re-scan of the same command) still references it.
		this->m_prepared_records.resize( total );

		std::size_t next{};
		for ( auto& cand : candidates )
		{
			const auto first = next;
			for ( auto r = 0; r < cand.record_count; ++r )
			{
				auto& prepared = this->m_prepared_records[ next++ ];
				prepared.record = cand.records[ r ];
				prepared.ordinal = static_cast<std::uint8_t>( r );
				prepared.hitboxes = {};
				if ( prepared.record )
				{
					prepared.penetration = g_shared.pen( ).prepare_target( cand.pawn, prepared.record );
					for ( const auto& hitbox : prepared.penetration.hitboxes )
					{
						if ( hitbox.index >= 0 && hitbox.index < static_cast<int>( prepared.hitboxes.size( ) ) )
						{
							prepared.hitboxes[ static_cast<std::size_t>( hitbox.index ) ] = &hitbox;
						}
					}
				}
			}
			for ( auto r = 0; r < cand.record_count; ++r ) cand.prepared[ r ] = &this->m_prepared_records[ first + static_cast<std::size_t>( r ) ];
		}
	}

	void rage::scan_players( const math::vector3& eye, int eye_index, float inaccuracy, const aim_context& ctx, const scan_config& scan, std::vector<candidate>& candidates, const systems::local::snapshot& local, std::vector<scan_hit>& out, rage_stats* stats )
	{
		out.clear( );
		if ( candidates.empty( ) ) return;

		const auto collect = stats != nullptr && k_rage_diagnostics;
		this->m_scan_slots.resize( candidates.size( ) );
		this->m_debug_slots.resize( candidates.size( ) );
		for ( auto& slot : this->m_scan_slots ) slot.clear( );
		for ( auto& slot : this->m_debug_slots ) slot.clear( );
		if ( collect ) this->m_scan_counters.resize( candidates.size( ) );

		threadpool::parallel_for( 0, static_cast<int>( candidates.size( ) ), [ & ]( int begin, int end )
		{
			for ( auto i = begin; i < end; ++i )
			{
				auto& cand = candidates[ static_cast<std::size_t>( i ) ];
				auto& slot = this->m_scan_slots[ static_cast<std::size_t>( i ) ];
				auto& debug = this->m_debug_slots[ static_cast<std::size_t>( i ) ];
				auto* counters = collect ? &this->m_scan_counters[ static_cast<std::size_t>( i ) ] : nullptr;
				if ( counters ) *counters = {};
				for ( auto r = 0; r < cand.record_count; ++r )
				{
					auto* prepared = cand.prepared[ r ];
					if ( !prepared || !prepared->record || !prepared->record->valid ) continue;
					this->scan_player( eye, eye_index, inaccuracy, ctx, scan, cand, *prepared, local, slot, counters, debug );
				}
			}
		}, 1 );

		// Deterministic merge: candidate order, then record order, then the
		// point order produced by scan_player.
		std::size_t total{};
		for ( const auto& slot : this->m_scan_slots ) total += slot.size( );
		out.reserve( total );
		for ( auto& slot : this->m_scan_slots )
		{
			for ( const auto& hit : slot ) out.push_back( hit );
		}

		if ( collect )
		{
			for ( const auto& counter : this->m_scan_counters ) stats->add( counter );
		}
		if ( scan.debug_points )
		{
			std::lock_guard lock( this->m_debug_mtx );
			for ( const auto& slot : this->m_debug_slots )
			{
				for ( const auto& point : slot ) this->m_debug_points.push_back( point );
			}
		}
	}

	void rage::scan_player( const math::vector3& eye, int eye_index, float inaccuracy, const aim_context& ctx, const scan_config& scan, const candidate& cand, const prepared_record& prepared, const systems::local::snapshot& local, std::vector<scan_hit>& out, rage_stats::counters* counters, std::vector<debug_point>& debug ) const
	{
		if ( !cand.pawn || !prepared.record || !prepared.record->valid || cand.health <= 0 ) return;
		const auto& penetration = prepared.penetration;
		if ( !penetration.target_pawn ) return;
		const auto bone_limit = std::min( prepared.record->bone_count, 27 );
		if ( bone_limit <= 0 ) return;

		struct point { math::vector3 position{}; systems::hitboxes::entry hitbox{}; int bone{}; bool center{}; };
		std::array<point, 19 * 5> points{};
		std::size_t point_count{};
		const auto append_point = [ & ]( const point& value )
		{
			if ( point_count < points.size( ) ) points[ point_count++ ] = value;
		};

		const auto group_for = []( int id )
		{
			if ( id == 0 ) return 0; if ( id >= 4 && id <= 6 ) return 1; if ( id == 2 || id == 3 ) return 2;
			if ( id >= 13 && id <= 18 ) return 3; if ( id >= 7 && id <= 10 ) return 4; if ( id == 11 || id == 12 ) return 5; return -1;
		};

		for ( auto i = 0; i < scan.hitbox_count; ++i )
		{
			const auto id = scan.hitbox_order[ static_cast<std::size_t>( i ) ];
			const auto* hitbox = id >= 0 && id < static_cast<int>( prepared.hitboxes.size( ) ) ? prepared.hitboxes[ static_cast<std::size_t>( id ) ] : nullptr;
			if ( hitbox == nullptr || hitbox->bone < 0 || hitbox->bone >= bone_limit ) continue;
			const auto& bone = prepared.record->bones[ hitbox->bone ];
			if ( bone.position.length_sqr( ) < 1.0f ) continue;
			const auto center = bone.rotation.rotate_vector( ( hitbox->mins + hitbox->maxs ) * 0.5f ) + bone.position;
			append_point( { center, *hitbox, hitbox->bone, true } );
			if ( scan.debug_points ) debug.push_back( { center, id, true } );

			const auto group = group_for( id );
			if ( group >= 0 && scan.pointscale > 0.0f && scan.multipoints[ static_cast<std::size_t>( group ) ] )
			{
				// The unoccluded damage ceiling cannot prove that the center
				// penetrates a wall. Keep enabled edge points; center_good below
				// skips their traces only after an actual viable center trace.
				for ( const auto& position : generate_multipoints( *hitbox, center, bone.rotation, scan.pointscale, eye, inaccuracy, ctx.spread, scan.dynamic_pointscale ) )
				{
					append_point( { position, *hitbox, hitbox->bone, false } );
					if ( scan.debug_points ) debug.push_back( { position, id, false } );
				}
			}
		}
		if ( counters ) counters->points += static_cast<int>( point_count );

		std::array<bool, 19> center_good{};
		for ( auto point_index = std::size_t{}; point_index < point_count; ++point_index )
		{
			const auto& point = points[ point_index ];
			if ( !point.center && point.hitbox.index >= 0 && point.hitbox.index < 19 && center_good[ static_cast<std::size_t>( point.hitbox.index ) ] ) continue;
			const auto angle = math::helpers::calculate_angle( eye, point.position );
			const auto fov = math::helpers::angle_distance( ctx.view_angles, angle );
			if ( fov > scan.max_fov ) continue;
			// The engine trace_bullet is the dominant per-point cost in the
			// multipoint scan. Resolve the ray against the precomputed world
			// hitboxes first and skip the trace when the resolved hitgroup
			// cannot reach the damage floor - run() discards those points
			// anyway, so the accepted hit set is unchanged.
			if ( g_shared.pen( ).max_damage_for_ray( eye, point.position, penetration ) < cand.min_damage ) continue;
			if ( counters ) ++counters->penetration_calls;
			shared::penetration::result damage{};
			if ( !g_shared.pen( ).run( eye, point.position, penetration, local.pawn, local.team, damage ) || damage.damage < cand.min_damage ) continue;
			if ( point.center && point.hitbox.index >= 0 && point.hitbox.index < 19 ) center_good[ static_cast<std::size_t>( point.hitbox.index ) ] = !damage.penetrated || damage.damage >= static_cast<float>( cand.health );
			if ( !point.center && point.hitbox.index == 0 && damage.hitgroup != systems::g_hitboxes.hitgroup_from_hitbox( 0 ) ) continue;
			out.push_back( scan_hit{ .position = point.position, .aim_angle = angle, .damage = damage.damage, .fov = fov,
				.hitbox_index = point.hitbox.index, .hitgroup = damage.hitgroup, .bone_index = point.bone, .hitbox = point.hitbox,
				.is_center = point.center, .penetrated = damage.penetrated, .pawn = cand.pawn, .health = cand.health, .record = prepared.record,
				.candidate_ordinal = cand.ordinal, .record_ordinal = prepared.ordinal, .source_eye_index = eye_index, .point_ordinal = point_index } );
		}
	}

	std::array<math::vector3, 4> rage::generate_multipoints( const systems::hitboxes::entry& hitbox, const math::vector3& center, const math::quaternion& rotation, float pointscale, const math::vector3& shoot_pos, float inaccuracy, float spread, bool dynamic_pointscale ) const
	{
		const auto scale = std::clamp( pointscale / 100.0f, 0.05f, 1.0f );
		const auto view = ( center - shoot_pos ).normalized( );
		math::vector3 right{}, up{};
		math::helpers::angle_vectors_left( math::helpers::vector_to_angle( view ), nullptr, &right, &up );
		const auto extent = ( hitbox.maxs - hitbox.mins ) * 0.5f;
		const auto local_support = [ & ]( const math::vector3& direction )
		{
			return math::vector3{ direction.x >= 0.0f ? hitbox.maxs.x : hitbox.mins.x,
				direction.y >= 0.0f ? hitbox.maxs.y : hitbox.mins.y, direction.z >= 0.0f ? hitbox.maxs.z : hitbox.mins.z };
		};
		auto inverse = rotation;
		inverse.x = -inverse.x;
		inverse.y = -inverse.y;
		inverse.z = -inverse.z;
		const auto midpoint = ( hitbox.mins + hitbox.maxs ) * 0.5f;
		const auto support = [ & ]( math::vector3 direction )
		{
			direction.normalize( );
			if ( hitbox.radius > 0.001f )
			{
				const auto a = rotation.rotate_vector( hitbox.mins - midpoint ) + center;
				const auto b = rotation.rotate_vector( hitbox.maxs - midpoint ) + center;
				return ( a.dot( direction ) > b.dot( direction ) ? a : b ) + direction * hitbox.radius;
			}
			const auto local_direction = inverse.rotate_vector( direction );
			return rotation.rotate_vector( local_support( local_direction ) - midpoint ) + center;
		};
		const auto dynamic = std::clamp( 1.0f - std::tanf( std::max( inaccuracy + spread, 0.0f ) ) * ( center - shoot_pos ).length( ) / std::max( hitbox.radius, extent.length( ) ), 0.15f, 1.0f );
		const auto factor = scale * ( dynamic_pointscale ? dynamic : 1.0f );
		return {
			center + ( support( right ) - center ) * factor,
			center + ( support( -right ) - center ) * factor,
			center + ( support( up ) - center ) * factor,
			center + ( support( -up ) - center ) * factor
		};
	}

	bool rage::hittable_after_brake( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
	{
		if ( !ctx.on_ground ) return false;
		// Abs velocity, not networked: the networked copy trails ~2 ticks,
		// so at peek start it reads "barely moving" and the trigger gate
		// below never opens until the player is fully exposed.
		const auto& velocity = systems::g_prediction.pre( ).velocity;
		if ( velocity.length_2d( ) <= ctx.accurate_threshold ) return false;

		auto eye = ctx.eyes.count > 0 ? ctx.eyes.entries[ 0 ].position : g_shared.get_shoot_position( );
		if ( eye.length_sqr( ) < 1.0f ) eye = g_shared.get_eye_position( local.pawn );
		if ( eye.length_sqr( ) < 1.0f ) return false;
		eye += brake_displacement( velocity );
		return this->any_damage_from_eye( eye, ctx, scan, candidates, local );
	}

	bool rage::hittable_after_landing( const aim_context& ctx, const scan_config& scan, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
	{
		if ( ctx.on_ground ) return false;
		const auto landing = g_misc.autostop( ).predict_landing( local.pawn );
		if ( !landing ) return false;

		// Eye height over the feet is kept from the current state; the
		// settled position is the landing spot plus the post-landing brake.
		auto eye = ctx.eyes.count > 0 ? ctx.eyes.entries[ 0 ].position : g_shared.get_eye_position( local.pawn );
		const auto eye_offset = eye - systems::g_prediction.pre( ).origin;
		auto settled = landing->origin + eye_offset;
		settled += brake_displacement( systems::g_prediction.pre( ).velocity );
		return this->any_damage_from_eye( settled, ctx, scan, candidates, local );
	}

	bool rage::any_damage_from_eye( const math::vector3& eye, const aim_context& ctx, const scan_config& scan, const std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		for ( const auto& candidate : candidates )
		{
			for ( auto r = 0; r < candidate.record_count; ++r )
			{
				auto* prepared = candidate.prepared[ r ];
				if ( !prepared || !prepared->record || !prepared->record->valid ) continue;
				const auto& penetration = prepared->penetration;
				if ( !penetration.target_pawn ) continue;
				const auto bone_limit = std::min( prepared->record->bone_count, 27 );
				if ( bone_limit <= 0 ) continue;
				for ( auto i = 0; i < scan.hitbox_count; ++i )
				{
					const auto id = scan.hitbox_order[ static_cast<std::size_t>( i ) ];
					const auto* hitbox = id >= 0 && id < static_cast<int>( prepared->hitboxes.size( ) ) ? prepared->hitboxes[ static_cast<std::size_t>( id ) ] : nullptr;
					if ( hitbox == nullptr || hitbox->bone < 0 || hitbox->bone >= bone_limit ) continue;
					const auto& bone = prepared->record->bones[ hitbox->bone ];
					if ( bone.position.length_sqr( ) < 1.0f ) continue;
					const auto center = bone.rotation.rotate_vector( ( hitbox->mins + hitbox->maxs ) * 0.5f ) + bone.position;
					const auto angle = math::helpers::calculate_angle( eye, center );
					if ( math::helpers::angle_distance( ctx.view_angles, angle ) > scan.max_fov ) continue;
					if ( g_shared.pen( ).max_damage_for_ray( eye, center, penetration ) < candidate.min_damage ) continue;
					shared::penetration::result damage{};
					if ( !g_shared.pen( ).run( eye, center, penetration, local.pawn, local.team, damage ) ) continue;
					if ( damage.damage >= candidate.min_damage ) return true;
				}
			}
		}
		return false;
	}
}
