#include <pch/pch.hpp>
#include <core/settings.hpp>
#include <core/systems/systems.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* k_cham_material_names[ ]{
			"liquid", "metallic", "matte", "flat", "bloom", "outlines", "glow", "electric", "distortion", "hologram", "pearl",
			"liquid (iz)", "matte (iz)", "flat (iz)", "bloom (iz)", "outlines (iz)", "glow (iz)", "distortion (iz)", "hologram (iz)"
		};
		constexpr auto k_cham_material_count = static_cast< int >( settings::esp::cham_ids::count );

		inline static void draw_chams_layer( const char* label, const char* popup_id, settings::esp::chams_layer& layer )
		{
			xui::checkbox( label, layer.enabled );
			if ( xui::begin_popup( popup_id, 220.0f ) )
			{
				xui::combo( "material", layer.material.value, k_cham_material_names, k_cham_material_count );
				xui::color_picker( "color", layer.color );
				xui::end_popup( );
			}
		}

		inline static void draw_chams_config( const char* label, const char* id_suffix, settings::esp::chams_config& cfg, bool show_overlay = true )
		{
			xui::checkbox( label, cfg.enabled );

			char label_buf[ 64 ]{};
			char popup_id[ 64 ]{};

			std::snprintf( label_buf, sizeof( label_buf ), "primary layer##%s", id_suffix );
			std::snprintf( popup_id, sizeof( popup_id ), "##primary_%s", id_suffix );
			draw_chams_layer( label_buf, popup_id, cfg.primary );

			std::snprintf( label_buf, sizeof( label_buf ), "secondary layer##%s", id_suffix );
			std::snprintf( popup_id, sizeof( popup_id ), "##secondary_%s", id_suffix );
			draw_chams_layer( label_buf, popup_id, cfg.secondary );

			if ( show_overlay )
			{
				std::snprintf( label_buf, sizeof( label_buf ), "overlay layer##%s", id_suffix );
				std::snprintf( popup_id, sizeof( popup_id ), "##overlay_%s", id_suffix );
				draw_chams_layer( label_buf, popup_id, cfg.overlay );
			}
		}

		[[nodiscard]] inline static xdraw::color blend_color( xdraw::color from, xdraw::color to, float amount )
		{
			amount = std::clamp( amount, 0.0f, 1.0f );
			const auto channel = [ amount ]( std::uint8_t a, std::uint8_t b )
			{
				return static_cast< std::uint8_t >( std::lerp( static_cast< float >( a ), static_cast< float >( b ), amount ) );
			};

			return { channel( from.r, to.r ), channel( from.g, to.g ), channel( from.b, to.b ), channel( from.a, to.a ) };
		}

		inline static void draw_player_esp_preview( const xui::rect& area, const settings::esp::player::overlay& ov, int team )
		{
			auto& dl = xui::draw::current( );
			const auto overlay_enabled = ov.enabled.value;
			g_model_preview_renderer.set_team( team );

			const auto box_h = std::min( 196.0f, area.h - 64.0f );
			const auto box_w = std::min( 108.0f, box_h * 0.54f );
			const auto box_x = std::floor( area.x + ( area.w - box_w ) * 0.46f );
			const auto box_y = std::floor( area.y + 27.0f );
			const auto center_x = box_x + box_w * 0.5f;

			// 模型纹理按纵横比裁切绘制；骨骼投影必须经过同一 UV 变换才能对上模型。
			float tex_u0 = 0.0f, tex_v0 = 0.0f, tex_u1 = 1.0f, tex_v1 = 1.0f;

			if ( const auto preview_texture = g_model_preview_renderer.texture( ) )
			{
				const auto model_w = box_w * 1.18f;
				auto u0 = 0.0f;
				auto v0 = 0.0f;
				auto u1 = 1.0f;
				auto v1 = 1.0f;

				Microsoft::WRL::ComPtr<ID3D11Resource> resource{};
				preview_texture->GetResource( &resource );
				Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
				if ( resource && SUCCEEDED( resource.As( &texture ) ) )
				{
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc( &desc );
					if ( desc.Width && desc.Height )
					{
						const auto source_aspect = static_cast< float >( desc.Width ) / static_cast< float >( desc.Height );
						const auto target_aspect = model_w / box_h;
						if ( source_aspect > target_aspect )
						{
							const auto visible = target_aspect / source_aspect;
							u0 = ( 1.0f - visible ) * 0.5f;
							u1 = 1.0f - u0;
						}
						else if ( source_aspect < target_aspect )
						{
							const auto visible = source_aspect / target_aspect;
							v0 = ( 1.0f - visible ) * 0.5f;
							v1 = 1.0f - v0;
						}
					}
				}

				tex_u0 = u0; tex_v0 = v0; tex_u1 = u1; tex_v1 = v1;
				dl.image_uv( center_x - model_w * 0.5f, box_y, model_w, box_h, preview_texture, u0, v0, u1, v1, xdraw::color{ 255, 255, 255, 235 } );
			}
			else
			{
				constexpr std::string_view loading{ "3D model loading" };
				const auto [ tw, th ] = xdraw::measure_text( xui::translate( loading ) );
				dl.text( center_x - tw * 0.5f, box_y + box_h * 0.5f - th * 0.5f, xui::translate( loading ), tokens::col_text_dim );
			}

			// 优先使用引擎投影的真实骨骼位置；预览面板未就绪时退回固定比例。
			const auto projected_bone = [ & ]( const char* name, float fallback_x, float fallback_y ) -> std::pair<float, float>
			{
				float nx{}, ny{};
				if ( name && g_model_preview_renderer.bone_point( name, nx, ny ) )
				{
					const auto model_w = box_w * 1.18f;
					const auto model_x = center_x - model_w * 0.5f;
					const auto su = std::clamp( ( nx - tex_u0 ) / std::max( tex_u1 - tex_u0, 0.01f ), 0.0f, 1.0f );
					const auto sv = std::clamp( ( ny - tex_v0 ) / std::max( tex_v1 - tex_v0, 0.01f ), 0.0f, 1.0f );
					return { model_x + model_w * su, box_y + box_h * sv };
				}
				return { box_x + box_w * fallback_x, box_y + box_h * fallback_y };
			};

			if ( !overlay_enabled )
			{
				return;
			}

			if ( ov.m_skeleton.enabled.value )
			{
				const auto color = ov.m_skeleton.visible_color.value;
				const auto thickness = ov.m_skeleton.thickness.value;

				const auto head = projected_bone( "head_0", 0.50f, 0.12f );
				const auto neck = projected_bone( "neck_0", 0.50f, 0.22f );
				const auto chest = projected_bone( "spine_3", 0.50f, 0.36f );
				const auto pelvis = projected_bone( "pelvis", 0.50f, 0.60f );
				const auto shoulder_l = projected_bone( "clavicle_L", 0.29f, 0.29f );
				const auto shoulder_r = projected_bone( "clavicle_R", 0.71f, 0.29f );
				const auto hand_l = projected_bone( "hand_L", 0.11f, 0.55f );
				const auto hand_r = projected_bone( "hand_R", 0.89f, 0.55f );
				const auto foot_l = projected_bone( "ankle_L", 0.30f, 0.96f );
				const auto foot_r = projected_bone( "ankle_R", 0.70f, 0.96f );

				dl.line( head.first, head.second + box_w * 0.105f, neck.first, neck.second, color, thickness );
				dl.line( neck.first, neck.second, pelvis.first, pelvis.second, color, thickness );
				dl.line( shoulder_l.first, shoulder_l.second, shoulder_r.first, shoulder_r.second, color, thickness );
				dl.line( shoulder_l.first, shoulder_l.second, hand_l.first, hand_l.second, color, thickness );
				dl.line( shoulder_r.first, shoulder_r.second, hand_r.first, hand_r.second, color, thickness );
				dl.line( pelvis.first, pelvis.second, foot_l.first, foot_l.second, color, thickness );
				dl.line( pelvis.first, pelvis.second, foot_r.first, foot_r.second, color, thickness );
				dl.circle_filled( chest.first, chest.second, std::max( 1.5f, thickness ), color, 12 );
			}

			if ( ov.m_box.enabled.value )
			{
				const auto color = ov.m_box.visible_color.value;
				if ( ov.m_box.fill.value )
					dl.rect_filled( box_x, box_y, box_w, box_h, color.alpha( 22 ) );

				const auto draw_full_box = [ & ]( xdraw::color col, float thickness )
				{
					dl.rect( box_x, box_y, box_w, box_h, col, thickness );
				};
				const auto draw_corner_box = [ & ]( xdraw::color col, float thickness )
				{
					const auto corner = std::clamp( ov.m_box.corner_length.value, 2.0f, std::min( box_w, box_h ) * 0.5f );
					dl.line( box_x, box_y, box_x + corner, box_y, col, thickness );
					dl.line( box_x, box_y, box_x, box_y + corner, col, thickness );
					dl.line( box_x + box_w - corner, box_y, box_x + box_w, box_y, col, thickness );
					dl.line( box_x + box_w, box_y, box_x + box_w, box_y + corner, col, thickness );
					dl.line( box_x, box_y + box_h - corner, box_x, box_y + box_h, col, thickness );
					dl.line( box_x, box_y + box_h, box_x + corner, box_y + box_h, col, thickness );
					dl.line( box_x + box_w, box_y + box_h - corner, box_x + box_w, box_y + box_h, col, thickness );
					dl.line( box_x + box_w - corner, box_y + box_h, box_x + box_w, box_y + box_h, col, thickness );
				};

				const auto cornered = ov.m_box.style.value == settings::esp::player::overlay::box::style_type::cornered;
				if ( ov.m_box.outline.value )
					cornered ? draw_corner_box( xdraw::color{ 0, 0, 0, 210 }, 3.0f ) : draw_full_box( xdraw::color{ 0, 0, 0, 210 }, 3.0f );
				cornered ? draw_corner_box( color, 1.0f ) : draw_full_box( color, 1.0f );
			}

			int left_bars{};
			int top_bars{};
			int bottom_bars{};
			const auto draw_bar = [ & ]( const auto& bar, float fraction, int value )
			{
				using bar_type = std::remove_cvref_t<decltype( bar )>;
				const auto position = bar.position.value;
				const auto vertical = position == bar_type::position_type::left;
				auto offset = 0;
				if ( vertical ) offset = left_bars++;
				else if ( position == bar_type::position_type::top ) offset = top_bars++;
				else offset = bottom_bars++;

				const auto low = bar.low_color.value;
				const auto full = bar.full_color.value;
				const auto fill = bar.gradient.value ? blend_color( low, full, fraction ) : full;
				const auto background = bar.background_color.value;
				const auto outline = bar.outline_color.value;
				constexpr auto thickness = 3.0f;
				const auto spacing = static_cast< float >( offset ) * 6.0f;

				if ( vertical )
				{
					const auto x = box_x - 6.0f - spacing;
					dl.rect_filled( x, box_y, thickness, box_h, background );
					if ( bar.glow.value ) dl.rect( x - 1.0f, box_y - 1.0f, thickness + 2.0f, box_h + 2.0f, bar.glow_color.value.alpha( 45 ), 2.0f );
					dl.rect_filled( x, box_y + box_h * ( 1.0f - fraction ), thickness, box_h * fraction, fill );
					if ( bar.outline_setting.value ) dl.rect( x - 1.0f, box_y - 1.0f, thickness + 2.0f, box_h + 2.0f, outline );
					if ( bar.show_value.value )
					{
						char value_text[ 8 ]{};
						std::snprintf( value_text, sizeof( value_text ), "%d", value );
						const auto [ tw, th ] = xdraw::measure_text( value_text );
						dl.text( x - tw - 3.0f, box_y + box_h * ( 1.0f - fraction ) - th * 0.5f, value_text, bar.text_color.value, xdraw::text_style::outlined );
					}
				}
				else
				{
					const auto y = position == bar_type::position_type::top ? box_y - 6.0f - spacing : box_y + box_h + 3.0f + spacing;
					dl.rect_filled( box_x, y, box_w, thickness, background );
					if ( bar.glow.value ) dl.rect( box_x - 1.0f, y - 1.0f, box_w + 2.0f, thickness + 2.0f, bar.glow_color.value.alpha( 45 ), 2.0f );
					dl.rect_filled( box_x, y, box_w * fraction, thickness, fill );
					if ( bar.outline_setting.value ) dl.rect( box_x - 1.0f, y - 1.0f, box_w + 2.0f, thickness + 2.0f, outline );
					if ( bar.show_value.value )
					{
						char value_text[ 8 ]{};
						std::snprintf( value_text, sizeof( value_text ), "%d", value );
						dl.text( box_x + box_w * fraction + 3.0f, y - 5.0f, value_text, bar.text_color.value, xdraw::text_style::outlined );
					}
				}
			};

			if ( ov.m_health_bar.enabled.value ) draw_bar( ov.m_health_bar, 0.76f, 76 );
			if ( ov.m_ammo_bar.enabled.value ) draw_bar( ov.m_ammo_bar, 0.64f, 19 );

			if ( ov.m_name.enabled.value )
			{
				constexpr std::string_view name{ "Vagal" };
				const auto [ tw, th ] = xdraw::measure_text( name );
				dl.text( center_x - tw * 0.5f, box_y - th - 4.0f - top_bars * 6.0f, name, ov.m_name.color.value, xdraw::text_style::outlined );
			}

			if ( ov.m_info_flags.enabled.value )
			{
				using flags = settings::esp::player::overlay::info_flags;
				float flag_y = box_y;
				const auto add_flag = [ & ]( flags::flag flag, std::string_view text, xdraw::color color )
				{
					if ( !ov.m_info_flags.has( flag ) ) return;
					dl.text( box_x + box_w + 7.0f, flag_y, text, color, xdraw::text_style::outlined );
					flag_y += xdraw::measure_text( text ).second + 1.0f;
				};

				add_flag( flags::money, "$16000", ov.m_info_flags.money_color.value );
				add_flag( flags::armor, "HK", ov.m_info_flags.armor_color.value );
				add_flag( flags::kit, "KIT", ov.m_info_flags.kit_color.value );
				add_flag( flags::scoped, "ZOOM", ov.m_info_flags.scoped_color.value );
				add_flag( flags::defusing, "DEFUSE", ov.m_info_flags.defusing_color.value );
				add_flag( flags::flashed, "FLASH", ov.m_info_flags.flashed_color.value );
				add_flag( flags::ping, "42 MS", ov.m_info_flags.distance_color.value );
				add_flag( flags::distance, "18 M", ov.m_info_flags.distance_color.value );
			}

			if ( ov.m_weapon.enabled.value )
			{
				using display = settings::esp::player::overlay::weapon::display_type;
				const auto mode = ov.m_weapon.display.value;
				const auto weapon_y = box_y + box_h + 9.0f + bottom_bars * 6.0f;
				if ( mode != display::text )
				{
					const auto icon_x = center_x - 15.0f;
					dl.line( icon_x, weapon_y + 2.0f, icon_x + 23.0f, weapon_y + 2.0f, ov.m_weapon.icon_color.value, 2.0f );
					dl.line( icon_x + 7.0f, weapon_y + 2.0f, icon_x + 12.0f, weapon_y + 7.0f, ov.m_weapon.icon_color.value, 2.0f );
					dl.line( icon_x + 21.0f, weapon_y + 2.0f, icon_x + 30.0f, weapon_y - 1.0f, ov.m_weapon.icon_color.value, 1.5f );
				}
				if ( mode != display::icon )
				{
					constexpr std::string_view weapon{ "AK-47" };
					const auto [ tw, th ] = xdraw::measure_text( weapon );
					const auto text_y = weapon_y + ( mode == display::text_and_icon ? 8.0f : 0.0f );
					dl.text( center_x - tw * 0.5f, text_y, weapon, ov.m_weapon.text_color.value, xdraw::text_style::outlined );
				}
			}
		}

	} // namespace detail

	void menu::draw_player( float group_w ) const
	{
		auto& esp = settings::g_esp;
		auto& p = esp.m_player;

		const auto col_w = ( this->m_body_w - tokens::gap ) * 0.5f;
		const auto subtab = this->m_subtab;
		const auto has_overlay = ( subtab <= 1 );

		auto& glow = (subtab == 0) ? p.m_glow.enemy : p.m_glow.team;
		auto& glow_ragdoll = (subtab == 0) ? p.m_glow.enemy_ragdoll : p.m_glow.team_ragdoll;
		auto& doll = (subtab == 0) ? p.m_doll.enemy : p.m_doll.team;
		auto& doll_gravity = (subtab == 0) ? p.m_doll.enemy_gravity : p.m_doll.team_gravity;

		xui::layout::set_cursor( this->m_body_x - this->m_x, this->m_body_y - this->m_y );

		if ( has_overlay )
		{
			auto& ov = p.m_overlay[ subtab ];

			if ( xui::begin_child( "##player_esp", col_w ) )
			{
				xui::checkbox( "enable", ov.enabled );

				xui::checkbox( "box", ov.m_box.enabled );
				if ( xui::begin_popup( "##box_popup", 220.0f ) )
				{
					constexpr const char* box_styles[ ]{ "full", "cornered" };
					xui::combo( "style##box", ov.m_box.style.value, box_styles, 2 );

					xui::checkbox( "fill", ov.m_box.fill );
					xui::checkbox( "outline", ov.m_box.outline );
					xui::slider_float( "corner length", ov.m_box.corner_length, 2.0f, 20.0f, "%.0f" );
					xui::color_picker( "visible color##box", ov.m_box.visible_color );
					xui::color_picker( "occluded color##box", ov.m_box.occluded_color );
					xui::end_popup( );
				}

				xui::checkbox( "skeleton", ov.m_skeleton.enabled );
				if ( xui::begin_popup( "##skeleton_popup", 220.0f ) )
				{
					constexpr const char* skel_modes[ ]{ "normal", "backtrack" };
					xui::combo( "mode##skel", ov.m_skeleton.type.value, skel_modes, 2 );

					xui::slider_float( "thickness##skel", ov.m_skeleton.thickness, 0.5f, 4.0f, "%.1f" );
					xui::color_picker( "visible color##skel", ov.m_skeleton.visible_color );
					xui::color_picker( "occluded color##skel", ov.m_skeleton.occluded_color );
					xui::end_popup( );
				}

				xui::checkbox( "health bar", ov.m_health_bar.enabled );
				if ( xui::begin_popup( "##health_popup", 220.0f ) )
				{
					constexpr const char* bar_positions[ ]{ "left", "top", "bottom" };
					xui::combo( "position##hp", ov.m_health_bar.position.value, bar_positions, 3 );

					xui::checkbox( "outline##hp", ov.m_health_bar.outline_setting );
					xui::checkbox( "gradient##hp", ov.m_health_bar.gradient );
					xui::checkbox( "show value##hp", ov.m_health_bar.show_value );
					xui::checkbox( "glow##hp", ov.m_health_bar.glow );
					xui::color_picker( "full color##hp", ov.m_health_bar.full_color );
					xui::color_picker( "low color##hp", ov.m_health_bar.low_color );
					xui::color_picker( "background##hp", ov.m_health_bar.background_color );
					xui::color_picker( "outline color##hp", ov.m_health_bar.outline_color );
					xui::color_picker( "text color##hp", ov.m_health_bar.text_color );
					xui::color_picker( "glow color##hp", ov.m_health_bar.glow_color );
					xui::slider_float( "glow strength##hp", ov.m_health_bar.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::checkbox( "ammo bar", ov.m_ammo_bar.enabled );
				if ( xui::begin_popup( "##ammo_popup", 220.0f ) )
				{
					constexpr const char* bar_positions[ ]{ "left", "top", "bottom" };
					xui::combo( "position##ammo", ov.m_ammo_bar.position.value, bar_positions, 3 );

					xui::checkbox( "outline##ammo", ov.m_ammo_bar.outline_setting );
					xui::checkbox( "gradient##ammo", ov.m_ammo_bar.gradient );
					xui::checkbox( "show value##ammo", ov.m_ammo_bar.show_value );
					xui::checkbox( "glow##ammo", ov.m_ammo_bar.glow );
					xui::color_picker( "full color##ammo", ov.m_ammo_bar.full_color );
					xui::color_picker( "low color##ammo", ov.m_ammo_bar.low_color );
					xui::color_picker( "background##ammo", ov.m_ammo_bar.background_color );
					xui::color_picker( "outline color##ammo", ov.m_ammo_bar.outline_color );
					xui::color_picker( "text color##ammo", ov.m_ammo_bar.text_color );
					xui::color_picker( "glow color##ammo", ov.m_ammo_bar.glow_color );
					xui::slider_float( "glow strength##ammo", ov.m_ammo_bar.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::checkbox( "name", ov.m_name.enabled );
				if ( xui::begin_popup( "##name_popup", 220.0f ) )
				{
					xui::color_picker( "color##name", ov.m_name.color );
					xui::end_popup( );
				}

				xui::checkbox( "weapon", ov.m_weapon.enabled );
				if ( xui::begin_popup( "##weapon_popup", 220.0f ) )
				{
					constexpr const char* display_types[ ]{ "text", "icon", "text + icon" };
					xui::combo( "display##wep", ov.m_weapon.display.value, display_types, 3 );

					xui::color_picker( "text color##wep", ov.m_weapon.text_color );
					xui::color_picker( "icon color##wep", ov.m_weapon.icon_color );
					xui::end_popup( );
				}

				xui::checkbox( "flags", ov.m_info_flags.enabled );
				if ( xui::begin_popup( "##flags_popup", 220.0f ) )
				{
					constexpr const char* flag_names[ ]{ "money", "armor", "kit", "scoped", "defusing", "flashed", "ping", "distance" };
					xui::multicombo( "flags##mc", ov.m_info_flags.flags, flag_names, settings::esp::player::overlay::info_flags::count );

					xui::color_picker( "money##flags", ov.m_info_flags.money_color );
					xui::color_picker( "armor##flags", ov.m_info_flags.armor_color );
					xui::color_picker( "kit##flags", ov.m_info_flags.kit_color );
					xui::color_picker( "scoped##flags", ov.m_info_flags.scoped_color );
					xui::color_picker( "defusing##flags", ov.m_info_flags.defusing_color );
					xui::color_picker( "flashed##flags", ov.m_info_flags.flashed_color );
					xui::color_picker( "distance##flags", ov.m_info_flags.distance_color );
					xui::end_popup( );
				}

				xui::checkbox( "oof arrows", ov.m_oof_arrow.enabled );
				if ( xui::begin_popup( "##oof_popup", 220.0f ) )
				{
					xui::checkbox( "glow##oof", ov.m_oof_arrow.glow );
					xui::slider_float( "width##oof", ov.m_oof_arrow.width, 4.0f, 40.0f, "%.0f" );
					xui::slider_float( "height##oof", ov.m_oof_arrow.height, 4.0f, 40.0f, "%.0f" );
					xui::slider_float( "radius x##oof", ov.m_oof_arrow.radius_x, 50.0f, 600.0f, "%.0f" );
					xui::slider_float( "radius y##oof", ov.m_oof_arrow.radius_y, 50.0f, 600.0f, "%.0f" );
					xui::slider_float( "glow strength##oof", ov.m_oof_arrow.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::color_picker( "visible color##oof", ov.m_oof_arrow.visible_color );
					xui::color_picker( "occluded color##oof", ov.m_oof_arrow.occluded_color );
					xui::end_popup( );
				}


				xui::end_child ();

				if (xui::begin_child ("##player_glow", col_w)) {
					xui::checkbox ("glow", glow.enabled);
					if (xui::begin_popup ("##glow_popup", 220.0f)) {
						xui::color_picker ("color##glow", glow.color);
						xui::end_popup ();
					}

					xui::checkbox ("ragdoll glow", glow_ragdoll.enabled);
				if (xui::begin_popup ("##glow_rag_popup", 220.0f)) {
					xui::color_picker ("color##glow_rag", glow_ragdoll.color);
					xui::end_popup ();
				}

				xui::checkbox( "floating doll", doll );
				if ( xui::begin_popup( "##doll_popup", 220.0f ) )
				{
					xui::slider_float( "gravity##doll", doll_gravity, 0.0f, 10.0f, "%.1f" );
					xui::end_popup( );
				}

				xui::end_child ();
				}


			}
		}
		else
		{
			if ( xui::begin_child( "##local_chams_glow", col_w ) )
			{
				detail::draw_chams_config( "chams", "local_main", p.m_chams.local );

				xui::layout::separator( );

				xui::checkbox( "lower opacity", esp.m_local_alpha.enabled );
				if ( xui::begin_popup( "##local_alpha_popup", 220.0f ) )
				{
					xui::slider_float( "opacity", esp.m_local_alpha.opacity, 0.0f, 1.0f, "%.2f" );
					xui::checkbox( "only when scoped", esp.m_local_alpha.only_scoped );
					xui::end_popup( );
				}

				xui::layout::separator( );

				detail::draw_chams_config( "ragdoll chams", "local_ragdoll", p.m_chams.local_ragdoll, false );

				xui::layout::separator( );

				xui::checkbox( "glow", p.m_glow.local.enabled );
				if ( xui::begin_popup( "##local_glow_popup", 220.0f ) )
				{
					xui::color_picker( "color##local_glow", p.m_glow.local.color );
					xui::end_popup( );
				}

				xui::checkbox( "ragdoll glow", p.m_glow.local_ragdoll.enabled );
			if ( xui::begin_popup( "##local_glow_rag_popup", 220.0f ) )
			{
				xui::color_picker( "color##local_glow_rag", p.m_glow.local_ragdoll.color );
				xui::end_popup( );
			}

			xui::checkbox( "floating doll##local", p.m_doll.local );
			if ( xui::begin_popup( "##local_doll_popup", 220.0f ) )
			{
				xui::slider_float( "gravity##local_doll", p.m_doll.local_gravity, 0.0f, 10.0f, "%.1f" );
				xui::end_popup( );
			}

			xui::end_child( );
			}
		}

		xui::layout::set_cursor( this->m_body_x - this->m_x + col_w + tokens::gap, this->m_body_y - this->m_y );

		if ( has_overlay )
		{
			auto& ov = p.m_overlay[ subtab ];
			auto& chams = ( subtab == 0 ) ? p.m_chams.enemy : p.m_chams.team;
			auto& chams_ragdoll = ( subtab == 0 ) ? p.m_chams.enemy_ragdoll : p.m_chams.team_ragdoll;
			const auto preview_h = std::min( 292.0f, this->m_body_h * 0.62f );
			if ( xui::begin_child( "##player_esp_preview", col_w, preview_h, false ) )
			{
				const auto [ preview_w, preview_inner_h ] = xui::layout::avail( );
				const auto preview = xui::layout::item( preview_w, preview_inner_h );
				const auto preview_team = subtab == 0 ? 2 : 3;
				detail::draw_player_esp_preview( preview, ov, preview_team );
				xui::end_child( );
			}

			const auto chams_h = std::max( 120.0f, this->m_body_h - preview_h - tokens::gap );
			if ( xui::begin_child( "##player_chams", col_w, chams_h, true ) )
			{
				detail::draw_chams_config( "chams", "main", chams );

				xui::layout::separator( );

				detail::draw_chams_config( "ragdoll chams", "ragdoll", chams_ragdoll, false );

				if ( subtab == 0 )
				{
					xui::layout::separator( );
					xui::checkbox( "hit visualization", p.m_chams.hittable.enabled );
					if ( xui::begin_popup( "##hittable_color", 220.0f ) )
					{
						xui::color_picker( "color##hittable", p.m_chams.hittable.primary.color );
						xui::end_popup( );
					}

					xui::layout::separator( );

					detail::draw_chams_config( "backtrack chams", "bt", p.m_chams.backtrack, false );
					detail::draw_chams_config ("onshot chams", "os", p.m_chams.onshot, false);

					xui::slider_float ("fade##ft", p.m_chams.onshot_fade_time, 0.1f, 5.0f, "%.0f");

				}

				xui::end_child( );
			}

	}
	else
		{
			if ( xui::begin_child( "##viewmodel", col_w ) )
			{
				detail::draw_chams_config( "weapon chams", "vm_weapon", esp.m_viewmodel.weapon );

				xui::layout::separator( );

				detail::draw_chams_config( "arms chams", "vm_arms", esp.m_viewmodel.arms );

				xui::end_child( );
			}
		}
	}

} // namespace rendering
