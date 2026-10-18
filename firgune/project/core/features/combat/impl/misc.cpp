#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>
#include <core/rendering/rendering.hpp>
namespace features::combat {
	namespace {

		void draw_radial_steering_marker( xdraw::draw_list& draw_list, float cx, float cy )
		{
			constexpr auto inner_radius{ 2.5f };
			constexpr auto color_radius{ 11.0f };
			constexpr auto outer_radius{ 18.0f };
			constexpr auto segments{ 40 };
			constexpr xdraw::color deep_blue{ 14, 58, 158, 238 };
			constexpr xdraw::color transparent_blue{ 14, 58, 158, 0 };

			draw_list.ensure_cmd( nullptr );

			std::array<std::uint32_t, segments * 3> vertices{};
			const auto angle_step = 2.0f * std::numbers::pi_v<float> / static_cast< float >( segments );

			for ( auto i = 0; i < segments; ++i )
			{
				const auto angle = angle_step * static_cast< float >( i );
				const auto cosine = std::cosf( angle );
				const auto sine = std::sinf( angle );
				const auto idx = static_cast< std::size_t >( i ) * 3;

				vertices[ idx ] = draw_list.emit_vtx(
					cx + cosine * inner_radius, cy + sine * inner_radius, 0.0f, 0.0f, transparent_blue );
				vertices[ idx + 1 ] = draw_list.emit_vtx(
					cx + cosine * color_radius, cy + sine * color_radius, 0.0f, 0.0f, deep_blue );
				vertices[ idx + 2 ] = draw_list.emit_vtx(
					cx + cosine * outer_radius, cy + sine * outer_radius, 0.0f, 0.0f, transparent_blue );
			}

			for ( auto i = 0; i < segments; ++i )
			{
				const auto next = ( i + 1 ) % segments;
				const auto current_idx = static_cast< std::size_t >( i ) * 3;
				const auto next_idx = static_cast< std::size_t >( next ) * 3;

				draw_list.emit_quad( vertices[ current_idx ], vertices[ next_idx ],
					vertices[ next_idx + 1 ], vertices[ current_idx + 1 ] );
				draw_list.emit_quad( vertices[ current_idx + 1 ], vertices[ next_idx + 1 ],
					vertices[ next_idx + 2 ], vertices[ current_idx + 2 ] );
			}
		}

	} // namespace

	void misc::antiaim::push_steering_trail_point( const math::vector2& cursor )
	{
		if ( this->m_steering_trail_count == steering_trail_capacity )
		{
			for ( auto i = 1ull; i < steering_trail_capacity; ++i )
			{
				this->m_steering_trail[ i - 1 ] = this->m_steering_trail[ i ];
			}

			--this->m_steering_trail_count;
		}

		this->m_steering_trail[ this->m_steering_trail_count++ ] = { cursor, 0.0f };
	}

	bool misc::antiaim::steering_requested( ) const
	{
		const auto& cfg = settings::g_combat.m_antiaim;
		if ( !cfg.enabled.value ) return false;

		const auto& steering = cfg.mouse_steering;
		// Read hold binds on the input thread, without waiting for Present.
		if ( steering.bind.key != 0 && steering.bind.mode == xui::bind_mode::hold_on )
			return ( GetAsyncKeyState( steering.bind.key ) & 0x8000 ) != 0;

		return steering.value;
	}

	bool misc::antiaim::get_steering_view( math::vector3& angles ) const
	{
		if ( !this->m_steering_engaged || !this->steering_requested( )
			|| rendering::g_menu.is_open( ) || !systems::g_local.get( ).is_alive
			|| systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
			return false;

		angles = systems::g_input.get_view_angles( );
		angles.x = this->m_steering_frozen_pitch;
		angles.y = this->m_steering_frozen_yaw;
		return true;
	}

	void misc::antiaim::begin_process_input_event( std::uintptr_t csgo_input, int slot )
	{
		if ( slot != 0 || !csgo_input )
		{
			return;
		}

		const auto& cfg = settings::g_combat.m_antiaim;
		const auto requested = this->steering_requested( );
		const auto local = systems::g_local.get( );

		if ( !requested )
		{
			if ( this->m_steering_hold_active && !this->m_steering_moved_this_hold )
			{
				this->m_steering_cursor = { 0.0f, std::max( cfg.mouse_steering_radius.value, 1.0f ) };
				this->m_steering_last_yaw_offset = 180.0f;
				this->m_steering_has_direction = false;
			}

			this->m_steering_hold_active = false;
			this->m_steering_engaged = false;
			this->m_steering_trail_count = 0;
			return;
		}

		if ( !this->m_steering_hold_active )
		{
			this->m_steering_hold_travel = 0.0f;
			this->m_steering_moved_this_hold = false;
			this->m_steering_hold_active = true;
		}

		auto active = requested
			&& !rendering::g_menu.is_open( )
			&& !systems::g_local.is_in_cinematic( ) && !systems::g_local.is_in_time_freeze( )
			&& local.is_alive && local.pawn;

		if ( active )
		{
			// Same bail-outs as the anti aim itself: grenade throwing,
			// ladder and noclip hand the camera back to the engine.
			const auto& ctx = g_shared.ctx( );
			if ( ctx.weapon_type == cstypes::weapon_type::grenade
				&& memory::read<float>( ctx.weapon + SCHEMA( "C_BaseCSGrenade", "m_fThrowTime"_hash ) ) > 0.0f )
			{
				active = false;
			}
			else
			{
				const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
				if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
				{
					active = false;
				}
			}
		}

		if ( !active )
		{
			// Eligibility interruptions pause camera capture without ending the
			// physical hold or turning the interruption into a reset tap.
			this->m_steering_engaged = false;
			this->m_steering_trail_count = 0;
			return;
		}

		auto view_angles = systems::g_input.get_view_angles( );

		if ( !this->m_steering_engaged )
		{
			// Capture the camera for this hold. A retained absolute yaw is
			// converted back into a cursor position relative to the new view,
			// so adjusting can continue even if the camera moved while released.
			this->m_steering_frozen_pitch = view_angles.x;
			this->m_steering_frozen_yaw = view_angles.y;

			const auto radius = std::max( cfg.mouse_steering_radius.value, 1.0f );
			if ( this->m_steering_has_direction )
			{
				this->m_steering_last_yaw_offset = math::helpers::normalize_yaw(
					this->m_steering_yaw - this->m_steering_frozen_yaw );

				const auto offset_rad = this->m_steering_last_yaw_offset
					* ( std::numbers::pi_v<float> / 180.0f );
				this->m_steering_cursor = {
					-std::sinf( offset_rad ) * radius,
					-std::cosf( offset_rad ) * radius
				};
			}
			else
			{
				this->m_steering_cursor = { 0.0f, radius };
				this->m_steering_last_yaw_offset = 180.0f;
			}

			this->m_steering_trail_count = 0;
			this->m_steering_engaged = true;
		}

		// Reset the actual camera input before the engine applies mouse input.
		view_angles.x = this->m_steering_frozen_pitch;
		view_angles.y = this->m_steering_frozen_yaw;
		systems::g_input.set_view_angles( view_angles );
	}

	void misc::antiaim::end_process_input_event( std::uintptr_t csgo_input, int slot )
	{
		if ( slot != 0 || !csgo_input || !this->m_steering_engaged )
		{
			return;
		}

		const auto& cfg = settings::g_combat.m_antiaim;
		auto view_angles = systems::g_input.get_view_angles( );

		// The engine already applied the mouse delta to the view angles:
		// harvest it as this frame's cursor movement.
		auto d_pitch = view_angles.x - this->m_steering_frozen_pitch;
		auto d_yaw = view_angles.y - this->m_steering_frozen_yaw;
		while ( d_yaw > 180.0f ) d_yaw -= 360.0f;
		while ( d_yaw < -180.0f ) d_yaw += 360.0f;

		// Angle delta -> pixels using the default m_yaw/m_pitch counts.
		const auto sens = std::max( CONVAR( "sensitivity" )->get<float>( ), 0.01f );
		const auto deg_per_px = 0.022f * sens;
		const auto cursor_dx = -d_yaw / deg_per_px;
		const auto cursor_dy = d_pitch / deg_per_px;
		const auto previous_cursor = this->m_steering_cursor;
		this->m_steering_cursor.x += cursor_dx;
		this->m_steering_cursor.y += cursor_dy;

		// Screen X points right; positive world yaw points left. Convert the
		// camera delta into screen coordinates before updating the cursor.
		const auto input_distance = std::sqrtf( cursor_dx * cursor_dx + cursor_dy * cursor_dy );
		this->m_steering_hold_travel += input_distance;
		if ( this->m_steering_hold_travel >= 2.0f )
		{
			this->m_steering_moved_this_hold = true;
			this->m_steering_has_direction = true;
		}

		// Clamp the cursor inside the invisible steering circle.
		const auto radius = std::max( cfg.mouse_steering_radius.value, 1.0f );
		const auto len = std::sqrtf( this->m_steering_cursor.x * this->m_steering_cursor.x
			+ this->m_steering_cursor.y * this->m_steering_cursor.y );
		if ( len > radius )
		{
			const auto scale = radius / len;
			this->m_steering_cursor.x *= scale;
			this->m_steering_cursor.y *= scale;
		}

		const auto trail_dx = this->m_steering_cursor.x - previous_cursor.x;
		const auto trail_dy = this->m_steering_cursor.y - previous_cursor.y;
		if ( input_distance >= 0.5f && trail_dx * trail_dx + trail_dy * trail_dy >= 0.25f )
		{
			if ( this->m_steering_trail_count == 0 )
			{
				this->push_steering_trail_point( previous_cursor );
			}

			this->push_steering_trail_point( this->m_steering_cursor );
		}

		if ( this->m_steering_moved_this_hold )
		{
			this->m_steering_yaw = this->get_steering_yaw( );
		}

		// Freeze the camera: undo the mouse movement of this frame.
		view_angles.x = this->m_steering_frozen_pitch;
		view_angles.y = this->m_steering_frozen_yaw;
		systems::g_input.set_view_angles( view_angles );
	}

	float misc::antiaim::get_steering_yaw( )
	{
		const auto& cursor = this->m_steering_cursor;
		const auto len = std::sqrtf( cursor.x * cursor.x + cursor.y * cursor.y );

		// Dead zone: the direction is undefined at the exact center, so the
		// last offset is held while the cursor hovers near it.
		if ( len < 12.0f )
		{
			return math::helpers::normalize_yaw(
				this->m_steering_frozen_yaw + this->m_steering_last_yaw_offset );
		}

		// Screen direction -> world yaw: up = forward, left = +90,
		// right = -90, down = backwards. The inverse uses -sin above.
		const auto offset = std::atan2f( -cursor.x, -cursor.y ) * ( 180.0f / std::numbers::pi_v<float> );
		this->m_steering_last_yaw_offset = offset;
		return math::helpers::normalize_yaw( this->m_steering_frozen_yaw + offset );
	}

	void misc::antiaim::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_antiaim_active = false;

		if ( !settings::g_combat.m_antiaim.enabled.value )
		{
			return;
		}

		if ( systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			return;
		}

		if ( settings::g_combat.m_antiaim.manual_left.value && settings::g_combat.m_antiaim.manual_right.value )
		{
			settings::g_combat.m_antiaim.manual_right.value = false;
			settings::g_combat.m_antiaim.manual_right.bind.active = false;
		}

		if ( settings::g_combat.m_antiaim.manual_left.value )
		{
			this->m_yaw_side = -1;
		}
		else if ( settings::g_combat.m_antiaim.manual_right.value )
		{
			this->m_yaw_side = 1;
		}
		else
		{
			this->m_yaw_side = 0;
		}

		const auto local = systems::g_local.get( );
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto view_angles = systems::g_input.get_view_angles( );
		const auto& ctx = g_shared.ctx( );

		if ( cmd->buttons.value & cstypes::command_buttons::in_use )
		{
			return;
		}

		if ( ctx.weapon_type == cstypes::weapon_type::grenade )
		{
			if ( memory::read<float>( ctx.weapon + SCHEMA( "C_BaseCSGrenade", "m_fThrowTime"_hash ) ) > 0.0f )  // bail regardless of pin state
			{
				return;
			}
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		this->m_old_angles = view_angles;
		this->m_antiaim_active = true;

		this->m_modified_angles = this->m_old_angles;
		this->m_modified_angles.x = this->get_pitch( this->m_old_angles.x );

		// A moved hold stores an absolute yaw. Hold-on binds keep using it
		// after release; a no-movement tap clears it at the next input begin.
		const auto& steering = settings::g_combat.m_antiaim.mouse_steering;
		const auto retained_hold_direction = this->m_steering_has_direction
			&& steering.bind.key != 0 && steering.bind.mode == xui::bind_mode::hold_on;
		if ( this->m_steering_has_direction && ( this->m_steering_engaged || retained_hold_direction ) )
		{
			this->m_modified_angles.y = this->m_steering_yaw;
			this->m_indicator_yaw = this->m_steering_yaw;
		}
		else
		{
			this->m_modified_angles.y = this->get_yaw( this->m_old_angles, local );
		}

		math::helpers::normalize_angles( this->m_modified_angles );

		base->mutable_viewangles( )->set_x( this->m_modified_angles.x );
		base->mutable_viewangles( )->set_y( this->m_modified_angles.y );
		base->mutable_viewangles( )->set_z( this->m_modified_angles.z );

		// Movement is reframed once at the end of create_move, after every
		// combat and movement feature has written its final command values.
	}

	void misc::antiaim::on_render( xdraw::draw_list& draw_list )
	{
		if ( !settings::g_combat.m_antiaim.enabled.value )
		{
			this->m_steering_trail_count = 0;
			return;
		}

		const auto& cfg = settings::g_combat.m_antiaim;

		// Mouse steering: a recent-path trail followed by a deep-blue radial
		// marker whose center and outer edge both fade to transparency.
		if ( this->m_steering_engaged )
		{
			const auto [ width, height ] = xdraw::viewport_size( );
			const auto center_x = std::floorf( static_cast< float >( width ) * 0.5f );
			const auto center_y = std::floorf( static_cast< float >( height ) * 0.5f );

			constexpr auto trail_lifetime{ 0.34f };
			const auto dt = std::clamp( xdraw::delta_time( ), 0.0f, 0.05f );
			for ( auto i = 0ull; i < this->m_steering_trail_count; ++i )
			{
				this->m_steering_trail[ i ].age += dt;
			}

			while ( this->m_steering_trail_count > 0
				&& this->m_steering_trail[ 0 ].age >= trail_lifetime )
			{
				for ( auto i = 1ull; i < this->m_steering_trail_count; ++i )
				{
					this->m_steering_trail[ i - 1 ] = this->m_steering_trail[ i ];
				}

				--this->m_steering_trail_count;
			}

			if ( this->m_steering_trail_count >= 2 )
			{
				std::array<float, steering_trail_capacity * 2> trail_points{};
				std::array<xdraw::color, steering_trail_capacity> glow_colors{};
				std::array<xdraw::color, steering_trail_capacity> core_colors{};

				for ( auto i = 0ull; i < this->m_steering_trail_count; ++i )
				{
					const auto& point = this->m_steering_trail[ i ];
					trail_points[ i * 2 ] = center_x + point.cursor.x;
					trail_points[ i * 2 + 1 ] = center_y + point.cursor.y;

					const auto freshness = std::clamp( 1.0f - point.age / trail_lifetime, 0.0f, 1.0f );
					const auto path_position = static_cast< float >( i + 1 )
						/ static_cast< float >( this->m_steering_trail_count );
					const auto strength = freshness * freshness * ( 0.15f + path_position * 0.85f );
					glow_colors[ i ] = { 10, 43, 132, static_cast< std::uint8_t >( 78.0f * strength ) };
					core_colors[ i ] = { 18, 68, 176, static_cast< std::uint8_t >( 210.0f * strength ) };
				}

				const auto point_count = this->m_steering_trail_count;
				draw_list.polyline_gradient(
					std::span<const float>{ trail_points.data( ), point_count * 2 },
					std::span<const xdraw::color>{ glow_colors.data( ), point_count },
					false, 9.0f );
				draw_list.polyline_gradient(
					std::span<const float>{ trail_points.data( ), point_count * 2 },
					std::span<const xdraw::color>{ core_colors.data( ), point_count },
					false, 4.0f );
			}

			const auto cursor_x = center_x + this->m_steering_cursor.x;
			const auto cursor_y = center_y + this->m_steering_cursor.y;
			draw_radial_steering_marker( draw_list, cursor_x, cursor_y );
		}

		if ( !settings::g_combat.m_antiaim.direction_indicator.value )
		{
			return;
		}

		if ( !this->m_antiaim_active )
		{
			return;
		}

		if ( !systems::g_frame_data.valid( ) )
		{
			return;
		}

		const auto origin = systems::g_frame_data.origin( );
		const auto aa_yaw_rad = this->m_indicator_yaw * ( std::numbers::pi_v<float> / 180.0f );

		constexpr auto radius{ 28.0f };
		constexpr auto feet_offset{ -2.0f };
		constexpr auto arc_sweep_deg{ 60.0f };
		constexpr auto arc_segments{ 48 };
		constexpr auto max_thickness{ 3.0f };

		const auto& color = cfg.direction_indicator_color;
		const auto base = math::vector3{ origin.x, origin.y, origin.z + feet_offset };

		const auto half_sweep = ( arc_sweep_deg * 0.5f ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto start_angle = aa_yaw_rad - half_sweep;
		const auto end_angle = aa_yaw_rad + half_sweep;
		const auto angle_step = ( end_angle - start_angle ) / static_cast< float >( arc_segments );

		std::vector<math::vector2> pts;
		pts.reserve( arc_segments + 1 );

		for ( auto i = 0; i <= arc_segments; ++i )
		{
			const auto angle = start_angle + angle_step * static_cast< float >( i );

			const auto world_pt = math::vector3
			{
				base.x + std::cosf( angle ) * radius,
				base.y + std::sinf( angle ) * radius,
				base.z
			};

			const auto sp = systems::g_view.project( world_pt );

			if ( !systems::g_view.projection_valid( sp ) )
			{
				return;
			}

			pts.push_back( { sp.x, sp.y } );
		}

		if ( pts.size( ) < 2 )
		{
			return;
		}

		const auto total = static_cast< float >( pts.size( ) - 1 );

		const auto fade_at = [ ]( std::size_t idx, float total ) -> float
			{
				const auto frac = static_cast< float >( idx ) / total;
				const auto edge = 1.0f - std::fabsf( frac - 0.5f ) * 2.0f;
				return edge * edge * edge * ( edge * ( edge * 6.0f - 15.0f ) + 10.0f );
			};

		if ( cfg.direction_indicator_glow )
		{
			auto& glow = xdraw::get_glow( );

			for ( auto i = 0ull; i + 1 < pts.size( ); ++i )
			{
				const auto f0 = fade_at( i, total );
				const auto f1 = fade_at( i + 1, total );

				const auto ga0 = static_cast< std::uint8_t >( static_cast< float >( color.value.a ) * cfg.direction_indicator_glow_strength * std::fmaxf( f0, 0.05f ) );
				const auto ga1 = static_cast< std::uint8_t >( static_cast< float >( color.value.a ) * cfg.direction_indicator_glow_strength * std::fmaxf( f1, 0.05f ) );

				const auto thickness = ( max_thickness + 2.0f ) * ( ( f0 + f1 ) * 0.5f * 0.85f + 0.15f );

				const float seg[ ]{ pts[ i ].x, pts[ i ].y, pts[ i + 1 ].x, pts[ i + 1 ].y };
				const xdraw::color cols[ ]{ { color.value.r, color.value.g, color.value.b, ga0 }, { color.value.r, color.value.g, color.value.b, ga1 } };

				glow.polyline_gradient( seg, cols, false, thickness );
			}
		}

		for ( auto i = 0ull; i + 1 < pts.size( ); ++i )
		{
			const auto f0 = fade_at( i, total );
			const auto f1 = fade_at( i + 1, total );

			const auto a0 = static_cast< std::uint8_t >( color.value.a * std::fmaxf( f0, 0.05f ) );
			const auto a1 = static_cast< std::uint8_t >( color.value.a * std::fmaxf( f1, 0.05f ) );

			const auto thickness = max_thickness * ( ( f0 + f1 ) * 0.5f * 0.85f + 0.15f );

			const float seg[ ]{ pts[ i ].x, pts[ i ].y, pts[ i + 1 ].x, pts[ i + 1 ].y };
			const xdraw::color cols[ ]{ { color.value.r, color.value.g, color.value.b, a0 }, { color.value.r, color.value.g, color.value.b, a1 } };

			draw_list.polyline_gradient( seg, cols, false, thickness );
		}
	}

	float misc::antiaim::get_pitch( float view_pitch )
	{
		switch ( settings::g_combat.m_antiaim.pitch )
		{
		case settings::combat::antiaim::pitch_mode::down:
			return 89.0f;
		case settings::combat::antiaim::pitch_mode::up:
			return -89.0f;
		default:
			return view_pitch;
		}
	}

	float misc::antiaim::get_yaw( const math::vector3& view_angles, const systems::local::snapshot& local )
	{
		auto base_yaw_offset{ 180.0f };

		const auto view_yaw = view_angles.y;
		auto base_yaw = view_yaw - base_yaw_offset;

		const auto local_game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto local_origin = memory::read<math::vector3>( local_game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );

		if ( settings::g_combat.m_antiaim.avoid_backstab.value )
		{
			constexpr auto backstab_range_sq = 350.0f * 350.0f;
			auto knife_dist = std::numeric_limits<float>::max( );
			auto knife_yaw{ 0.0f };
			auto knife_found{ false };

			for ( const auto& p : players )
			{
				if ( !p.ptr || p.ptr == local.controller )
				{
					continue;
				}

				if ( !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
				{
					continue;
				}

				const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
				const auto pawn = systems::g_entities.lookup( pawn_handle );

				if ( !pawn || pawn == local.pawn )
				{
					continue;
				}

				const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
				if ( !local.is_this_other_team( team ) )
				{
					continue;
				}

				const auto health = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
				if ( health <= 0 )
				{
					continue;
				}

				const auto enemy_game_scene_node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
				if ( !enemy_game_scene_node )
				{
					continue;
				}

				const auto enemy_origin = memory::read<math::vector3>( enemy_game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
				const auto dx = enemy_origin.x - local_origin.x;
				const auto dy = enemy_origin.y - local_origin.y;
				const auto dist_sq = dx * dx + dy * dy;

				if ( dist_sq > backstab_range_sq )
				{
					continue;
				}

				const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
				if ( !weapon_services )
				{
					continue;
				}

				const auto weapon_handle = memory::read<std::uint32_t>( weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hActiveWeapon"_hash ) );
				if ( !weapon_handle )
				{
					continue;
				}

				const auto weapon = systems::g_entities.lookup( weapon_handle );
				if ( !weapon )
				{
					continue;
				}

				const auto weapon_vdata = memory::read<std::uintptr_t>( weapon + SCHEMA( "C_BaseEntity", "m_nSubclassID"_hash ) + 0x8 );
				if ( !weapon_vdata )
				{
					continue;
				}

				const auto weapon_type = memory::read<std::uint32_t>( weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_WeaponType"_hash ) );
				if ( weapon_type != cstypes::weapon_type::knife )
				{
					continue;
				}

				if ( dist_sq < knife_dist )
				{
					knife_dist = dist_sq;
					knife_yaw = std::atan2f( dy, dx ) * ( 180.0f / std::numbers::pi_v<float> );
					knife_found = true;
				}
			}

			if ( knife_found )
			{
				this->m_indicator_yaw = knife_yaw;
				return knife_yaw;
			}
		}

		// Model orientation is camera-relative: the base yaw is the local
		// view yaw mirrored by 180 degrees, never the direction toward the
		// highest-threat enemy.
		auto indicator = base_yaw;
		if ( this->m_yaw_side == -1 )
		{
			indicator -= 90.0f;
		}
		else if ( this->m_yaw_side == 1 )
		{
			indicator += 90.0f;
		}

		this->m_indicator_yaw = indicator;

		auto yaw = base_yaw;
		if ( this->m_yaw_side == -1 )
		{
			yaw -= 90.0f;
		}
		else if ( this->m_yaw_side == 1 )
		{
			yaw += 90.0f;
		}

		if ( settings::g_combat.m_antiaim.body_angle_rotation.value )
			yaw += settings::g_combat.m_antiaim.body_angle.value;

		return yaw;
	}

	void misc::antiaim::correct_movement (systems::input::usercmd* cmd) {
		if (!this->m_should_correct) {
			return;
		}

		this->m_should_correct = false;

		const auto base = cmd->csgo_user_cmd.mutable_base ();
		const auto forward_move = base->forwardmove ();
		const auto side_move = base->leftmove ();

		if (forward_move == 0.0f && side_move == 0.0f) {
			return;
		}

		math::vector3 new_forward {}, new_left {};
		math::helpers::angle_vectors_left (this->m_modified_angles, &new_forward, &new_left, nullptr);

		math::vector3 old_forward {}, old_left {};
		math::helpers::angle_vectors_left (this->m_old_angles, &old_forward, &old_left, nullptr);

		new_forward.z = 0.0f; new_left.z = 0.0f;
		old_forward.z = 0.0f; old_left.z = 0.0f;
		new_forward.normalize ();
		new_left.normalize ();
		old_forward.normalize ();
		old_left.normalize ();

		const auto intent = old_forward * forward_move + old_left * -side_move;
		const auto intent_len = intent.length ();

		if (intent_len == 0.0f) {
			return;
		}

		const auto intent_dir = intent / intent_len;
		const auto corrected_forward = new_forward.dot (intent_dir) * intent_len;
		const auto corrected_side = -new_left.dot (intent_dir) * intent_len;

		base->set_forwardmove (std::clamp (corrected_forward, -1.0f, 1.0f));
		base->set_leftmove (std::clamp (corrected_side, -1.0f, 1.0f));

		{
			auto buttons = cmd->buttons.value;
			const auto movement_buttons = static_cast<std::uintptr_t>( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );
			buttons &= ~movement_buttons;

			if (base->forwardmove () > 0.0f) {
				buttons |= cstypes::command_buttons::in_forward;
			} else if (base->forwardmove () < 0.0f) {
				buttons |= cstypes::command_buttons::in_back;
			}

			if (base->leftmove () > 0.0f) {
				buttons |= cstypes::command_buttons::in_moveleft;
			} else if (base->leftmove () < 0.0f) {
				buttons |= cstypes::command_buttons::in_moveright;
			}

			cmd->buttons.value = buttons;
			cmd->buttons.value_changed |= movement_buttons;
		}
	}

	namespace {

		math::vector3 quickpeek_ground_snap( std::uintptr_t skip_pawn, const math::vector3& feet_pos )
		{
			const auto start = math::vector3{ feet_pos.x, feet_pos.y, feet_pos.z + 64.0f };
			const auto end = math::vector3{ feet_pos.x, feet_pos.y, feet_pos.z - 8192.0f };
			const auto tr = systems::g_tracing.trace( start, end, skip_pawn );

			if ( tr.fraction <= 0.0f || tr.fraction >= 0.997f )
			{
				return feet_pos;
			}

			auto out = tr.position;
			out.z += 1.0f;
			return out;
		}

	} // namespace

	void misc::duckpeek::on_create_move( systems::input::usercmd* cmd )
	{
		if ( !cmd || !settings::g_combat.m_duckpeek.enabled.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn || systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			return;
		}

		if ( g_rage.should_release_duck_for_shot( ) )
		{
			// Let native movement update posture, collision and view height together.
			// Direct duck-amount writes are corrected by prediction/server state.
			cmd->buttons.value &= ~cstypes::command_buttons::in_duck;
			cmd->buttons.value_changed &= ~cstypes::command_buttons::in_duck;
			cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_duck;
			return;
		}

		cmd->buttons.value |= cstypes::command_buttons::in_duck;
		cmd->buttons.value_changed |= cstypes::command_buttons::in_duck;
		cmd->buttons.value_scroll &= ~cstypes::command_buttons::in_duck;
	}

	void misc::duckpeek::on_override_view( std::uintptr_t view_setup )
	{
		if ( !settings::g_combat.m_duckpeek.enabled.value || !view_setup )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn || systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) )
		{
			return;
		}

		// Third person already follows the native eye and resolves wall collision.
		// A late height offset disagrees with that eye during duck transitions.
		if ( settings::g_misc.m_camera.thirdperson.value )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto duck_amount = std::clamp( memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) ), 0.0f, 1.0f );
		auto camera_position = memory::read<math::vector3>( view_setup + 0x4a0 );
		camera_position.z += 18.0f * duck_amount;
		memory::write<math::vector3>( view_setup + 0x4a0, camera_position );
	}

	void misc::quickpeek::on_create_move( systems::input::usercmd* cmd )
	{
		if ( g_rage.landing_stop_active( ) )
		{
			return;
		}

		if ( !settings::g_combat.m_quickpeek.enabled.value )
		{
			this->reset( );
			return;
		}

		const auto local = systems::g_local.get( );
		const auto& ctx = g_shared.ctx( );

		if ( ctx.weapon_type < cstypes::weapon_type::pistol || ctx.weapon_type > cstypes::weapon_type::lmg )
		{
			this->reset( );
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		constexpr auto movement_cancel_mask = static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );
		const auto curr_movement_bits = cmd->buttons.value & movement_cancel_mask;

		const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );

		if ( this->m_saved_origin.length_sqr( ) < 0.001f )
		{
			this->m_saved_origin = quickpeek_ground_snap( local.pawn, origin );
			this->m_should_retrack = false;
			this->m_fired = false;
			this->m_active = true;
			this->create_particle( );
			this->m_prev_movement_bits = curr_movement_bits;
			return;
		}

		this->update_particle( );

		const auto distance = ( origin - this->m_saved_origin ).length_2d( );

		if ( this->m_should_retrack && ( curr_movement_bits & ~this->m_prev_movement_bits ) != 0 )
		{
			this->m_should_retrack = false;
		}

		if ( this->m_should_retrack && ( systems::g_prediction.pre( ).flags & cstypes::entity_flags::on_ground ) )
		{
			const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
			const auto speed = velocity.length_2d( );

			if ( distance < 5.0f && speed < 15.0f )
			{
				this->m_should_retrack = false;
				this->m_fired = false;
			}
			else if ( distance < speed * 0.1f && speed > 15.0f )
			{
				const auto vel_angle = math::helpers::vector_to_angle( velocity * -1.0f );
				const auto yaw_diff = math::helpers::deg_to_rad( features::movement::g_movement_fix.source_yaw( ) - vel_angle.y );

				base->set_forwardmove( std::cosf( yaw_diff ) );
				base->set_leftmove( -std::sinf( yaw_diff ) );

				auto buttons = cmd->buttons.value;
				buttons &= ~static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );

				if ( base->forwardmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_forward;
				}
				else if ( base->forwardmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_back;
				}

				if ( base->leftmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveleft;
				}
				else if ( base->leftmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveright;
				}

				cmd->buttons.value = buttons;
			}
			else
			{
				const auto diff = this->m_saved_origin - origin;
				const auto angle_to_pos = math::helpers::vector_to_angle( diff );
				const auto yaw_diff = math::helpers::deg_to_rad( features::movement::g_movement_fix.source_yaw( ) - angle_to_pos.y );

				base->set_forwardmove( std::cosf( yaw_diff ) );
				base->set_leftmove( -std::sinf( yaw_diff ) );

				auto buttons = cmd->buttons.value;
				buttons &= ~static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back | cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright );

				if ( base->forwardmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_forward;
				}
				else if ( base->forwardmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_back;
				}

				if ( base->leftmove( ) > 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveleft;
				}
				else if ( base->leftmove( ) < 0.0f )
				{
					buttons |= cstypes::command_buttons::in_moveright;
				}

				cmd->buttons.value = buttons;
			}
		}

		const auto is_revolver = g_shared.ctx( ).item_def_idx == cstypes::item_definition_index::weapon_r8_revolver;
		const auto fired = is_revolver
			? ( g_rage.is_firing_this_tick( ) || ( cmd->buttons.value & cstypes::command_buttons::in_second_attack ) != 0 )
			: ( cmd->buttons.value & cstypes::command_buttons::in_attack ) != 0;
		if ( fired && !g_rage.is_cocking_revolver( ) )
		{
			this->m_should_retrack = true;
			this->m_fired = true;
		}

		this->m_prev_movement_bits = curr_movement_bits;
	}

	void misc::quickpeek::reset_if_needed( )
	{
		if ( !this->m_active )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn )
		{
			this->reset( );
			return;
		}

		if ( !settings::g_combat.m_quickpeek.enabled.value )
		{
			this->reset( );
		}
	}

	void misc::quickpeek::create_particle( )
	{
		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( !particle_manager )
		{
			return;
		}

		constexpr auto particle_path{ "particles/embedded/halo.vpcf" };

		if ( !this->m_particle_loaded )
		{
			struct buffer_string
			{
				std::uint32_t m_unknown1{};
				std::uint32_t m_unknown2{ 0xc00000c8 };

				union
				{
					std::uintptr_t m_str_ptr;
					std::uint8_t data[ 0xc8 ];
				};

				std::uintptr_t m_unknown3{ 0 };
				std::uintptr_t m_unknown4{ 0 };
			} buffer;

			memory::call<void>(PATTERN (patterns::init_particle_path_buffer), &buffer, particle_path );
			buffer.m_unknown4 = 'fcpv';
			memory::call<void>(PATTERN (patterns::resource_system_precache), addresses::globals::resource_system, &buffer, "" );

			this->m_particle_loaded = true;
		}

		auto effect_index{ invalid_effect_index };
		memory::call<int*>(PATTERN (patterns::particle_create_effect), particle_manager, &effect_index, particle_path, 8, 0ll, 0ll, 0ll, 0 );

		this->m_particle_effect = effect_index;

		if ( effect_index == invalid_effect_index )
		{
			return;
		}

		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, effect_index, 0, &this->m_saved_origin, 0 );
	}

	void misc::quickpeek::update_particle( )
	{
		if ( this->m_particle_effect == invalid_effect_index )
		{
			return;
		}

		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( !particle_manager )
		{
			return;
		}

		const auto& cfg = settings::g_combat.m_quickpeek;
		const auto& col = this->m_should_retrack ? cfg.retrack_color : cfg.color;
		const auto color = math::vector3{ static_cast< float >( col.value.r ), static_cast< float >( col.value.g ), static_cast< float >( col.value.b ) };

		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, this->m_particle_effect, 1, &color, 0 );
		memory::call<bool>(PATTERN (patterns::particle_set_control_point), particle_manager, this->m_particle_effect, 0, &this->m_saved_origin, 0 );
	}

	void misc::quickpeek::release_particle( )
	{
		if ( this->m_particle_effect == invalid_effect_index )
		{
			return;
		}

		const auto particle_manager = memory::read<std::uintptr_t>( addresses::globals::particle_manager );
		if ( particle_manager )
		{
			memory::call<void>(PATTERN (patterns::particle_destroy_effect), particle_manager, this->m_particle_effect, true, true );
		}

		this->m_particle_effect = invalid_effect_index;
	}

	void misc::quickpeek::reset( )
	{
		this->release_particle( );
		this->m_saved_origin = {};
		this->m_should_retrack = false;
		this->m_fired = false;
		this->m_active = false;
		this->m_prev_movement_bits = 0;
	}

} // namespace features::combat
