#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* hitbox_names[ ]{ "head", "chest", "stomach", "arms", "legs", "feet" };
		constexpr const char* pitch_items[ ]{ "none", "down", "up" };
		constexpr const char* precision_mode_items[ ]{ "best performance", "best efficiency" };

	} // namespace detail

	void menu::draw_ragebot( float group_w ) const
	{
		auto& s = settings::g_combat;
		auto& rb = s.m_ragebot;
		auto& aa = s.m_antiaim;
		auto& qp = s.m_quickpeek;
		auto& dp = s.m_duckpeek;
		auto& zb = s.m_zeusbot;
		auto& kb = s.m_knifebot;
		auto& autos = s.m_autos;
		auto& lg = s.m_lagcomp;

		auto& wg = rb.groups[ this->m_subtab ];

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = wx + tokens::gap + tokens::sidebar_w + tokens::gap;
		const auto body_y = wy + tokens::gap + tokens::subtab_bar_h + tokens::gap;
		const auto content_w = this->m_w - tokens::gap * 2.0f - tokens::sidebar_w - tokens::gap;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;
		const auto other_h = std::clamp( this->m_body_h * 0.4f, 170.0f, 220.0f );
		const auto lower_y = body_y + other_h + tokens::gap;
		const auto lower_h = std::max( 0.0f, this->m_body_h - other_h - tokens::gap );

		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "##ragebot_other", content_w, other_h, true ) )
		{
			xui::text( "other functions", tokens::col_text );

			xui::checkbox( "precision", rb.precision );
			if ( xui::begin_popup( "##precision_settings", 220.0f ) )
			{
				xui::combo( "precision mode", rb.precision_mode.value, detail::precision_mode_items, 2 );
				xui::end_popup( );
			}

			xui::checkbox( "extrapolation", lg.extrapolation );
			if ( xui::begin_popup( "##extrapolation_settings", 220.0f ) )
			{
				xui::slider_int( "max extrapolate", lg.max_extrapolate_ticks, 1, 16, "%d tick(s)" );
				xui::end_popup( );
			}

			xui::checkbox( "autostop", wg.autostop );
			if ( xui::begin_popup( "##autostop_settings", 220.0f ) )
			{
				xui::checkbox( "early autostop", wg.autostop_early );
				xui::checkbox( "air autostop", wg.autostop_in_air );
				xui::checkbox( "stop on landing", wg.autostop_on_landing );
				xui::end_popup( );
			}

			if ( this->m_subtab == 4 )
			{
				xui::checkbox( "autoscope", autos.scope );
			}

			xui::checkbox( "auto revolver", autos.revolver );

			xui::checkbox( "zeusbot", zb.enabled );
			if ( xui::begin_popup( "##zb_settings", 220.0f ) )
			{
				xui::slider_float( "max fov##zb", zb.max_fov, 1, 180, "%.0f°" );
				xui::checkbox( "drop after##zb", zb.drop_after );
				xui::end_popup( );
			}

			xui::checkbox( "knifebot", kb.enabled );
			if ( xui::begin_popup( "##kb_settings", 220.0f ) )
			{
				xui::slider_float( "max fov##kb", kb.max_fov, 1, 180, "%.0f°" );
				xui::end_popup( );
			}

			xui::checkbox( "quick peek assist", qp.enabled );
			if ( xui::begin_popup( "##qp_colors", 220.0f ) )
			{
				xui::color_picker( "base color##qp", qp.color );
				xui::color_picker( "retracting color##qp", qp.retrack_color );
				xui::end_popup( );
			}

			xui::checkbox( "duck peek assist", dp.enabled );

			xui::end_child( );
		}

		xui::layout::set_cursor( content_x - wx, lower_y - wy );

		if ( xui::begin_child( "##ragebot_aimbot", col_w, lower_h, true ) )
		{
			xui::text( "ragebot", tokens::col_text );
			xui::checkbox( "enabled", rb.enabled );
			xui::checkbox( "double tap", rb.m_double_tap.enabled );
			if ( xui::begin_popup( "##dt_settings", 220.0f ) )
			{
				xui::checkbox( "charge preview", rb.m_double_tap.preview );
				xui::color_picker( "charging color##dt", rb.m_double_tap.charging_color );
				xui::color_picker( "ready color##dt", rb.m_double_tap.ready_color );
				xui::color_picker( "failed color##dt", rb.m_double_tap.failed_color );
				xui::end_popup( );
			}
			xui::checkbox( "silent", wg.silent );
			xui::checkbox( "nospread", wg.no_spread );
			xui::slider_float( "fov", wg.max_fov, 1.0f, 180.0f, "%.0f°" );

			xui::multicombo( "hitboxes", wg.hitboxes, detail::hitbox_names, 6 );
			xui::multicombo( "multipoints", wg.multipoints, detail::hitbox_names, 6 );
			xui::checkbox( "dynamic point scale", wg.dynamic_pointscale );
			xui::slider_float( "pointscale", wg.pointscale, 0.0f, 100.0f, "%.0f%%" );
			xui::checkbox( "debug multipoints", wg.debug_multipoints );
			xui::checkbox( "force bodyaim", wg.body_aim );

			xui::slider_int( "hitchance", wg.hitchance, 0, 100, "%d%%" );
			xui::checkbox( "hitchance override", wg.hitchance_override );
			if ( xui::begin_popup( "##hitchance_popup", 220.0f ) )
			{
				xui::slider_int( "value##hc", wg.hitchance_override_value, 0, 100, "%d%%" );
				xui::end_popup( );
			}

			xui::slider_int( "mindamage", wg.min_damage, 5, 125, "%d" );
			xui::checkbox( "mindamage override", wg.min_damage_override );
			if ( xui::begin_popup( "##mindamage_popup", 220.0f ) )
			{
				xui::slider_int( "value##md", wg.min_damage_override_value, 0, 130, "%d" );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		xui::layout::set_cursor( right_x - wx, lower_y - wy );

		if ( xui::begin_child( "##ragebot_antiaim", col_w, lower_h, true ) )
		{
			xui::checkbox( "anti aim", aa.enabled );

			xui::combo( "pitch", aa.pitch.value, detail::pitch_items, 3 );

			xui::checkbox( "body angle rotation", aa.body_angle_rotation );
			if ( xui::begin_popup( "##body_angle_rotation_popup", 220.0f ) )
			{
				xui::slider_float( "angle##body", aa.body_angle, -180.0f, 180.0f, "%.0f deg" );
				xui::end_popup( );
			}
			xui::checkbox( "force left", aa.manual_left );
			xui::checkbox( "force right", aa.manual_right );
			xui::checkbox( "hide onshot", aa.hide_shots );
			xui::checkbox( "avoid backstab", aa.avoid_backstab );

			xui::checkbox( "mouse steering", aa.mouse_steering );
			if ( xui::begin_popup( "##mouse_steering_popup", 220.0f ) )
			{
				xui::slider_float( "radius##ms", aa.mouse_steering_radius, 100.0f, 800.0f, "%.0f px" );
				xui::end_popup( );
			}

			xui::checkbox( "direction indicator", aa.direction_indicator );

			if ( xui::begin_popup( "##aa_indicator", 220.0f ) )
			{
				xui::color_picker( "color##aa_ind", aa.direction_indicator_color );
				xui::checkbox( "glow##aa_ind", aa.direction_indicator_glow );
				xui::slider_float( "glow strength##aa_ind", aa.direction_indicator_glow_strength, 0.1f, 1.0f, "%.2f" );
				xui::end_popup( );
			}

			xui::end_child( );
		}
	}

} // namespace rendering
