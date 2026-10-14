#pragma once

#include <string>
#include <vector>
#include <functional>

struct lua_State;

namespace features::scripting {

	struct script_info
	{
		std::string name;
		std::string path;
		bool loaded{};
		bool has_paint{};
		bool has_menu{};
		bool has_create_move{};
	};

	class lua_engine
	{
	public:
		lua_engine( ) = default;
		~lua_engine( );

		lua_engine( const lua_engine& ) = delete;
		lua_engine& operator=( const lua_engine& ) = delete;

		bool initialize( );
		void shutdown( );

		bool load_script( const std::string& name );
		void unload_script( const std::string& name );
		void reload_script( const std::string& name );

		void on_paint( );
		void on_menu( );
		void on_create_move( void* cmd, std::uintptr_t local_pawn );
		void on_frame_stage_notify( int stage );

		void scan_scripts( );
		[[nodiscard]] const std::vector<script_info>& scripts( ) const { return m_scripts; }

		[[nodiscard]] bool initialized( ) const { return m_state != nullptr; }
		[[nodiscard]] const std::string& last_error( ) const { return m_last_error; }
		[[nodiscard]] const std::string& last_log( ) const { return m_last_log; }
		void set_last_log( const std::string& msg ) { m_last_log = msg; }
		void set_last_error( const std::string& err ) { m_last_error = err; }
		[[nodiscard]] const std::string& current_script_name( ) const { return m_current_script; }
		void set_current_script( const std::string& name ) { m_current_script = name; }

	private:
		void register_api( );
		void unregister_callbacks( const std::string& name );

		static constexpr const char* k_lua_dir{ "C:\\Vagal\\lua\\" };

		lua_State* m_state{};
		std::vector<script_info> m_scripts{};
		std::string m_last_error{};
		std::string m_last_log{};
		std::string m_current_script{};
	};

	inline lua_engine g_lua{};

} // namespace features::scripting
