#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

extern "C" {
#include <external/luajit/lua.h>
#include <external/luajit/lauxlib.h>
#include <external/luajit/lualib.h>
}

#include "../scripting.hpp"

namespace features::scripting {

	// ── helpers ──────────────────────────────────────────────────────────

	namespace {

		using FnConMsg = void( __cdecl* )( const char* fmt, ... );

		FnConMsg resolve_con_msg( )
		{
			static FnConMsg fn{};
			static bool resolved = false;
			if ( resolved ) return fn;
			resolved = true;

			auto tier0 = GetModuleHandleA( "tier0.dll" );
			if ( !tier0 ) tier0 = LoadLibraryA( "tier0.dll" );
			if ( !tier0 ) return nullptr;

			fn = reinterpret_cast< FnConMsg >( GetProcAddress( tier0, "?ConMsg@@YAXPEBDZZ" ) );
			if ( !fn ) fn = reinterpret_cast< FnConMsg >( GetProcAddress( tier0, "ConMsg" ) );
			return fn;
		}

		void console_print( const char* script_name, const char* msg )
		{
			auto fn = resolve_con_msg( );
			if ( !fn || !msg ) return;

			char buf[ 512 ]{};
			if ( script_name && script_name[ 0 ] )
			{
				std::snprintf( buf, sizeof( buf ), "[Vagal] %s.lua: %s\n", script_name, msg );
			}
			else
			{
				std::snprintf( buf, sizeof( buf ), "[Vagal] %s\n", msg );
			}

			__try { fn( "%s", buf ); } __except( EXCEPTION_EXECUTE_HANDLER ) {}
		}

		lua_engine* self_from_state( lua_State* L )
		{
			lua_getglobal( L, "__engine_ptr" );
			auto* ptr = static_cast< lua_engine** >( lua_touserdata( L, -1 ) );
			lua_pop( L, 1 );
			return ptr ? *ptr : nullptr;
		}

		int lua_print( lua_State* L )
		{
			auto* eng = self_from_state( L );
			if ( !eng ) return 0;

			std::string msg;
			const auto n = lua_gettop( L );
			for ( auto i = 1; i <= n; ++i )
			{
				if ( i > 1 ) msg += '\t';
				msg += luaL_checkstring( L, i );
			}

			console_print( eng->current_script_name( ).c_str( ), msg.c_str( ) );
			eng->set_last_log( msg );
			return 0;
		}

		// ── client API ──────────────────────────────────────────────────

		int api_client_log( lua_State* L )
		{
			auto* eng = self_from_state( L );
			if ( eng && lua_gettop( L ) >= 1 )
			{
				const auto msg = luaL_checkstring( L, 1 );
				console_print( eng->current_script_name( ).c_str( ), msg );
				eng->set_last_log( msg );
			}
			return 0;
		}

		// ── cfg API ─────────────────────────────────────────────────────

		int api_cfg_get( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushnil( L ); return 1; }

			const auto name = std::string( luaL_checkstring( L, 1 ) );
			auto& s = settings::g_movement;

			if ( name == "bhopEnabled" ) lua_pushboolean( L, s.bhop.value );
			else if ( name == "bhopLegit" ) lua_pushboolean( L, s.perfect_bunny_hop.value );
			else if ( name == "airstrafe" ) lua_pushboolean( L, s.airstrafe.value );
			else if ( name == "jumpbug" || name == "edgebug" || name == "landing_assist" ) lua_pushboolean( L, s.landing_assist.value );
			else if ( name == "fastladder" ) lua_pushboolean( L, s.fastladder.value );
			else if ( name == "slowwalk" ) lua_pushboolean( L, s.slowwalk.value );
			else lua_pushnil( L );

			return 1;
		}

		int api_cfg_set( lua_State* L )
		{
			if ( lua_gettop( L ) < 2 ) return 0;

			const auto name = std::string( luaL_checkstring( L, 1 ) );
			auto& s = settings::g_movement;

			if ( lua_isboolean( L, 2 ) )
			{
				const auto val = lua_toboolean( L, 2 ) != 0;
				if ( name == "bhopEnabled" ) s.bhop.value = val;
				else if ( name == "bhopLegit" ) s.perfect_bunny_hop.value = val;
				else if ( name == "airstrafe" ) s.airstrafe.value = val;
				else if ( name == "jumpbug" || name == "edgebug" || name == "landing_assist" ) s.landing_assist.value = val;
				else if ( name == "fastladder" ) s.fastladder.value = val;
				else if ( name == "slowwalk" ) s.slowwalk.value = val;
			}

			return 0;
		}

		// ── render API (2D overlay draw) ────────────────────────────────

		int api_render_text( lua_State* L )
		{
			if ( lua_gettop( L ) < 3 ) return 0;
			const auto text = std::string( luaL_checkstring( L, 1 ) );
			const auto x = static_cast< float >( luaL_checknumber( L, 2 ) );
			const auto y = static_cast< float >( luaL_checknumber( L, 3 ) );

			auto r = 255u, g = 255u, b = 255u, a = 255u;
			if ( lua_istable( L, 4 ) )
			{
				lua_getfield( L, 4, "r" ); r = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 4, "g" ); g = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 4, "b" ); b = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 4, "a" ); a = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
			}

			auto& dl = xdraw::get( xdraw::layer::top );
			dl.text( x, y, text.c_str( ), xdraw::color{
				static_cast< std::uint8_t >( r ),
				static_cast< std::uint8_t >( g ),
				static_cast< std::uint8_t >( b ),
				static_cast< std::uint8_t >( a ) } );
			return 0;
		}

		int api_render_rect( lua_State* L )
		{
			if ( lua_gettop( L ) < 5 ) return 0;
			const auto x = static_cast< float >( luaL_checknumber( L, 1 ) );
			const auto y = static_cast< float >( luaL_checknumber( L, 2 ) );
			const auto w = static_cast< float >( luaL_checknumber( L, 3 ) );
			const auto h = static_cast< float >( luaL_checknumber( L, 4 ) );
			const auto filled = lua_toboolean( L, 5 ) != 0;

			auto r = 255u, g = 255u, b = 255u, a = 255u;
			if ( lua_istable( L, 6 ) )
			{
				lua_getfield( L, 6, "r" ); r = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "g" ); g = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "b" ); b = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "a" ); a = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
			}

			auto& dl = xdraw::get( xdraw::layer::top );
			const xdraw::color col{
				static_cast< std::uint8_t >( r ),
				static_cast< std::uint8_t >( g ),
				static_cast< std::uint8_t >( b ),
				static_cast< std::uint8_t >( a ) };

			if ( filled )
			{
				dl.rect_filled( x, y, w, h, col, xdraw::corner_radius{ 0.0f } );
			}
			else
			{
				dl.rect( x, y, w, h, col, xdraw::corner_radius{ 0.0f }, 1.0f );
			}
			return 0;
		}

		int api_render_line( lua_State* L )
		{
			if ( lua_gettop( L ) < 5 ) return 0;
			const auto x1 = static_cast< float >( luaL_checknumber( L, 1 ) );
			const auto y1 = static_cast< float >( luaL_checknumber( L, 2 ) );
			const auto x2 = static_cast< float >( luaL_checknumber( L, 3 ) );
			const auto y2 = static_cast< float >( luaL_checknumber( L, 4 ) );
			const auto thickness = static_cast< float >( luaL_optnumber( L, 5, 1.0 ) );

			auto r = 255u, g = 255u, b = 255u, a = 255u;
			if ( lua_istable( L, 6 ) )
			{
				lua_getfield( L, 6, "r" ); r = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "g" ); g = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "b" ); b = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
				lua_getfield( L, 6, "a" ); a = static_cast< unsigned >( luaL_optinteger( L, -1, 255 ) ); lua_pop( L, 1 );
			}

			auto& dl = xdraw::get( xdraw::layer::top );
			dl.line( x1, y1, x2, y2, xdraw::color{
				static_cast< std::uint8_t >( r ),
				static_cast< std::uint8_t >( g ),
				static_cast< std::uint8_t >( b ),
				static_cast< std::uint8_t >( a ) }, thickness );
			return 0;
		}

		int api_render_screen_size( lua_State* L )
		{
			const auto [ w, h ] = xdraw::viewport_size( );
			lua_pushinteger( L, w );
			lua_pushinteger( L, h );
			return 2;
		}

		int api_render_text_size( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushinteger( L, 0 ); lua_pushinteger( L, 0 ); return 2; }
			const auto text = std::string( luaL_checkstring( L, 1 ) );
			const auto [ w, h ] = xdraw::measure_text( text.c_str( ) );
			lua_pushnumber( L, w );
			lua_pushnumber( L, h );
			return 2;
		}

		// ── input API ───────────────────────────────────────────────────

		int api_input_key_down( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushboolean( L, 0 ); return 1; }
			const auto vk = static_cast< int >( luaL_checkinteger( L, 1 ) );
			lua_pushboolean( L, ( GetAsyncKeyState( vk ) & 0x8000 ) != 0 );
			return 1;
		}

		// ── entity API ──────────────────────────────────────────────────

		int api_entity_get_local_pawn( lua_State* L )
		{
			const auto local = systems::g_local.get( );
			lua_pushinteger( L, static_cast< lua_Integer >( local.pawn ) );
			return 1;
		}

		int api_entity_get_local_controller( lua_State* L )
		{
			const auto local = systems::g_local.get( );
			lua_pushinteger( L, static_cast< lua_Integer >( local.controller ) );
			return 1;
		}

		int api_entity_get_health( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushinteger( L, 0 ); return 1; }
			const auto pawn = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			if ( !pawn ) { lua_pushinteger( L, 0 ); return 1; }
			lua_pushinteger( L, memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) );
			return 1;
		}

		int api_entity_get_team( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushinteger( L, 0 ); return 1; }
			const auto pawn = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			if ( !pawn ) { lua_pushinteger( L, 0 ); return 1; }
			lua_pushinteger( L, memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) ) );
			return 1;
		}

		int api_entity_get_origin( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 3; }
			const auto pawn = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			if ( !pawn ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 3; }
			const auto node = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( !node ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 3; }
			const auto origin = memory::read<math::vector3>( node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
			lua_pushnumber( L, origin.x );
			lua_pushnumber( L, origin.y );
			lua_pushnumber( L, origin.z );
			return 3;
		}

		int api_entity_get_velocity( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 3; }
			const auto pawn = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			if ( !pawn ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 3; }
			const auto vel = memory::read<math::vector3>( pawn + SCHEMA( "C_BaseEntity", "m_vecVelocity"_hash ) );
			lua_pushnumber( L, vel.x );
			lua_pushnumber( L, vel.y );
			lua_pushnumber( L, vel.z );
			return 3;
		}

		int api_entity_is_alive( lua_State* L )
		{
			if ( lua_gettop( L ) < 1 ) { lua_pushboolean( L, 0 ); return 1; }
			const auto pawn = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			if ( !pawn ) { lua_pushboolean( L, 0 ); return 1; }
			const auto health = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) );
			const auto life = memory::read<std::uint8_t>( pawn + SCHEMA( "C_BaseEntity", "m_lifeState"_hash ) );
			lua_pushboolean( L, health > 0 && life == 0 );
			return 1;
		}

		// ── movement API (only valid during on_create_move) ─────────────

		thread_local systems::input::usercmd* g_current_cmd{};

		int api_movement_is_on_ground( lua_State* L )
		{
			const auto& prestate = systems::g_prediction.pre( );
			lua_pushboolean( L, ( prestate.flags & cstypes::entity_flags::on_ground ) != 0 );
			return 1;
		}

		int api_movement_get_velocity( lua_State* L )
		{
			const auto& prestate = systems::g_prediction.pre( );
			lua_pushnumber( L, prestate.networked_velocity.x );
			lua_pushnumber( L, prestate.networked_velocity.y );
			lua_pushnumber( L, prestate.networked_velocity.z );
			return 3;
		}

		int api_movement_set_move( lua_State* L )
		{
			if ( !g_current_cmd || lua_gettop( L ) < 2 ) return 0;
			auto* base = g_current_cmd->csgo_user_cmd.mutable_base( );
			if ( !base ) return 0;
			features::movement::g_airstrafe.cancel_pending( );
			base->set_forwardmove( static_cast< float >( luaL_checknumber( L, 1 ) ) );
			base->set_leftmove( static_cast< float >( luaL_checknumber( L, 2 ) ) );
			return 0;
		}

		int api_movement_get_move( lua_State* L )
		{
			if ( !g_current_cmd ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 2; }
			auto* base = g_current_cmd->csgo_user_cmd.mutable_base( );
			if ( !base ) { lua_pushnumber( L, 0 ); lua_pushnumber( L, 0 ); return 2; }
			lua_pushnumber( L, base->forwardmove( ) );
			lua_pushnumber( L, base->leftmove( ) );
			return 2;
		}

		int api_cmd_set_button( lua_State* L )
		{
			if ( !g_current_cmd || lua_gettop( L ) < 2 ) return 0;
			const auto button = static_cast< std::uintptr_t >( luaL_checkinteger( L, 1 ) );
			const auto pressed = lua_toboolean( L, 2 ) != 0;
			const auto movement_mask = cstypes::command_buttons::in_forward | cstypes::command_buttons::in_back
				| cstypes::command_buttons::in_moveleft | cstypes::command_buttons::in_moveright;
			if ( button & movement_mask ) features::movement::g_airstrafe.cancel_pending( );
			if ( pressed )
			{
				g_current_cmd->buttons.value |= button;
				g_current_cmd->buttons.value_changed |= button;
			}
			else
			{
				g_current_cmd->buttons.value &= ~button;
				g_current_cmd->buttons.value_changed |= button;
			}
			return 0;
		}

		int api_cmd_get_buttons( lua_State* L )
		{
			if ( !g_current_cmd ) { lua_pushinteger( L, 0 ); return 1; }
			lua_pushinteger( L, static_cast< lua_Integer >( g_current_cmd->buttons.value ) );
			return 1;
		}

	} // anonymous namespace

	// ── lua_engine implementation ─────────────────────────────────────────

	lua_engine::~lua_engine( )
	{
		shutdown( );
	}

	bool lua_engine::initialize( )
	{
		if ( m_state ) return true;

		m_state = luaL_newstate( );
		if ( !m_state )
		{
			m_last_error = "luaL_newstate failed";
			return false;
		}

		luaL_openlibs( m_state );

		// Store engine pointer for callbacks
		auto** ud = static_cast< lua_engine** >( lua_newuserdata( m_state, sizeof( lua_engine* ) ) );
		*ud = this;
		lua_setglobal( m_state, "__engine_ptr" );

		register_api( );

		// Ensure script directory exists
		CreateDirectoryW( L"C:\\Vagal", nullptr );
		CreateDirectoryW( L"C:\\Vagal\\lua", nullptr );

		return true;
	}

	void lua_engine::shutdown( )
	{
		if ( !m_state ) return;

		for ( auto& script : m_scripts )
		{
			if ( script.loaded )
			{
				unload_script( script.name );
			}
		}

		lua_close( m_state );
		m_state = nullptr;
		m_scripts.clear( );
	}

	void lua_engine::register_api( )
	{
		if ( !m_state ) return;

		// Override print
		lua_register( m_state, "print", lua_print );

		// client table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_client_log );
		lua_setfield( m_state, -2, "log" );
		lua_setglobal( m_state, "client" );

		// cfg table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_cfg_get );
		lua_setfield( m_state, -2, "get" );
		lua_pushcfunction( m_state, api_cfg_set );
		lua_setfield( m_state, -2, "set" );
		lua_setglobal( m_state, "cfg" );

		// render table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_render_text );
		lua_setfield( m_state, -2, "text" );
		lua_pushcfunction( m_state, api_render_rect );
		lua_setfield( m_state, -2, "rect" );
		lua_pushcfunction( m_state, api_render_line );
		lua_setfield( m_state, -2, "line" );
		lua_pushcfunction( m_state, api_render_screen_size );
		lua_setfield( m_state, -2, "screen_size" );
		lua_pushcfunction( m_state, api_render_text_size );
		lua_setfield( m_state, -2, "text_size" );
		lua_setglobal( m_state, "render" );

		// input table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_input_key_down );
		lua_setfield( m_state, -2, "is_key_down" );
		lua_setglobal( m_state, "input" );

		// entity table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_entity_get_local_pawn );
		lua_setfield( m_state, -2, "get_local_player" );
		lua_pushcfunction( m_state, api_entity_get_local_controller );
		lua_setfield( m_state, -2, "get_local_controller" );
		lua_pushcfunction( m_state, api_entity_get_health );
		lua_setfield( m_state, -2, "get_health" );
		lua_pushcfunction( m_state, api_entity_get_team );
		lua_setfield( m_state, -2, "get_team" );
		lua_pushcfunction( m_state, api_entity_get_origin );
		lua_setfield( m_state, -2, "get_origin" );
		lua_pushcfunction( m_state, api_entity_get_velocity );
		lua_setfield( m_state, -2, "get_velocity" );
		lua_pushcfunction( m_state, api_entity_is_alive );
		lua_setfield( m_state, -2, "is_alive" );
		lua_setglobal( m_state, "entity" );

		// movement table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_movement_is_on_ground );
		lua_setfield( m_state, -2, "is_on_ground" );
		lua_pushcfunction( m_state, api_movement_get_velocity );
		lua_setfield( m_state, -2, "get_velocity" );
		lua_pushcfunction( m_state, api_movement_set_move );
		lua_setfield( m_state, -2, "set_move" );
		lua_pushcfunction( m_state, api_movement_get_move );
		lua_setfield( m_state, -2, "get_move" );
		lua_setglobal( m_state, "movement" );

		// cmd table
		lua_newtable( m_state );
		lua_pushcfunction( m_state, api_cmd_set_button );
		lua_setfield( m_state, -2, "set_button" );
		lua_pushcfunction( m_state, api_cmd_get_buttons );
		lua_setfield( m_state, -2, "get_buttons" );
		lua_setglobal( m_state, "cmd" );

		// buttons constants
		lua_newtable( m_state );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_attack ) );
		lua_setfield( m_state, -2, "attack" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_jump ) );
		lua_setfield( m_state, -2, "jump" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_duck ) );
		lua_setfield( m_state, -2, "duck" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_forward ) );
		lua_setfield( m_state, -2, "forward" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_back ) );
		lua_setfield( m_state, -2, "back" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_moveleft ) );
		lua_setfield( m_state, -2, "moveleft" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_moveright ) );
		lua_setfield( m_state, -2, "moveright" );
		lua_pushinteger( m_state, static_cast< lua_Integer >( cstypes::command_buttons::in_sprint ) );
		lua_setfield( m_state, -2, "speed" );
		lua_setglobal( m_state, "buttons" );

		// register_callback function
		lua_pushcfunction( m_state, [ ]( lua_State* L ) -> int {
			if ( lua_gettop( L ) < 2 ) return 0;
			const auto name = luaL_checkstring( L, 1 );
			if ( !lua_isfunction( L, 2 ) ) return 0;

			lua_getglobal( L, "callbacks" );
			if ( !lua_istable( L, -1 ) )
			{
				lua_newtable( L );
				lua_replace( L, -2 );
			}
			lua_pushvalue( L, 2 );
			lua_setfield( L, -2, name );
			lua_setglobal( L, "callbacks" );
			return 0;
		} );
		lua_setglobal( m_state, "register_callback" );
	}

	bool lua_engine::load_script( const std::string& name )
	{
		if ( !m_state )
		{
			m_last_error = "engine not initialized";
			return false;
		}

		// Build path
		std::string path = std::string( k_lua_dir ) + name + ".lua";

		// Check file exists
		if ( GetFileAttributesA( path.c_str( ) ) == INVALID_FILE_ATTRIBUTES )
		{
			m_last_error = "file not found: " + path;
			return false;
		}

		// Unload if already loaded
		unload_script( name );

		// Create callbacks table
		lua_newtable( m_state );
		lua_setglobal( m_state, "callbacks" );

		// Execute file
		if ( luaL_dofile( m_state, path.c_str( ) ) != LUA_OK )
		{
			m_last_error = std::string( "lua error: " ) + lua_tostring( m_state, -1 );
			console_print( name.c_str( ), m_last_error.c_str( ) );
			lua_pop( m_state, 1 );
			return false;
		}

		// Read callbacks
		lua_getglobal( m_state, "callbacks" );
		const auto has_callbacks = lua_istable( m_state, -1 );

		bool has_paint = false;
		bool has_menu = false;
		bool has_create_move = false;

		if ( has_callbacks )
		{
			lua_getfield( m_state, -1, "on_paint" );
			has_paint = lua_isfunction( m_state, -1 );
			lua_pop( m_state, 1 );

			lua_getfield( m_state, -1, "on_menu" );
			has_menu = lua_isfunction( m_state, -1 );
			lua_pop( m_state, 1 );

			lua_getfield( m_state, -1, "on_create_move" );
			has_create_move = lua_isfunction( m_state, -1 );
			lua_pop( m_state, 1 );
		}

		// Store callbacks in registry
		int cb_ref = LUA_REFNIL;
		if ( has_callbacks )
		{
			cb_ref = luaL_ref( m_state, LUA_REGISTRYINDEX );
		}
		else
		{
			lua_pop( m_state, 1 );
		}

		// Register script: update the scan entry in place when present.
		// scan_scripts owns the file list, and appending unconditionally
		// duplicated the row on every load (unload_script only flips the
		// loaded flag, the entry stays) - the ghost row only vanished on
		// the next refresh. In-place update also keeps the list order and
		// the menu selection index stable.
		script_info* existing = nullptr;
		for ( auto& script : m_scripts )
		{
			if ( script.name == name )
			{
				existing = &script;
				break;
			}
		}

		if ( existing )
		{
			existing->path = path;
			existing->loaded = true;
			existing->has_paint = has_paint;
			existing->has_menu = has_menu;
			existing->has_create_move = has_create_move;
		}
		else
		{
			script_info info;
			info.name = name;
			info.path = path;
			info.loaded = true;
			info.has_paint = has_paint;
			info.has_menu = has_menu;
			info.has_create_move = has_create_move;
			m_scripts.push_back( std::move( info ) );
		}

		m_last_error.clear( );
		return true;
	}

	void lua_engine::unload_script( const std::string& name )
	{
		for ( auto it = m_scripts.begin( ); it != m_scripts.end( ); ++it )
		{
			if ( it->name == name && it->loaded )
			{
				// Call on_unload if exists
				lua_getglobal( m_state, "callbacks" );
				if ( lua_istable( m_state, -1 ) )
				{
					lua_getfield( m_state, -1, "on_unload" );
					if ( lua_isfunction( m_state, -1 ) )
					{
						if ( lua_pcall( m_state, 0, 0, 0 ) != LUA_OK )
						{
							lua_pop( m_state, 1 );
						}
					}
					else
					{
						lua_pop( m_state, 1 );
					}
				}
				lua_pop( m_state, 1 );

				// Clean up globals the script may have created
				lua_pushnil( m_state );
				lua_setglobal( m_state, "callbacks" );

				it->loaded = false;
				break;
			}
		}
	}

	void lua_engine::reload_script( const std::string& name )
	{
		unload_script( name );
		load_script( name );
	}

	void lua_engine::scan_scripts( )
	{
		m_scripts.clear( );

		WIN32_FIND_DATAW find_data{};
		const auto handle = FindFirstFileW( ( std::wstring( L"C:\\Vagal\\lua\\" ) + L"*.lua" ).c_str( ), &find_data );
		if ( handle == INVALID_HANDLE_VALUE ) return;

		do
		{
			if ( find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue;

			char narrow[ 256 ]{};
			WideCharToMultiByte( CP_UTF8, 0, find_data.cFileName, -1, narrow, sizeof( narrow ), nullptr, nullptr );

			std::string name{ narrow };
			if ( name.size( ) > 4 ) name.resize( name.size( ) - 4 );

			script_info info;
			info.name = name;
			info.path = std::string( k_lua_dir ) + narrow;
			info.loaded = false;
			m_scripts.push_back( std::move( info ) );
		}
		while ( FindNextFileW( handle, &find_data ) );

		FindClose( handle );
	}

	void lua_engine::on_paint( )
	{
		if ( !m_state ) return;

		for ( auto& script : m_scripts )
		{
			if ( !script.loaded || !script.has_paint ) continue;

			m_current_script = script.name;
			lua_getglobal( m_state, "callbacks" );
			if ( !lua_istable( m_state, -1 ) ) { lua_pop( m_state, 1 ); continue; }
			lua_getfield( m_state, -1, "on_paint" );
			if ( !lua_isfunction( m_state, -1 ) ) { lua_pop( m_state, 2 ); continue; }

			if ( lua_pcall( m_state, 0, 0, 0 ) != LUA_OK )
			{
				m_last_error = std::string( "on_paint error: " ) + lua_tostring( m_state, -1 );
				console_print( script.name.c_str( ), m_last_error.c_str( ) );
				lua_pop( m_state, 1 );
			}
			lua_pop( m_state, 1 );
		}
		m_current_script.clear( );
	}

	void lua_engine::on_menu( )
	{
		if ( !m_state ) return;

		for ( auto& script : m_scripts )
		{
			if ( !script.loaded || !script.has_menu ) continue;

			m_current_script = script.name;
			lua_getglobal( m_state, "callbacks" );
			if ( !lua_istable( m_state, -1 ) ) { lua_pop( m_state, 1 ); continue; }
			lua_getfield( m_state, -1, "on_menu" );
			if ( !lua_isfunction( m_state, -1 ) ) { lua_pop( m_state, 2 ); continue; }

			if ( lua_pcall( m_state, 0, 0, 0 ) != LUA_OK )
			{
				m_last_error = std::string( "on_menu error: " ) + lua_tostring( m_state, -1 );
				console_print( script.name.c_str( ), m_last_error.c_str( ) );
				lua_pop( m_state, 1 );
			}
			lua_pop( m_state, 1 );
		}
		m_current_script.clear( );
	}

	void lua_engine::on_create_move( void* cmd, std::uintptr_t local_pawn )
	{
		if ( !m_state ) return;

		g_current_cmd = static_cast< systems::input::usercmd* >( cmd );

		for ( auto& script : m_scripts )
		{
			if ( !script.loaded || !script.has_create_move ) continue;

			m_current_script = script.name;
			lua_getglobal( m_state, "callbacks" );
			if ( !lua_istable( m_state, -1 ) ) { lua_pop( m_state, 1 ); continue; }
			lua_getfield( m_state, -1, "on_create_move" );
			if ( !lua_isfunction( m_state, -1 ) ) { lua_pop( m_state, 2 ); continue; }

			lua_pushinteger( m_state, static_cast< lua_Integer >( reinterpret_cast< std::uintptr_t >( cmd ) ) );
			lua_pushinteger( m_state, static_cast< lua_Integer >( local_pawn ) );

			if ( lua_pcall( m_state, 2, 0, 0 ) != LUA_OK )
			{
				m_last_error = std::string( "on_create_move error: " ) + lua_tostring( m_state, -1 );
				console_print( script.name.c_str( ), m_last_error.c_str( ) );
				lua_pop( m_state, 1 );
			}
			lua_pop( m_state, 1 );
		}

		m_current_script.clear( );
		g_current_cmd = nullptr;
	}

	void lua_engine::on_frame_stage_notify( int stage )
	{
		if ( !m_state ) return;

		for ( auto& script : m_scripts )
		{
			if ( !script.loaded ) continue;

			m_current_script = script.name;
			lua_getglobal( m_state, "callbacks" );
			if ( !lua_istable( m_state, -1 ) ) { lua_pop( m_state, 1 ); continue; }
			lua_getfield( m_state, -1, "on_frame_stage_notify" );
			if ( !lua_isfunction( m_state, -1 ) ) { lua_pop( m_state, 2 ); continue; }

			lua_pushinteger( m_state, stage );

			if ( lua_pcall( m_state, 1, 0, 0 ) != LUA_OK )
			{
				m_last_error = std::string( "on_frame_stage_notify error: " ) + lua_tostring( m_state, -1 );
				console_print( script.name.c_str( ), m_last_error.c_str( ) );
				lua_pop( m_state, 1 );
			}
			lua_pop( m_state, 1 );
		}
		m_current_script.clear( );
	}

} // namespace features::scripting
