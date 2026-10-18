#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rendering {

	class model_preview_renderer
	{
	public:
		bool initialize( ID3D11Device* device, ID3D11DeviceContext* context, HMODULE module );
		void shutdown( );
		void render( ) { }
		void on_frame_stage_notify( );
		void on_ui_transition( );
		void capture_texture( ID3D11ShaderResourceView* texture );
		void set_team( int team ) { m_team.store( team, std::memory_order_release ); }
		[[nodiscard]] int team( ) const { return m_team.load( std::memory_order_acquire ); }
		[[nodiscard]] bool is_preview_player( std::uint32_t owner_hash, std::uintptr_t owner_entity );
		[[nodiscard]] bool bone_point( std::string_view name, float& x, float& y ) const;
		[[nodiscard]] ID3D11ShaderResourceView* texture( ) const;
		[[nodiscard]] const char* status_text( ) const;
		[[nodiscard]] bool waiting_for_texture( ) const;
		[[nodiscard]] bool fallback_capture_window( ) const;
	private:
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv{};
		mutable std::mutex m_mutex{};
		bool m_initialized{};
		bool m_script_injected{};
		std::uint32_t m_refresh_counter{};
		bool m_panorama_ready{};
		std::uintptr_t m_script_root{};
		std::uint64_t m_script_tick{};
		std::uintptr_t m_preview_panel{};
		std::uintptr_t m_preview_player{};
		struct projected_bone { const char* name{}; float x{}, y{}; bool valid{}; };
		std::array<projected_bone, 16> m_bones{};
		std::atomic<int> m_team{ 2 };
	};

	inline model_preview_renderer g_model_preview_renderer{};

	enum class startup_stage : std::uint8_t
	{
		bootstrap,
		configuration,
		runtime_services,
		addresses,
		systems,
		economy,
		hooks,
		cvars,
		skyboxes,
		complete
	};

	class startup_progress
	{
	public:
		[[nodiscard]] std::uint32_t report( const startup_stage stage )
		{
			const auto state = this->m_state.load( std::memory_order_relaxed );
			const auto generation = ( state >> 8u ) + 1u;
			this->m_state.store(
				( generation << 8u ) | static_cast< std::uint32_t >( stage ),
				std::memory_order_release );
			return generation;
		}

		[[nodiscard]] std::uint32_t snapshot( ) const
		{
			return this->m_state.load( std::memory_order_acquire );
		}

		[[nodiscard]] static startup_stage stage_from( const std::uint32_t state )
		{
			return static_cast< startup_stage >( state & 0xffu );
		}

		[[nodiscard]] static std::uint32_t generation_from( const std::uint32_t state )
		{
			return state >> 8u;
		}

		void acknowledge_display( const std::uint32_t generation )
		{
			auto acknowledged = this->m_displayed_generation.load( std::memory_order_relaxed );
			while ( acknowledged < generation &&
				!this->m_displayed_generation.compare_exchange_weak(
					acknowledged,
					generation,
					std::memory_order_release,
					std::memory_order_relaxed ) )
			{
			}
		}

		[[nodiscard]] bool is_display_acknowledged( const std::uint32_t generation ) const
		{
			return this->m_displayed_generation.load( std::memory_order_acquire ) >= generation;
		}

		void mark_runtime_ready( )
		{
			this->m_runtime_ready.store( true, std::memory_order_release );
		}

		[[nodiscard]] bool is_runtime_ready( ) const
		{
			return this->m_runtime_ready.load( std::memory_order_acquire );
		}

	private:
		std::atomic< std::uint32_t > m_state{ static_cast< std::uint32_t >( startup_stage::bootstrap ) };
		std::atomic< std::uint32_t > m_displayed_generation{};
		std::atomic_bool m_runtime_ready{};
	};

	class context
	{
	public:
		bool initialize( IDXGISwapChain* swap_chain );
		void shutdown( );
		void on_present( IDXGISwapChain* swap_chain );
		void on_resize_buffers( );
		void on_resize_buffers_post( IDXGISwapChain* swap_chain );

		[[nodiscard]] HWND get_window( ) const { return this->m_window; }
		[[nodiscard]] ID3D11Device* get_device( ) const { return this->m_device; }
		[[nodiscard]] ID3D11DeviceContext* get_context( ) const { return this->m_context; }
		[[nodiscard]] bool is_initialized( ) const { return this->m_initialized; }
		[[nodiscard]] bool ui_assets_ready( ) const { return this->m_ui_assets_ready; }
        ID3D11RenderTargetView* get_rtv( ) const { return this->m_rtv; }

	private:
		void create_rtv( IDXGISwapChain* swap_chain );
		void setup_zdraw( HWND window );
		bool try_bind_ui_assets( );

		ID3D11Device* m_device{ nullptr };
		ID3D11DeviceContext* m_context{ nullptr };
		ID3D11RenderTargetView* m_rtv{ nullptr };
		HWND m_window{ nullptr };
		bool m_initialized{ false };
		bool m_ui_assets_ready{ false };
	};

	namespace detail
	{
		// 配置页（menu.config.cpp）与顶栏配置下拉（menu.core.cpp）共用这一份列表：
		// 配置页加载/保存/删除后置 needs_refresh，两侧下一帧都会重新拉取。
		extern std::vector<std::wstring> config_list;
		extern int selected;
		extern bool needs_refresh;
	}

    class menu
    {
    public:
        void initialize_graphics( );

        void draw( );
        void shutdown( ) const;

		void toggle( )
		{
			if ( this->m_intro_finished )
			{
				this->m_open = !this->m_open;
			}
		}
		[[nodiscard]] bool is_open( ) const { return this->m_intro_finished && this->m_open; }
		[[nodiscard]] bool is_startup_finished( ) const { return this->m_intro_finished; }
		void apply_saved_cursor( );

        enum class tab : int
        {
            ragebot, legitbot, player, world, skins, misc, config, script, count
        };

    private:
		bool draw_intro( );
        void draw_side_bar( float h );
        void draw_top_bar( float w );
        void apply_theme_preset( int preset );
        void sync_theme_style( ) const;
        void draw_search_results( float x, float y, float w, float h );
        void rebuild_search_index( );
        void close_search( );
        void activate_search_result( std::size_t index );

        void draw_ragebot( float group_w ) const;
        void draw_legitbot( float group_w ) const;
        void draw_player( float group_w ) const;
        void draw_world( float group_w ) const;
        void draw_skins( float group_w ) const;
        void draw_misc( float group_w ) const;
        void draw_config( float group_w );
        void draw_script( float group_w );
		void draw_profile_page( float group_w );

		bool m_open{};
        bool m_config_modal_open{};
        bool m_config_cloud_refresh_pending{};
        bool m_config_advanced_open{};
		bool m_last_open{};
		float m_open_anim{};
        std::uint8_t m_saved_relative_mouse{};
		bool m_has_saved_cursor{};
		int m_saved_cursor_x{};
		int m_saved_cursor_y{};

        float m_x{ 100.0f };
        float m_y{ 100.0f };
		float m_w{ 820.0f };
		float m_h{ 600.0f };
        float m_body_x{};
        float m_body_y{};
        float m_body_w{};
        float m_body_h{};

        int m_tab{};
        int m_subtab{};
		float m_intro_visible_elapsed{};
		float m_intro_displayed_progress{};
		float m_intro_stage_elapsed{};
		float m_intro_complete_elapsed{};
		std::uint32_t m_intro_stage_generation{};
		bool m_graphics_initialized{};
		bool m_intro_finished{};
		bool m_search_open{};
		bool m_profile_page{};
		bool m_config_items_dirty{ true };
		std::vector<std::string> m_config_names{};
		std::vector<const char*> m_config_items{};
		int m_theme_preset{};
		std::string m_search_query{};
		std::vector<std::size_t> m_search_visible_indices{};

		struct search_entry
		{
			std::string name{};
			std::string category{};
			std::string name_lower{};
			std::string category_lower{};
			int tab{};
			int subtab{};
			int bind_key{};
			bool profile_page{};
		};
		std::vector<search_entry> m_search_entries{};

        struct textures
        {
            struct entry
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource{};
                int width{};
                int height{};
            };

            entry menu_header{};
            entry tabs[ static_cast< std::size_t >( tab::count ) ]{};
            entry search{};
            entry settings{};
            entry cfg_folder_on{};
            entry cfg_folder_off{};
            entry cfg_cloud_on{};
            entry cfg_cloud_off{};
            entry cfg_plus{};
            entry startup_logo{};
		} m_textures{};

        static constexpr auto k_max_subtabs{ 6 };

        struct subtab_info
        {
            const char* names[ k_max_subtabs ]{};
            int count{};
        };

        static constexpr subtab_info k_subtab_defs[ static_cast< int >( tab::count ) ]
        {
            { { "pistol", "smg", "rifle", "shotgun", "sniper", "lmg" }, 6 },
            { { "pistol", "smg", "rifle", "shotgun", "sniper", "lmg" }, 6 },
            { { "enemies", "allies", "local" },                         3 },
            { { "esp", "scene", "weather" },                            3 },
            { { },                                                              0 },
			{ { "main", "removals" },                                   2 },
            { { "general" },                                            1 },
            { { },                                                      0 }
        };
    };

	class widgets
	{
	public:
		void draw( );
		void interact_external( xdraw::draw_list& draw_list, std::string_view id, float& x, float& y, float w, float h, float& scale );
		void draw_external_grip( xdraw::draw_list& draw_list, std::string_view id, float x, float y, float w, float h, float scale );

		static inline std::string s_map_name{};

	private:
		void watermark( xdraw::draw_list& draw_list );
		void deepseek_character( xdraw::draw_list& draw_list );
		void duck_indicator( xdraw::draw_list& draw_list );
		void slow_indicator( xdraw::draw_list& draw_list );
		void hitchance_indicator( xdraw::draw_list& draw_list );
		void min_damage_indicator( xdraw::draw_list& draw_list );
		void low_fps_indicator( xdraw::draw_list& draw_list );
		void keybinds( xdraw::draw_list& draw_list );
	};

	class fonts
	{
	public:
		enum class size : std::uint8_t
		{
			petite,
			normal,
			big,
			count
		};

		struct family_t
		{
			std::array<xdraw::font*, static_cast< std::size_t >( size::count )> sizes{ };

			xdraw::font* operator[]( size size ) const { return this->sizes[ static_cast< std::size_t >( size ) ]; }
			xdraw::font*& operator[]( size size ) { return this->sizes[ static_cast< std::size_t >( size ) ]; }
		};

		void initialize( );

		family_t inter_medium{};
		family_t inter_bold{};
		family_t smallest_pixel7{};

	private:
		void load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes );
		void attach_fallback( family_t& family, const family_t& fallback ) const;

		family_t cjk_regular{};
	};

	inline context g_context{};
	inline startup_progress g_startup_progress{};
	inline menu g_menu{};
	inline widgets g_widgets{};
	inline fonts g_fonts{};

} // namespace rendering
