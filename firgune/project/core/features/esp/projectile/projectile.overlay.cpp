#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/rendering/rendering.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

namespace features::esp::projectile {

	namespace detail {

		static constexpr const char* k_fire_svg{ R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 24 24"><path fill="#ffffff" d="M12 23a7.5 7.5 0 0 1-5.138-12.963C8.204 8.774 11.5 6.5 11 1.5c6 4 9 8 3 14c1 0 2.5 0 5-2.47c.27.773.5 1.604.5 2.47A7.5 7.5 0 0 1 12 23"/></svg>)" };

		struct fire_icon
		{
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture{};
			int width{};
			int height{};
		};

		[[nodiscard]] static const fire_icon* get_fire_icon( )
		{
			static fire_icon ico{};
			static auto loaded{ false };

			if ( !loaded )
			{
				ico.texture = xdraw::load_svg( k_fire_svg, 0.75f, &ico.width, &ico.height );
				loaded = true;
			}

			return ico.texture ? &ico : nullptr;
		}

		// Approximate coverage per burning network patch, not an exact damage contour.
		constexpr auto fire_radius{ 60.0f };
		constexpr auto fire_segments{ 24 };
		constexpr auto fire_plane_lift{ 1.5f };
		constexpr auto fire_near_w{ 0.01f };

		[[nodiscard]] static bool finite_fire_position( const math::vector3& p )
		{
			return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z )
				&& std::fabsf( p.x ) < 1000000.0f && std::fabsf( p.y ) < 1000000.0f && std::fabsf( p.z ) < 1000000.0f;
		}

		[[nodiscard]] static bool same_fire_plane( const math::vector3& center, const math::vector3& normal,
			const math::vector3& other_center, const math::vector3& other_normal )
		{
			const auto delta = other_center - center;
			return normal.x * other_normal.x + normal.y * other_normal.y + normal.z * other_normal.z > 0.98f
				&& std::fabsf( delta.x * normal.x + delta.y * normal.y + delta.z * normal.z ) < 4.0f;
		}

		// Sutherland-Hodgman half-plane clipping. All coordinates here are local XY.
		static void clip_fire_cell( std::vector<math::vector3>& polygon, std::vector<math::vector3>& scratch,
			const math::vector3& delta, float limit )
		{
			scratch.clear( );
			if ( polygon.empty( ) ) return;
			auto a = polygon.back( );
			auto da = a.x * delta.x + a.y * delta.y - limit;
			for ( const auto& b : polygon )
			{
				const auto db = b.x * delta.x + b.y * delta.y - limit;
				if ( ( da <= 0.0f ) != ( db <= 0.0f ) )
				{
					scratch.push_back( a + ( b - a ) * ( da / ( da - db ) ) );
				}
				if ( db <= 0.0f ) scratch.push_back( b );
				a = b;
				da = db;
			}
			polygon.swap( scratch );
		}

		[[nodiscard]] static bool clip_fire_edge( math::vector3& a, math::vector3& b,
			const math::vector3& delta, float limit )
		{
			const auto da = a.x * delta.x + a.y * delta.y - limit;
			const auto db = b.x * delta.x + b.y * delta.y - limit;
			if ( da > 0.0f && db > 0.0f ) return false;
			if ( ( da <= 0.0f ) != ( db <= 0.0f ) )
			{
				const auto intersection = a + ( b - a ) * ( da / ( da - db ) );
				if ( da > 0.0f ) a = intersection;
				else b = intersection;
			}
			return true;
		}

		[[nodiscard]] static math::vector3 fire_plane_point( const math::vector3& offset,
			const math::vector3& center, const math::vector3& normal )
		{
			return { center.x + offset.x, center.y + offset.y,
				center.z - ( normal.x * offset.x + normal.y * offset.y ) / normal.z + fire_plane_lift };
		}

		constexpr auto edge_padding{ 52.0f };
		constexpr auto icon_radius{ 18.0f };
		constexpr auto arc_radius{ 22.0f };
		constexpr auto arc_thickness{ 2.75f };
		constexpr auto segments{ 32 };
		constexpr auto arrow_length{ 11.0f };
		constexpr auto arrow_spread{ 0.45f };
		constexpr auto arrow_arc_segments{ 10 };
		constexpr auto arrow_gap{ 3.5f };
		constexpr auto arrow_base_r{ arc_radius + arrow_gap };
		constexpr auto arrow_tip_r{ arrow_base_r + arrow_length };

		constexpr auto fade_speed{ 3.0f };
		constexpr auto anchor_world_offset_z{ 80.0f };

	} // namespace detail

	void overlay::on_render( xdraw::draw_list& draw_list, xdraw::draw_list& middle_draw_list )
	{
		const auto& overlay_cfg = settings::g_esp.m_projectile.m_overlay;

		// 固定数组替代每帧 unordered_set：活跃烟雾同时存在数量
		// 远小于 8，线性查找即可，避免每帧集合节点分配。
		std::array<std::uintptr_t, 8> live_smokes{};
		auto live_smoke_count{ 0 };

		const auto live_smokes_contains = [ & ]( const std::uintptr_t ptr )
			{
				for ( auto i = 0; i < live_smoke_count; ++i )
				{
					if ( live_smokes[ i ] == ptr )
					{
						return true;
					}
				}

				return false;
			};

		const auto snapshot = systems::g_entities.snapshot( );
		for ( const auto& projectile : *snapshot )
		{
			if ( projectile.type != systems::entities::type::projectile )
			{
				continue;
			}

			if ( projectile.schema_hash == "C_Inferno"_hash )
			{
				if ( overlay_cfg.is_active( 5 ) )
				{
					this->add_inferno( draw_list, middle_draw_list, projectile, overlay_cfg.m_infernos );
				}
				continue;
			}

			const auto info = this->get_info( projectile );
			if ( !info.valid( ) )
			{
				continue;
			}

			if ( info.group_id == 2 )
			{
				if ( live_smoke_count < static_cast< int >( live_smokes.size( ) ) )
				{
					live_smokes[ live_smoke_count++ ] = projectile.ptr;
				}

				if ( overlay_cfg.is_active( 2 ) && overlay_cfg.m_smoke_areas.enabled.value )
				{
					this->add_smoke_area( draw_list, projectile, overlay_cfg.m_smoke_areas );
				}
			}

			if ( info.detonated )
			{
				continue;
			}

			const auto& cfg = overlay_cfg.get_group( info.group_id );
			if ( !overlay_cfg.is_active( info.group_id ) || info.distance > cfg.max_distance )
			{
				continue;
			}

			const auto screen = systems::g_view.project( info.origin );
			if ( !systems::g_view.projection_valid( screen ) )
			{
				continue;
			}

			this->add_label( middle_draw_list, screen, info, cfg );
		}

		std::erase_if( this->m_smoke_states, [ & ]( const auto& entry )
			{
				return !live_smokes_contains( entry.first );
			} );

		std::erase_if( this->m_inferno_states, [ & ]( const auto& entry )
			{
				return !overlay_cfg.is_active( 5 ) || std::ranges::none_of( *snapshot, [ & ]( const auto& cached )
					{ return cached.ptr == entry.first && cached.schema_hash == "C_Inferno"_hash; } );
			} );

		this->add_landing_indicators( middle_draw_list );
	}

	void overlay::add_smoke_area( xdraw::draw_list& draw_list, const systems::entities::cached& entity, const settings::esp::projectile::overlay::smoke_areas& cfg )
	{
		const auto active = memory::read<bool>( entity.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect"_hash ) )
			|| memory::read<int>( entity.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin"_hash ) ) > 0;
		if ( !active )
		{
			return;
		}

		auto origin = memory::read<math::vector3>( entity.ptr + SCHEMA( "C_SmokeGrenadeProjectile", "m_vSmokeDetonationPos"_hash ) );
		if ( origin.length_sqr( ) < 1.0f )
		{
			const auto scene_node = memory::read<std::uintptr_t>( entity.ptr + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( !scene_node )
			{
				return;
			}
			origin = memory::read<math::vector3>( scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		}

		constexpr auto radius{ 144.0f };
		constexpr auto segments{ 64 };
		constexpr auto lifetime{ 18.5f };
		constexpr auto fade_in{ 0.15f };
		constexpr auto fade_out{ 0.30f };
		const auto now = std::chrono::steady_clock::now( );
		auto& state = this->m_smoke_states[ entity.ptr ];
		if ( state.start_time == std::chrono::steady_clock::time_point{} )
		{
			state.start_time = now;
		}

		const auto elapsed = std::chrono::duration<float>( now - state.start_time ).count( );
		const auto remaining = std::max( lifetime - elapsed, 0.0f );
		auto target_alpha = elapsed < fade_in
			? elapsed / fade_in
			: ( remaining < fade_out ? remaining / fade_out : 1.0f );
		target_alpha = std::clamp( target_alpha, 0.0f, 1.0f );
		state.fade_alpha += ( target_alpha - state.fade_alpha ) * std::min( 12.0f * xdraw::delta_time( ), 1.0f );
		if ( state.fade_alpha <= 0.01f )
		{
			return;
		}

		auto& points = this->m_smoke_points_scratch;
		points.clear( );
		for ( auto i = 0; i < segments; ++i )
		{
			const auto angle = static_cast<float>( i ) * ( std::numbers::pi_v<float> * 2.0f / static_cast<float>( segments ) );
			const auto projected = systems::g_view.project( origin + math::vector3{ std::cosf( angle ) * radius, std::sinf( angle ) * radius, 0.0f } );
			if ( systems::g_view.projection_valid( projected ) )
			{
				points.push_back( projected );
			}
		}

		if ( points.size( ) < 3 )
		{
			return;
		}

		const auto span = std::span<const float>( reinterpret_cast<const float*>( points.data( ) ), points.size( ) * 2 );
		const auto& fill = cfg.fill_color.value;
		const auto& outline = cfg.outline_color.value;
		const auto fill_alpha = static_cast<std::uint8_t>( static_cast<float>( fill.a ) * state.fade_alpha );
		const auto outline_alpha = static_cast<std::uint8_t>( static_cast<float>( outline.a ) * state.fade_alpha );

		draw_list.convex_filled( span, { fill.r, fill.g, fill.b, fill_alpha } );
		draw_list.polyline( span, { outline.r, outline.g, outline.b, outline_alpha }, true, cfg.outline_thickness.value );
		if ( cfg.glow.value )
		{
			auto& glow = xdraw::get_glow( );
			const auto strength = std::clamp( cfg.glow_strength.value, 0.0f, 1.0f );
			glow.convex_filled( span, { fill.r, fill.g, fill.b, static_cast<std::uint8_t>( fill_alpha * strength ) } );
			glow.polyline( span, { outline.r, outline.g, outline.b, static_cast<std::uint8_t>( outline_alpha * strength ) }, true, cfg.outline_thickness.value + 2.0f );
		}
	}

	void overlay::add_landing_indicators( xdraw::draw_list& draw_list )
	{
		const auto now = std::chrono::steady_clock::now( );
		const auto delta_time = xdraw::delta_time( );

		const auto [screen_w, screen_h] = xdraw::viewport_size( );

		const auto sw = static_cast< float >( screen_w );
		const auto sh = static_cast< float >( screen_h );

		const auto center_x = sw * 0.5f;
		const auto center_y = sh * 0.5f;

		std::unordered_set<std::uintptr_t> seen;

		const auto& ind = settings::g_esp.m_projectile.m_overlay.m_indicator;

		for ( const auto& gren : features::misc::g_projectile_trajectory.in_flight( ) )
		{
			if ( !gren.traj.valid || gren.detonated )
			{
				continue;
			}

			const auto is_he = gren.weapon_hash == "weapon_hegrenade"_hash;
			const auto is_molotov = gren.weapon_hash == "weapon_molotov"_hash || gren.weapon_hash == "weapon_incgrenade"_hash;

			if ( !is_he && !is_molotov )
			{
				continue;
			}

			const auto group_id = is_molotov ? 1u : 0u;
			const auto& cfg = ind.get_group( group_id );

			if ( !cfg.enabled.value )
			{
				continue;
			}

			seen.insert( gren.entity );

			auto& state = this->m_indicator_states[ gren.entity ];
			state.was_active = true;
			state.fade_alpha = 1.0f;

			const auto elapsed = std::chrono::duration<float>( now - gren.throw_time ).count( );
			if ( elapsed >= gren.traj.duration )
			{
				continue;
			}

			const auto remaining = std::max( 0.0f, gren.traj.duration - elapsed );
			const auto frac = std::clamp( remaining / gren.traj.duration, 0.0f, 1.0f );

			const auto proj = systems::g_view.project_full( gren.traj.end_pos );
			const auto on_screen = proj.on_screen;

			if ( on_screen )
			{
				this->add_indicator( draw_list, proj.screen.x, proj.screen.y, 0.0f, false, frac, state.fade_alpha, is_molotov, cfg );
				continue;
			}

			auto dx = proj.screen.x - center_x;
			auto dy = proj.screen.y - center_y;

			if ( proj.w <= 0.0f )
			{
				dx = -dx;
				dy = -dy;
			}

			const auto len = std::sqrtf( dx * dx + dy * dy );
			if ( len < 1.0f )
			{
				continue;
			}

			dx /= len;
			dy /= len;

			auto t_min = std::numeric_limits<float>::max( );

			if ( std::fabsf( dx ) > 0.001f )
			{
				const auto t_left = ( detail::edge_padding - center_x ) / dx;
				const auto t_right = ( sw - detail::edge_padding - center_x ) / dx;

				if ( t_left > 0.0f ) 
				{
					t_min = std::fminf( t_min, t_left );
				}

				if ( t_right > 0.0f ) 
				{
					t_min = std::fminf( t_min, t_right );
				}
			}

			if ( std::fabsf( dy ) > 0.001f )
			{
				const auto t_top = ( detail::edge_padding - center_y ) / dy;
				const auto t_bottom = ( sh - detail::edge_padding - center_y ) / dy;

				if ( t_top > 0.0f )
				{
					t_min = std::fminf( t_min, t_top );
				}

				if ( t_bottom > 0.0f )
				{
					t_min = std::fminf( t_min, t_bottom );
				}
			}

			const auto edge_x = std::clamp( center_x + dx * t_min, detail::edge_padding, sw - detail::edge_padding );
			const auto edge_y = std::clamp( center_y + dy * t_min, detail::edge_padding, sh - detail::edge_padding );

			const auto dir_angle = std::atan2f( dy, dx );
			const auto cx = edge_x - std::cosf( dir_angle ) * detail::arrow_tip_r;
			const auto cy = edge_y - std::sinf( dir_angle ) * detail::arrow_tip_r;

			this->add_indicator( draw_list, cx, cy, dir_angle, true, frac, state.fade_alpha, is_molotov, cfg );
		}

		for ( auto it = this->m_indicator_states.begin( ); it != this->m_indicator_states.end( ); )
		{
			if ( seen.contains( it->first ) )
			{
				++it;
				continue;
			}

			it->second.fade_alpha -= detail::fade_speed * delta_time;

			if ( it->second.fade_alpha <= 0.0f )
			{
				it = this->m_indicator_states.erase( it );
			}
			else
			{
				++it;
			}
		}
	}

	void overlay::add_indicator( xdraw::draw_list& draw_list, float cx, float cy, float dir_angle, bool has_arrow, float timer_frac, float alpha, bool is_fire, const settings::esp::projectile::overlay::indicator::group& cfg )
	{
		if ( alpha <= 0.0f )
		{
			return;
		}

		constexpr auto two_pi{ std::numbers::pi_v<float> *2.0f };
		constexpr auto half_pi{ std::numbers::pi_v<float> *0.5f };

		{
			const auto& bg = cfg.background_color.value;
			const auto ba = static_cast< std::uint8_t >( static_cast< float >( bg.a ) * alpha );
			draw_list.circle_filled( cx, cy, detail::icon_radius, xdraw::color{ bg.r, bg.g, bg.b, ba }, detail::segments );
		}

		const auto& icon_col = cfg.icon_color.value;
		const auto icon_a = static_cast< std::uint8_t >( static_cast< float >( icon_col.a ) * alpha );
		const auto icon_tint = xdraw::color{ icon_col.r, icon_col.g, icon_col.b, icon_a };

		if ( is_fire )
		{
			const auto ico = detail::get_fire_icon( );
			if ( ico && ico->texture )
			{
				const auto iw = static_cast< float >( ico->width );
				const auto ih = static_cast< float >( ico->height );
				draw_list.image( std::floorf( cx - iw * 0.5f ), std::floorf( cy - ih * 0.5f ), iw, ih, ico->texture.Get( ), icon_tint );
			}
		}
		else
		{
			const auto ico = systems::g_icons.get( "hegrenade", 0.55f );
			if ( ico && ico->texture )
			{
				const auto iw = static_cast< float >( ico->width );
				const auto ih = static_cast< float >( ico->height );
				draw_list.image( std::floorf( cx - iw * 0.5f ), std::floorf( cy - ih * 0.5f ), iw, ih, ico->texture.Get( ), icon_tint );
			}
		}

		const auto arc_end = timer_frac * two_pi;

		if ( arc_end > 0.01f )
		{
			const auto& arc_col = cfg.arc_color.value;
			const auto aa = static_cast< std::uint8_t >( static_cast< float >( arc_col.a ) * alpha );
			const auto arc_tint = xdraw::color{ arc_col.r, arc_col.g, arc_col.b, aa };
			const auto arc_segs = std::max( 3, static_cast< int >( detail::segments * timer_frac ) );

			auto& arc_pts = this->m_arc_scratch;
			arc_pts.clear( );

			for ( auto i = 0; i <= arc_segs; ++i )
			{
				const auto t = static_cast< float >( i ) / static_cast< float >( arc_segs );
				const auto a = -half_pi + t * arc_end;
				arc_pts.push_back( cx + std::cosf( a ) * detail::arc_radius );
				arc_pts.push_back( cy + std::sinf( a ) * detail::arc_radius );
			}

			const auto span = std::span<const float>( arc_pts.data( ), arc_pts.size( ) );
			draw_list.polyline( span, arc_tint, false, detail::arc_thickness );

			if ( cfg.glow.value )
			{
				auto& glow = xdraw::get_glow( );
				const auto ga = static_cast< std::uint8_t >( static_cast< float >( arc_col.a ) * cfg.glow_strength * alpha );
				glow.polyline( span, xdraw::color{ arc_col.r, arc_col.g, arc_col.b, ga }, false, detail::arc_thickness + 2.0f );
			}
		}

		if ( has_arrow )
		{
			const auto tip_x = cx + std::cosf( dir_angle ) * detail::arrow_tip_r;
			const auto tip_y = cy + std::sinf( dir_angle ) * detail::arrow_tip_r;

			const auto base_left_angle = dir_angle - detail::arrow_spread;
			const auto base_right_angle = dir_angle + detail::arrow_spread;

			auto& arrow_pts = this->m_arrow_scratch;
			arrow_pts.clear( );

			arrow_pts.push_back( tip_x );
			arrow_pts.push_back( tip_y );

			arrow_pts.push_back( cx + std::cosf( base_right_angle ) * detail::arrow_base_r );
			arrow_pts.push_back( cy + std::sinf( base_right_angle ) * detail::arrow_base_r );

			for ( auto i = detail::arrow_arc_segments - 1; i >= 1; --i )
			{
				const auto t = static_cast< float >( i ) / static_cast< float >( detail::arrow_arc_segments );
				const auto a = base_left_angle + t * ( base_right_angle - base_left_angle );
				arrow_pts.push_back( cx + std::cosf( a ) * detail::arrow_base_r );
				arrow_pts.push_back( cy + std::sinf( a ) * detail::arrow_base_r );
			}

			arrow_pts.push_back( cx + std::cosf( base_left_angle ) * detail::arrow_base_r );
			arrow_pts.push_back( cy + std::sinf( base_left_angle ) * detail::arrow_base_r );

			const auto arrow_span = std::span<const float>( arrow_pts.data( ), arrow_pts.size( ) );

			const auto& arc_col = cfg.arc_color.value;
			const auto aa = static_cast< std::uint8_t >( static_cast< float >( arc_col.a ) * alpha );
			draw_list.convex_filled( arrow_span, xdraw::color{ arc_col.r, arc_col.g, arc_col.b, aa } );

			if ( cfg.glow.value )
			{
				auto& glow = xdraw::get_glow( );
				const auto ga = static_cast< std::uint8_t >( static_cast< float >( arc_col.a ) * cfg.glow_strength * alpha );
				glow.convex_filled( arrow_span, xdraw::color{ arc_col.r, arc_col.g, arc_col.b, ga } );
			}
		}
	}

	void overlay::add_inferno( xdraw::draw_list& draw_list, xdraw::draw_list& middle_draw_list, const systems::entities::cached& entity, const settings::esp::projectile::overlay::infernos& cfg )
	{
		const auto count_offset = SCHEMA( "C_Inferno", "m_fireCount"_hash );
		const auto positions_offset = SCHEMA( "C_Inferno", "m_firePositions"_hash );
		const auto burning_offset = SCHEMA( "C_Inferno", "m_bFireIsBurning"_hash );
		const auto normals_offset = SCHEMA( "C_Inferno", "m_BurnNormal"_hash );
		const auto post_offset = SCHEMA( "C_Inferno", "m_bInPostEffectTime"_hash );
		const auto tick_offset = SCHEMA( "C_Inferno", "m_nFireEffectTickBegin"_hash );
		if ( !count_offset || !positions_offset || !burning_offset || !post_offset || !tick_offset ) return;

		// Read the network arrays, never the version-dependent private drawable layout.
		const auto raw_count = memory::safe_read<int>( entity.ptr + count_offset );
		const auto positions = memory::safe_read<std::array<math::vector3, 64>>( entity.ptr + positions_offset );
		const auto burning = memory::safe_read<std::array<bool, 64>>( entity.ptr + burning_offset );
		const auto begin_tick = memory::safe_read<int>( entity.ptr + tick_offset );
		const auto post_effect = memory::safe_read<bool>( entity.ptr + post_offset );
		if ( !raw_count || !positions || !burning || !begin_tick || !post_effect || *post_effect )
		{
			this->m_inferno_states.erase( entity.ptr );
			return;
		}
		const auto normals = normals_offset
			? memory::safe_read<std::array<math::vector3, 64>>( entity.ptr + normals_offset ) : std::nullopt;
		const auto count = std::clamp( *raw_count, 0, 64 );
		auto& state = this->m_inferno_states[ entity.ptr ];
		if ( state.begin_tick != *begin_tick || state.entity_index != entity.index )
		{
			state = {};
			state.begin_tick = *begin_tick;
			state.entity_index = entity.index;
		}

		auto changed{ false };
		auto active_count{ 0 };
		math::vector3 avg_pos{};
		for ( auto i = 0; i < count; ++i )
		{
			if ( !( *burning )[ i ] || !detail::finite_fire_position( ( *positions )[ i ] ) ) continue;
			auto normal = normals ? ( *normals )[ i ] : math::vector3{ 0.0f, 0.0f, 1.0f };
			const auto normal_length = normal.length( );
			if ( !detail::finite_fire_position( normal ) || !std::isfinite( normal_length ) || normal_length < 0.1f
				|| normal.z / normal_length < 0.2f ) normal = { 0.0f, 0.0f, 1.0f };
			else normal = normal * ( 1.0f / normal_length );
			const auto& center = ( *positions )[ i ];
			auto& patch = state.patches[ active_count ];
			changed = changed || ( patch.center - center ).length_sqr( ) > 0.0f || ( patch.normal - normal ).length_sqr( ) > 0.0f;
			patch = { center, normal };
			avg_pos = avg_pos + center;
			++active_count;
		}
		if ( active_count == 0 )
		{
			this->m_inferno_states.erase( entity.ptr );
			return;
		}
		changed = changed || state.patch_count != active_count;
		state.patch_count = active_count;
		state.last_avg_pos = avg_pos * ( 1.0f / static_cast<float>( active_count ) );
		const auto delta_time = std::max( xdraw::delta_time( ), 0.0f );
		state.fade_alpha = std::min( state.fade_alpha + detail::fade_speed * delta_time, 1.0f );
		if ( state.spawn_time == std::chrono::steady_clock::time_point{} ) state.spawn_time = std::chrono::steady_clock::now( );

		if ( changed )
		{
			static const auto directions = [ ]( )
				{
					std::array<math::vector3, detail::fire_segments> result{};
					for ( auto d = 0; d < detail::fire_segments; ++d )
					{
						const auto angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>( d ) / detail::fire_segments;
						result[ d ] = { std::cosf( angle ) * detail::fire_radius, std::sinf( angle ) * detail::fire_radius, 0.0f };
					}
					return result;
				}( );
			state.surfaces.resize( active_count );
			auto& polygon = this->m_inferno_polygon_scratch;
			auto& scratch = this->m_inferno_clip_scratch;
			for ( auto i = 0; i < active_count; ++i )
			{
				auto& surface = state.surfaces[ i ];
				surface.points.clear( );
				surface.edges.clear( );
				const auto& patch = state.patches[ i ];
				struct half_plane { math::vector3 delta; float limit; };
				std::array<half_plane, 64> planes{};
				auto plane_count{ 0 };
				polygon.assign( directions.begin( ), directions.end( ) );
				for ( auto j = 0; j < active_count && !polygon.empty( ); ++j )
				{
					if ( i == j ) continue;
					const auto& other = state.patches[ j ];
					if ( !detail::same_fire_plane( patch.center, patch.normal, other.center, other.normal ) ) continue;
					const auto delta = other.center - patch.center;
					const auto distance_sq = delta.x * delta.x + delta.y * delta.y;
					if ( distance_sq >= 4.0f * detail::fire_radius * detail::fire_radius ) continue;
					if ( distance_sq < 0.0001f )
					{
						if ( j < i ) polygon.clear( ); // Coincident patches have one owner.
						continue;
					}
					planes[ plane_count++ ] = { delta, distance_sq * 0.5f };
					detail::clip_fire_cell( polygon, scratch, delta, distance_sq * 0.5f );
				}
				if ( polygon.size( ) < 3 ) continue;
				for ( const auto& point : polygon ) surface.points.push_back( detail::fire_plane_point( point, patch.center, patch.normal ) );
				// Only exposed circle edges get an outline; internal cell seams stay invisible.
				for ( auto d = 0; d < detail::fire_segments; ++d )
				{
					auto a = directions[ d ];
					auto b = directions[ ( d + 1 ) % detail::fire_segments ];
					auto visible{ true };
					for ( auto p = 0; p < plane_count && visible; ++p )
						visible = detail::clip_fire_edge( a, b, planes[ p ].delta, planes[ p ].limit );
					if ( visible ) surface.edges.emplace_back( detail::fire_plane_point( a, patch.center, patch.normal ), detail::fire_plane_point( b, patch.center, patch.normal ) );
				}
			}
		}

		if ( !systems::g_view.has_camera( ) ) return;
		const auto matrix = systems::g_view.matrix( );
		const auto [width, height] = xdraw::viewport_size( );
		const auto depth = [ & ]( const math::vector3& p )
			{ return matrix[ 3 ][ 0 ] * p.x + matrix[ 3 ][ 1 ] * p.y + matrix[ 3 ][ 2 ] * p.z + matrix[ 3 ][ 3 ]; };
		const auto project = [ & ]( const math::vector3& p ) -> math::vector2
			{
				const auto w = std::max( depth( p ), detail::fire_near_w );
				return { width * 0.5f * ( 1.0f + ( matrix[ 0 ][ 0 ] * p.x + matrix[ 0 ][ 1 ] * p.y + matrix[ 0 ][ 2 ] * p.z + matrix[ 0 ][ 3 ] ) / w ),
					height * 0.5f * ( 1.0f - ( matrix[ 1 ][ 0 ] * p.x + matrix[ 1 ][ 1 ] * p.y + matrix[ 1 ][ 2 ] * p.z + matrix[ 1 ][ 3 ] ) / w ) };
			};
		const auto& fill = cfg.fill_color.value;
		const auto& outline = cfg.outline_color.value;
		const xdraw::color fill_color{ fill.r, fill.g, fill.b, static_cast<std::uint8_t>( fill.a * state.fade_alpha ) };
		const xdraw::color outline_color{ outline.r, outline.g, outline.b, static_cast<std::uint8_t>( outline.a * state.fade_alpha ) };
		const auto glow_strength = std::clamp( cfg.glow_strength.value, 0.0f, 1.0f );
		for ( const auto& surface : state.surfaces )
		{
			if ( surface.points.empty( ) ) continue;
			// Clip before projection, including patches crossing the camera.
			auto& clipped = this->m_inferno_clip_scratch;
			clipped.clear( );
			auto a = surface.points.back( );
			auto da = depth( a ) - detail::fire_near_w;
			for ( const auto& b : surface.points )
			{
				const auto db = depth( b ) - detail::fire_near_w;
				if ( ( da >= 0.0f ) != ( db >= 0.0f ) ) clipped.push_back( a + ( b - a ) * ( da / ( da - db ) ) );
				if ( db >= 0.0f ) clipped.push_back( b );
				a = b;
				da = db;
			}
			auto& points = this->m_inferno_points_scratch;
			points.clear( );
			for ( const auto& point : clipped ) points.push_back( project( point ) );
			const auto span = std::span<const float>( reinterpret_cast<const float*>( points.data( ) ), points.size( ) * 2 );
			// No AA fringe on shared edges: adjacent cells must not darken or leave seams.
			draw_list.convex_filled( span, fill_color, false );
			if ( cfg.glow.value )
			{
				const xdraw::color glow_fill{ fill.r, fill.g, fill.b, static_cast<std::uint8_t>( fill_color.a * glow_strength ) };
				xdraw::get_glow( ).convex_filled( span, glow_fill, false );
			}
			for ( auto [edge_a, edge_b] : surface.edges )
			{
				const auto wa = depth( edge_a ) - detail::fire_near_w;
				const auto wb = depth( edge_b ) - detail::fire_near_w;
				if ( wa < 0.0f && wb < 0.0f ) continue;
				if ( ( wa >= 0.0f ) != ( wb >= 0.0f ) )
				{
					const auto intersection = edge_a + ( edge_b - edge_a ) * ( wa / ( wa - wb ) );
					if ( wa < 0.0f ) edge_a = intersection;
					else edge_b = intersection;
				}
				const auto screen_a = project( edge_a );
				const auto screen_b = project( edge_b );
				draw_list.line( screen_a.x, screen_a.y, screen_b.x, screen_b.y, outline_color, cfg.outline_thickness.value );
				if ( cfg.glow.value )
				{
					const xdraw::color glow_color{ outline.r, outline.g, outline.b, static_cast<std::uint8_t>( outline_color.a * glow_strength ) };
					xdraw::get_glow( ).line( screen_a.x, screen_a.y, screen_b.x, screen_b.y, glow_color, cfg.outline_thickness.value + 2.0f );
				}
			}
		}

		const auto& ind_cfg = settings::g_esp.m_projectile.m_overlay.m_indicator.get_group( 2 );
		if ( ind_cfg.enabled.value )
		{
			if ( state.last_avg_pos.length_sqr( ) > 0.0f )
			{
				const auto indicator_pos = state.last_avg_pos;
				const auto lifetime_offset = SCHEMA( "C_Inferno", "m_nFireLifetime"_hash );
				const auto fire_lifetime = lifetime_offset ? memory::safe_read<float>( entity.ptr + lifetime_offset ).value_or( 0.0f ) : 0.0f;
				const auto elapsed = std::chrono::duration<float>( std::chrono::steady_clock::now( ) - state.spawn_time ).count( );
				const auto remaining = std::max( 0.0f, fire_lifetime - elapsed );
				const auto frac = std::isfinite( fire_lifetime ) && fire_lifetime > 0.0f
					? std::clamp( remaining / fire_lifetime, 0.0f, 1.0f ) : 0.0f;

				const auto target_proj = systems::g_view.project_full( indicator_pos );
				const auto anchor_proj = systems::g_view.project_full( indicator_pos + math::vector3{ 0.0f, 0.0f, detail::anchor_world_offset_z } );

				const auto [screen_w, screen_h] = xdraw::viewport_size( );
				const auto sw = static_cast< float >( screen_w );
				const auto sh = static_cast< float >( screen_h );
				const auto center_x = sw * 0.5f;
				const auto center_y = sh * 0.5f;

				constexpr auto half_pi{ std::numbers::pi_v<float> *0.5f };

				float icx{}, icy{};
				float dir_angle{};

				if ( anchor_proj.on_screen && target_proj.w > 0.0f )
				{
					icx = anchor_proj.screen.x;
					icy = anchor_proj.screen.y;

					const auto dx = target_proj.screen.x - icx;
					const auto dy = target_proj.screen.y - icy;
					const auto len = std::sqrtf( dx * dx + dy * dy );

					dir_angle = len > 0.1f ? std::atan2f( dy, dx ) : half_pi;
				}
				else
				{
					auto dx = target_proj.screen.x - center_x;
					auto dy = target_proj.screen.y - center_y;

					if ( target_proj.w <= 0.0f )
					{
						dx = -dx;
						dy = -dy;
					}

					const auto len = std::sqrtf( dx * dx + dy * dy );
					if ( len >= 1.0f )
					{
						dx /= len;
						dy /= len;

						auto t_min = std::numeric_limits<float>::max( );

						if ( std::fabsf( dx ) > 0.001f )
						{
							const auto t_left = ( detail::edge_padding - center_x ) / dx;
							const auto t_right = ( sw - detail::edge_padding - center_x ) / dx;

							if ( t_left > 0.0f )
							{
								t_min = std::fminf( t_min, t_left );
							}

							if ( t_right > 0.0f ) 
							{
								t_min = std::fminf( t_min, t_right );
							}
						}

						if ( std::fabsf( dy ) > 0.001f )
						{
							const auto t_top = ( detail::edge_padding - center_y ) / dy;
							const auto t_bottom = ( sh - detail::edge_padding - center_y ) / dy;

							if ( t_top > 0.0f ) 
							{
								t_min = std::fminf( t_min, t_top );
							}

							if ( t_bottom > 0.0f ) 
							{
								t_min = std::fminf( t_min, t_bottom );
							}
						}

						const auto edge_x = std::clamp( center_x + dx * t_min, detail::edge_padding, sw - detail::edge_padding );
						const auto edge_y = std::clamp( center_y + dy * t_min, detail::edge_padding, sh - detail::edge_padding );

						dir_angle = std::atan2f( dy, dx );
						icx = edge_x - std::cosf( dir_angle ) * detail::arrow_tip_r;
						icy = edge_y - std::sinf( dir_angle ) * detail::arrow_tip_r;

						this->add_indicator( middle_draw_list, icx, icy, dir_angle, true, frac, state.fade_alpha, true, ind_cfg );
					}

					return;
				}

				this->add_indicator( middle_draw_list, icx, icy, dir_angle, true, frac, state.fade_alpha, true, ind_cfg );
			}
		}
	}

	void overlay::add_label( xdraw::draw_list& draw_list, const math::vector2& screen, const info& info, const settings::esp::projectile::overlay::group& cfg )
	{
		static constexpr const char* k_names[ ]{ "he", "flash", "smoke", "molotov", "decoy" };
		static constexpr std::uint32_t k_hashes[ ]{ "C_HEGrenadeProjectile"_hash, "C_FlashbangProjectile"_hash, "C_SmokeGrenadeProjectile"_hash, "C_MolotovProjectile"_hash, "C_DecoyProjectile"_hash };

		const auto show_icon = cfg.display == settings::esp::projectile::overlay::group::display_type::icon || cfg.display == settings::esp::projectile::overlay::group::display_type::text_and_icon;
		const auto show_text = cfg.display == settings::esp::projectile::overlay::group::display_type::text || cfg.display == settings::esp::projectile::overlay::group::display_type::text_and_icon;
		auto y = screen.y;

		if ( show_icon )
		{
			const auto ico = systems::g_icons.get( k_hashes[ info.group_id ], 0.35f );
			if ( ico && ico->texture )
			{
				const auto iw = static_cast< float >( ico->width );
				const auto ih = static_cast< float >( ico->height );
				const auto ix = std::floorf( screen.x - iw * 0.5f );
				const auto iy = std::floorf( y - ih * 0.5f );

				constexpr auto outline{ xdraw::color{ 0, 0, 0, 255 } };

				draw_list.image( ix - 1.0f, iy, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix + 1.0f, iy, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy - 1.0f, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy + 1.0f, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy, iw, ih, ico->texture.Get( ), cfg.icon_color );

				y += ih * 0.5f + 1.0f;
			}
		}

		if ( show_text )
		{
			xdraw::push_font( rendering::g_fonts.smallest_pixel7[ rendering::fonts::size::normal ] );

			const auto name = k_names[ info.group_id ];
			const auto [w, h] = xdraw::measure_text( name );
			draw_list.text( std::floorf( screen.x - w * 0.5f ), std::floorf( y ), name, cfg.text_color, xdraw::text_style::outlined );

			xdraw::pop_font( );
		}
	}

	overlay::info overlay::get_info( const systems::entities::cached& entity )
	{
		info info{};
		info.entity = entity.ptr;
		info.schema_hash = entity.schema_hash;
		info.group_id = this->get_projectile_group( entity.schema_hash );

		if ( !info.entity || info.group_id == UINT32_MAX )
		{
			return info;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( info.entity + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			info.entity = 0;
			return info;
		}

		info.origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		info.distance = systems::g_view.origin( ).distance( info.origin ) * 0.01905f;

		if ( info.group_id == 0 || info.group_id == 1 )
		{
			const auto explode_tick = memory::read<int>( info.entity + SCHEMA( "C_BaseCSGrenadeProjectile", "m_nExplodeEffectTickBegin"_hash ) );
			info.detonated = explode_tick > 0;
		}
		else if ( info.group_id == 2 )
		{
			info.effect_tick_begin = memory::read<int>( info.entity + SCHEMA( "C_SmokeGrenadeProjectile", "m_nSmokeEffectTickBegin"_hash ) );
			info.smoke_active = memory::read<bool>( info.entity + SCHEMA( "C_SmokeGrenadeProjectile", "m_bDidSmokeEffect"_hash ) );
		}
		else if ( info.group_id == 4 )
		{
			info.effect_tick_begin = memory::read<int>( info.entity + SCHEMA( "C_DecoyProjectile", "m_nDecoyShotTick"_hash ) );
		}

		return info;
	}

	std::uint32_t overlay::get_projectile_group( std::uint32_t schema_hash )
	{
		switch ( schema_hash )
		{
		case "C_HEGrenadeProjectile"_hash:    return 0;
		case "C_FlashbangProjectile"_hash:    return 1;
		case "C_SmokeGrenadeProjectile"_hash: return 2;
		case "C_MolotovProjectile"_hash:      return 3;
		case "C_DecoyProjectile"_hash:        return 4;
		default:                              return UINT32_MAX;
		}
	}

} // namespace features::esp::projectile
