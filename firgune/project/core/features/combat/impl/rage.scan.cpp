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

	std::vector<rage::candidate> rage::gather_candidates( const systems::local::snapshot& local, float max_distance_sq ) const
	{
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );
		const auto origin = systems::g_prediction.pre( ).origin;
		auto* self = const_cast< rage* >( this );
		self->m_scan_records.clear( );
		self->m_scan_records.reserve( players.size( ) * k_max_scan_records + 1 );

		std::vector<candidate> result{};
		result.reserve( players.size( ) );
		const auto weapon_type = g_shared.ctx( ).weapon_type;
		const auto is_gun = weapon_type >= cstypes::weapon_type::pistol && weapon_type <= cstypes::weapon_type::lmg;
		const auto& weapon_config = settings::g_combat.m_ragebot.get_group( weapon_type );
		const auto precision = is_gun && settings::g_combat.m_ragebot.precision.value && !weapon_config.no_spread.value;
		const auto precision_performance = precision && settings::g_combat.m_ragebot.precision_mode.value
			== settings::combat::ragebot::precision_mode::performance;
		const auto append_record = [ & ]( candidate& out, shared::lagcomp::record&& record )
		{
			if ( out.record_count >= k_max_scan_records || !record.valid ) return;
			self->m_scan_records.push_back( std::move( record ) );
			out.records[ out.record_count++ ] = &self->m_scan_records.back( );
		};

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

			candidate out{};
			out.pawn = pawn;
			out.health = health;
			out.armor = memory::read<int>( pawn + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) );
			if ( g_shared.ctx( ).weapon_type >= cstypes::weapon_type::pistol && g_shared.ctx( ).weapon_type <= cstypes::weapon_type::lmg )
			{
				const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
				out.min_damage = this->get_min_damage( config, health, config.min_damage_override.value );
			}
			out.ordinal = result.size( );
			for ( auto i = 0; i < selected_count; ++i ) append_record( out, std::move( selected[ i ] ) );

			if ( out.record_count > 0 ) result.push_back( out );
		}

		// Double tap kill efficiency: with a single visible target the pair
		// kills it together - each bullet only needs half its HP, which
		// opens up body shots instead of waiting for a headshot-only lethal
		// point. Multiple targets keep the per-bullet lethal floor.
		if ( is_gun && this->m_double_tap.can_pair( ) && result.size( ) == 1 )
		{
			const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
			if ( !config.min_damage_override.value && config.min_damage >= 101 )
			{
				auto& cand = result.front( );
				cand.min_damage = std::max( 1.0f, std::ceil( static_cast< float >( cand.health ) * 0.5f ) );
			}
		}

		return result;
	}

	std::vector<rage::scan_hit> rage::scan_players( const math::vector3& eye, float inaccuracy, const aim_context& ctx, std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		std::vector<std::vector<scan_hit>> slots( candidates.size( ) );
		threadpool::parallel_for( 0, static_cast<int>( candidates.size( ) ), [ & ]( int begin, int end )
		{
			for ( auto i = begin; i < end; ++i )
			{
				auto& slot = slots[ static_cast<std::size_t>( i ) ];
				for ( auto r = 0; r < candidates[ i ].record_count; ++r )
				{
					if ( auto* record = candidates[ i ].records[ r ]; record && record->valid )
					{
						auto hits = scan_player( eye, inaccuracy, ctx, candidates[ i ], record, static_cast<std::uint8_t>( r ), local );
						for ( auto& hit : hits ) slot.push_back( std::move( hit ) );
					}
				}
			}
		}, 1 );

		std::size_t count = 0;
		for ( const auto& slot : slots ) count += slot.size( );
		std::vector<scan_hit> result{};
		result.reserve( count );
		for ( auto& slot : slots ) for ( auto& hit : slot ) result.push_back( std::move( hit ) );
		return result;
	}

	std::vector<rage::scan_hit> rage::scan_player( const math::vector3& eye, float inaccuracy, const aim_context& ctx, const candidate& cand, shared::lagcomp::record* record, std::uint8_t record_ordinal, const systems::local::snapshot& local ) const
	{
		if ( !cand.pawn || !record || !record->valid || cand.health <= 0 ) return {};
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto scene = memory::read<std::uintptr_t>( cand.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !scene ) return {};
		const auto hitbox_set = systems::g_hitboxes.query( scene );
		if ( hitbox_set.count <= 0 ) return {};
		const auto skeleton = g_shared.lc( ).get_skeleton( *record );
		const auto penetration = g_shared.pen( ).prepare_target( cand.pawn, record );

		std::array<int, 19> order{};
		const auto order_count = build_hitbox_order( config, order );

		// Index the hitbox set once: the order loop below used a linear
		// find_if per hitbox per record, which adds up with many players.
		std::array<const systems::hitboxes::entry*, 19> indexed{};
		for ( const auto& h : hitbox_set )
		{
			if ( h.index >= 0 && h.index < static_cast< int >( indexed.size( ) ) ) indexed[ h.index ] = &h;
		}

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
		const auto dynamic_pointscale = config.dynamic_pointscale.value;
		const auto spread = g_shared.ctx( ).spread;
		for ( auto i = 0; i < order_count; ++i )
		{
			const auto id = order[ i ];
			const auto hitbox = id >= 0 && id < static_cast< int >( indexed.size( ) ) ? indexed[ id ] : nullptr;
			if ( hitbox == nullptr || hitbox->bone < 0 || hitbox->bone >= static_cast<int>( skeleton.size( ) ) ) continue;
			const auto& bone = skeleton[ hitbox->bone ];
			if ( bone.position.length_sqr( ) < 1.0f ) continue;
			const auto center = bone.rotation.rotate_vector( ( hitbox->mins + hitbox->maxs ) * 0.5f ) + bone.position;
			append_point( { center, *hitbox, hitbox->bone, true } );
			if ( config.debug_multipoints.value ) { std::lock_guard lock( m_debug_mtx ); m_debug_points.push_back( { center, id, true } ); }
			const auto group = group_for( id );
			if ( group >= 0 && config.pointscale > 0.0f && config.multipoints.values[ static_cast<std::size_t>( group ) ] )
			{
				// The unoccluded damage ceiling cannot prove that the center
				// penetrates a wall. Keep enabled edge points; center_good below
				// skips their traces only after an actual viable center trace.
				for ( const auto& position : generate_multipoints( *hitbox, center, bone.rotation, config.pointscale, eye, inaccuracy, spread, dynamic_pointscale ) )
				{
					append_point( { position, *hitbox, hitbox->bone, false } );
					if ( config.debug_multipoints.value ) { std::lock_guard lock( m_debug_mtx ); m_debug_points.push_back( { position, id, false } ); }
				}
			}
		}

		std::array<bool, 19> center_good{};
		std::vector<scan_hit> result{};
		result.reserve( point_count );
		for ( auto point_index = std::size_t{}; point_index < point_count; ++point_index )
		{
			const auto& point = points[ point_index ];
			if ( !point.center && point.hitbox.index >= 0 && point.hitbox.index < 19 && center_good[ point.hitbox.index ] ) continue;
			const auto angle = math::helpers::calculate_angle( eye, point.position );
			const auto fov = math::helpers::angle_distance( ctx.view_angles, angle );
			if ( fov > config.max_fov ) continue;
			// The engine trace_bullet is the dominant per-point cost in the
			// multipoint scan. Resolve the ray against the precomputed world
			// hitboxes first and skip the trace when the resolved hitgroup
			// cannot reach the damage floor - run() discards those points
			// anyway, so the accepted hit set is unchanged.
			if ( g_shared.pen( ).max_damage_for_ray( eye, point.position, penetration ) < cand.min_damage ) continue;
			shared::penetration::result damage{};
			if ( !g_shared.pen( ).run( eye, point.position, penetration, local.pawn, local.team, damage ) || damage.damage < cand.min_damage ) continue;
			if ( point.center && point.hitbox.index >= 0 && point.hitbox.index < 19 ) center_good[ point.hitbox.index ] = !damage.penetrated || damage.damage >= static_cast<float>( cand.health );
			if ( !point.center && point.hitbox.index == 0 && damage.hitgroup != systems::g_hitboxes.hitgroup_from_hitbox( 0 ) ) continue;
			result.push_back( scan_hit{ .position = point.position, .aim_angle = angle, .damage = damage.damage, .fov = fov,
				.hitbox_index = point.hitbox.index, .hitgroup = damage.hitgroup, .bone_index = point.bone, .hitbox = point.hitbox,
				.is_center = point.center, .penetrated = damage.penetrated, .pawn = cand.pawn, .health = cand.health, .record = record,
				.candidate_ordinal = cand.ordinal, .record_ordinal = record_ordinal } );
		}
		return result;
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

	bool rage::hittable_after_brake( const aim_context& ctx, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
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
		return this->any_damage_from_eye( eye, ctx, candidates, local );
	}

	bool rage::hittable_after_landing( const aim_context& ctx, const systems::local::snapshot& local, const std::vector<candidate>& candidates ) const
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
		return this->any_damage_from_eye( settled, ctx, candidates, local );
	}

	bool rage::any_damage_from_eye( const math::vector3& eye, const aim_context& ctx, const std::vector<candidate>& candidates, const systems::local::snapshot& local ) const
	{
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		std::array<int, 19> order{};
		const auto order_count = build_hitbox_order( config, order );
		for ( const auto& candidate : candidates )
		{
			const auto scene = memory::read<std::uintptr_t>( candidate.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( !scene ) continue;
			const auto hitbox_set = systems::g_hitboxes.query( scene );
			if ( hitbox_set.count <= 0 ) continue;
			std::array<const systems::hitboxes::entry*, 19> indexed{};
			for ( const auto& h : hitbox_set )
			{
				if ( h.index >= 0 && h.index < static_cast< int >( indexed.size( ) ) ) indexed[ h.index ] = &h;
			}
			for ( auto r = 0; r < candidate.record_count; ++r )
			{
				auto* record = candidate.records[ r ];
				if ( !record || !record->valid ) continue;
				const auto skeleton = g_shared.lc( ).get_skeleton( *record );
				const auto penetration = g_shared.pen( ).prepare_target( candidate.pawn, record );
				for ( auto i = 0; i < order_count; ++i )
				{
					const auto hitbox = order[ i ] >= 0 && order[ i ] < static_cast< int >( indexed.size( ) ) ? indexed[ order[ i ] ] : nullptr;
					if ( hitbox == nullptr || hitbox->bone < 0 || hitbox->bone >= static_cast< int >( skeleton.size( ) ) ) continue;
					const auto& bone = skeleton[ hitbox->bone ];
					if ( bone.position.length_sqr( ) < 1.0f ) continue;
					const auto center = bone.rotation.rotate_vector( ( hitbox->mins + hitbox->maxs ) * 0.5f ) + bone.position;
					const auto angle = math::helpers::calculate_angle( eye, center );
					if ( math::helpers::angle_distance( ctx.view_angles, angle ) > config.max_fov ) continue;
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
