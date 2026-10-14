#include <pch/pch.hpp>
#include <utilities/math/math.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <core/resources/images/vagal_watermark_logo.hpp>
#include <core/resources/images/deepseek_character.hpp>

#include "../rendering.hpp"
#include <utilities/security/security.hpp>

namespace rendering {
	namespace {
		struct widget_interaction_state
		{
			std::uintptr_t dragging{};
			std::uintptr_t resizing{};
			float drag_offset_x{};
			float drag_offset_y{};
		};

		widget_interaction_state& interaction_state( )
		{
			static widget_interaction_state state{};
			return state;
		}

		struct scoped_widget_font
		{
			explicit scoped_widget_font( float scale )
			{
				const auto size = scale >= 1.22f ? fonts::size::big : scale <= 0.86f ? fonts::size::petite : fonts::size::normal;
				xdraw::push_font( g_fonts.inter_bold[ size ] );
			}
			~scoped_widget_font( ) { xdraw::pop_font( ); }
		};

		void interact_widget( xdraw::draw_list& dl, std::string_view name, float& x, float& y, float w, float h, float& scale )
		{
			const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
			x = std::clamp( x, 0.0f, std::max( 0.0f, static_cast<float>( screen_w ) - w ) );
			y = std::clamp( y, 0.0f, std::max( 0.0f, static_cast<float>( screen_h ) - h ) );

			if ( !g_menu.is_open( ) ) return;

			auto& state = interaction_state( );
			const auto& input = xui::ctx( ).input;
			const auto id = xui::fnv1a( name );
			const auto grip_size = std::clamp( 12.0f * scale, 10.0f, 18.0f );
			const xui::rect bounds{ x, y, w, h };
			const xui::rect grip{ x + w - grip_size, y + h - grip_size, grip_size, grip_size };

			if ( input.mouse_released )
			{
				if ( state.dragging == id ) state.dragging = 0;
				if ( state.resizing == id ) state.resizing = 0;
			}

			if ( input.mouse_clicked && state.dragging == 0 && state.resizing == 0 )
			{
				if ( input.in_rect( grip ) )
				{
					state.resizing = id;
				}
				else if ( input.in_rect( bounds ) )
				{
					state.dragging = id;
					state.drag_offset_x = input.mouse_x - x;
					state.drag_offset_y = input.mouse_y - y;
				}
			}

			if ( state.dragging == id && input.mouse_down )
			{
				x = input.mouse_x - state.drag_offset_x;
				y = input.mouse_y - state.drag_offset_y;
			}
			else if ( state.resizing == id && input.mouse_down )
			{
				scale = std::clamp( scale + ( input.mouse_delta_x( ) + input.mouse_delta_y( ) ) * 0.0035f, 0.72f, 1.8f );
			}

			x = std::clamp( x, 0.0f, std::max( 0.0f, static_cast<float>( screen_w ) - w ) );
			y = std::clamp( y, 0.0f, std::max( 0.0f, static_cast<float>( screen_h ) - h ) );

		}

		void draw_widget_grip( xdraw::draw_list& dl, std::string_view name, float x, float y, float w, float h, float scale )
		{
			if ( !g_menu.is_open( ) ) return;
			const auto& input = xui::ctx( ).input;
			const auto id = xui::fnv1a( name );
			const auto grip_size = std::clamp( 12.0f * scale, 10.0f, 18.0f );
			const xui::rect grip{ x + w - grip_size, y + h - grip_size, grip_size, grip_size };
			const auto active = interaction_state( ).resizing == id;
			const auto grip_col = xui::ctx( ).style.accent.alpha( input.in_rect( grip ) || active ? 220 : 110 );
			dl.line( x + w - grip_size * 0.65f, y + h - 3.0f, x + w - 3.0f, y + h - grip_size * 0.65f, grip_col, 1.0f );
			dl.line( x + w - grip_size * 0.38f, y + h - 3.0f, x + w - 3.0f, y + h - grip_size * 0.38f, grip_col, 1.0f );
		}

		template <typename config_t>
		void draw_progress_indicator( xdraw::draw_list& dl, std::string_view id, std::string_view label, config_t& cfg, float progress, bool crouched )
		{
			const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
			const auto& style = xui::ctx( ).style;
			cfg.scale.value = std::clamp( cfg.scale.value, 0.72f, 1.8f );
			const auto scale = cfg.scale.value;
			const scoped_widget_font font{ scale };
			const auto h = 28.0f * scale;
			const auto w = 126.0f * scale;
			const auto pad = 6.0f * scale;
			const auto radius = 8.0f * scale;
			const auto icon_cx = 18.0f * scale;
			const auto icon_cy = h * 0.5f;
			const auto track_x = 33.0f * scale;
			const auto track_h = 5.0f * scale;
			const auto clamped_progress = std::clamp( progress, 0.0f, 1.0f );

			if ( cfg.position_x.value < 0.0f ) cfg.position_x = ( static_cast<float>( screen_w ) - w ) * 0.5f;
			if ( cfg.position_y.value < 0.0f ) cfg.position_y = std::clamp( static_cast<float>( screen_h ) - cfg.bottom_offset.value, 12.0f, static_cast<float>( screen_h ) - h );
			auto x = cfg.position_x.value;
			auto y = cfg.position_y.value;
			interact_widget( dl, id, x, y, w, h, cfg.scale.value );
			cfg.position_x = x;
			cfg.position_y = y;

			const auto fill = xui::lerp( cfg.color.value, cfg.active_color.value, clamped_progress );
			dl.rect_filled_blurred( x, y, w, h, xdraw::corner_radius{ radius }, fill.alpha( 70 ) );
			dl.rect_filled( x, y, w, h, style.window_bg.alpha( 205 ), xdraw::corner_radius{ radius } );
			dl.circle_filled( x + icon_cx, y + icon_cy, 9.0f * scale, style.child_bg.alpha( 230 ) );

			const auto icon = fill;
			if ( crouched )
			{
				dl.circle_filled( x + icon_cx, y + icon_cy - 4.0f * scale, 2.1f * scale, icon );
				dl.line( x + icon_cx, y + icon_cy - 1.5f * scale, x + icon_cx + 4.5f * scale, y + icon_cy + 2.5f * scale, icon, 1.7f * scale );
				dl.line( x + icon_cx + 4.5f * scale, y + icon_cy + 2.5f * scale, x + icon_cx + 7.0f * scale, y + icon_cy + 2.5f * scale, icon, 1.7f * scale );
				dl.line( x + icon_cx + 1.5f * scale, y + icon_cy + 1.0f * scale, x + icon_cx - 4.5f * scale, y + icon_cy + 5.0f * scale, icon, 1.7f * scale );
			}
			else
			{
				dl.circle_filled( x + icon_cx, y + icon_cy - 4.0f * scale, 2.1f * scale, icon );
				dl.line( x + icon_cx, y + icon_cy - 1.5f * scale, x + icon_cx, y + icon_cy + 5.5f * scale, icon, 1.7f * scale );
				dl.line( x + icon_cx, y + icon_cy + 1.0f * scale, x + icon_cx - 4.0f * scale, y + icon_cy - 1.0f * scale, icon, 1.7f * scale );
				dl.line( x + icon_cx, y + icon_cy + 1.0f * scale, x + icon_cx + 4.0f * scale, y + icon_cy - 1.0f * scale, icon, 1.7f * scale );
			}

			const auto translated = xui::translate( label );
			const auto [ text_w, text_h ] = xdraw::measure_text( translated );
			dl.text( x + track_x, y + ( h - text_h ) * 0.5f + 0.5f, translated, clamped_progress > 0.0f ? fill : style.text_dim );
			const auto bar_x = x + track_x + text_w + 7.0f * scale;
			const auto bar_w = std::max( 12.0f * scale, x + w - pad - bar_x );
			const auto bar_y = y + ( h - track_h ) * 0.5f;
			dl.rect_filled( bar_x, bar_y, bar_w, track_h, style.child_bg.alpha( 215 ), xdraw::corner_radius{ track_h * 0.5f } );
			dl.rect_filled( bar_x, bar_y, bar_w * clamped_progress, track_h, fill, xdraw::corner_radius{ track_h * 0.5f } );
			draw_widget_grip( dl, id, x, y, w, h, scale );
		}

		enum class value_indicator_icon : std::uint8_t
		{
			scope,
			weapon
		};

		template <typename config_t>
		void draw_value_indicator( xdraw::draw_list& dl, std::string_view id, std::string_view label, int value, config_t& cfg, value_indicator_icon icon_type )
		{
			const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
			const auto& style = xui::ctx( ).style;
			cfg.scale.value = std::clamp( cfg.scale.value, 0.72f, 1.8f );
			const auto scale = cfg.scale.value;
			const scoped_widget_font font{ scale };
			const auto w = 104.0f * scale;
			const auto h = 28.0f * scale;
			const auto radius = 8.0f * scale;
			const auto icon_x = 18.0f * scale;
			const auto icon_y = h * 0.5f;

			if ( cfg.position_x.value < 0.0f ) cfg.position_x = ( static_cast<float>( screen_w ) - w ) * 0.5f;
			if ( cfg.position_y.value < 0.0f ) cfg.position_y = std::clamp( static_cast<float>( screen_h ) - cfg.bottom_offset.value, 12.0f, static_cast<float>( screen_h ) - h );
			auto x = cfg.position_x.value;
			auto y = cfg.position_y.value;
			interact_widget( dl, id, x, y, w, h, cfg.scale.value );
			cfg.position_x = x;
			cfg.position_y = y;

			dl.rect_filled_blurred( x, y, w, h, xdraw::corner_radius{ radius }, cfg.color.value.alpha( 72 ) );
			dl.rect_filled( x, y, w, h, style.window_bg.alpha( 205 ), xdraw::corner_radius{ radius } );
			dl.circle_filled( x + icon_x, y + icon_y, 9.0f * scale, style.child_bg.alpha( 230 ) );

			if ( icon_type == value_indicator_icon::scope )
			{
				dl.circle( x + icon_x, y + icon_y, 5.0f * scale, cfg.color.value, 1.5f * scale, 20 );
				dl.line( x + icon_x - 7.0f * scale, y + icon_y, x + icon_x - 3.0f * scale, y + icon_y, cfg.color.value, 1.4f * scale );
				dl.line( x + icon_x + 3.0f * scale, y + icon_y, x + icon_x + 7.0f * scale, y + icon_y, cfg.color.value, 1.4f * scale );
				dl.line( x + icon_x, y + icon_y - 7.0f * scale, x + icon_x, y + icon_y - 3.0f * scale, cfg.color.value, 1.4f * scale );
				dl.line( x + icon_x, y + icon_y + 3.0f * scale, x + icon_x, y + icon_y + 7.0f * scale, cfg.color.value, 1.4f * scale );
			}
			else
			{
				dl.rect_filled( x + icon_x - 6.0f * scale, y + icon_y - 3.0f * scale, 10.0f * scale, 4.0f * scale, cfg.color.value, xdraw::corner_radius{ 1.0f * scale } );
				dl.line( x + icon_x + 3.0f * scale, y + icon_y - 2.0f * scale, x + icon_x + 7.0f * scale, y + icon_y - 2.0f * scale, cfg.color.value, 1.7f * scale );
				dl.line( x + icon_x - 1.0f * scale, y + icon_y + 1.0f * scale, x + icon_x + 1.0f * scale, y + icon_y + 6.0f * scale, cfg.color.value, 2.0f * scale );
				dl.line( x + icon_x + 1.0f * scale, y + icon_y + 5.0f * scale, x + icon_x + 4.0f * scale, y + icon_y + 5.0f * scale, cfg.color.value, 1.5f * scale );
			}

			char text[ 20 ]{};
			std::snprintf( text, sizeof( text ), "%.*s %d", static_cast<int>( label.size( ) ), label.data( ), value );
			const auto [ text_w, text_h ] = xdraw::measure_text( text );
			dl.text( x + 34.0f * scale, y + ( h - text_h ) * 0.5f + 0.5f, text, style.text );
			draw_widget_grip( dl, id, x, y, w, h, scale );
		}
	}

	void widgets::draw( )
	{
		auto& dl = xdraw::get( );
		features::misc::g_impacts.decay_local_hurt_progress( std::clamp( xdraw::delta_time( ), 0.0f, 0.1f ) );

		if ( settings::g_misc.m_watermark.enabled.value )
		{
			this->watermark( dl );
		}

		if ( settings::g_misc.m_deepseek_character.enabled.value )
		{
			this->deepseek_character( dl );
		}

		const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
		const auto stack_x = 14.0f;
		auto& widget_cfg = settings::g_misc.m_widgets;
		widget_cfg.indicator_stack_y.value = std::clamp( widget_cfg.indicator_stack_y.value, 40.0f, std::max( 40.0f, static_cast<float>( screen_h ) - 220.0f ) );
		auto stack_y = widget_cfg.indicator_stack_y.value;
		auto place_indicator = [ & ]( auto& cfg )
		{
			cfg.position_x = stack_x;
			cfg.position_y = stack_y;
			stack_y += 28.0f * std::clamp( cfg.scale.value, 0.72f, 1.8f ) + 6.0f;
		};

		const auto combat_indicators = widget_cfg.combat_indicators_enabled.value;
		if ( combat_indicators
			&& settings::g_combat.m_duckpeek.enabled.value
			&& settings::g_misc.m_hud.m_duck_indicator.enabled.value )
		{
			place_indicator( settings::g_misc.m_hud.m_duck_indicator );
			this->duck_indicator( dl );
		}

		if ( settings::g_misc.m_hud.m_slow_indicator.enabled.value && ( features::misc::g_impacts.local_hurt_progress( ) > 0.0f || g_menu.is_open( ) ) )
		{
			place_indicator( settings::g_misc.m_hud.m_slow_indicator );
			this->slow_indicator( dl );
		}

		if ( combat_indicators && settings::g_misc.m_hud.m_hitchance_indicator.enabled.value )
		{
			place_indicator( settings::g_misc.m_hud.m_hitchance_indicator );
			this->hitchance_indicator( dl );
		}

		if ( combat_indicators && settings::g_misc.m_hud.m_min_damage_indicator.enabled.value )
		{
			place_indicator( settings::g_misc.m_hud.m_min_damage_indicator );
			this->min_damage_indicator( dl );
		}

		constexpr auto low_fps_threshold{ 100.0f };
		if ( settings::g_misc.m_hud.m_low_fps_indicator.enabled.value && ( xdraw::framerate( ) < low_fps_threshold || g_menu.is_open( ) ) )
		{
			place_indicator( settings::g_misc.m_hud.m_low_fps_indicator );
			this->low_fps_indicator( dl );
		}

		if ( settings::g_misc.m_widgets.keybinds_enabled.value )
		{
			this->keybinds( dl );
		}
	}

	void widgets::interact_external( xdraw::draw_list& draw_list, std::string_view id, float& x, float& y, float w, float h, float& scale )
	{
		interact_widget( draw_list, id, x, y, w, h, scale );
	}

	void widgets::draw_external_grip( xdraw::draw_list& draw_list, std::string_view id, float x, float y, float w, float h, float scale )
	{
		draw_widget_grip( draw_list, id, x, y, w, h, scale );
	}

	void widgets::watermark( xdraw::draw_list& draw_list )
	{
		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto& s  = xui::ctx( ).style;
		auto& wm = settings::g_misc.m_watermark;
		wm.scale.value = std::clamp( wm.scale.value, 0.72f, 1.8f );
		const auto scale = wm.scale.value;
		const scoped_widget_font font{ scale };
		const auto framerate = xdraw::framerate( );
		const auto local = systems::g_local.get( );

		const auto h = 28.0f * scale;
		const auto margin = 10.0f * scale;
		const auto r = 8.0f * scale;
		const auto inner_r = 6.0f * scale;
		const auto inner_pad = 2.0f * scale;
		const auto text_pad_x = 8.0f * scale;
		constexpr auto text_nudge{ 0.5f };
		const auto section_spacing = 2.0f * scale;
		const auto logo_icon_size = 16.0f * scale;
		const auto logo_icon_pad = 7.0f * scale;

		// ── account link state ──────────────────────────────────────────────
		// 服务器失联（含启动即无注入器映射）：整个水印替换为单个
		// "connect server..." pill，其余信息全部隐藏；复验恢复后自动回来。
		const auto account_data = systems::g_account.get( );
		if ( account_data.status != systems::account::state::linked )
		{
			constexpr std::string_view offline{ "connect server..." };
			const auto [offline_w, offline_h] = xdraw::measure_text( offline );
			const auto offline_pill_w = offline_w + text_pad_x * 2.0f + inner_pad * 2.0f;

			auto offline_x = wm.position_x.value;
			auto offline_y = wm.position_y.value;
			if ( wm.position_x.value < 0.0f ) offline_x = static_cast<float>( screen_w ) - offline_pill_w - margin;
			interact_widget( draw_list, "watermark_widget", offline_x, offline_y, offline_pill_w, h, wm.scale.value );
			wm.position_x = offline_x;
			wm.position_y = offline_y;

			draw_list.rect_filled_blurred( offline_x, offline_y, offline_pill_w, h, xdraw::corner_radius{ r } );
			draw_list.rect_filled( offline_x, offline_y, offline_pill_w, h, s.window_bg, xdraw::corner_radius{ r } );
			draw_list.rect_filled( offline_x + r, offline_y, offline_pill_w - r * 2.0f, 2.0f, s.accent, {} );
			draw_list.text( offline_x + inner_pad + text_pad_x, offline_y + ( h - offline_h ) * 0.5f + text_nudge, offline, s.accent );
			return;
		}

		// ── time ────────────────────────────────────────────────────────────
		SYSTEMTIME st{};
		GetLocalTime( &st );
		char time_buf[ 8 ]{};
		std::snprintf( time_buf, sizeof( time_buf ), "%02d:%02d", st.wHour, st.wMinute );

		// ── fps ─────────────────────────────────────────────────────────────
		static auto smoothed_fps{ 0.0f };
		if ( smoothed_fps == 0.0f ) smoothed_fps = framerate;
		smoothed_fps += ( framerate - smoothed_fps ) * std::min( 2.0f * xdraw::delta_time( ), 1.0f );
		char fps_val[ 8 ]{};
		std::snprintf( fps_val, sizeof( fps_val ), "%.0f", smoothed_fps );

		// ── ping ────────────────────────────────────────────────────────────
		auto ping{ 0 };
		if ( local.is_alive && local.controller && systems::g_entities.exists( local.controller ) )
			ping = memory::read<std::uint32_t>( local.controller + SCHEMA( "CCSPlayerController", "m_iPing"_hash ) );
		char ping_val[ 8 ]{};
		std::snprintf( ping_val, sizeof( ping_val ), "%d", ping );

		// ── map name (stored reliably from level_initialization hook) ────────
		const bool has_map = wm.show_map.value && !s_map_name.empty( );

		// ── tick rate (measured from server_tick delta over ~2 s of real time) ─
		static auto last_server_tick{ 0 };
		static auto last_curtime{ 0.0f };
		static auto measured_tickrate{ 0 };

		if ( local.controller )
		{
			const auto net_for_tick = addresses::globals::network_client_service;
			const auto tick_state   = net_for_tick ? memory::call_vfunc<std::uintptr_t>( net_for_tick, 23 ) : 0;
			const auto server_tick  = tick_state   ? memory::read<int>( tick_state + 892 ) : 0;
			const auto gv           = memory::read<std::uintptr_t>( addresses::globals::global_vars );
			const auto curtime      = gv ? memory::read<float>( gv + 0x30 ) : 0.0f;

			if ( server_tick > 0 && last_server_tick > 0 && curtime - last_curtime >= 2.0f )
			{
				const auto tick_delta = server_tick - last_server_tick;
				const auto time_delta = curtime - last_curtime;
				if ( tick_delta > 0 && time_delta > 0.5f )
				{
					const auto rate = static_cast<int>( std::round( tick_delta / time_delta ) );
					if ( rate >= 16 && rate <= 256 ) measured_tickrate = rate;
				}
				last_server_tick = server_tick;
				last_curtime     = curtime;
			}
			else if ( last_server_tick == 0 && server_tick > 0 )
			{
				last_server_tick = server_tick;
				last_curtime     = curtime;
			}
		}
		else
		{
			last_server_tick = 0;
			last_curtime     = 0.0f;
			measured_tickrate = 0;
		}

		const bool has_tick = wm.show_tick.value && local.controller && measured_tickrate > 0;
		char tick_val[ 8 ]{};
		if ( has_tick ) std::snprintf( tick_val, sizeof( tick_val ), "%d", measured_tickrate );

		// ── velocity ──────────────────────────────────────────────────────
		const bool has_velocity = wm.show_velocity.value && local.is_alive && local.pawn;
		static auto smoothed_velocity{ 0.0f };
		char vel_val[ 8 ]{};
		if ( has_velocity )
		{
			const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
			const auto speed = velocity.length_2d( );
			smoothed_velocity += ( speed - smoothed_velocity ) * std::min( 8.0f * xdraw::delta_time( ), 1.0f );
			std::snprintf( vel_val, sizeof( vel_val ), "%.0f", smoothed_velocity );
		}
		else
		{
			smoothed_velocity = 0.0f;
		}


		// ── logo ────────────────────────────────────────────────────────────
		static const auto logo = xdraw::load_texture(
			std::as_bytes( std::span{ resources::images::vagal_watermark_logo::data } ) );

		const auto inner_h     = h - inner_pad * 2.0f;
		const auto logo_draw_w = logo_icon_size;

		// ── measure text ─────────────────────────────────────────────────────
		constexpr std::string_view brand{ "Vagal" };
		const auto [name_tw, name_th] = xdraw::measure_text( brand );
		const std::string username = account_data.username.empty( ) ? std::string{ "Vagal" } : account_data.username;
		const auto [user_tw, user_th] = xdraw::measure_text( username );
		const auto [ping_vw, ping_vh] = xdraw::measure_text( ping_val );
		const auto [ping_uw, ping_uh] = xdraw::measure_text( " ms" );
		const auto [fps_vw,  fps_vh]  = xdraw::measure_text( fps_val );
		const auto [fps_uw,  fps_uh]  = xdraw::measure_text( " fps" );
		const auto [time_tw, time_th] = xdraw::measure_text( time_buf );

		float map_tw{}, map_th{};
		if ( has_map ) std::tie( map_tw, map_th ) = xdraw::measure_text( s_map_name.c_str( ) );

		float tick_vw{}, tick_vh{}, tick_uw{}, tick_uh{};
		if ( has_tick )
		{
			std::tie( tick_vw, tick_vh ) = xdraw::measure_text( tick_val );
			std::tie( tick_uw, tick_uh ) = xdraw::measure_text( " tick" );
		}

		float vel_vw{}, vel_vh{}, vel_uw{}, vel_uh{};
		if ( has_velocity )
		{
			std::tie( vel_vw, vel_vh ) = xdraw::measure_text( vel_val );
			std::tie( vel_uw, vel_uh ) = xdraw::measure_text( " u/s" );
		}


		// ── pill widths ──────────────────────────────────────────────────────
		const auto logo_pill_w = logo_icon_pad + logo_draw_w + logo_icon_pad + name_tw + text_pad_x;
		const auto user_pill_w = user_tw + text_pad_x * 2.0f;
		const auto ping_pill_w = ping_vw + ping_uw + text_pad_x * 2.0f;
		const auto fps_pill_w  = fps_vw  + fps_uw  + text_pad_x * 2.0f;
		const auto time_pill_w = time_tw + text_pad_x * 2.0f;
		const auto map_pill_w  = map_tw  + text_pad_x * 2.0f;
		const auto tick_pill_w = tick_vw + tick_uw + text_pad_x * 2.0f;
		const auto vel_pill_w  = vel_vw + vel_uw + text_pad_x * 2.0f;

		// ── dynamic total width ──────────────────────────────────────────────
		float target_w = inner_pad + logo_pill_w + section_spacing;
		if ( wm.show_user.value ) target_w += user_pill_w + section_spacing;
		if ( has_map )            target_w += map_pill_w  + section_spacing;
		if ( wm.show_ping.value ) target_w += ping_pill_w + section_spacing;
		if ( has_velocity )       target_w += vel_pill_w  + section_spacing;
		if ( wm.show_fps.value )  target_w += fps_pill_w  + section_spacing;
		if ( has_tick )           target_w += tick_pill_w + section_spacing;
		if ( wm.show_time.value ) target_w += time_pill_w + section_spacing;
		target_w = target_w - section_spacing + inner_pad;

		static auto smoothed_w{ 0.0f };
		if ( smoothed_w == 0.0f ) smoothed_w = target_w;
		smoothed_w += ( target_w - smoothed_w ) * std::min( 8.0f * xdraw::delta_time( ), 1.0f );

		const auto w = smoothed_w;
		if ( wm.position_x.value < 0.0f ) wm.position_x = static_cast<float>( screen_w ) - w - margin;
		auto x = wm.position_x.value;
		auto y = wm.position_y.value;
		interact_widget( draw_list, "watermark_widget", x, y, w, h, wm.scale.value );
		wm.position_x = x;
		wm.position_y = y;

		draw_list.rect_filled_blurred( x, y, w, h, xdraw::corner_radius{ r } );
		draw_list.rect_filled( x, y, w, h, s.window_bg, xdraw::corner_radius{ r } );
		draw_list.rect_filled( x + r, y, w - r * 2.0f, 2.0f, s.accent, {} );

		auto cx = x + inner_pad;

		auto draw_split_pill = [ & ]( const char* value, float vw, float vh, const char* unit, float uw, float uh, float pill_w )
			{
				draw_list.rect_filled( cx, y + inner_pad, pill_w, inner_h, s.child_bg, xdraw::corner_radius{ inner_r } );
				draw_list.text( cx + text_pad_x, y + ( h - vh ) * 0.5f + text_nudge, value, s.accent );
				draw_list.text( cx + text_pad_x + vw, y + ( h - uh ) * 0.5f + text_nudge, unit, s.text_dim );
				cx += pill_w + section_spacing;
			};

		auto draw_pill = [ & ]( const char* text, float tw, float th, float pill_w )
			{
				draw_list.rect_filled( cx, y + inner_pad, pill_w, inner_h, s.child_bg, xdraw::corner_radius{ inner_r } );
				draw_list.text( cx + text_pad_x, y + ( h - th ) * 0.5f + text_nudge, text, s.accent );
				cx += pill_w + section_spacing;
			};

		// logo pill (always shown)
		draw_list.rect_filled( cx, y + inner_pad, logo_pill_w, inner_h, s.accent, xdraw::corner_radius{ inner_r } );
		if ( logo )
			draw_list.image( cx + logo_icon_pad, y + ( h - logo_icon_size ) * 0.5f,
				logo_icon_size, logo_icon_size, logo.Get( ), s.checkbox_mark_icon );
		draw_list.text( cx + logo_icon_pad + logo_draw_w + logo_icon_pad,
			y + ( h - name_th ) * 0.5f + text_nudge, brand, s.checkbox_mark_icon );
		cx += logo_pill_w + section_spacing;

		if ( wm.show_user.value ) draw_pill( username.c_str( ), user_tw, user_th, user_pill_w );
		if ( has_map )            draw_pill( s_map_name.c_str( ), map_tw, map_th, map_pill_w );
		if ( wm.show_ping.value ) draw_split_pill( ping_val, ping_vw, ping_vh, " ms",   ping_uw, ping_uh, ping_pill_w );
		if ( has_velocity )       draw_split_pill( vel_val,  vel_vw,  vel_vh,  " u/s",  vel_uw,  vel_uh,  vel_pill_w );
		if ( wm.show_fps.value )  draw_split_pill( fps_val,  fps_vw,  fps_vh,  " fps",  fps_uw,  fps_uh,  fps_pill_w );
		if ( has_tick )           draw_split_pill( tick_val, tick_vw, tick_vh, " tick", tick_uw, tick_uh, tick_pill_w );
		if ( wm.show_time.value ) draw_pill( time_buf, time_tw, time_th, time_pill_w );
		draw_widget_grip( draw_list, "watermark_widget", x, y, w, h, scale );
	}

	void widgets::deepseek_character( xdraw::draw_list& draw_list )
	{
		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		auto& ds = settings::g_misc.m_deepseek_character;

		static int texture_w{ 0 }, texture_h{ 0 };
		static const auto character_texture = xdraw::load_texture(
			std::as_bytes( std::span{ resources::images::deepseek_character::data } ), &texture_w, &texture_h );

		if ( !character_texture || texture_w <= 0 || texture_h <= 0 )
			return;

		// Keep the source aspect ratio: the embedded sprite is 212x320.
		constexpr auto character_height = 160.0f;
		const auto character_width = character_height * ( static_cast< float >( texture_w ) / static_cast< float >( texture_h ) );

		if ( ds.position_x.value < 0.0f )
			ds.position_x = static_cast<float>( screen_w ) - character_width - 10.0f;
		if ( ds.position_y.value < 0.0f )
			ds.position_y = static_cast<float>( screen_h ) - character_height - 10.0f;

		auto x = ds.position_x.value;
		auto y = ds.position_y.value;

		x = std::clamp( x, 0.0f, std::max( 0.0f, static_cast<float>( screen_w ) - character_width ) );
		y = std::clamp( y, 0.0f, std::max( 0.0f, static_cast<float>( screen_h ) - character_height ) );

		if ( g_menu.is_open( ) )
		{
			auto& state = interaction_state( );
			const auto& input = xui::ctx( ).input;
			const auto id = xui::fnv1a( "deepseek_character_widget" );

			if ( input.mouse_clicked
				&& input.mouse_x >= x && input.mouse_x <= x + character_width
				&& input.mouse_y >= y && input.mouse_y <= y + character_height )
			{
				state.dragging = id;
				state.drag_offset_x = input.mouse_x - x;
				state.drag_offset_y = input.mouse_y - y;
			}

			if ( state.dragging == id )
			{
				if ( input.mouse_down )
				{
					x = input.mouse_x - state.drag_offset_x;
					y = input.mouse_y - state.drag_offset_y;
					x = std::clamp( x, 0.0f, std::max( 0.0f, static_cast<float>( screen_w ) - character_width ) );
					y = std::clamp( y, 0.0f, std::max( 0.0f, static_cast<float>( screen_h ) - character_height ) );
				}
				else
				{
					state.dragging = 0;
				}
			}

			draw_list.rect( x, y, character_width, character_height, xui::ctx( ).style.accent, 2.0f );
		}

		draw_list.image( x, y, character_width, character_height, character_texture.Get( ) );

		ds.position_x = x;
		ds.position_y = y;
	}

	void widgets::duck_indicator( xdraw::draw_list& draw_list )
	{
		const auto local = systems::g_local.get( );
		float duck_amount{};
		if ( local.pawn )
		{
			const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			if ( movement_services )
			{
				duck_amount = std::clamp( memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) ), 0.0f, 1.0f );
			}
		}
		draw_progress_indicator( draw_list, "duck_indicator_widget", "Fake Crouch", settings::g_misc.m_hud.m_duck_indicator, duck_amount, true );
	}

	void widgets::slow_indicator( xdraw::draw_list& draw_list )
	{
		const auto progress = features::misc::g_impacts.local_hurt_progress( );
		if ( progress <= 0.0f && !g_menu.is_open( ) )
		{
			return;
		}
		draw_progress_indicator( draw_list, "slow_indicator_widget", "Slow", settings::g_misc.m_hud.m_slow_indicator, progress, false );
	}

	void widgets::hitchance_indicator( xdraw::draw_list& draw_list )
	{
		const auto& ctx = features::combat::g_shared.ctx( );
		const auto has_weapon = ctx.valid && ctx.weapon_type >= cstypes::weapon_type::pistol && ctx.weapon_type <= cstypes::weapon_type::lmg;
		if ( ( !settings::g_combat.m_ragebot.enabled.value || !has_weapon ) && !g_menu.is_open( ) )
		{
			return;
		}

		const auto& group = settings::g_combat.m_ragebot.get_group( has_weapon ? ctx.weapon_type : cstypes::weapon_type::rifle );
		const auto value = group.hitchance_override.value ? group.hitchance_override_value.value : group.hitchance.value;
		draw_value_indicator( draw_list, "hc_indicator_widget", "HC", value, settings::g_misc.m_hud.m_hitchance_indicator, value_indicator_icon::scope );
	}

	void widgets::min_damage_indicator( xdraw::draw_list& draw_list )
	{
		const auto& ctx = features::combat::g_shared.ctx( );
		const auto has_weapon = ctx.valid && ctx.weapon_type >= cstypes::weapon_type::pistol && ctx.weapon_type <= cstypes::weapon_type::lmg;
		if ( ( !settings::g_combat.m_ragebot.enabled.value || !has_weapon ) && !g_menu.is_open( ) )
		{
			return;
		}

		const auto& group = settings::g_combat.m_ragebot.get_group( has_weapon ? ctx.weapon_type : cstypes::weapon_type::rifle );
		const auto value = group.min_damage_override.value ? group.min_damage_override_value.value : group.min_damage.value;
		draw_value_indicator( draw_list, "md_indicator_widget", "MD", value, settings::g_misc.m_hud.m_min_damage_indicator, value_indicator_icon::weapon );
	}

	void widgets::low_fps_indicator( xdraw::draw_list& draw_list )
	{
		auto& cfg = settings::g_misc.m_hud.m_low_fps_indicator;
		const auto& style = xui::ctx( ).style;
		cfg.scale.value = std::clamp( cfg.scale.value, 0.72f, 1.8f );
		const auto scale = cfg.scale.value;
		const scoped_widget_font font{ scale };
		const auto h = 28.0f * scale;
		const auto radius = 8.0f * scale;
		const auto icon_x = 18.0f * scale;
		const auto icon_y = h * 0.5f;

		const auto label = xui::translate( "low fps" );
		const auto [ text_w, text_h ] = xdraw::measure_text( label );
		const auto w = 34.0f * scale + text_w + 12.0f * scale;

		auto x = cfg.position_x.value;
		auto y = cfg.position_y.value;
		interact_widget( draw_list, "low_fps_indicator_widget", x, y, w, h, cfg.scale.value );
		cfg.position_x = x;
		cfg.position_y = y;

		draw_list.rect_filled_blurred( x, y, w, h, xdraw::corner_radius{ radius }, cfg.color.value.alpha( 72 ) );
		draw_list.rect_filled( x, y, w, h, style.window_bg.alpha( 205 ), xdraw::corner_radius{ radius } );
		draw_list.circle_filled( x + icon_x, y + icon_y, 9.0f * scale, style.child_bg.alpha( 230 ) );

		draw_list.line( x + icon_x, y + icon_y - 4.5f * scale, x + icon_x, y + icon_y + 1.5f * scale, cfg.color.value, 2.0f * scale );
		draw_list.circle_filled( x + icon_x, y + icon_y + 4.5f * scale, 1.1f * scale, cfg.color.value );

		draw_list.text( x + 34.0f * scale, y + ( h - text_h ) * 0.5f + 0.5f, label, cfg.color.value );
		draw_widget_grip( draw_list, "low_fps_indicator_widget", x, y, w, h, scale );
	}

	void widgets::keybinds( xdraw::draw_list& draw_list )
	{
		struct row_anim_t
		{
			animation::fade alpha;
			animation::spring offset_y;
			bool active_this_frame{ false };
		};

		static std::map<std::string, row_anim_t> row_states;
		static animation::fade container_alpha;

		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto& s = xui::ctx( ).style;
		auto& widget_cfg = settings::g_misc.m_widgets;
		widget_cfg.keybinds_scale.value = std::clamp( widget_cfg.keybinds_scale.value, 0.72f, 1.8f );
		const auto scale = widget_cfg.keybinds_scale.value;
		const scoped_widget_font font{ scale };

		const auto row_spacing = 3.0f * scale;
		const auto row_h = 21.0f * scale;
		const auto header_h = 24.0f * scale;
		const auto r = 8.0f * scale;
		const auto inner_r = 6.0f * scale;
		const auto inner_pad = 2.0f * scale;
		const auto text_pad_x = 8.0f * scale;
		constexpr auto text_nudge{ 0.5f };
		const auto icon_size = 20.0f * scale;
		const auto icon_inner_pad = 4.0f * scale;

		struct bind_entry
		{
			const char* name;
			char value[ 32 ];
			bool has_value_pill;
			xui::bind_mode mode;
		};

		bind_entry entries[ 32 ]{};
		auto count{ 0 };

		const auto& ctx = features::combat::g_shared.ctx( );
		const auto has_weapon = ctx.valid && ctx.weapon_type >= cstypes::weapon_type::pistol && ctx.weapon_type <= cstypes::weapon_type::lmg;

		for ( const auto setting : xui::binds::all( ) )
		{
			if ( !setting || setting->bind.key == 0 || !setting->bind.active || count >= 32 )
			{
				continue;
			}

			auto is_rage_group{ false };
			for ( auto i = 0u; i < settings::combat::ragebot::k_group_count; ++i )
			{
				const auto& g = settings::g_combat.m_ragebot.groups[ i ];
				if ( setting == &g.min_damage_override || setting == &g.hitchance_override || setting == &g.force_shot || setting == &g.force_shot_air || setting == &g.body_aim || setting == &g.silent || setting == &g.no_spread )
				{
					is_rage_group = true;
					break;
				}
			}

			if ( is_rage_group )
			{
				if ( !settings::g_combat.m_ragebot.enabled || !has_weapon )
				{
					continue;
				}

				const auto active_group = &settings::g_combat.m_ragebot.get_group( ctx.weapon_type );
				auto is_active{ false };

				for ( auto i = 0u; i < settings::combat::ragebot::k_group_count; ++i )
				{
					const auto& g = settings::g_combat.m_ragebot.groups[ i ];
					if ( &g == active_group )
					{
						if ( setting == &g.min_damage_override || setting == &g.hitchance_override || setting == &g.force_shot || setting == &g.force_shot_air || setting == &g.body_aim )
						{
							is_active = true;
						}
						break;
					}
				}

				if ( !is_active )
				{
					continue;
				}

				auto& e = entries[ count++ ];
				e.name = setting->name.c_str( );
				e.mode = setting->bind.mode;

				if ( setting == &active_group->min_damage_override )
				{
					std::snprintf( e.value, sizeof( e.value ), "%d", active_group->min_damage_override_value.value );
					e.has_value_pill = true;
				}
				else if ( setting == &active_group->hitchance_override )
				{
					std::snprintf( e.value, sizeof( e.value ), "%d%%", active_group->hitchance_override_value.value );
					e.has_value_pill = true;
				}
				else
				{
					e.value[ 0 ] = '\0';
					e.has_value_pill = false;
				}
				continue;
			}

			auto is_legit_group{ false };
			for ( auto i = 0u; i < settings::combat::legitbot::k_group_count; ++i )
			{
				const auto& g = settings::g_combat.m_legitbot.groups[ i ];
				if ( setting == &g.aimbot || setting == &g.rcs || setting == &g.standalone_rcs || setting == &g.triggerbot || setting == &g.autowall || setting == &g.visualize_fov || setting == &g.trigger_head_only || setting == &g.give_me_your_seed )
				{
					is_legit_group = true;
					break;
				}
			}

			if ( is_legit_group )
			{
				if ( !settings::g_combat.m_legitbot.enabled.value || !has_weapon )
				{
					continue;
				}

				const auto* active_group = &settings::g_combat.m_legitbot.get_group( ctx.weapon_type );
				auto is_active{ false };

				for ( auto i = 0u; i < settings::combat::legitbot::k_group_count; ++i )
				{
					if ( &settings::g_combat.m_legitbot.groups[ i ] == active_group )
					{
						const auto& g = settings::g_combat.m_legitbot.groups[ i ];
						if ( setting == &g.aimbot || setting == &g.rcs || setting == &g.standalone_rcs || setting == &g.triggerbot || setting == &g.autowall || setting == &g.visualize_fov || setting == &g.trigger_head_only || setting == &g.give_me_your_seed )
						{
							is_active = true;
						}

						if ( is_active && setting == &active_group->give_me_your_seed && !active_group->triggerbot.value )
						{
							is_active = false;
						}
						break;
					}
				}

				if ( !is_active )
				{
					continue;
				}

				auto& e = entries[ count++ ];
				e.name = setting->name.c_str( );
				e.mode = setting->bind.mode;
				e.value[ 0 ] = '\0';
				e.has_value_pill = false;
				continue;
			}

			if ( setting == &settings::g_combat.m_antiaim.enabled || setting == &settings::g_combat.m_antiaim.manual_left || setting == &settings::g_combat.m_antiaim.manual_right || setting == &settings::g_combat.m_antiaim.hide_shots || setting == &settings::g_combat.m_antiaim.avoid_backstab || setting == &settings::g_combat.m_antiaim.direction_indicator )
			{
				if ( !settings::g_combat.m_antiaim.enabled.value )
				{
					continue;
				}
			}

			auto& e = entries[ count++ ];
			e.name = setting->name.c_str( );
			e.mode = setting->bind.mode;
			e.value[ 0 ] = '\0';
			e.has_value_pill = false;
		}

		if ( count < 32 && settings::g_combat.m_ragebot.enabled.value && has_weapon && features::combat::g_rage.should_stop( ) )
		{
			const auto& prestate = systems::g_prediction.pre( );
			const auto airborne = !( prestate.flags & cstypes::entity_flags::on_ground );
			auto& e = entries[ count++ ];
			e.name = airborne ? "air autostop" : "autostop";
			e.mode = xui::bind_mode::hold_on;
			e.value[ 0 ] = '\0';
			e.has_value_pill = false;
		}

		if ( count > 0 )
			container_alpha.fade_in( 0.2f );
		else
			container_alpha.fade_out( 0.2f );

		container_alpha.update( );
		if ( !container_alpha.visible( ) )
			return;

		const auto master_alpha = container_alpha.alpha( );
		const auto total_h = header_h + row_spacing + ( static_cast< float >( count ) * ( row_h + row_spacing ) );

		static auto icon_w_px = 0, icon_h_px = 0;
		static const auto kb_icon = xdraw::load_svg( R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><circle cx="5.2" cy="8.1" r="2.45" fill="#FFFFFF"/><circle cx="9.7" cy="5.1" r="2.65" fill="#FFFFFF"/><circle cx="15.1" cy="5.1" r="2.65" fill="#FFFFFF"/><circle cx="19.2" cy="8.5" r="2.45" fill="#FFFFFF"/><path d="M12.15 9.1C8.55 9.1 5.45 12.65 5.45 16.05C5.45 18.65 7.35 20.15 9.45 19.25C11.2 18.5 13.05 18.5 14.85 19.25C17 20.15 18.9 18.65 18.9 16.05C18.9 12.65 15.75 9.1 12.15 9.1Z" fill="#FFFFFF"/></svg>)", 0.5f, &icon_w_px, &icon_h_px );

		const auto header_text = xui::translate( "keybinds" );
		const auto [header_tw, header_th] = xdraw::measure_text( header_text );
		const auto header_w = inner_pad + icon_size + inner_pad + header_tw + text_pad_x * 2.0f + inner_pad;
		const auto header_inner_h = header_h - inner_pad * 2.0f;
		const auto inner_h = row_h - inner_pad * 2.0f;
		const auto master_u8 = static_cast< std::uint8_t >( 255.0f * master_alpha );

		auto widget_w = header_w;
		for ( auto i = 0; i < count; ++i )
		{
			const auto name_w = xdraw::measure_text( xui::translate( entries[ i ].name ) ).first;
			auto row_w = inner_pad + name_w + text_pad_x * 2.0f + inner_pad;
			if ( entries[ i ].has_value_pill )
				row_w += xdraw::measure_text( entries[ i ].value ).first + text_pad_x * 2.0f + inner_pad;
			widget_w = std::max( widget_w, row_w );
		}

		if ( widget_cfg.keybinds_y.value < 0.0f )
			widget_cfg.keybinds_y = ( static_cast<float>( screen_h ) - total_h ) * 0.5f;
		auto x = widget_cfg.keybinds_x.value;
		auto target_base_y = widget_cfg.keybinds_y.value;
		interact_widget( draw_list, "keybinds_widget", x, target_base_y, widget_w, total_h, widget_cfg.keybinds_scale.value );
		widget_cfg.keybinds_x = x;
		widget_cfg.keybinds_y = target_base_y;

		const auto base_ry = target_base_y;

		draw_list.rect_filled_blurred( x, base_ry, header_w, header_h, xdraw::corner_radius{ r }, xdraw::color{ 255, 255, 255, master_u8 } );
		draw_list.rect_filled( x, base_ry, header_w, header_h, s.window_bg.alpha( static_cast< std::uint8_t >( s.window_bg.a * master_alpha ) ), xdraw::corner_radius{ r } );
		draw_list.rect_filled( x + inner_pad, base_ry + inner_pad, icon_size, header_inner_h, s.accent.alpha( static_cast< std::uint8_t >( s.accent.a * master_alpha ) ), xdraw::corner_radius{ inner_r } );

		if ( kb_icon )
		{
			const auto icon_draw = icon_size - icon_inner_pad * 2.0f;
			const auto ix = std::floor( x + inner_pad + icon_inner_pad );
			const auto iy = std::floor( base_ry + inner_pad + ( header_inner_h - icon_draw ) * 0.5f + 1.0f );
			draw_list.image( ix, iy, icon_draw, icon_draw, kb_icon.Get( ), s.checkbox_mark_icon.alpha( static_cast< std::uint8_t >( s.checkbox_mark_icon.a * master_alpha ) ) );
		}

		const auto htx = x + inner_pad + icon_size + inner_pad;
		const auto htw = header_tw + text_pad_x * 2.0f;
		draw_list.rect_filled( htx, base_ry + inner_pad, htw, header_inner_h, s.child_bg.alpha( static_cast< std::uint8_t >( s.child_bg.a * master_alpha ) ), xdraw::corner_radius{ inner_r } );
		draw_list.text( htx + text_pad_x, base_ry + ( header_h - header_th ) * 0.5f + text_nudge, header_text, s.accent.alpha( static_cast< std::uint8_t >( s.accent.a * master_alpha ) ) );

		for ( auto& [name, state] : row_states )
			state.active_this_frame = false;

		float current_offset_y = header_h + row_spacing;
		for ( auto i = 0; i < count; ++i )
		{
			const auto& e = entries[ i ];
			auto& anim = row_states[ e.name ];

			if ( !anim.active_this_frame && anim.alpha.alpha( ) <= 0.01f )
				anim.offset_y.snap( current_offset_y );

			anim.active_this_frame = true;
			anim.alpha.fade_in( 0.2f );
			anim.offset_y.set_target( current_offset_y );
			anim.alpha.update( );
			anim.offset_y.update( );

			const auto row_alpha = anim.alpha.alpha( ) * master_alpha;
			const auto draw_y = base_ry + anim.offset_y.value( );
			const auto entry_name = xui::translate( e.name );
			const auto [nw, nh] = xdraw::measure_text( entry_name );
			const auto row_u8 = static_cast< std::uint8_t >( 255.0f * row_alpha );

			if ( e.has_value_pill )
			{
				const auto [vw, vh] = xdraw::measure_text( e.value );
				const auto name_pill_w = nw + text_pad_x * 2.0f;
				const auto value_pill_w = vw + text_pad_x * 2.0f;
				const auto row_w = inner_pad + name_pill_w + inner_pad + value_pill_w + inner_pad;

				draw_list.rect_filled_blurred( x, draw_y, row_w, row_h, xdraw::corner_radius{ r }, xdraw::color{ 255, 255, 255, row_u8 } );
				draw_list.rect_filled( x, draw_y, row_w, row_h, s.window_bg.alpha( static_cast< std::uint8_t >( s.window_bg.a * row_alpha ) ), xdraw::corner_radius{ r } );
				draw_list.rect_filled( x + inner_pad, draw_y + inner_pad, name_pill_w, inner_h, s.child_bg.alpha( static_cast< std::uint8_t >( s.child_bg.a * row_alpha ) ), xdraw::corner_radius{ inner_r } );
				draw_list.text( x + inner_pad + text_pad_x, draw_y + ( row_h - nh ) * 0.5f + text_nudge, entry_name, s.accent.alpha( static_cast< std::uint8_t >( s.accent.a * row_alpha ) ) );

				const auto vpx = x + inner_pad + name_pill_w + inner_pad;
				draw_list.rect_filled( vpx, draw_y + inner_pad, value_pill_w, inner_h, s.accent.alpha( static_cast< std::uint8_t >( s.accent.a * row_alpha ) ), xdraw::corner_radius{ inner_r } );
				draw_list.text( vpx + text_pad_x, draw_y + ( row_h - vh ) * 0.5f + text_nudge, e.value, s.checkbox_mark_icon.alpha( static_cast< std::uint8_t >( s.checkbox_mark_icon.a * row_alpha ) ) );
			}
			else
			{
				const auto name_pill_w = nw + text_pad_x * 2.0f;
				const auto row_w = inner_pad + name_pill_w + inner_pad;
				const auto text_col = ( e.mode == xui::bind_mode::toggle ) ? s.text_dim : s.accent;

				draw_list.rect_filled_blurred( x, draw_y, row_w, row_h, xdraw::corner_radius{ r }, xdraw::color{ 255, 255, 255, row_u8 } );
				draw_list.rect_filled( x, draw_y, row_w, row_h, s.window_bg.alpha( static_cast< std::uint8_t >( s.window_bg.a * row_alpha ) ), xdraw::corner_radius{ r } );
				draw_list.rect_filled( x + inner_pad, draw_y + inner_pad, name_pill_w, inner_h, s.child_bg.alpha( static_cast< std::uint8_t >( s.child_bg.a * row_alpha ) ), xdraw::corner_radius{ inner_r } );
				draw_list.text( x + inner_pad + text_pad_x, draw_y + ( row_h - nh ) * 0.5f + text_nudge, entry_name, text_col.alpha( static_cast< std::uint8_t >( text_col.a * row_alpha ) ) );
			}

			current_offset_y += row_h + row_spacing;
		}

		for ( auto it = row_states.begin( ); it != row_states.end( ); )
		{
			if ( !it->second.active_this_frame )
			{
				it->second.alpha.fade_out( 0.15f );
				it->second.alpha.update( );
				it->second.offset_y.update( );

				if ( it->second.alpha.alpha( ) <= 0.001f )
				{
					it = row_states.erase( it );
					continue;
				}

				const auto row_alpha = it->second.alpha.alpha( ) * master_alpha;
				const auto row_u8 = static_cast< std::uint8_t >( 255.0f * row_alpha );
				const auto draw_y = base_ry + it->second.offset_y.value( );
				const auto stale_name = xui::translate( it->first );
				const auto [nw, nh] = xdraw::measure_text( stale_name );
				const auto row_w = inner_pad + ( nw + text_pad_x * 2.0f ) + inner_pad;

				draw_list.rect_filled_blurred( x, draw_y, row_w, row_h, xdraw::corner_radius{ r }, xdraw::color{ 255, 255, 255, row_u8 } );
				draw_list.rect_filled( x, draw_y, row_w, row_h, s.window_bg.alpha( static_cast< std::uint8_t >( s.window_bg.a * row_alpha ) ), xdraw::corner_radius{ r } );
			}
			++it;
		}

		draw_widget_grip( draw_list, "keybinds_widget", x, base_ry, widget_w, total_h, scale );
	}

} // namespace rendering
