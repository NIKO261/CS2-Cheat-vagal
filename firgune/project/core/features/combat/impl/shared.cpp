#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {
	namespace {
		constexpr int k_max_spread_pellets{ 16 };
		constexpr float k_seed_angle_step{ 0.5f };

		[[nodiscard]] math::vector3 calculate_bullet_direction( const math::vector3& transmitted, const math::vector3& punch, const math::vector2& spread )
		{
			math::vector3 shot_angles{ transmitted.x + punch.x, transmitted.y + punch.y, transmitted.z };
			math::vector3 forward{}, left{}, up{};
			math::helpers::angle_vectors_left( shot_angles, &forward, &left, &up );
			return ( forward + left * spread.x + up * spread.y ).normalized( );
		}
	}

	void shared::update( )
	{
		this->m_ctx = {};

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto global_vars = memory::read<std::uintptr_t>( addresses::globals::global_vars );
		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );

		if ( !global_vars || !movement_services )
		{
			return;
		}

		this->m_ctx.current_tick = memory::read<int>( global_vars + 0x44 );
		this->m_ctx.current_time = memory::read<float>( global_vars + 0x30 );
		this->m_ctx.is_scoped = memory::read<bool>( local.pawn + SCHEMA( "C_CSPlayerPawn", "m_bIsScoped"_hash ) );
		this->m_ctx.ticks_since_land = this->m_ctx.current_tick - memory::read<int>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_ModernJump"_hash ) + SCHEMA( "CCSPlayerModernJump", "m_nLastLandedTick"_hash ) );
		this->m_ctx.weapon_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );

		if ( !this->m_ctx.weapon_services )
		{
			return;
		}

		const auto weapon_handle = memory::read<std::uint32_t>( this->m_ctx.weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hActiveWeapon"_hash ) );
		if ( !weapon_handle )
		{
			return;
		}

		this->m_ctx.weapon = systems::g_entities.lookup( weapon_handle );
		if ( !this->m_ctx.weapon )
		{
			return;
		}

		this->m_ctx.weapon_vdata = memory::read<std::uintptr_t>( this->m_ctx.weapon + SCHEMA( "C_BaseEntity", "m_nSubclassID"_hash ) + 0x8 );
		if ( !this->m_ctx.weapon_vdata )
		{
			return;
		}

		this->m_ctx.range = memory::read<float>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flRange"_hash ) );
		this->m_ctx.weapon_type = memory::read<std::uint32_t>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_WeaponType"_hash ) );
		this->m_ctx.item_def_idx = memory::read<std::uint16_t>( this->m_ctx.weapon + SCHEMA( "C_EconEntity", "m_AttributeManager"_hash ) + SCHEMA( "C_AttributeContainer", "m_Item"_hash ) + SCHEMA( "C_EconItemView", "m_iItemDefinitionIndex"_hash ) );
		this->m_ctx.num_bullets = memory::read<int>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_nNumBullets"_hash ) );
		this->m_ctx.weapon_mode = memory::read<int>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_weaponMode"_hash ) );
		this->m_ctx.clip = memory::read<int>( this->m_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_iClip1"_hash ) );
		this->m_ctx.next_primary_attack_tick = memory::read<int>( this->m_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextPrimaryAttackTick"_hash ) );
		this->m_ctx.next_secondary_attack_tick = memory::read<int>( this->m_ctx.weapon + SCHEMA( "C_BasePlayerWeapon", "m_nNextSecondaryAttackTick"_hash ) );
		this->m_ctx.recoil_index = memory::read<float>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash ) );
		this->m_ctx.cycle_time = memory::read<float>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flCycleTime"_hash ) );
		this->m_ctx.weapon_max_speed = memory::read<float>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) );
		this->m_ctx.in_reload = memory::read<bool>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_bInReload"_hash ) );
		this->m_ctx.is_jump_scouting = ( systems::g_prediction.pre( ).flags & cstypes::entity_flags::on_ground ) == 0 && this->m_ctx.item_def_idx == cstypes::item_definition_index::weapon_ssg_08 && this->m_ctx.is_scoped;
		this->m_ctx.valid = true;

		this->m_pen.prepare( this->m_ctx.weapon_vdata, this->m_ctx.weapon );
	}

	void shared::invalidate_if_needed( )
	{
		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn || !local.controller )
		{
			this->m_ctx = {};
			this->m_last_shoot_tick = 0;
		}
	}

	std::uint32_t shared::get_spread_seed( const math::vector3& angles, int tick ) const
	{
		return memory::call<std::uint32_t>(PATTERN (patterns::get_tick_view_angles), nullptr, &angles, tick );
	}

	shared::shot_spread_state shared::capture_shot_spread_state( ) const
	{
		if ( !this->m_ctx.weapon || this->m_ctx.item_def_idx == 0 || this->m_ctx.num_bullets <= 0 )
		{
			return {};
		}

		const auto inaccuracy = this->get_inaccuracy( false );
		const auto spread = this->get_spread( );
		const auto recoil_index = memory::read<float>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash ) );
		const auto weapon_mode = memory::read<int>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_weaponMode"_hash ) );
		if ( !std::isfinite( inaccuracy ) || !std::isfinite( spread ) || !std::isfinite( recoil_index ) || inaccuracy < 0.0f || spread < 0.0f )
		{
			return {};
		}

		return { inaccuracy, spread, recoil_index, this->m_ctx.item_def_idx, this->m_ctx.num_bullets, weapon_mode };
	}

	math::vector2 shared::calculate_spread( std::uint32_t seed, float accuracy, float spread, float recoil_index, int item_def_idx, int num_bullets, int weapon_mode ) const
	{
		const auto pellet_count = std::clamp( num_bullets, 1, k_max_spread_pellets );
		std::array<float, k_max_spread_pellets> horizontal{};
		std::array<float, k_max_spread_pellets> vertical{};

		memory::call<void>(PATTERN (patterns::weapon_calculate_spread), static_cast< std::int16_t >( item_def_idx ), pellet_count, weapon_mode, seed + 1u, accuracy, spread, recoil_index, horizontal.data( ), vertical.data( ) );

		if ( !std::isfinite( horizontal[ 0 ] ) || !std::isfinite( vertical[ 0 ] ) )
		{
			return {};
		}

		return { horizontal[ 0 ], vertical[ 0 ] };
	}

	math::vector3 shared::get_aim_punch( std::uintptr_t local_pawn ) const
	{
		math::vector3 out{};
		if ( !local_pawn ) return out;
		const auto services = memory::read<std::uintptr_t>( local_pawn + SCHEMA( "C_CSPlayerPawn", "m_pAimPunchServices"_hash ) );
		if ( !services ) return out;
		memory::call<void>(PATTERN (patterns::get_aim_punch), services, &out, 0u );

		return out;
	}

	float shared::calculate_hitchance( const math::vector3& shoot_position, const math::vector3& aim_angle, const systems::hitboxes::entry& hitbox, const systems::bones::data& bone, float inaccuracy, float spread, int samples ) const
	{
		if ( samples <= 0 )
		{
			return 0.0f;
		}

		const auto cone = std::max( inaccuracy + spread, 0.0f );
		if ( cone <= 0.000001f )
		{
			return 1.0f;
		}

		const auto capsule_start = bone.rotation.rotate_vector( hitbox.mins ) + bone.position;
		const auto capsule_end = bone.rotation.rotate_vector( hitbox.maxs ) + bone.position;
		const auto is_capsule = hitbox.radius > 0.001f;
		auto inverse_rotation = bone.rotation;
		inverse_rotation.x = -inverse_rotation.x;
		inverse_rotation.y = -inverse_rotation.y;
		inverse_rotation.z = -inverse_rotation.z;

		math::vector3 forward{}, left{}, up{};
		math::helpers::angle_vectors_left( aim_angle, &forward, &left, &up );

		struct spread_cache
		{
			float inaccuracy{};
			float spread{};
			float recoil_index{};
			int item_def_idx{};
			int num_bullets{};
			int weapon_mode{};
			int count{};
			bool engine_valid{};
			bool initialized{};
			std::array<math::vector2, 512> values{};
		};

		thread_local spread_cache cache{};
		const auto cache_changed = !cache.initialized || cache.inaccuracy != inaccuracy || cache.spread != spread ||
			cache.recoil_index != this->m_ctx.recoil_index || cache.item_def_idx != this->m_ctx.item_def_idx ||
			cache.num_bullets != this->m_ctx.num_bullets || cache.weapon_mode != this->m_ctx.weapon_mode;
		if ( cache_changed )
		{
			cache.inaccuracy = inaccuracy;
			cache.spread = spread;
			cache.recoil_index = this->m_ctx.recoil_index;
			cache.item_def_idx = this->m_ctx.item_def_idx;
			cache.num_bullets = this->m_ctx.num_bullets;
			cache.weapon_mode = this->m_ctx.weapon_mode;
			cache.count = 0;
			cache.engine_valid = true;
			cache.initialized = true;

			auto full_energy{ 0.0f };
			auto reduced_energy{ 0.0f };
			for ( auto i = 0; i < 8; ++i )
			{
				const auto full = this->calculate_spread( i, inaccuracy, spread, this->m_ctx.recoil_index, this->m_ctx.item_def_idx, this->m_ctx.num_bullets, this->m_ctx.weapon_mode );
				const auto reduced = this->calculate_spread( i, inaccuracy * 0.25f, spread * 0.25f, this->m_ctx.recoil_index, this->m_ctx.item_def_idx, this->m_ctx.num_bullets, this->m_ctx.weapon_mode );
				cache.values[ i ] = full;
				full_energy += std::sqrtf( full.x * full.x + full.y * full.y );
				reduced_energy += std::sqrtf( reduced.x * reduced.x + reduced.y * reduced.y );
			}

			cache.engine_valid = std::isfinite( full_energy ) && std::isfinite( reduced_energy ) &&
				full_energy > 0.000001f && full_energy > reduced_energy * 1.8f;
			cache.count = cache.engine_valid ? 8 : 0;
		}

		const auto radical_inverse = [ ]( std::uint32_t value, std::uint32_t base )
		{
			auto scale = 1.0f / static_cast< float >( base );
			auto result = 0.0f;
			while ( value != 0 )
			{
				result += static_cast< float >( value % base ) * scale;
				value /= base;
				scale /= static_cast< float >( base );
			}
			return result;
		};
		const auto fallback_spread = [ & ]( int index )
		{
			const auto sequence = static_cast< std::uint32_t >( index + 1 );
			const auto theta = radical_inverse( sequence, 2 ) * 2.0f * std::numbers::pi_v<float>;
			const auto radius = std::sqrtf( radical_inverse( sequence, 3 ) );
			const auto offset = std::tanf( cone ) * radius;
			return math::vector2{ std::cosf( theta ) * offset, std::sinf( theta ) * offset };
		};

		const auto cached_samples = std::min( samples, static_cast< int >( cache.values.size( ) ) );
		for ( auto i = cache.count; i < cached_samples; ++i )
		{
			cache.values[ i ] = cache.engine_valid
				? this->calculate_spread( i, inaccuracy, spread, this->m_ctx.recoil_index, this->m_ctx.item_def_idx, this->m_ctx.num_bullets, this->m_ctx.weapon_mode )
				: fallback_spread( i );
		}
		cache.count = std::max( cache.count, cached_samples );

		const auto capsule_mid = ( capsule_start + capsule_end ) * 0.5f;
		const auto target_reach = ( capsule_mid - shoot_position ).length( ) +
			( capsule_end - capsule_start ).length( ) * 0.5f + std::max( hitbox.radius, 0.0f ) + 32.0f;
		const auto max_ray_length = this->m_ctx.range >= 64.0f ? this->m_ctx.range : 8192.0f;
		const auto ray_length = std::clamp( target_reach, 64.0f, max_ray_length );
		const auto local_ray_start = inverse_rotation.rotate_vector( shoot_position - bone.position );
		auto hits{ 0 };

		for ( auto i = 0; i < samples; ++i )
		{
			const auto calculated_spread = i < cached_samples
				? cache.values[ i ]
				: ( cache.engine_valid
					? this->calculate_spread( i, inaccuracy, spread, this->m_ctx.recoil_index, this->m_ctx.item_def_idx, this->m_ctx.num_bullets, this->m_ctx.weapon_mode )
					: fallback_spread( i ) );
			const auto direction = forward + ( left * calculated_spread.x ) + ( up * calculated_spread.y );
			const auto ray_delta = direction.normalized( ) * ray_length;

			auto hit{ false };
			if ( is_capsule )
			{
				auto fraction{ 1.0f };
				hit = this->ray_vs_capsule( shoot_position, ray_delta, capsule_start, capsule_end, hitbox.radius, fraction );
			}
			else
			{
				const auto local_delta = inverse_rotation.rotate_vector( ray_delta );
				auto entry{ 0.0f };
				auto leave{ 1.0f };
				const auto intersect_axis = [ & ]( float origin, float delta, float minimum, float maximum )
				{
					if ( std::fabsf( delta ) <= 1.0e-8f )
					{
						return origin >= minimum && origin <= maximum;
					}
					auto first = ( minimum - origin ) / delta;
					auto second = ( maximum - origin ) / delta;
					if ( first > second ) std::swap( first, second );
					entry = std::max( entry, first );
					leave = std::min( leave, second );
					return entry <= leave;
				};
				hit = intersect_axis( local_ray_start.x, local_delta.x, hitbox.mins.x, hitbox.maxs.x ) &&
					intersect_axis( local_ray_start.y, local_delta.y, hitbox.mins.y, hitbox.maxs.y ) &&
					intersect_axis( local_ray_start.z, local_delta.z, hitbox.mins.z, hitbox.maxs.z );
			}

			if ( hit )
			{
				++hits;
			}
		}

		return static_cast< float >( hits ) / static_cast< float >( samples );
	}

	std::optional<math::vector3> shared::find_spread_correction( const math::vector3& aim_angle, int tick, const math::vector3& punch, const shot_spread_state& state ) const
	{
		if ( tick < 0 || state.item_def_idx == 0 || state.num_bullets <= 0 ||
			!std::isfinite( state.inaccuracy ) || !std::isfinite( state.spread ) || !std::isfinite( state.recoil_index ) )
		{
			return std::nullopt;
		}

		const auto base_pitch = aim_angle.x - punch.x;
		const auto base_yaw = aim_angle.y - punch.y;
		math::vector3 desired_direction{};
		math::helpers::angle_vectors_left( aim_angle, &desired_direction );

		// CalcSpread can contribute both horizontal and vertical components up to
		// inaccuracy + spread. Search every quantized pitch bucket that can
		// contain a valid compensated angle, then close the seed against the
		// final angle actually sent in the attack history.
		const auto component_limit = state.inaccuracy + state.spread;
		const auto max_offset = math::helpers::rad_to_deg( std::atan( std::sqrt( 2.0f ) * component_limit ) ) + k_seed_angle_step;
		const auto first_bucket = std::max( -178, static_cast<int>( std::floor( ( base_pitch - max_offset ) / k_seed_angle_step ) ) );
		const auto last_bucket = std::min( 178, static_cast<int>( std::ceil( ( base_pitch + max_offset ) / k_seed_angle_step ) ) );

		std::optional<math::vector3> best{};
		auto best_alignment = -1.0f;
		for ( auto bucket = first_bucket; bucket <= last_bucket; ++bucket )
		{
			const math::vector3 seed_angles{ static_cast<float>( bucket ) * k_seed_angle_step, base_yaw, 0.0f };
			const auto seed = this->get_spread_seed( seed_angles, tick );
			const auto spread = this->calculate_spread( seed, state.inaccuracy, state.spread, state.recoil_index,
				state.item_def_idx, state.num_bullets, state.weapon_mode );
			if ( !std::isfinite( spread.x ) || !std::isfinite( spread.y ) ) continue;

			const auto magnitude = math::helpers::rad_to_deg( std::atan( std::sqrt( spread.x * spread.x + spread.y * spread.y ) ) );
			const auto roll = -math::helpers::rad_to_deg( std::atan2( spread.x, spread.y ) );
			for ( const auto mirrored : { false, true } )
			{
				const auto pitch = base_pitch + ( mirrored ? -magnitude : magnitude );
				if ( pitch < -89.0f || pitch > 89.0f ) continue;

				const math::vector3 transmitted{ pitch, base_yaw, roll + ( mirrored ? 180.0f : 0.0f ) };
				if ( this->get_spread_seed( transmitted, tick ) != seed ) continue;

				const auto alignment = calculate_bullet_direction( transmitted, punch, spread ).dot( desired_direction );
				if ( alignment > best_alignment )
				{
					best_alignment = alignment;
					best = transmitted;
				}
			}
		}

		// Do not emit a partially compensated shot. At this tolerance the
		// remaining angular error is below one tenth of a degree.
		if ( best_alignment < 0.999999f ) return std::nullopt;
		return best;
	}

	math::vector3 shared::get_eye_position( std::uintptr_t local_pawn ) const
	{
		if ( !local_pawn ) return {};
		const auto game_scene_node = memory::read<std::uintptr_t>( local_pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node ) return {};
		const auto origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto view_offset = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_hash ) );
		return origin + view_offset;
	}

	math::vector3 shared::get_shoot_position( ) const
	{
		math::vector3 out{};
		if ( !this->m_ctx.weapon_services ) return out;
		memory::call_vfunc<void>( this->m_ctx.weapon_services, 29, reinterpret_cast< std::uintptr_t >( &out ) );
		return out;
	}

	math::vector3 shared::get_interpolated_shoot_position( std::uintptr_t local_pawn, bool newest ) const
	{
		const auto ws = this->m_ctx.weapon_services;
		if ( !ws || !local_pawn ) return {};
		const auto head = memory::read<int>( ws + 872 );
		const auto count = memory::read<int>( ws + 876 );

		if ( count < 1 || count > 32 || head < 0 || head >= 32 )
		{
			return this->get_shoot_position( );
		}

		if ( newest )
		{
			const auto newest_idx = ( head + count - 1 ) % 32;
			const auto newest_off = ws + 232 + 20ull * newest_idx;
			return memory::read<math::vector3>( newest_off + 8 );
		}

		if ( count < 2 )
		{
			return this->get_shoot_position( );
		}

		const auto interp = memory::call<float>(PATTERN (patterns::get_interp_amount), local_pawn );
		const auto newest_idx = ( head + count - 1 ) % 32;
		const auto newest_off = ws + 232 + 20ull * newest_idx;
		const auto newest_tick = memory::read<int>( newest_off );
		const auto newest_frac = memory::read<float>( newest_off + 4 );

		const auto target = cstypes::tick_fraction{ newest_tick, newest_frac }.subtract_value( interp * 64.0f );

		for ( auto i = 0; i < count - 1; ++i )
		{
			const auto idx_a = ( head + static_cast< std::size_t >( i ) ) % 32;
			const auto idx_b = ( head + static_cast< std::size_t >( i ) + 1 ) % 32;

			const auto a_off = ws + 232 + 20ull * idx_a;
			const auto b_off = ws + 232 + 20ull * idx_b;

			const auto a_tick = memory::read<int>( a_off );
			const auto a_frac = memory::read<float>( a_off + 4 );
			const auto b_tick = memory::read<int>( b_off );
			const auto b_frac = memory::read<float>( b_off + 4 );

			const auto a_before = a_tick < target.tick || ( a_tick == target.tick && a_frac <= target.frac );
			if ( !a_before )
			{
				break;
			}

			const auto b_after = b_tick > target.tick || ( b_tick == target.tick && b_frac >= target.frac );
			if ( !b_after )
			{
				continue;
			}

			const auto a_pos = memory::read<math::vector3>( a_off + 8 );
			const auto b_pos = memory::read<math::vector3>( b_off + 8 );

			const auto span = cstypes::tick_fraction{ b_tick, b_frac }.subtract( { a_tick, a_frac } );
			const auto partial = target.subtract( { a_tick, a_frac } );

			const auto total_f = static_cast< float >( span.tick ) + span.frac;
			const auto partial_f = static_cast< float >( partial.tick ) + partial.frac;

			const auto t = total_f > 0.0f ? partial_f / total_f : 0.0f;

			return a_pos + ( b_pos - a_pos ) * t;
		}

		return this->get_shoot_position( );
	}

	float shared::get_spread( ) const
	{
		static const auto get_spread = PATTERN( patterns::get_spread );
		return memory::call<float>( get_spread, this->m_ctx.weapon );
	}

	float shared::get_inaccuracy( bool update_accuracy_penalty ) const
	{
		const auto accuracy_state_begin = SCHEMA( "C_CSWeaponBase", "m_flTurningInaccuracyDelta"_hash );
		const auto accuracy_state_end = SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash );
		if ( !this->m_ctx.weapon || accuracy_state_begin <= 0 || accuracy_state_end < accuracy_state_begin )
		{
			return 0.0f;
		}

		const auto accuracy_state_size = static_cast< std::size_t >( accuracy_state_end - accuracy_state_begin ) + sizeof( float );
		if ( accuracy_state_size > 0x100 )
		{
			return 0.0f;
		}

		std::vector<std::uint8_t> backup( accuracy_state_size );
		std::memcpy( backup.data( ), reinterpret_cast< const void* >( this->m_ctx.weapon + accuracy_state_begin ), accuracy_state_size );

		if ( update_accuracy_penalty )
		{
			memory::call<void>(PATTERN (patterns::weapon_update_accuracy), this->m_ctx.weapon );
		}

		static const auto get_inaccuracy = PATTERN( patterns::get_inaccuracy );
		const auto inaccuracy = memory::call<float>(
			get_inaccuracy, this->m_ctx.weapon,
			static_cast<float*>( nullptr ), static_cast<float*>( nullptr ) );

		std::memcpy( reinterpret_cast< void* >( this->m_ctx.weapon + accuracy_state_begin ), backup.data( ), accuracy_state_size );

		return inaccuracy;
	}

	float shared::get_inaccuracy_at_velocity( std::uintptr_t local_pawn, const math::vector3& velocity ) const
	{
		const auto accuracy_state_begin = SCHEMA( "C_CSWeaponBase", "m_flTurningInaccuracyDelta"_hash );
		const auto accuracy_state_end = SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash );
		if ( !this->m_ctx.weapon || !local_pawn || accuracy_state_begin <= 0 || accuracy_state_end < accuracy_state_begin )
		{
			return 0.0f;
		}

		const auto accuracy_state_size = static_cast< std::size_t >( accuracy_state_end - accuracy_state_begin ) + sizeof( float );
		if ( accuracy_state_size > 0x100 )
		{
			return 0.0f;
		}

		std::vector<std::uint8_t> backup( accuracy_state_size );
		std::memcpy( backup.data( ), reinterpret_cast< const void* >( this->m_ctx.weapon + accuracy_state_begin ), accuracy_state_size );

		const auto old_velocity = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
		const auto old_eflags = memory::read<std::uint32_t>( local_pawn + SCHEMA( "C_BaseEntity", "m_iEFlags"_hash ) );

		memory::write( local_pawn + SCHEMA( "C_BaseEntity", "m_iEFlags"_hash ), old_eflags & ~0x1000u );
		memory::write( local_pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ), velocity );

		memory::call<void>(PATTERN (patterns::weapon_update_accuracy), this->m_ctx.weapon );

		static const auto get_inaccuracy = PATTERN( patterns::get_inaccuracy );
		const auto inaccuracy = memory::call<float>(
			get_inaccuracy, this->m_ctx.weapon,
			static_cast<float*>( nullptr ), static_cast<float*>( nullptr ) );

		memory::write( local_pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ), old_velocity );
		memory::write( local_pawn + SCHEMA( "C_BaseEntity", "m_iEFlags"_hash ), old_eflags );

		std::memcpy( reinterpret_cast< void* >( this->m_ctx.weapon + accuracy_state_begin ), backup.data( ), accuracy_state_size );

		return inaccuracy;
	}

	bool shared::can_shoot( systems::input::usercmd* cmd, std::uintptr_t local_controller, bool check_next_attack ) const
	{
		if ( this->m_ctx.weapon_type != cstypes::weapon_type::knife )
		{
			if ( this->m_ctx.in_reload )
			{
				return false;
			}

			if ( this->m_ctx.clip <= 0 )
			{
				return false;
			}
		}

		if ( !check_next_attack )
		{
			return true;
		}

		const auto tick_base = memory::read<int>( local_controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		const auto base_cmd = cmd->csgo_user_cmd.base( );
		const auto client_tick = base_cmd ? base_cmd->client_tick( ) : tick_base;
		if ( this->m_ctx.weapon_type == cstypes::weapon_type::knife )
		{
			return tick_base >= this->m_last_shoot_tick + 2 &&
				( client_tick >= this->m_ctx.next_primary_attack_tick || client_tick >= this->m_ctx.next_secondary_attack_tick );
		}

		return tick_base >= this->m_last_shoot_tick + 2 && client_tick >= this->m_ctx.next_primary_attack_tick;
	}

	bool shared::is_max_accuracy( float inaccuracy ) const
	{
		const auto& prestate = systems::g_prediction.pre( );
		const auto on_ground = ( prestate.flags & 1 ) != 0;
		const auto is_ducking = ( prestate.flags & 4 ) != 0;
		// Abs velocity, not networked: networked trails ~2 ticks, so a
		// fresh peek would otherwise read as "still standing" and force a
		// high-inaccuracy bullet into the wall edge.
		const auto speed = prestate.velocity.length_2d( );

		if ( on_ground )
		{
			if ( this->m_ctx.weapon_type == cstypes::weapon_type::sniper )
			{
				if ( !this->m_ctx.is_scoped )
				{
					return false;
				}

				if ( is_ducking )
				{
					const auto rounded = std::floorf( inaccuracy * 300.0f ) / 300.0f;
					return rounded < inaccuracy;
				}

				if ( speed <= 0.1f )
				{
					const auto rounded = std::floorf( inaccuracy * 170.0f ) / 170.0f;
					return rounded < inaccuracy;
				}

				return false;
			}

			return speed <= this->m_ctx.weapon_max_speed * 0.34f;
		}

		const auto inaccuracy_jump_apex = memory::read<float>( this->m_ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flInaccuracyJumpApex"_hash ) );
		const auto accuracy_penalty = memory::read<float>( this->m_ctx.weapon + SCHEMA( "C_CSWeaponBase", "m_fAccuracyPenalty"_hash ) );
		const auto min_air_inaccuracy = accuracy_penalty + inaccuracy_jump_apex;

		constexpr auto tolerance{ 0.001f };
		return inaccuracy <= min_air_inaccuracy + tolerance;
	}

	bool shared::ray_vs_capsule( const math::vector3& ray_origin, const math::vector3& ray_dir, const math::vector3& capsule_a, const math::vector3& capsule_b, float radius, float& out_fraction ) const
	{
		const auto ab = capsule_b - capsule_a;
		const auto ab_sq = ab.dot( ab );
		const auto oc = ray_origin - capsule_a;
		const auto dir_sq = ray_dir.dot( ray_dir );

		if ( dir_sq < 1e-8f )
		{
			return false;
		}

		auto best_t{ 1.0f };
		auto hit{ false };

		if ( ab_sq > 1e-8f )
		{
			const float m = ab.dot( ray_dir ) / ab_sq;
			const float n = ab.dot( oc ) / ab_sq;

			const auto d_perp = ray_dir - ab * m;
			const auto oc_perp = oc - ab * n;

			const auto a = d_perp.dot( d_perp );
			const auto half_b = d_perp.dot( oc_perp );
			const auto c = oc_perp.dot( oc_perp ) - radius * radius;

			if ( a > 1e-8f )
			{
				const auto disc = half_b * half_b - a * c;
				if ( disc >= 0.0f )
				{
					const auto sqrt_disc = std::sqrt( disc );

					for ( int r = 0; r < 2; r++ )
					{
						const auto t = ( -half_b + ( r == 0 ? -sqrt_disc : sqrt_disc ) ) / a;
						if ( t < 0.0f || t >= best_t )
						{
							continue;
						}

						const auto s = m * t + n;
						if ( s >= 0.0f && s <= 1.0f )
						{
							best_t = t;
							hit = true;
							break;
						}
					}
				}
			}
		}

		const math::vector3 caps[ ]{ capsule_a, capsule_b };

		for ( int i = 0; i < 2; i++ )
		{
			const auto co = ray_origin - caps[ i ];
			const auto half_b = co.dot( ray_dir );
			const auto c = co.dot( co ) - radius * radius;
			const auto disc = half_b * half_b - dir_sq * c;

			if ( disc < 0.0f )
			{
				continue;
			}

			const auto sqrt_disc = std::sqrt( disc );

			for ( int r = 0; r < 2; r++ )
			{
				const auto t = ( -half_b + ( r == 0 ? -sqrt_disc : sqrt_disc ) ) / dir_sq;
				if ( t < 0.0f || t >= best_t )
				{
					continue;
				}

				if ( ab_sq > 1e-8f )
				{
					const auto hit_point = ray_origin + ray_dir * t - caps[ i ];
					const auto sign = i == 0 ? -1.0f : 1.0f;

					if ( sign * ab.dot( hit_point ) < 0.0f )
					{
						continue;
					}
				}

				best_t = t;
				hit = true;
				break;
			}
		}

		if ( hit )
		{
			out_fraction = best_t;
		}

		return hit;
	}

} // namespace features::combat
