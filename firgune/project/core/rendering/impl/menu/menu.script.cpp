#include <pch/pch.hpp>
#include <core/settings.hpp>
#include <core/features/scripting/scripting.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		std::string script_search_buf{};
		auto script_selected{ -1 };
		auto script_needs_refresh{ true };
		auto script_confirm_unload{ false };
		auto script_confirm_timer{ 0.0f };

		static inline bool script_matches_search( const std::string& name )
		{
			if ( script_search_buf.empty( ) )
			{
				return true;
			}

			std::string lower_name{ name };
			std::string lower_search{ script_search_buf };

			for ( auto& c : lower_name )
			{
				c = static_cast< char >( std::tolower( c ) );
			}

			for ( auto& c : lower_search )
			{
				c = static_cast< char >( std::tolower( c ) );
			}

			return lower_name.find( lower_search ) != std::string::npos;
		}

	} // namespace detail

	void menu::draw_script( float group_w )
	{
		( void )group_w;

		if ( detail::script_needs_refresh )
		{
			features::scripting::g_lua.scan_scripts( );
			detail::script_needs_refresh = false;

			if ( detail::script_selected >= static_cast< int >( features::scripting::g_lua.scripts( ).size( ) ) )
			{
				detail::script_selected = -1;
			}
		}

		const auto dt = xdraw::delta_time( );

		if ( detail::script_confirm_unload )
		{
			detail::script_confirm_timer += dt;

			if ( detail::script_confirm_timer > 3.0f )
			{
				detail::script_confirm_unload = false;
			}
		}

		auto& dl = xui::draw::current( );
		const auto& s = xui::ctx( ).style;
		const auto& input = xui::ctx( ).input;

		xui::layout::set_cursor( this->m_body_x - this->m_x, this->m_body_y - this->m_y );

		if ( !xui::begin_child( "##script_panel", this->m_body_w, this->m_body_h, false ) )
		{
			return;
		}

		xui::text( "C:\\Vagal\\lua", tokens::col_text_dim );
		xui::text_input( "##script_search", detail::script_search_buf, 64, "search scripts..." );

		constexpr auto btn_h{ 28.0f };
		const auto [ avail_w, avail_h ] = xui::layout::avail( );
		const auto list_h = std::max( 80.0f, avail_h - btn_h - s.item_spacing_y );

		if ( xui::begin_child( "##script_list", avail_w, list_h, true ) )
		{
			const auto row_w = xui::layout::avail( ).first;
			constexpr auto row_h{ 28.0f };
			auto visible_rows{ 0 };

			const auto& scripts = features::scripting::g_lua.scripts( );

			for ( auto i = 0; i < static_cast< int >( scripts.size( ) ); ++i )
			{
				const auto& name = scripts[ i ].name;

				if ( !detail::script_matches_search( name ) )
				{
					continue;
				}

				const auto row = xui::layout::item( row_w, row_h );
				const auto is_selected = ( detail::script_selected == i );
				const auto is_hovered = input.in_rect( row );

				if ( is_hovered && input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
				{
					detail::script_selected = i;
					detail::script_confirm_unload = false;
				}

				const auto hover_anim = xui::anim::lerp( xui::fnv1a( "scrrow" ) + i, is_hovered ? 1.0f : 0.0f, 14.0f );
				const auto sel_anim = xui::anim::lerp( xui::fnv1a( "scrsel" ) + i, is_selected ? 1.0f : 0.0f, 10.0f );

				if ( sel_anim > 0.01f )
				{
					dl.rect_filled( row.x, row.y, row.w, row.h, tokens::col_accent.alpha( static_cast< std::uint8_t >( 70.0f * sel_anim ) ), xdraw::corner_radius{ 6.0f } );
				}
				else if ( hover_anim > 0.01f )
				{
					dl.rect_filled( row.x, row.y, row.w, row.h, tokens::col_elevated.alpha( static_cast< std::uint8_t >( 255.0f * hover_anim * 0.5f ) ), xdraw::corner_radius{ 6.0f } );
				}

				const auto [ tw, th ] = xdraw::measure_text( name.c_str( ) );
				const auto text_col = is_selected
					? xui::lerp( tokens::col_text, tokens::col_accent, sel_anim )
					: xui::lerp( tokens::col_text_dim, tokens::col_text, hover_anim );

				dl.text( row.x + 10.0f, row.y + ( row.h - th ) * 0.5f, name.c_str( ), text_col );

				visible_rows++;
			}

			if ( visible_rows == 0 )
			{
				const auto row = xui::layout::item( row_w, row_h );
				dl.text( row.x + 10.0f, row.y + 6.0f, xui::translate( scripts.empty( ) ? "no scripts found" : "no matches" ), tokens::col_text_dim );
			}

			xui::end_child( );
		}

		const auto btn_w = ( avail_w - s.item_spacing_x * 3.0f ) / 4.0f;
		const auto& scripts = features::scripting::g_lua.scripts( );
		const auto has_selection = detail::script_selected >= 0 && detail::script_selected < static_cast< int >( scripts.size( ) );

		if ( xui::button( "load", btn_w, btn_h ) && has_selection )
		{
			features::scripting::g_lua.load_script( scripts[ detail::script_selected ].name );
			detail::script_confirm_unload = false;
		}

		xui::layout::same_line( );

		if ( detail::script_confirm_unload )
		{
			if ( xui::button( "confirm", btn_w, btn_h ) && has_selection )
			{
				features::scripting::g_lua.unload_script( scripts[ detail::script_selected ].name );
				detail::script_confirm_unload = false;
			}
		}
		else if ( xui::button( "unload", btn_w, btn_h ) && has_selection )
		{
			detail::script_confirm_unload = true;
			detail::script_confirm_timer = 0.0f;
		}

		xui::layout::same_line( );

		if ( xui::button( "reload", btn_w, btn_h ) && has_selection )
		{
			features::scripting::g_lua.reload_script( scripts[ detail::script_selected ].name );
		}

		xui::layout::same_line( );

		if ( xui::button( "refresh", btn_w, btn_h ) )
		{
			detail::script_needs_refresh = true;
		}

		// Status line
		const auto& err = features::scripting::g_lua.last_error( );
		if ( !err.empty( ) )
		{
			xui::text( err.c_str( ), xdraw::color{ 255, 100, 100, 200 } );
		}
		else
		{
			const auto& log = features::scripting::g_lua.last_log( );
			if ( !log.empty( ) )
			{
				xui::text( log.c_str( ), tokens::col_text_dim );
			}
		}

		xui::end_child( );
	}

} // namespace rendering
