#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	namespace detail {

		constexpr auto trace_mask{ static_cast< std::uintptr_t >( 0x1c300b ) };
		constexpr auto trace_layer{ std::uint8_t{ 3 } };
		constexpr auto trace_filter_type{ 15 };
		constexpr auto max_penetrations{ 4 };

		struct bullet_trace_record
		{
			float enter_fraction{};
			float exit_fraction{};
			float damage{};
			int contact_team{};
			std::uint16_t enter_contact_index{};
			std::uint16_t exit_contact_index{};
			std::uint8_t flags{};
			std::byte pad[ 3 ]{};

			[[nodiscard]] bool is_penetration( ) const { return ( this->flags & 1 ) != 0; }
			[[nodiscard]] bool stops_inside_surface( ) const { return !std::isfinite( this->exit_fraction ) || this->exit_fraction >= 1.0f; }
			[[nodiscard]] std::size_t exit_index( ) const { return this->exit_contact_index; }
		};

		static_assert( sizeof( bullet_trace_record ) == 0x18 );
		static_assert( offsetof( bullet_trace_record, damage ) == 0x8 );
		static_assert( offsetof( bullet_trace_record, exit_contact_index ) == 0x12 );
		static_assert( offsetof( bullet_trace_record, flags ) == 0x14 );
		constexpr auto max_trace_records{ sizeof( systems::tracing::trace_data::hit_elements ) / sizeof( bullet_trace_record ) };
		static_assert( max_trace_records == 8 );

		struct trace_contact
		{
			std::byte pad0[ 0x2c ]{};
			std::uint32_t entity_handle{};
			std::byte pad1[ 0x8 ]{};
		};

		static_assert( sizeof( trace_contact ) == 0x38 );

		enum class hitgroup : int
		{
			head = 1,
			chest = 2,
			stomach = 3,
			left_arm = 4,
			right_arm = 5,
			left_leg = 6,
			right_leg = 7,
			neck = 8
		};

		[[nodiscard]] bool finite_positive( float value )
		{
			return std::isfinite( value ) && value > 0.0f;
		}

		[[nodiscard]] bool intersect_oriented_box( const math::vector3& ray_start, const math::vector3& ray_delta, const math::quaternion& rotation, const math::vector3& position, const math::vector3& mins, const math::vector3& maxs, float& fraction )
		{
			auto inverse = rotation;
			inverse.x = -inverse.x;
			inverse.y = -inverse.y;
			inverse.z = -inverse.z;

			const auto local_start = inverse.rotate_vector( ray_start - position );
			const auto local_delta = inverse.rotate_vector( ray_delta );
			auto enter{ 0.0f };
			auto leave{ 1.0f };

			const auto update_interval = [ & ]( float origin, float delta, float minimum, float maximum )
				{
					if ( std::fabsf( delta ) < 1.0e-8f )
					{
						return origin >= minimum && origin <= maximum;
					}

					auto first = ( minimum - origin ) / delta;
					auto second = ( maximum - origin ) / delta;
					if ( first > second )
					{
						std::swap( first, second );
					}

					enter = std::max( enter, first );
					leave = std::min( leave, second );
					return enter <= leave;
				};

			if ( !update_interval( local_start.x, local_delta.x, mins.x, maxs.x ) ||
				!update_interval( local_start.y, local_delta.y, mins.y, maxs.y ) ||
				!update_interval( local_start.z, local_delta.z, mins.z, maxs.z ) )
			{
				return false;
			}

			fraction = enter;
			return std::isfinite( fraction ) && fraction >= 0.0f && fraction <= 1.0f;
		}

	} // namespace detail

	struct shared::penetration::trace_snapshot
	{
		const detail::bullet_trace_record* records{};
		const detail::trace_contact* contacts{};
		int record_count{};
		int contact_count{};
		float trace_range{};
	};

	void shared::penetration::prepare( std::uintptr_t weapon_vdata, std::uintptr_t weapon )
	{
		this->m_weapon_data = {};

		if ( !weapon_vdata || !weapon )
		{
			return;
		}

		this->m_weapon_data = weapon_data
		{
			.damage = static_cast< float >( memory::read<int>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_nDamage"_hash ) ) ),
			.penetration = memory::read<float>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flPenetration"_hash ) ),
			.range_modifier = memory::read<float>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flRangeModifier"_hash ) ),
			.range = memory::read<float>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flRange"_hash ) ),
			.armor_ratio = memory::read<float>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flArmorRatio"_hash ) ),
			// 服务器 TraceAttack 硬编码 ×4.0（IDA 取证 sub_180A2B300），不读 vdata
			// 的 m_flHeadshotMultiplier——vdata 值与服务器行为不一致时以服务器为准。
			.headshot_multiplier = 4.0f
		};

		// Damage scales change at most on server configuration reloads;
		// reading them here (main thread, once per tick) keeps the worker
		// threads inside prepare_target free of convar lookups.
		this->m_damage_scales = damage_scales
		{
			.ct_head = CONVAR ("mp_damage_scale_ct_head")->get<float>( ),
			.t_head = CONVAR ("mp_damage_scale_t_head")->get<float>( ),
			.ct_body = CONVAR ("mp_damage_scale_ct_body")->get<float>( ),
			.t_body = CONVAR ("mp_damage_scale_t_body")->get<float>( )
		};
	}

	shared::penetration::run_context shared::penetration::prepare_target( std::uintptr_t target_pawn, lagcomp::record* record ) const
	{
		run_context ctx{};

		if ( !target_pawn )
		{
			return ctx;
		}

		ctx.target_pawn = target_pawn;
		ctx.record = record;
		if ( record && record->game_scene_node )
		{
			ctx.hitboxes = systems::g_hitboxes.query( record->game_scene_node );
		}

		ctx.target_armor = memory::read<int>( target_pawn + SCHEMA( "C_CSPlayerPawn", "m_ArmorValue"_hash ) );
		ctx.target_team = memory::read<int>( target_pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );

		if ( ctx.target_armor > 0 )
		{
			const auto services = memory::read<std::uintptr_t>( target_pawn + SCHEMA( "C_BasePlayerPawn", "m_pItemServices"_hash ) );
			if ( services )
			{
				ctx.has_helmet = memory::read<bool>( services + SCHEMA( "CCSPlayer_ItemServices", "m_bHasHelmet"_hash ) );
			}
		}

		ctx.scales = this->m_damage_scales;
		ctx.armor_ratio = this->m_weapon_data.armor_ratio;
		ctx.headshot_multiplier = this->m_weapon_data.headshot_multiplier;

		// Precompute the world-space hitbox geometry once per record: the
		// scan fires up to ~95 rays per record and each resolve previously
		// re-derived every capsule endpoint / oriented-box frame.
		if ( record )
		{
			for ( const auto& hitbox : ctx.hitboxes )
			{
				if ( hitbox.bone < 0 || hitbox.bone >= record->bone_count || ctx.world_count >= static_cast< int >( ctx.world.size( ) ) )
				{
					continue;
				}

				const auto& bone = record->bones[ hitbox.bone ];
				auto& world = ctx.world[ ctx.world_count++ ];
				world.index = hitbox.index;
				world.radius = hitbox.radius;
				world.capsule = hitbox.radius > 0.001f;
				if ( world.capsule )
				{
					world.capsule_start = bone.rotation.rotate_vector( hitbox.mins ) + bone.position;
					world.capsule_end = bone.rotation.rotate_vector( hitbox.maxs ) + bone.position;
				}
				else
				{
					world.rotation = bone.rotation;
					world.position = bone.position;
					world.mins = hitbox.mins;
					world.maxs = hitbox.maxs;
				}
			}
		}

		return ctx;
	}

	bool shared::penetration::has_valid_weapon_data( ) const
	{
		return detail::finite_positive( this->m_weapon_data.damage ) &&
			detail::finite_positive( this->m_weapon_data.penetration ) &&
			detail::finite_positive( this->m_weapon_data.range_modifier ) &&
			detail::finite_positive( this->m_weapon_data.range ) &&
			detail::finite_positive( this->m_weapon_data.armor_ratio ) &&
			detail::finite_positive( this->m_weapon_data.headshot_multiplier );
	}

	bool shared::penetration::simulate_trace( const math::vector3& start, const math::vector3& direction, std::uintptr_t local_pawn, int local_team, lagcomp::record* record, trace_snapshot& out, float max_distance ) const
	{
		out = {};

		if ( !local_pawn || !this->has_valid_weapon_data( ) || !detail::finite_positive( direction.length( ) ) )
		{
			return false;
		}

		// 0 = full weapon range for the penetration crosshair. Target scans pass
		// target_distance + 32 units so the native trace includes the complete
		// historical hitbox but never consumes geometry behind the target.
		const auto range = max_distance > 0.0f ? std::min( max_distance, this->m_weapon_data.range ) : this->m_weapon_data.range;

		const auto filter = systems::g_tracing.make_filter( local_pawn, detail::trace_mask, detail::trace_layer, detail::trace_filter_type );
		thread_local systems::tracing::trace_data trace_storage{};
		trace_storage = {};
		trace_storage.array_pointer = trace_storage.elements.data( );
		trace_storage.hit_array_pointer = trace_storage.hit_elements;

		if ( record )
		{
			shared::m_current_autowall_record = record;
			shared::m_autowalling = true;
		}

		systems::g_tracing.setup_trace( &trace_storage, start, direction.normalized( ) * range, filter, detail::max_penetrations, true );

		if ( record )
		{
			shared::m_autowalling = false;
			shared::m_current_autowall_record = nullptr;
		}

		if ( trace_storage.num_hits <= 0 || trace_storage.num_hits > detail::max_trace_records || !trace_storage.array_pointer || !trace_storage.hit_array_pointer )
		{
			return false;
		}

		memory::call<void>( PATTERN( patterns::trace_bullet ), &trace_storage, this->m_weapon_data.damage, this->m_weapon_data.penetration, this->m_weapon_data.range_modifier, detail::max_penetrations, local_team, static_cast< std::uintptr_t >( 0 ) );

		out.records = reinterpret_cast< const detail::bullet_trace_record* >( trace_storage.hit_array_pointer );
		out.contacts = reinterpret_cast< const detail::trace_contact* >( trace_storage.array_pointer );
		out.record_count = trace_storage.num_hits;
		out.contact_count = static_cast< int >( trace_storage.elements.size( ) );
		out.trace_range = range;

		return true;
	}

	bool shared::penetration::record_hits_target( const trace_snapshot& trace, int record_index, std::uintptr_t target_pawn ) const
	{
		if ( !target_pawn || !trace.records || !trace.contacts || record_index < 0 || record_index >= trace.record_count )
		{
			return false;
		}

		const auto& record = trace.records[ record_index ];
		// Native air records identify the entity at the end of the segment (+0x12),
		// not the previous contact (+0x10). Stop at its arrival damage.
		const auto contact_index = record.exit_index( );
		if ( contact_index >= static_cast< std::size_t >( trace.contact_count ) )
		{
			return false;
		}

		const auto hit_entity = systems::g_entities.lookup( trace.contacts[ contact_index ].entity_handle );
		return hit_entity != 0 && hit_entity == target_pawn;
	}

	int shared::penetration::resolve_hitbox( const math::vector3& start, const math::vector3& ray_delta, const run_context& ctx ) const
	{
		if ( !ctx.record || ctx.world_count <= 0 )
		{
			return -1;
		}

		auto closest_hitbox{ -1 };
		auto closest_fraction{ 1.0f };

		// World-space geometry (capsule endpoints / oriented-box frames)
		// was precomputed in prepare_target - this loop runs once per scan
		// ray and must stay free of quaternion rotations.
		for ( auto i = 0; i < ctx.world_count; ++i )
		{
			const auto& world = ctx.world[ i ];
			auto fraction{ 1.0f };
			const auto intersects = world.capsule
				? g_shared.ray_vs_capsule( start, ray_delta, world.capsule_start, world.capsule_end, world.radius, fraction )
				: detail::intersect_oriented_box( start, ray_delta, world.rotation, world.position, world.mins, world.maxs, fraction );

			if ( intersects && std::isfinite( fraction ) && fraction < closest_fraction )
			{
				closest_fraction = fraction;
				closest_hitbox = world.index;
			}
		}

		return closest_hitbox;
	}

	bool shared::penetration::run( const math::vector3& start, const math::vector3& end, const run_context& ctx, std::uintptr_t local_pawn, int local_team, result& out ) const
	{
		out = {};

		if ( !ctx.target_pawn || !local_pawn || !this->has_valid_weapon_data( ) )
		{
			return false;
		}

		const auto target_delta = end - start;
		const auto target_distance = target_delta.length( );
		if ( !detail::finite_positive( target_distance ) )
		{
			return false;
		}

		const auto direction = target_delta / target_distance;
		trace_snapshot trace{};
		if ( !this->simulate_trace( start, direction, local_pawn, local_team, ctx.record, trace, target_distance + 32.0f ) )
		{
			return false;
		}

		const auto hitbox = this->resolve_hitbox( start, direction * this->m_weapon_data.range, ctx );
		if ( hitbox < 0 )
		{
			return false;
		}

		bool penetrated{};
		auto hit_target{ false };
		auto bullet_dead{ false };
		auto last_damage{ 0.0f };
		auto last_exit_fraction{ 0.0f };
		for ( auto index = 0; index < trace.record_count; ++index )
		{
			const auto& record = trace.records[ index ];
			if ( !detail::finite_positive( record.damage ) )
			{
				// 子弹伤害耗尽：与停在墙内等价，必须拒绝。
				bullet_dead = true;
				break;
			}

			last_damage = record.damage;

			if ( record.is_penetration( ) )
			{
				penetrated = true;
				last_exit_fraction = record.exit_fraction;
				if ( record.stops_inside_surface( ) )
				{
					// 子弹停在墙内：穿不过，保持拒绝。
					bullet_dead = true;
					break;
				}
				continue;
			}

			if ( !this->record_hits_target( trace, index, ctx.target_pawn ) )
			{
				continue;
			}

			hit_target = true;
			break;
		}

		if ( bullet_dead || ( !hit_target && !penetrated ) )
		{
			// 无穿透且实时 trace 未命中目标：非本次修复的路径，保持拒绝。
			return false;
		}

		// 穿墙精度修复（该穿不穿）：引擎实时 trace 的目标命中记录依赖目标实时姿势，
		// 而扫描/结算用的是回溯记录姿势。目标在记录后移动（peek/跳跃）时实时 trace
		// 可能碰不到回溯姿势的目标，之前这里直接放弃整条射线。子弹实际已穿透所有
		// 表面且未停在墙内、按回溯姿势解析射线可达 hitbox，服务器回退后会按回溯姿势
		// 结算命中，因此按最后一条穿透记录的剩余伤害接受该点，并补上记录点到目标点
		// 的距离衰减（与引擎 DamageAtDistance 一致）。
		auto damage = last_damage;
		if ( !hit_target && last_exit_fraction > 0.0f && last_exit_fraction < 1.0f )
		{
			const auto exit_distance = last_exit_fraction * trace.trace_range;
			const auto remaining = target_distance - exit_distance;
			if ( remaining > 0.0f )
			{
				damage *= std::powf( this->m_weapon_data.range_modifier, remaining / 500.0f );
			}
		}

		result candidate
		{
			.damage = damage,
			.hitbox = hitbox,
			.hitgroup = systems::g_hitboxes.hitgroup_from_hitbox( hitbox ),
			.penetrated = penetrated
		};

		this->apply_player_damage( candidate.hitgroup, ctx.target_armor, ctx.has_helmet, ctx.target_team, ctx.armor_ratio, ctx.headshot_multiplier, ctx.scales, candidate.damage );
		if ( candidate.damage <= 0.0f )
		{
			return false;
		}

		out = candidate;
		return true;
	}

	float shared::penetration::max_damage_for_ray( const math::vector3& start, const math::vector3& end, const run_context& ctx ) const
	{
		if ( !ctx.target_pawn || !this->has_valid_weapon_data( ) )
		{
			return 0.0f;
		}

		const auto ray = end - start;
		const auto distance = ray.length( );
		if ( !detail::finite_positive( distance ) )
		{
			return 0.0f;
		}

		// Same resolve the trace path uses (see run): the resolved hitbox
		// decides the hitgroup, so the damage ceiling is that hitbox's
		// un-penetrated maximum. The full-range ray mirrors simulate_trace.
		const auto direction = ray / distance;
		const auto hitbox = this->resolve_hitbox( start, direction * this->m_weapon_data.range, ctx );
		if ( hitbox < 0 )
		{
			return 0.0f;
		}

		return this->get_max_damage( systems::g_hitboxes.hitgroup_from_hitbox( hitbox ), ctx.target_armor, ctx.has_helmet, ctx.target_team );
	}

	bool shared::penetration::can( const math::vector3& start, const math::vector3& direction, float& out_damage, const systems::local::snapshot& local ) const
	{
		out_damage = 0.0f;

		if ( !local.is_alive || !local.pawn || !this->has_valid_weapon_data( ) || !detail::finite_positive( direction.length( ) ) )
		{
			return false;
		}

		const auto local_team = memory::read<int>( local.pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
		trace_snapshot trace{};
		if ( !this->simulate_trace( start, direction.normalized( ), local.pawn, local_team, nullptr, trace ) )
		{
			return false;
		}

		for ( auto index = 0; index < trace.record_count; ++index )
		{
			const auto& record = trace.records[ index ];
			if ( !detail::finite_positive( record.damage ) )
			{
				break;
			}

			if ( !record.is_penetration( ) )
			{
				continue;
			}

			if ( record.stops_inside_surface( ) )
			{
				break;
			}

			out_damage = record.damage;
			return true;
		}

		return false;
	}

	float shared::penetration::get_max_damage( int hitgroup, int target_armor, bool has_helmet, int target_team ) const
	{
		if ( !this->has_valid_weapon_data( ) )
		{
			return 0.0f;
		}

		auto damage = this->m_weapon_data.damage;
		this->apply_player_damage( hitgroup, target_armor, has_helmet, target_team, this->m_weapon_data.armor_ratio, this->m_weapon_data.headshot_multiplier, this->m_damage_scales, damage );
		return damage;
	}

	void shared::penetration::apply_player_damage( int hitgroup, int armor, bool has_helmet, int team, float armor_ratio, float headshot_multiplier, const damage_scales& scales, float& damage ) const
	{
		if ( !detail::finite_positive( damage ) || !detail::finite_positive( armor_ratio ) || !detail::finite_positive( headshot_multiplier ) )
		{
			damage = 0.0f;
			return;
		}

		const auto is_ct = ( team == 3 );
		const auto head_scale = is_ct ? scales.ct_head : scales.t_head;
		const auto body_scale = is_ct ? scales.ct_body : scales.t_body;
		if ( !detail::finite_positive( head_scale ) || !detail::finite_positive( body_scale ) )
		{
			damage = 0.0f;
			return;
		}

		switch ( static_cast< detail::hitgroup >( hitgroup ) )
		{
		case detail::hitgroup::head:
			damage *= headshot_multiplier * head_scale;
			break;
		case detail::hitgroup::chest:
		case detail::hitgroup::left_arm:
		case detail::hitgroup::right_arm:
		case detail::hitgroup::neck:
			damage *= body_scale;
			break;
		case detail::hitgroup::stomach:
			damage *= 1.25f * body_scale;
			break;
		case detail::hitgroup::left_leg:
		case detail::hitgroup::right_leg:
			damage *= 0.75f * body_scale;
			break;
		default:
			break;
		}

		if ( !detail::finite_positive( damage ) )
		{
			damage = 0.0f;
			return;
		}

		const auto is_head = static_cast< detail::hitgroup >( hitgroup ) == detail::hitgroup::head;
		const auto armor_protects_hitgroup = ( hitgroup >= static_cast< int >( detail::hitgroup::head ) && hitgroup <= static_cast< int >( detail::hitgroup::right_arm ) ) || hitgroup == static_cast< int >( detail::hitgroup::neck );

		if ( armor <= 0 || !armor_protects_hitgroup || ( is_head && !has_helmet ) )
		{
			damage = std::floor( damage );
			return;
		}

		constexpr auto armor_damage_scale{ 0.5f };
		const auto health_damage = damage * ( armor_ratio * armor_damage_scale );
		const auto armor_damage = ( damage - health_damage ) * armor_damage_scale;
		if ( armor_damage > static_cast< float >( armor ) )
		{
			damage = damage - ( static_cast< float >( armor ) / armor_damage_scale );
		}
		else
		{
			damage = health_damage;
		}

		damage = std::floor( std::max( damage, 0.0f ) );
	}


} // namespace features::combat
