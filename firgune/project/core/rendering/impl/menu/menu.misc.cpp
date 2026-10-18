#include <pch/pch.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* sound_types[ ]{ "shop click", "home click", "bell", "killcard", "bullet casing", "coin pickup", "item drop", "popcan", "key press", "custom" };
		constexpr auto k_sound_type_count{ static_cast< int >( std::size( sound_types ) ) };

		void draw_custom_sound_picker( config::str& file_setting, std::string_view combo_label, std::string_view preview_id, float preview_volume )
		{
			const auto files = features::misc::impacts::list_custom_sounds( );

			static std::vector<std::string> cached_files{};
			static std::vector<const char*> cached_ptrs{};
			cached_files = files;
			cached_ptrs.clear( );
			cached_ptrs.reserve( cached_files.size( ) );

			for ( const auto& file : cached_files )
			{
				cached_ptrs.push_back( file.c_str( ) );
			}

			if ( !cached_ptrs.empty( ) )
			{
				auto selected{ 0 };
				for ( auto i = 0; i < static_cast< int >( cached_files.size( ) ); ++i )
				{
					if ( cached_files[ static_cast< std::size_t >( i ) ] == file_setting.value )
					{
						selected = i;
						break;
					}
				}

				if ( xui::combo( combo_label, selected, cached_ptrs.data( ), static_cast< int >( cached_ptrs.size( ) ) ) )
				{
					file_setting = cached_files[ static_cast< std::size_t >( selected ) ];
				}
			}

			xui::text_input( "file", file_setting.value, 64, "hit.wav" );

			if ( xui::button( preview_id, 96.0f, 22.0f ) )
			{
				features::misc::g_impacts.play_custom_sound( file_setting.value, preview_volume );
			}
		}
		constexpr const char* marker_types[ ]{ "classic", "damage", "both" };

		constexpr const char* primary_weapons[ ]{ "none", "rifle", "scoped rifle", "scout", "awp", "auto sniper" };
		constexpr const char* secondary_weapons[ ]{ "none", "dual elites", "five-seven/tec-9", "deagle", "revolver" };
		constexpr const char* grenade_names[ ]{ "molotov", "he grenade", "smoke", "flashbang", "decoy" };
		constexpr const char* language_names[ ]{ "English", "Chinese" };

	} // namespace detail

	void menu::draw_misc( float group_w ) const
	{
		auto& m = settings::g_misc;

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = wx + tokens::gap + tokens::sidebar_w + tokens::gap;
		const auto body_y = wy + tokens::gap + tokens::subtab_bar_h + tokens::gap;
		const auto content_w = this->m_w - tokens::gap * 2.0f - tokens::sidebar_w - tokens::gap;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		const auto subtab = this->m_subtab;

		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( subtab == 0 )
		{
			auto& impacts = m.m_impacts;
			auto& traj = m.m_projectile_trajectory;
			auto& dlights = m.m_dlight;
			auto& pen = settings::g_combat.m_penetration_crosshair;
			auto& mov = settings::g_movement;
			auto& ab = m.m_autobuy;

			if ( xui::begin_child( "##misc_impacts", col_w ) )
			{
				xui::checkbox( "hit display", impacts.hit_display );
				if ( xui::begin_popup( "##hit_display_popup", 260.0f ) )
				{
					xui::checkbox( "hit logs", impacts.hit_log );
					if ( impacts.hit_log.value )
					{
						xui::slider_float( "duration##hl", impacts.hit_log_duration, 0.5f, 10.0f, "%.1fs" );
						xui::slider_float( "scale##hl", impacts.hit_log_scale, 0.72f, 1.8f, "%.2f" );
					}
					xui::checkbox( "console logs", impacts.console_log );
					xui::checkbox( "chat logs", impacts.chat_log );
					xui::checkbox( "miss logs", impacts.miss_log );
					if ( impacts.miss_log.value )
						xui::slider_float( "duration##ml", impacts.miss_log_duration, 0.5f, 10.0f, "%.1fs" );

					xui::checkbox( "hit sound", impacts.hit_sound );
					if ( impacts.hit_sound.value )
					{
						xui::combo( "type##hs", impacts.hit_sound_type.value, detail::sound_types, detail::k_sound_type_count );
						xui::slider_float( "volume##hs", impacts.hit_sound_volume, 1.0f, 100.0f, "%.0f%%" );
						if ( impacts.hit_sound_type.value == settings::misc::impacts::sound_type::custom )
							detail::draw_custom_sound_picker( impacts.custom_hit_sound, "sound##hs", "preview##hs", impacts.hit_sound_volume.value );
					}

					xui::checkbox( "hit marker", impacts.hit_marker );
					if ( impacts.hit_marker.value )
					{
						xui::combo( "type##hm", impacts.hit_marker_type.value, detail::marker_types, 3 );
						xui::slider_float( "duration##hm", impacts.hit_marker_duration, 0.1f, 5.0f, "%.1fs" );
						xui::color_picker( "color##hm", impacts.hit_marker_color );
					}

					xui::checkbox( "hit effect", impacts.hit_effect );
					if ( impacts.hit_effect.value )
					{
						xui::color_picker( "color##hitfx", impacts.hit_effect_color );
						xui::slider_float( "duration##hitfx", impacts.hit_effect_duration, 0.1f, 5.0f, "%.1fs" );
						xui::slider_float( "strength##hitfx", impacts.hit_effect_strength, 1.0f, 100.0f, "%.0f%%" );
					}
					xui::end_popup( );
				}

				xui::checkbox( "death sound", impacts.death_sound );
				if ( xui::begin_popup( "##deathsound_popup", 220.0f ) )
				{
					xui::combo( "type##ds", impacts.death_sound_type.value, detail::sound_types, detail::k_sound_type_count );

					xui::slider_float( "volume##ds", impacts.death_sound_volume, 1.0f, 100.0f, "%.0f%%" );

					if ( impacts.death_sound_type.value == settings::misc::impacts::sound_type::custom )
					{
						detail::draw_custom_sound_picker( impacts.custom_death_sound, "sound##ds", "preview##ds", impacts.death_sound_volume.value );
					}

					xui::end_popup( );
				}

				xui::checkbox( "death effect", impacts.death_effect );
				if ( xui::begin_popup( "##deathfx_popup", 220.0f ) )
				{
					xui::color_picker( "color##deathfx", impacts.death_effect_color );
					xui::end_popup( );
				}

				xui::checkbox ("scoreboard weapons", m.m_scoreboard_weapons.enabled);
				if (xui::begin_popup ("##scoreboardeq_popup", 220.0f)) {
					xui::color_picker ("color##scoreboardeq", m.m_scoreboard_weapons.color);
					xui::end_popup ();
				}

				xui::end_child( );
			}

			if ( xui::begin_child( "##misc_visuals", col_w ) )
			{
				xui::checkbox( "projectile trajectory", traj.enabled );
				if ( xui::begin_popup( "##traj_popup", 220.0f ) )
				{
					xui::checkbox( "straight throw", traj.straight_throw );
					xui::color_picker( "held color", traj.held_color );
					xui::color_picker( "thrown color", traj.thrown_color );
					xui::color_picker( "will damage held color", traj.will_deal_damage_held_color );
					xui::color_picker( "will damage thrown color", traj.will_deal_damage_thrown_color );
					xui::end_popup( );
				}

				xui::checkbox( "dynamic light", dlights.enabled );
				if ( xui::begin_popup( "##dlight_popup", 220.0f ) )
				{
					xui::color_picker( "color##dl", dlights.color );
					xui::slider_float( "radius##dl", dlights.radius, 50.0f, 15000.0f, "%.0f" );
					xui::slider_float( "z offset##dl", dlights.z_offset, 0.0f, 100.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::checkbox( "penetration crosshair", pen.enabled );
				if ( xui::begin_popup( "##pen_popup", 220.0f ) )
				{
					xui::checkbox( "glow##pen", pen.glow );
					xui::slider_float( "glow strength##pen", pen.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::color_picker( "can penetrate##pen", pen.can_penetrate_fill );
					xui::color_picker( "can pen outline##pen", pen.can_penetrate_outline );
					xui::color_picker( "blocked##pen", pen.blocked_fill );
					xui::color_picker( "blocked outline##pen", pen.blocked_outline );
					xui::end_popup( );
				}

				xui::end_child( );
			}

			xui::layout::set_cursor( right_x - wx, body_y - wy );

			if ( xui::begin_child( "##misc_movement", col_w ) )
			{
				xui::checkbox( "bhop", mov.bhop );
				if ( xui::begin_popup( "##bhop_popup", 220.0f ) )
				{
					xui::checkbox( "perfect bunny hop", mov.perfect_bunny_hop );
					xui::end_popup( );
				}
				xui::checkbox( "wasd steering", mov.bhop_wasd_steering );
				xui::checkbox( "autostrafe", mov.airstrafe );
				if ( xui::begin_popup( "##autostrafe_popup", 220.0f ) )
				{
					xui::slider_int( "smoothness##as", mov.autostrafe_smoothness, 0, 100, "%d%%" );
					xui::end_popup( );
				}
				xui::checkbox( "jumpbug / edgebug", mov.landing_assist );
				if ( xui::begin_popup( "##landing_assist_popup", 220.0f ) )
				{
					xui::slider_int( "passes##eb", mov.edgebug_passes, 1, 5, "%d" );
					xui::end_popup( );
				}
				xui::checkbox( "fastladder", mov.fastladder );
				xui::checkbox( "edgejump", mov.edgejump );
				xui::checkbox( "edgestop", mov.edgestop );
				xui::checkbox( "slowwalk", mov.slowwalk );

				if ( xui::begin_popup( "##slowwalk_popup", 220.0f ) )
				{
					xui::slider_float( "speed", mov.slowwalk_speed, 1.0f, 100.0f, "%.2fs" );
					xui::end_popup( );
				}

				xui::checkbox( "fast stop", mov.fast_stop );

				xui::end_child( );
			}

			if ( xui::begin_child( "##misc_other", col_w ) )
			{
				xui::combo( "language", m.language.value, detail::language_names, static_cast< int >( std::size( detail::language_names ) ) );
				xui::layout::separator( );

				xui::checkbox( "preserve killfeed", m.preserve_killfeed );
				xui::checkbox( "disable game logs", m.disable_game_logs );
				xui::checkbox( "fake vac", m.m_fake_vac.enabled );
				xui::checkbox( "invalid steam logon", m.m_invalid_steam_logon.enabled );

				xui::checkbox( "deepseek", m.m_deepseek_character.enabled );
				if ( xui::begin_popup( "##deepseek_popup", 220.0f ) )
				{
					xui::slider_float( "position x##ds", m.m_deepseek_character.position_x, -1.0f, 3840.0f, "%.0f" );
					xui::slider_float( "position y##ds", m.m_deepseek_character.position_y, -1.0f, 2160.0f, "%.0f" );
					xui::end_popup( );
				}

				xui::checkbox( "auto buy", ab.enabled );
				if ( xui::begin_popup( "##autobuy_popup", 220.0f ) )
				{
					xui::combo( "primary##ab", ab.primary_weapon, detail::primary_weapons, 6 );
					xui::combo( "secondary##ab", ab.secondary_weapon, detail::secondary_weapons, 5 );
					xui::checkbox( "armor##ab", ab.armor );
					xui::checkbox( "defuser##ab", ab.defuser );
					xui::checkbox( "taser##ab", ab.taser );
					xui::multicombo( "grenades##ab", ab.grenades, detail::grenade_names, 5 );
					xui::end_popup( );
				}

				xui::checkbox( "clantag", m.m_name_changer.clantag );
				xui::checkbox( "override name", m.m_name_changer.override_name );
				if ( xui::begin_popup( "##override_name_popup", 220.0f ) )
				{
					xui::text_input( "name##nc", m.m_name_changer.name.value, 32, "player name..." );
					xui::end_popup( );
				}

				xui::checkbox( "watermark", m.m_watermark.enabled );
				if ( xui::begin_popup( "##watermark_popup", 200.0f ) )
				{
					xui::checkbox( "fps##wm",      m.m_watermark.show_fps );
					xui::checkbox( "ping##wm",     m.m_watermark.show_ping );
					xui::checkbox( "time##wm",     m.m_watermark.show_time );
					xui::checkbox( "user##wm",     m.m_watermark.show_user );
					xui::checkbox( "map##wm",      m.m_watermark.show_map );
					xui::checkbox( "tick rate##wm",m.m_watermark.show_tick );
					xui::checkbox( "velocity##wm", m.m_watermark.show_velocity );
					xui::slider_float( "scale##wm", m.m_watermark.scale, 0.72f, 1.8f, "%.2f" );
					xui::end_popup( );
				}

				xui::end_child( );
			}
		}

		if ( subtab == 1 )
		{
			auto& rem = m.m_removals;

			if ( xui::begin_child( "##misc_removals", col_w ) )
			{
				xui::checkbox( "remove crosshair", rem.crosshair );
				xui::checkbox( "remove scope", rem.scope );
				xui::checkbox( "remove overhead", rem.overhead );
				xui::checkbox( "remove legs", rem.legs );
				xui::checkbox( "remove recoil", rem.recoil );
				xui::checkbox( "remove skybox fog", rem.skybox_fog );
				xui::checkbox( "remove 3d skybox", rem.skybox_3d );
				xui::checkbox( "remove decals", rem.decals );
				xui::checkbox( "remove smoke", rem.smoke );
				xui::slider_float( "flash alpha##flash", rem.flash_alpha, 0.0f, 100.0f, "%.0f%%" );

				xui::end_child( );
			}
		}

	}

} // namespace rendering
