#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/diag.hpp>
#include <utilities/hooking/hooking.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/security/security.hpp>
#include <core/rendering/rendering.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/features/scripting/scripting.hpp>
#include <core/features/esp/primitive_buffer.hpp>
#include <protection/game_addresses.hpp>
#include <protection/offsets.hpp>
#include "../hooks.hpp"

namespace hooks {

	namespace {

		// owner_entity -> schema hash 小缓存，仅渲染线程访问（is_glowing/get_glow_color/generate_primitives
		// 都跑在引擎渲染路径上）；level 切换时清空。避免对每个对象每帧重复做
		// 4 层 SEH 内存读 + 字符串哈希（该开销在 glow/chams 全关时原本也全额发生）。
		struct schema_hash_cache
		{
			static constexpr auto k_slots{ 256u };

			struct slot
			{
				std::uintptr_t key{};
				std::uint32_t hash{};
				bool resolved{};
			};

			slot m_slots[ k_slots ]{};

			void clear( )
			{
				for ( auto& s : this->m_slots )
				{
					s.key = 0;
					s.hash = 0;
					s.resolved = false;
				}
			}

			[[nodiscard]] std::uint32_t find( std::uintptr_t key ) const
			{
				const auto& s = this->m_slots[ ( key >> 4 ) & ( k_slots - 1 ) ];
				return s.key == key && s.resolved ? s.hash : ~0u;
			}

			void insert( std::uintptr_t key, std::uint32_t hash )
			{
				auto& s = this->m_slots[ ( key >> 4 ) & ( k_slots - 1 ) ];
				s.key = key;
				s.hash = hash;
				s.resolved = true;
			}
		};

		schema_hash_cache g_schema_hash_cache{};

		[[nodiscard]] std::uint32_t cached_owner_hash( std::uintptr_t owner_entity )
		{
			const auto table_hash = g_schema_hash_cache.find( owner_entity );
			if ( table_hash != ~0u )
			{
				return table_hash;
			}

			systems::entities::cached cached{};
			if ( systems::g_entities.find_by_ptr( owner_entity, cached ) )
			{
				g_schema_hash_cache.insert( owner_entity, cached.schema_hash );
				return cached.schema_hash;
			}

			const auto name = systems::g_entities.get_schema_name( owner_entity );
			const auto hash = name ? fnv1a::runtime_hash( name ) : 0;
			g_schema_hash_cache.insert( owner_entity, hash );
			return hash;
		}

	} // namespace

	bool cheat::initialize_rendering( )
	{
		return hooking::manager::create( {
			{ &m_present, &present, xs( "present" ), addresses::functions::present },
			{ &m_resize_buffers, &resize_buffers, xs( "resize_buffers" ), addresses::functions::resize_buffers }
		} );
	}

	bool cheat::initialize_feature_hooks( )
	{
		// GetInterpolatedShootPosition runs synchronously during firing; keep it on the engine path.
		const hooking::manager::entry feature_hooks[] {
			{ &m_cmd_interpreter, &cmd_interpreter, xs ("cmd_interpreter"), PATTERN (patterns::cmd_interpreter) },
			{ &m_frame_stage_notify, &frame_stage_notify, xs ("frame_stage_notify"), PATTERN (patterns::frame_stage_notify) },
			{ &m_create_move, &create_move, xs ("create_move"), PATTERN (patterns::create_move) },
			{ &m_handle_view_angles, &handle_view_angles, xs ("handle_view_angles"), PATTERN (patterns::handle_view_angles) },
			{ &m_add_entity, &add_entity, xs ("add_entity"), PATTERN (patterns::add_entity) },
			{ &m_remove_entity, &remove_entity, xs ("remove_entity"), PATTERN (patterns::remove_entity) },
			{ &m_render_view, &render_view, xs ("render_view"), PATTERN (patterns::render_view) },
			{ &m_draw_skybox_array, &draw_skybox_array, xs ("draw_skybox_array"), PATTERN (patterns::draw_skybox_array) },
			{ &m_light_scene_object, &light_scene_object, xs ("light_scene_object"), PATTERN (patterns::light_scene_object) },
			{ &m_draw_scene_object_array, &draw_scene_object_array, xs ("draw_scene_object_array"), PATTERN (patterns::draw_scene_object_array) },
			{ &m_draw_scene_object, &draw_scene_object, xs ("draw_scene_object"), PATTERN (patterns::draw_scene_object) },
			{ &m_is_glowing, &is_glowing, xs ("is_glowing"), PATTERN (patterns::is_glowing) },
			{ &m_get_glow_color, &get_glow_color, xs ("get_glow_color"), PATTERN (patterns::get_glow_color) },
			{ &m_get_resource_view, &get_resource_view, xs ("get_resource_view"), PATTERN (patterns::get_resource_view) },
			{ &m_generate_primitives, &generate_primitives, xs ("generate_primitives"), PATTERN (patterns::generate_primitives) },
			{ &m_parse_report_hit, &parse_report_hit, xs ("parse_report_hit"), PATTERN (patterns::parse_report_hit) },
			{ &m_setup_fog, &setup_fog, xs ("setup_fog"), PATTERN (patterns::setup_fog) },
			{ &m_set_shader_param, &set_shader_param, xs ("set_shader_param"), PATTERN (patterns::set_shader_param) },
			{ &m_set_postprocess_vec, &set_postprocess_vec, xs ("set_postprocess_vec"), PATTERN (patterns::set_postprocess_vec) },
			{ &m_override_view, &override_view, xs ("override_view"), PATTERN (patterns::override_view) },
			{ &m_camera_services_calc_view, &camera_services_calc_view, xs ("camera_services_calc_view"), PATTERN (patterns::update_fov_sensitivity) },
			{ &m_get_viewmodel_offsets, &get_viewmodel_offsets, xs ("get_viewmodel_offsets"), PATTERN (patterns::get_viewmodel_offsets) },
			{ &m_render_scope, &render_scope, xs ("render_scope"), PATTERN (patterns::render_scope) },
			{ &m_render_crosshair, &render_crosshair, xs ("render_crosshair"), PATTERN (patterns::render_crosshair) },
			{ &m_prepare_scene_material, &prepare_scene_material, xs ("prepare_scene_material"), PATTERN (patterns::prepare_scene_material) },
			{ &m_update_animation_state, &update_animation_state, xs ("update_animation_state"), PATTERN (patterns::update_animation_state) },
			{ &m_pawn_on_land, &pawn_on_land, xs ("pawn_on_land"), PATTERN (patterns::pawn_on_land) },
			{ &m_post_network_data_received, &post_network_data_received, xs ("post_network_data_received"), PATTERN (patterns::post_network_data_received) },
			{ &m_draw_overhead, &draw_overhead, xs ("draw_overhead"), PATTERN (patterns::draw_overhead) },
			{ &m_draw_legs, &draw_legs, xs ("draw_legs"), PATTERN (patterns::draw_legs) },
			{ &m_get_transforms_for_hitbox_list, &get_transforms_for_hitbox_list, xs ("get_transforms_for_hitbox_list"), PATTERN (patterns::get_transforms_for_hitbox_list) },
			{ &m_sort_primitives, &sort_primitives, xs ("sort_primitives"), PATTERN (patterns::sort_primitives) },
			{ &m_level_initialization, &level_initialization, xs ("level_initialization"), PATTERN (patterns::level_initialization) },
			{ &m_level_shutdown, &level_shutdown, xs ("level_shutdown"), PATTERN (patterns::level_shutdown) },
			{ &m_read_frame_input, &read_frame_input, xs ("read_frame_input"), PATTERN (patterns::read_frame_input) },
			{ &m_process_input_event, &process_input_event, xs ("process_input_event"), PATTERN (patterns::process_input_event) },
			{ &m_render_decals, &render_decals, xs ("render_decals"), PATTERN (patterns::render_decals) },
			{ &m_render_smoke, &render_smoke, xs ("render_smoke"), PATTERN (patterns::render_smoke) },
			{ &m_draw_flash_effect, &draw_flash_effect, xs ("draw_flash_effect"), PATTERN (patterns::draw_flash_effect) },
			{ &m_set_info, &set_info, xs ("set_info"), PATTERN (patterns::set_info) }
		};

		auto unavailable_hooks = 0u;
		for (const auto& entry : feature_hooks) {
			if (!hooking::manager::create ({ entry })) {
				++unavailable_hooks;
				logging::console::print (xs ("skipping unavailable hook: {}"), entry.name);
			}
		}

		systems::g_prediction.set_animation_isolation_ready( m_update_animation_state.is_enabled( ) );
		systems::g_prediction.set_landing_observation_ready( m_pawn_on_land.is_enabled( ) );
		logging::console::print( xs( "prediction landing observer: hooked={}" ), m_pawn_on_land.is_enabled( ) );
		logging::console::print( xs( "prediction animation isolation: hooked={}" ), m_update_animation_state.is_enabled( ) );

		if ( unavailable_hooks ) {
			logging::console::print (
				xs ("feature hooks initialized with {} unavailable"),
				unavailable_hooks);
		}

		return true;
	}

	void cheat::shutdown( )
	{
		systems::g_prediction.set_animation_isolation_ready( false );
		systems::g_prediction.set_landing_observation_ready( false );
		m_pawn_on_land.reset( );
		m_update_animation_state.reset( );
		m_wnd_proc.reset( );
		m_om_set_render_targets.reset( );
		m_present.reset( );
		m_resize_buffers.reset( );
		m_cmd_interpreter.reset( );
		m_frame_stage_notify.reset( );
		m_create_move.reset( );
		m_handle_view_angles.reset( );
		m_add_entity.reset( );
		m_remove_entity.reset( );
		m_render_view.reset( );
		m_draw_skybox_array.reset( );
		m_light_scene_object.reset( );
		m_draw_scene_object_array.reset( );
		m_draw_scene_object.reset( );
		m_is_glowing.reset( );
		m_get_glow_color.reset( );
		m_get_resource_view.reset( );
		m_generate_primitives.reset( );
		m_parse_report_hit.reset( );
		m_setup_fog.reset( );
		m_set_shader_param.reset( );
		m_set_postprocess_vec.reset( );
		m_override_view.reset( );
		m_camera_services_calc_view.reset( );
		m_get_viewmodel_offsets.reset( );
		m_render_scope.reset( );
		m_render_crosshair.reset( );
		m_prepare_scene_material.reset( );
		m_post_network_data_received.reset( );
		m_draw_overhead.reset( );
		m_draw_legs.reset( );
		m_get_transforms_for_hitbox_list.reset( );
		m_sort_primitives.reset( );
		m_get_inaccuracy.reset( );
		m_get_interpolated_shoot_position.reset( );
		m_level_initialization.reset( );
		m_level_shutdown.reset( );
		m_read_frame_input.reset( );
		m_process_input_event.reset( );
		m_render_decals.reset( );
		m_render_smoke.reset( );
		m_render_smoke_map.reset( );
		m_render_smoke_unmap.reset( );
		m_draw_flash_effect.reset( );
		m_set_info.reset( );
	}

	HRESULT __fastcall cheat::present( IDXGISwapChain* thisptr, UINT sync_interval, UINT flags )
	{
		rendering::g_context.on_present( thisptr );

		if ( rendering::g_startup_progress.is_runtime_ready( ) &&
			!m_wnd_proc.is_enabled( ) && rendering::g_context.get_window( ) )
		{
			if ( m_wnd_proc.create( reinterpret_cast< void* >( GetWindowLongPtrW( rendering::g_context.get_window( ), GWLP_WNDPROC ) ), &wnd_proc ) )
			{
				m_wnd_proc.enable( );
			}
		}

		if ( rendering::g_startup_progress.is_runtime_ready( ) &&
			!m_om_set_render_targets.is_enabled( ) && rendering::g_context.is_initialized( ) )
		{
			const auto om_addr = memory::get_vfunc( reinterpret_cast< std::uintptr_t >( rendering::g_context.get_context( ) ), 33 );
			if ( m_om_set_render_targets.create( reinterpret_cast< void* >( om_addr ), &om_set_render_targets ) )
			{
				m_om_set_render_targets.enable( );
			}
		}

		return m_present.call<HRESULT>( thisptr, sync_interval, flags );
	}

	HRESULT __fastcall cheat::resize_buffers( IDXGISwapChain* thisptr, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags )
	{
		rendering::g_context.on_resize_buffers( );

		const auto result = m_resize_buffers.call<long>( thisptr, buffer_count, width, height, new_format, swap_chain_flags );
		if ( SUCCEEDED( result ) )
		{
			rendering::g_context.on_resize_buffers_post( thisptr );
		}

		return result;
	}

	LRESULT __stdcall cheat::wnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
	{
		if ( msg == WM_ACTIVATE && LOWORD( wparam ) != WA_INACTIVE && rendering::g_menu.is_open( ) && rendering::g_context.get_window( ) == hwnd )
		{
			if ( addresses::globals::input_system )
			{
				memory::call_vfunc<void>( addresses::globals::input_system, 76, false );
			}

			SetCursor( LoadCursor( nullptr, IDC_ARROW ) );
			rendering::g_menu.apply_saved_cursor( );
		}

		if ( msg == WM_KEYDOWN && !( lparam & ( 1 << 30 ) ) && static_cast< int >( wparam ) == VK_INSERT )
		{
			rendering::g_menu.toggle( );
			return 0;
		}

		// 4:3 等非原生宽高比下，WM_MOUSEMOVE 的客户区坐标与渲染视口坐标不一致：
		// 黑边模式（引擎把 D3D viewport 缩到屏幕中央）需要减去视口原点；
		// 拉伸模式（低分辨率背缓冲被拉伸到全屏窗口）需要按 视口/客户区 缩放。
		// 仅在菜单打开时变换，避免影响游戏自身输入。
		if ( rendering::g_menu.is_open( ) && rendering::g_context.get_window( ) == hwnd )
		{
			switch ( msg )
			{
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			{
				RECT rc{};
				if ( GetClientRect( hwnd, &rc ) )
				{
					const auto client_w = rc.right - rc.left;
					const auto client_h = rc.bottom - rc.top;
					const auto [ vp_w, vp_h ] = xdraw::viewport_size( );
					const auto [ vp_x, vp_y ] = xdraw::viewport_offset( );

					auto mx = static_cast< float >( static_cast< short >( LOWORD( lparam ) ) );
					auto my = static_cast< float >( static_cast< short >( HIWORD( lparam ) ) );

					if ( vp_w > 0 && vp_h > 0 && client_w > 0 && client_h > 0 )
					{
						if ( vp_x != 0 || vp_y != 0 )
						{
							// 黑边：绘制原点在视口左上角，直接平移。
							mx -= static_cast< float >( vp_x );
							my -= static_cast< float >( vp_y );
						}
						else if ( vp_w != client_w || vp_h != client_h )
						{
							// 拉伸：视口铺满背缓冲，但窗口客户区更大，按比例缩放。
							mx *= static_cast< float >( vp_w ) / static_cast< float >( client_w );
							my *= static_cast< float >( vp_h ) / static_cast< float >( client_h );
						}

						lparam = MAKELPARAM(
							static_cast< SHORT >( std::clamp( mx, -32768.0f, 32767.0f ) ),
							static_cast< SHORT >( std::clamp( my, -32768.0f, 32767.0f ) ) );
					}
				}

				break;
			}
			default:
				break;
			}
		}

		xui::wndproc( msg, wparam, lparam );

		if ( rendering::g_menu.is_open( ) )
		{
			switch ( msg )
			{
			case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
			case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
			case WM_MOUSEMOVE:
				return 0;
			default:
				break;
			}
		}

		return m_wnd_proc.call<LRESULT>( hwnd, msg, wparam, lparam );
	}

	void __stdcall cheat::om_set_render_targets( ID3D11DeviceContext* ctx, UINT num_views, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv )
	{
		m_om_set_render_targets.call<void>( ctx, num_views, rtvs, dsv );
	}

	void __fastcall cheat::cmd_interpreter( std::uintptr_t render_thread, std::uintptr_t item, std::uint8_t flag )
	{
		m_cmd_interpreter.call<void>( render_thread, item, flag );
	}

	void __fastcall cheat::frame_stage_notify( std::uintptr_t thisptr, int stage )
	{
		if ( systems::g_entities.is_empty( ) )
		{
			systems::g_entities.force_update( );
		}

		features::misc::g_invalid_steam_logon.on_frame_stage_notify( );

		// 原实现每个 frame stage（每帧 10-16 次）都重建 local 快照。
		// stage 6 之前没有消费方读取 local；stage 6 之后本帧内数据不再变化，
		// 收敛到 stage 6 单次更新，present 与 stage 12 复用同一快照。
		if ( stage == 6 )
		{
			systems::g_local.update( );
		}

		if ( systems::g_local.get( ).is_valid( ) && systems::g_view.has_camera( ) )
		{
			if ( stage == 6 )
			{
				features::changer::g_guns.on_frame_stage_notify( );
			}

			if ( stage == 7 )
			{
				features::changer::g_agents.on_frame_stage_notify( );
				features::changer::g_gloves.on_frame_stage_notify( );
				features::changer::g_knives.on_frame_stage_notify( );

				features::world::g_scene.on_frame_stage_notify( );
				features::world::g_weather.on_frame_stage_notify( );
				features::misc::g_other.on_frame_stage_notify( );
				features::misc::g_impacts.on_frame_stage_notify( );

				
			}
		}

		{
			static auto was_active{ false };
			const auto is_active = settings::g_misc.m_removals.skybox_3d.value;

			if ( is_active != was_active )
			{
				CONVAR ("r_draw3dskybox")->m_value.i1 = is_active;
				was_active = is_active;
			}
		}

		// Source 2 copies dynamic-light entries into scene objects during this stage.
		// Publish our entry first, while keeping all manager mutations on the game thread.
		// Stage 6 is not emitted consistently while the main menu is active. Render
		// start (12) remains available there and lets the preview bind to the lobby
		// Panorama root before a match has been entered.
		if ( stage == 6 || stage == 12 )
		{
			rendering::g_model_preview_renderer.on_frame_stage_notify( );
			if ( stage == 6 ) features::misc::g_dlight.on_frame_stage_notify( );
		}

		m_frame_stage_notify.call<void>( thisptr, stage );

		// The current frame's world-to-projection matrix is published by the
		// engine during render-start stage 12.
		if ( stage == 12 )
		{
			systems::g_view.update_matrix( );
			systems::g_frame_data.update( );
		}

		if (systems::g_local.get ().is_valid () && systems::g_view.has_camera ()) {
			if (stage == 6) {
				// Capture lag records only after Source 2 has committed this network update,
				// so the simulation timestamp, world origin and evaluated bones agree.
				features::combat::g_shared.lc( ).run( );
				features::esp::player::g_chams.bt( ).update( );
				features::esp::player::g_chams.os ().update ();

				features::misc::g_scoreboard_weapons.on_frame_stage_notify ();
				features::misc::g_other.do_kill_feed_preservation( );
			}
		}
	}

	ID3D11ShaderResourceView* __fastcall cheat::get_resource_view( void* thisptr, int** resource, char a3, char a4, const char* name )
	{
		auto* view = m_get_resource_view.call<ID3D11ShaderResourceView*>( thisptr, resource, a3, a4, name );
		static std::atomic_bool hook_seen{};
		if ( !hook_seen.exchange( true, std::memory_order_relaxed ) )
			logging::console::print( xs( "[model_preview] GetResourceView hook active" ) );

		if ( view )
		{
			char texture_name[128]{};
			char argument_name[128]{};
			const auto copy_safe_string = []( const char* source, char ( &output )[128] )
			{
				if ( !source ) return;
				for ( std::size_t i{}; i + 1 < sizeof( output ); ++i )
				{
					const auto ch = memory::safe_read<char>( reinterpret_cast<std::uintptr_t>( source ) + i ).value_or( '\0' );
					output[i] = ch;
					if ( ch == '\0' ) break;
				}
			};
			const auto field = memory::safe_read<std::uintptr_t>( reinterpret_cast<std::uintptr_t>( resource ) + sizeof( void* ) ).value_or( 0 );
			const auto text = field ? memory::safe_read<const char*>( field ).value_or( nullptr ) : nullptr;
			copy_safe_string( text, texture_name );
			copy_safe_string( name, argument_name );
			const auto is_named_preview = std::strstr( texture_name, "vagal_model_preview" ) ||
				std::strstr( argument_name, "vagal_model_preview" );
			if ( is_named_preview )
			{
				rendering::g_model_preview_renderer.capture_texture( view );
			}
			else if ( std::strstr( texture_name, "scratchrendertarget" ) &&
				rendering::g_model_preview_renderer.fallback_capture_window( ) )
			{
				Microsoft::WRL::ComPtr<ID3D11Resource> resource_object;
				view->GetResource( resource_object.GetAddressOf() );
				Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_2d;
				if ( resource_object && SUCCEEDED( resource_object.As( &texture_2d ) ) )
				{
					D3D11_TEXTURE2D_DESC desc{};
					texture_2d->GetDesc( &desc );
					if ( desc.Width == 512 && desc.Height == 512 )
					{
						logging::console::print( xs( "[model_preview] stable fallback target '{}'" ), texture_name );
						rendering::g_model_preview_renderer.capture_texture( view );
					}
				}
			}
		}
		return view;
	}

	void __fastcall cheat::create_move( std::uintptr_t thisptr, int slot, std::uintptr_t command )
	{
		const auto local = systems::g_local.get( );

		if ( !local.pawn || !local.controller )
		{
			return m_create_move.call<void>( thisptr, slot, command );
		}

		// The caller marks ordinary commands as type 2 before invoking slot 25.
		// It is not a reason to skip native CreateMove or the feature pipeline.
		const auto cmd = reinterpret_cast<systems::input::usercmd*>( command );

		m_create_move.call<void>( thisptr, slot, command );

		{
			features::combat::g_shared.invalidate_if_needed( );
			features::combat::g_legit.invalidate_if_needed( );
			features::combat::g_misc.quickpeek( ).reset_if_needed( );
		}

		if ( !local.is_alive || !systems::g_view.has_camera( ) )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto last_cmd_processed = memory::read<std::uint32_t>( movement_services + SCHEMA( "CPlayer_MovementServices", "m_nLastCommandNumberProcessed"_hash ) );
		if ( !last_cmd_processed )
		{
			return;
		}

		// AA_FIRE_DIAGNOSTICS_BEGIN
		// Read-only, command-correlated samples. Toggling AA rearms a bounded capture.
		static thread_local int diagnostic_aa_mode{ -1 };
		static thread_local unsigned diagnostic_reports{};
		const auto diagnostic_aa = static_cast<int>( settings::g_combat.m_antiaim.enabled.value );
		if ( diagnostic_aa_mode != diagnostic_aa )
		{
			diagnostic_aa_mode = diagnostic_aa;
			diagnostic_reports = 0;
		}
		const auto trace_combat = slot == 0 && cmd && cmd->csgo_user_cmd.has_base( )
			&& settings::g_combat.m_ragebot.enabled.value
			&& cmd->command_number % 31 == 0 && diagnostic_reports < 240;
		if ( trace_combat ) ++diagnostic_reports;
		const auto trace_command = [ & ]( const char* stage )
		{
			if ( !trace_combat ) return;
			const auto* base = cmd->csgo_user_cmd.base( );
			if ( !base ) return;
			const auto* angles = base->viewangles( );
			const auto input = systems::g_input.get_view_angles( );
			constexpr auto camera_offset = sdk::offsets::client::dwViewAngles - sdk::offsets::client::dwCSGOInput;
			const auto camera = memory::safe_read<math::vector3>( thisptr + camera_offset ).value_or( math::vector3{} );
			const auto& history = cmd->csgo_user_cmd.m_input_history;
			const auto attack1 = cmd->csgo_user_cmd.attack1_start_history_index( );
			const auto attack2 = cmd->csgo_user_cmd.attack2_start_history_index( );
			const auto index = attack1 >= 0 ? attack1 : ( attack2 >= 0 ? attack2 : history.size( ) - 1 );
			const proto::input_history_entry* entry{};
			if ( history.m_rep && index >= 0 && index < history.size( ) && index < history.m_rep->allocated_size )
				entry = proto::impl_ptr<proto::input_history_entry>( static_cast<const void*>( history.m_rep->elements[ index ] ) );
			const auto* history_angles = entry ? entry->view_angles( ) : nullptr;
			const auto* buttons = base->buttons_pb( );
			const auto& weapon = features::combat::g_shared.ctx( );
			const auto& config = settings::g_combat.m_ragebot.get_group( weapon.weapon_type );
			const auto tick = memory::safe_read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) ).value_or( -1 );
			diag::writef( diag::level::info,
				"[aa-fire-diag:v1] cmd=%d stage=%s aa=%d active=%d hide=%d ns=%d silent=%d dt=%d "
				"input=%.3f/%.3f camera=%.3f/%.3f base=%.3f/%.3f/%.3f "
				"hist=%d idx=%d/%d sampled=%d hangle=%.3f/%.3f/%.3f player=%d+%.5f render=%d "
				"buttons=%llx/%llx/%llx pb=%llx/%llx/%llx mask=%x steps=%d "
				"tick=%d client=%d next=%d last=%d clip=%d reload=%d weapon=%u valid=%d submitted=%d stop=%d",
				cmd->command_number, stage, diagnostic_aa, static_cast<int>( features::combat::g_misc.antiaim( ).is_active( ) ),
				static_cast<int>( settings::g_combat.m_antiaim.hide_shots.value ), static_cast<int>( config.no_spread.value ),
				static_cast<int>( config.silent.value ), static_cast<int>( settings::g_combat.m_ragebot.m_double_tap.enabled.value ),
				input.x, input.y, camera.x, camera.y, angles ? angles->x( ) : 0.0f, angles ? angles->y( ) : 0.0f, angles ? angles->z( ) : 0.0f,
				history.size( ), attack1, attack2, index, history_angles ? history_angles->x( ) : 0.0f,
				history_angles ? history_angles->y( ) : 0.0f, history_angles ? history_angles->z( ) : 0.0f,
				entry ? entry->player_tick_count( ) : -1, entry ? entry->player_tick_fraction( ) : 0.0f, entry ? entry->render_tick_count( ) : -1,
				static_cast<unsigned long long>( cmd->buttons.value ), static_cast<unsigned long long>( cmd->buttons.value_changed ),
				static_cast<unsigned long long>( cmd->buttons.value_scroll ),
				static_cast<unsigned long long>( buttons ? buttons->buttonstate1( ) : 0 ),
				static_cast<unsigned long long>( buttons ? buttons->buttonstate2( ) : 0 ),
				static_cast<unsigned long long>( buttons ? buttons->buttonstate3( ) : 0 ),
				cmd->csgo_user_cmd.m_has_bits.bits[ 0 ], base->subtick_moves_size( ), tick, base->client_tick( ),
				weapon.next_primary_attack_tick, features::combat::g_shared.last_shoot_tick( ), weapon.clip,
				static_cast<int>( weapon.in_reload ), static_cast<unsigned>( weapon.item_def_idx ), static_cast<int>( weapon.valid ),
				static_cast<int>( features::combat::g_rage.is_firing_this_tick( ) ), static_cast<int>( features::combat::g_rage.stop_mode( ) ) );
		};
		trace_command( "native" );
		// AA_FIRE_DIAGNOSTICS_END
		systems::g_input.update( cmd );
		{
			const auto current_cmd = systems::g_input.get( );
			if ( !current_cmd || !current_cmd->csgo_user_cmd.has_base( ) )
			{
				return;
			}

			static std::atomic_bool first_create_move_traced{};
			const auto trace = !first_create_move_traced.exchange( true, std::memory_order_relaxed );
			diag::exception_scope exception_scope{ "create_move: desubtick" };
			if ( trace )
			{
				diag::step( "create_move: feature pipeline begin" );
			}

			systems::g_input.desubtick( current_cmd );
			systems::g_prediction.capture_prestate( local.pawn, movement_services );
			features::movement::g_movement_fix.begin( current_cmd );

			{
				diag::set_exception_phase( "create_move: shared update" );
				features::combat::g_shared.update( );

				diag::set_exception_phase( "create_move: combat misc" );
				features::combat::g_misc.antiaim( ).on_create_move( current_cmd );
				// AA_FIRE_DIAGNOSTICS_BEGIN
				trace_command( "aa" );
				// AA_FIRE_DIAGNOSTICS_END
			}
			if ( trace )
			{
				diag::step( "create_move: shared and combat misc ready" );
			}

			{
				diag::set_exception_phase( "create_move: pre-combat movement" );
				features::movement::g_slowwalk.on_create_move( current_cmd );
				features::movement::g_fast_stop.on_create_move( current_cmd );
				features::movement::g_edgebug.on_create_move( current_cmd );
				features::movement::g_edgejump.on_create_move( current_cmd );
				features::movement::g_edgestop.on_create_move( current_cmd );
				features::movement::g_bhop.on_create_move( current_cmd );
				features::movement::g_fastladder.on_create_move( current_cmd );
				if ( trace )
				{
					diag::step( "create_move: pre-combat movement end" );
					diag::step( "create_move: rage begin" );
				}

				{
					diag::set_exception_phase( "create_move: rage" );
					features::combat::g_rage.on_create_move( current_cmd );
					// AA_FIRE_DIAGNOSTICS_BEGIN
					trace_command( "rage" );
					// AA_FIRE_DIAGNOSTICS_END
					if ( trace )
					{
						diag::step( "create_move: rage end" );
						diag::step( "create_move: legit begin" );
					}
					features::movement::g_jumpbug.on_create_move( current_cmd );
					features::combat::g_misc.autostop( ).on_create_move( current_cmd );
					diag::set_exception_phase( "create_move: legit" );
					features::combat::g_legit.on_create_move( current_cmd );
					if ( trace )
					{
						diag::step( "create_move: legit end" );
					}
				}

				diag::set_exception_phase( "create_move: post-combat movement" );
				features::combat::g_misc.duckpeek( ).on_create_move( current_cmd );
				features::movement::g_airstrafe.on_create_move( current_cmd );
				features::misc::g_projectile_trajectory.on_create_move( current_cmd );
			}
			if ( trace )
			{
				diag::step( "create_move: post-combat movement end" );
			}

			diag::set_exception_phase( "create_move: quickpeek" );
			features::combat::g_misc.quickpeek( ).on_create_move( current_cmd );
			if ( trace )
			{
				diag::step( "create_move: quickpeek end" );
				diag::step( "create_move: final subtick begin" );
			}

			diag::set_exception_phase( "create_move: lua scripts" );
			features::scripting::g_lua.on_create_move( current_cmd, local.pawn );

			diag::set_exception_phase( "create_move: movement fix" );
			features::movement::g_movement_fix.finish( current_cmd );

			// Build the movement anchor and DT pulse before predicting landing input.
			// apply() mirrors buttons and calculates the final move CRC.
			diag::set_exception_phase( "create_move: input movement anchor" );
			systems::g_input.prepare_subtick_movement( );
			diag::set_exception_phase( "create_move: double tap finalize" );
			features::combat::g_rage.dt( ).finalize_command( current_cmd );
			diag::set_exception_phase( "create_move: landing candidates" );
			features::movement::g_edgebug.finish_prediction( current_cmd );
			features::movement::g_jumpbug.finish_prediction( current_cmd );
			diag::set_exception_phase( "create_move: perfect bhop" );
			features::movement::g_bhop.finish_prediction( current_cmd );
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_command( "pre-shadow" );
			// AA_FIRE_DIAGNOSTICS_END
			diag::set_exception_phase( "create_move: no-spread state finalize" );
			features::combat::g_rage.finalize_no_spread_state( current_cmd, local );
			// AA_FIRE_DIAGNOSTICS_BEGIN
			trace_command( "post-shadow" );
			// AA_FIRE_DIAGNOSTICS_END
			if ( trace )
			{
				diag::step( "create_move: final subtick end" );
			}

		}
		static std::atomic_bool first_input_apply_traced{};
		const auto trace_apply =
			!first_input_apply_traced.exchange( true, std::memory_order_relaxed );
		if ( trace_apply )
		{
			diag::step( "create_move: input apply begin" );
		}
		diag::exception_scope exception_scope{ "create_move: input apply" };
		systems::g_input.apply( );
		// AA_FIRE_DIAGNOSTICS_BEGIN
		trace_command( "applied" );
		// AA_FIRE_DIAGNOSTICS_END
		if ( trace_apply )
		{
			diag::step( "create_move: input apply end" );
		}
	}

	std::uintptr_t __fastcall cheat::handle_view_angles( std::uintptr_t thisptr, int slot )
	{
		if ( slot != 0 )
		{
			return m_handle_view_angles.call<std::uintptr_t>( thisptr, slot );
		}

		const auto view_angles = *memory::call<const math::vector3*>( PATTERN( patterns::get_view_angles ), thisptr, slot );
		const auto result = m_handle_view_angles.call<std::uintptr_t>( thisptr, slot );

		// The native setter restores the last input frame, not the fixed camera, when frames exist.
		memory::call<void>( PATTERN( patterns::set_view_angles ), thisptr, slot, &view_angles );
		constexpr auto camera_angles_offset = sdk::offsets::client::dwViewAngles - sdk::offsets::client::dwCSGOInput;
		memory::write<math::vector3>( thisptr + camera_angles_offset, view_angles );
		return result;
	}

	void __fastcall cheat::add_entity( std::uintptr_t thisptr, std::uintptr_t entity, std::uint32_t handle )
	{
		systems::g_entities.on_add_entity( entity, handle );

		m_add_entity.call<void>( thisptr, entity, handle );
	}

	void __fastcall cheat::remove_entity( std::uintptr_t thisptr, std::uintptr_t entity, std::uint32_t handle )
	{
		systems::g_entities.on_remove_entity( entity, handle );

		m_remove_entity.call<void>( thisptr, entity, handle );
	}

	void __fastcall cheat::render_view( std::uintptr_t thisptr )
	{
		m_render_view.call<void>( thisptr );

		systems::g_view.update( thisptr + 0x10 );
		systems::g_frame_data.update( );
	}

	void __fastcall cheat::draw_skybox_array( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t mesh_array, int mesh_count, int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		features::world::g_scene.on_draw_skybox_array_pre( mesh_array, mesh_count );

		m_draw_skybox_array.call<void>( thisptr, a2, mesh_array, mesh_count, a5, a6, a7, a8 );

		features::world::g_scene.on_draw_skybox_array_post( );
	}

	std::uintptr_t __fastcall cheat::light_scene_object( std::uintptr_t thisptr, std::uintptr_t object, std::uintptr_t a3 )
	{
		features::world::g_scene.on_light_scene_object_pre( object );
		features::misc::g_dlight.apply_scene_color( object );

		const auto result = m_light_scene_object.call<std::uintptr_t>( thisptr, object, a3 );

		return result;
	}

	void __fastcall cheat::draw_scene_object_array( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t object_array )
	{
		m_draw_scene_object_array.call<void>( thisptr, a2, object_array );

		diag::exception_scope exception_scope{ "world: aggregate records" };
		features::world::g_scene.on_draw_scene_object_array( object_array );
	}

	std::uintptr_t __fastcall cheat::draw_scene_object( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t batch, int batch_count, int a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8 )
	{
		{
			diag::exception_scope exception_scope{ "world: primitive tint" };
			features::world::g_scene.on_draw_scene_object( batch, batch_count );
		}

		return m_draw_scene_object.call<std::uintptr_t>( a1, a2, batch, batch_count, a5, a6, a7, a8 );
	}

	bool __fastcall cheat::is_glowing( std::uintptr_t glow_property )
	{
		if ( glow_property )
		{
			const auto owner_entity = memory::read<std::uintptr_t>( glow_property + 0x18 );
			if ( owner_entity )
			{
				const auto owner_hash = cached_owner_hash( owner_entity );
				if ( owner_hash )
				{
					if ( features::esp::player::g_glow.on_is_glowing( owner_entity, owner_hash ) )
					{
						return true;
					}

					if ( features::esp::item::g_glow.on_is_glowing( owner_entity, owner_hash ) )
					{
						return true;
					}
				}
			}
		}

		return m_is_glowing.call<bool>( glow_property );
	}

	void __fastcall cheat::get_glow_color( std::uintptr_t glow_property, float* color )
	{
		if ( glow_property )
		{
			const auto owner_entity = memory::read<std::uintptr_t>( glow_property + 0x18 );
			if ( owner_entity )
			{
				const auto owner_hash = cached_owner_hash( owner_entity );
				if ( owner_hash )
				{
					if ( features::esp::player::g_glow.on_get_glow_color( owner_entity, owner_hash, color ) )
					{
						return;
					}

					if ( features::esp::item::g_glow.on_get_glow_color( owner_entity, owner_hash, color ) )
					{
						return;
					}
				}
			}
		}

		m_get_glow_color.call<void>( glow_property, color );
	}

	void __fastcall cheat::generate_primitives( std::uintptr_t thisptr, std::uintptr_t scene_object, std::uintptr_t scene_view, std::uintptr_t primitive_buffer )
	{
		diag::exception_scope exception_scope{ "chams: generate primitives" };

		if ( scene_object )
		{
			if ( features::esp::player::g_chams.bt( ).is_active( scene_object ) )
			{
				return;
			}

			if ( features::esp::player::g_chams.os( ).is_active( scene_object ) ) 
			{
				return;
			}

			const auto owner_handle = memory::read<std::uint32_t>( scene_object + 0xc0 );
			if ( owner_handle )
			{
				const auto owner_entity = systems::g_entities.lookup( owner_handle );
				if ( owner_entity )
				{
					const auto owner_hash = cached_owner_hash( owner_entity );
					if ( owner_hash )
					{
						if ( rendering::g_model_preview_renderer.is_preview_player( owner_hash, owner_entity ) )
						{
							const auto& cfg = rendering::g_model_preview_renderer.team( ) == 3
								? settings::g_esp.m_player.m_chams.team
								: settings::g_esp.m_player.m_chams.enemy;
							if ( features::esp::player::g_chams.on_preview_generate_primitives( cfg, scene_object, primitive_buffer,
								m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ), thisptr, scene_view ) )
								return;
						}
						if ( features::esp::player::g_chams.on_generate_primitives( owner_entity, owner_hash, scene_object, primitive_buffer, m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ), thisptr, scene_view ) )
						{
							return;
						}

						if ( features::esp::item::g_chams.on_generate_primitives( owner_entity, owner_hash, scene_object, primitive_buffer, m_generate_primitives.original<void( __fastcall* )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t )>( ), thisptr, scene_view ) )
						{
							return;
						}
					}
				}
			}
		}

		m_generate_primitives.call<void>( thisptr, scene_object, scene_view, primitive_buffer );
	}

	std::uintptr_t __fastcall cheat::parse_report_hit( std::uintptr_t thisptr, std::uint8_t deleting )
	{
		// Capture the protobuf fields before the deleting destructor can free them.
		features::misc::g_impacts.on_report_hit( thisptr );

		return m_parse_report_hit.call<std::uintptr_t>( thisptr, deleting );
	}

	std::uintptr_t __fastcall cheat::setup_fog( __m128i* output, int* mode )
	{
		if ( features::world::g_scene.on_setup_fog( output, mode ) )
		{
			return 0;
		}

		return m_setup_fog.call<std::uintptr_t>( output, mode );
	}

	std::uintptr_t __fastcall cheat::set_shader_param( __m128i* map, std::uint32_t hash, __m128i* value )
	{
		features::world::g_scene.on_set_shader_param( value, hash );

		return m_set_shader_param.call<std::uintptr_t>( map, hash, value );
	}

	std::uintptr_t __fastcall cheat::set_postprocess_vec( __m128i* map, std::uint32_t hash, __m128i* value )
	{
		constexpr std::uint32_t dof_ranges{ 0x2ACAB07C };

		// The engine only publishes DofRanges when the active camera enables DOF.
		// Insert it alongside any post-process vector, matching Artisan's live path.
		if ( settings::g_world.m_scene.dof.value && hash != dof_ranges )
		{
			__m128i* dof_value{};
			features::world::g_scene.on_set_shader_param( dof_value, dof_ranges );
			if ( dof_value )
			{
				m_set_postprocess_vec.call<std::uintptr_t>( map, dof_ranges, dof_value );
			}
		}

		features::world::g_scene.on_set_shader_param( value, hash );
		return m_set_postprocess_vec.call<std::uintptr_t>( map, hash, value );
	}

	void __fastcall cheat::override_view( std::uintptr_t thisptr, std::uintptr_t view_setup )
	{
		math::vector3 steering_view{};
		const auto lock_steering_view = features::combat::g_misc.antiaim( ).get_steering_view( steering_view );
		if ( lock_steering_view ) systems::g_input.set_view_angles( steering_view );

		m_override_view.call<void>( thisptr, view_setup );

		if ( lock_steering_view )
		{
			// Third-person positioning reads input angles; the rendered camera
			// already has its own copy. Freeze both before camera features run.
			systems::g_input.set_view_angles( steering_view );
			auto rendered_angles = memory::read<math::vector3>( view_setup + 0x4b8 );
			rendered_angles.x = steering_view.x;
			rendered_angles.y = steering_view.y;
			memory::write<math::vector3>( view_setup + 0x4b8, rendered_angles );
		}

		// Removals first: the recoil removal rewrites the full view angles, so the
		// camera lean must run after it to keep its roll from being overwritten.
		features::misc::g_removals.on_override_view( view_setup );
		features::misc::g_camera.on_override_view( view_setup );
		features::combat::g_misc.duckpeek( ).on_override_view( view_setup );
	}

	void __fastcall cheat::camera_services_calc_view( std::uintptr_t camera_services, math::vector3* origin, math::vector3* angles, float* z_near, float* z_far, float* fov )
	{
		const auto local = systems::g_local.get( );
		const auto local_camera_services = local.pawn && local.is_alive
			? memory::safe_read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pCameraServices"_hash ) ).value_or( 0 ) : 0;
		const auto separate_angles = camera_services && camera_services == local_camera_services && angles
			&& addresses::globals::csgo_input && settings::g_combat.m_antiaim.enabled.value
			&& features::combat::g_misc.antiaim( ).is_active( )
			&& !systems::g_local.is_in_cinematic( ) && !systems::g_local.is_in_time_freeze( );
		math::vector3 input_angles{}, native_eye_angles{}, eye_scratch{};
		bool restore_angles{};
		if ( separate_angles )
		{
			// CalcView starts from pawn eye angles, not CCSGOInput's restored camera.
			// Read the same native accessor before it adds punch/shake/other view effects.
			input_angles = systems::g_input.get_view_angles( );
			if ( const auto eye = memory::call_vfunc<const math::vector3*>(
				local.pawn, sdk::layout::player_pawn_camera_angles, &eye_scratch ) )
			{
				native_eye_angles = *eye;
				restore_angles = std::isfinite( input_angles.x ) && std::isfinite( input_angles.y )
					&& std::isfinite( native_eye_angles.x ) && std::isfinite( native_eye_angles.y );
			}
		}

		// Keep the six-argument ABI and every native origin/clip/FOV output.
		m_camera_services_calc_view.call<void>( camera_services, origin, angles, z_near, z_far, fov );
		if ( !local.pawn || !local.is_alive ) return;
		if ( camera_services != local_camera_services ) return;

		if ( restore_angles )
		{
			// Replace only the command-angle base; retain native pitch/yaw deltas and roll.
			// Fresh input also keeps per-frame mouse motion and non-silent aim responsive.
			angles->x += input_angles.x - native_eye_angles.x;
			angles->y = math::helpers::normalize_yaw( angles->y
				+ math::helpers::normalize_yaw( input_angles.y - native_eye_angles.y ) );
			systems::g_input.set_view_angles( input_angles );
			constexpr auto camera_angles_offset = sdk::offsets::client::dwViewAngles - sdk::offsets::client::dwCSGOInput;
			memory::write<math::vector3>( addresses::globals::csgo_input + camera_angles_offset, input_angles );

			static unsigned reports{};
			if ( reports < 4 )
			{
				++reports;
				diag::writef( diag::level::info, "[aa-camera] input=%.3f/%.3f eye=%.3f/%.3f render=%.3f/%.3f",
					input_angles.x, input_angles.y, native_eye_angles.x, native_eye_angles.y, angles->x, angles->y );
			}
		}
		features::misc::g_camera.update_fov_sensitivity( local.pawn );
	}

	void __fastcall cheat::get_viewmodel_offsets( std::uintptr_t viewmodel, float* offsets, float* fov )
	{
		m_get_viewmodel_offsets.call<void>( viewmodel, offsets, fov );

		const auto& cfg = settings::g_misc.m_viewmodel_adjust;
		if ( !cfg.enabled.value || !offsets )
		{
			return;
		}

		__try
		{
			offsets[ 0 ] += cfg.offset_x.value;
			offsets[ 1 ] += cfg.offset_y.value;
			offsets[ 2 ] += cfg.offset_z.value;

			if ( fov )
			{
				*fov = cfg.fov.value;
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
		}
	}

	void __fastcall cheat::render_scope( std::uintptr_t a1, std::uintptr_t a2 )
	{
		m_render_scope.call<void>( a1, a2 );

		if ( settings::g_misc.m_removals.scope.value )
		{
			memory::write<std::uint8_t>( a2 + 4, 0 );
		}
	}

	bool __fastcall cheat::render_crosshair( std::uintptr_t a1 )
	{
		if ( settings::g_misc.m_removals.crosshair.value )
		{
			return false;
		}

		return m_render_crosshair.call<bool>( a1 );
	}

	float __fastcall cheat::prepare_scene_material( std::uintptr_t material, void* a2, float a3 )
	{
		features::misc::g_removals.on_prepare_scene_material( material );

		return m_prepare_scene_material.call<float>( material, a2, a3 );
	}

	void __fastcall cheat::update_animation_state( std::uintptr_t thisptr )
	{
		// FinishMove also updates the live graph; trial predictions must not do so.
		if ( systems::g_prediction.is_simulating( ) ) return;
		m_update_animation_state.call<void>( thisptr );
	}

	void __fastcall cheat::pawn_on_land( std::uintptr_t pawn, float fall_speed )
	{
		// Observe only: never mute or skip the engine's real landing callback.
		if ( systems::g_prediction.is_simulating_pawn( pawn ) )
		{
			const auto services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			const auto buttons_offset = SCHEMA( "CPlayer_MovementServices", "m_nButtons"_hash );
			const auto held = services && buttons_offset > 0
				&& ( memory::read<systems::input::in_button_state>( services + buttons_offset ).value & cstypes::command_buttons::in_duck ) != 0;
			systems::g_prediction.observe_landing( pawn, held );
		}
		m_pawn_on_land.call<void>( pawn, fall_speed );
	}

	void __fastcall cheat::post_network_data_received( std::uintptr_t thisptr )
	{
		m_post_network_data_received.call<void>( thisptr );
	}

	bool __fastcall cheat::draw_overhead( std::uintptr_t pawn, std::uint32_t player_slot )
	{
		if ( settings::g_misc.m_removals.overhead.value && pawn == systems::g_local.get( ).pawn )
		{
			return false;
		}

		return m_draw_overhead.call<bool>( pawn, player_slot );
	}

	std::uintptr_t __fastcall cheat::draw_legs( std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5 )
	{
		if ( settings::g_misc.m_removals.legs.value )
		{
			return 0;
		}

		return m_draw_legs.call<std::uintptr_t>( a1, a2, a3, a4, a5 );
	}

	bool __fastcall cheat::get_transforms_for_hitbox_list( std::uintptr_t a1, std::uintptr_t a2, int* a3 )
	{
		if ( !features::combat::g_shared.autowalling( ) )
		{
			return m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );
		}

		const auto record = features::combat::g_shared.current_autowall_record( );
		if ( !record || !record->valid )
		{
			return m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );
		}

		const auto count = memory::safe_read<int>( reinterpret_cast< std::uintptr_t >( a3 ) ).value_or( 0 );
		const auto shape_array = memory::safe_read<std::uintptr_t>( reinterpret_cast< std::uintptr_t >( a3 ) + 8 ).value_or( 0 );
		const auto entity_bone_cache = memory::safe_read<std::uintptr_t>( a1 + 0x1c0 ).value_or( 0 );
		const auto model_handle = memory::safe_read<std::uintptr_t>( a1 + 0x1e0 ).value_or( 0 );
		const auto model = model_handle ? memory::safe_read<std::uintptr_t>( model_handle ).value_or( 0 ) : 0;

		if ( count <= 0 || count > 256 || !shape_array || !entity_bone_cache || !model )
		{
			return false;
		}

		static const auto get_bone_index = PATTERN( patterns::get_bone_index );
		if ( !get_bone_index )
		{
			return false;
		}

		// The client dereferences every resolved transform without checking the
		// backing cache. Reject a stale scene node instead of faulting in it.
		for ( auto i = 0; i < count; ++i )
		{
			const auto shape_ptr = shape_array + 16ull * i;
			const auto bone_index = memory::call<int>( get_bone_index, model, shape_ptr );

			if ( bone_index >= 0 &&
				( bone_index >= 256 ||
					!memory::safe_read<systems::bones::data>( entity_bone_cache + sizeof( systems::bones::data ) * bone_index ) ) )
			{
				return false;
			}
		}

		const auto target_scene = record->game_scene_node ? record->game_scene_node : memory::read<std::uintptr_t>( record->pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		const auto target_bone_cache = target_scene ? memory::safe_read<std::uintptr_t>( target_scene + SCHEMA( "CSkeletonInstance", "m_modelState"_hash ) + 0x80 ).value_or( 0 ) : 0;
		const auto result = m_get_transforms_for_hitbox_list.call<bool>( a1, a2, a3 );

		if ( !result )
		{
			return false;
		}

		const auto is_plausible = [ ]( std::uintptr_t ptr ) noexcept -> bool
			{
				return ptr >= 0x10000ull && ptr < 0x00007FFFFFFFFFFFull;
			};

		if ( !is_plausible( entity_bone_cache ) || !is_plausible( target_bone_cache ) || entity_bone_cache != target_bone_cache )
		{
			return result;
		}

		const auto output_array = memory::safe_read<std::uintptr_t>( a2 + 16 ).value_or( 0 );

		if ( !output_array || count <= 0 )
		{
			return result;
		}

		for ( auto i = 0; i < count; ++i )
		{
			const auto shape_ptr = shape_array + 16ull * i;
			const auto bone_index = memory::call<int>( get_bone_index, model, shape_ptr );

			if ( bone_index < 0 || bone_index >= record->bone_count )
			{
				continue;
			}

			const auto dst = output_array + 32ull * i;
			std::memcpy( reinterpret_cast< void* >( dst ), &record->bones[ bone_index ], 32 );
		}

		return true;
	}

	void __fastcall cheat::sort_primitives( std::uintptr_t thisptr, std::uintptr_t a2, std::uintptr_t a3, std::uint32_t a4 )
	{
		m_sort_primitives.call<void>( thisptr, a2, a3, a4 );

		diag::exception_scope exception_scope{ "chams: sort primitives" };
		features::esp::player::g_chams.on_sort_primitives( a3, a4 );
	}

	float __fastcall cheat::get_inaccuracy( std::uintptr_t thisptr, float* a2, float* a3 )
	{
		const auto result = m_get_inaccuracy.call<float>( thisptr, a2, a3 );

#if defined(__clang__) || defined(__GNUC__)
		const auto result_address = reinterpret_cast< std::uintptr_t >( __builtin_return_address( 0 ) );
#else
		const auto result_address = reinterpret_cast< std::uintptr_t >( _ReturnAddress( ) );
#endif

		static const auto base_fire_guns_get_inaccuracy = PATTERN( patterns::base_fire_guns_get_inaccuracy );
		if ( !systems::g_prediction.is_simulating( ) && !features::combat::g_rage.is_no_spread_state_prediction( ) && base_fire_guns_get_inaccuracy &&
			result_address > base_fire_guns_get_inaccuracy &&
			result_address < base_fire_guns_get_inaccuracy + 0x600 )
		{
			features::misc::g_impacts.on_base_fire_guns_get_inaccuracy( thisptr, result );
		}

		return result;
	}

	float* __fastcall cheat::get_interpolated_shoot_position( std::uintptr_t thisptr, float* out, int* tick_frac )
	{
		const auto result = m_get_interpolated_shoot_position.call<float*>( thisptr, out, tick_frac );

		// Prediction calls this helper while building the next command as well.
		// Only the call made for an actual rage shot is its authoritative origin.
		if ( !systems::g_prediction.is_simulating( ) && features::combat::g_rage.is_firing_this_tick( )
			&& !features::combat::g_rage.is_no_spread_state_prediction( ) )
		{
			features::misc::g_impacts.on_get_interpolated_shoot_position( thisptr, out );
		}

		return result;
	}

	void __fastcall cheat::level_initialization( std::uintptr_t a1, const char* new_map )
	{
		rendering::g_model_preview_renderer.on_ui_transition( );

		if ( new_map && new_map[ 0 ] )
		{
			const char* leaf = std::strrchr( new_map, '/' );
			rendering::g_widgets.s_map_name = leaf ? leaf + 1 : new_map;
		}
		else
		{
			rendering::g_widgets.s_map_name.clear( );
		}

		features::world::g_scene.reset_skybox_state( );
		features::world::g_scene.invalidate_material_cache( );
		features::misc::g_impacts.on_level_change( );
		features::misc::g_scoreboard_weapons.on_level_change( );
		g_schema_hash_cache.clear( );

		m_level_initialization.call<void>( a1, new_map );
	}

	void __fastcall cheat::level_shutdown( std::uintptr_t a1 )
	{
		rendering::g_model_preview_renderer.on_ui_transition( );
		rendering::g_widgets.s_map_name.clear();

		// Release feature-owned scene objects before Source 2 tears their parents down.
		features::misc::g_dlight.on_level_shutdown( );

		// Convars survive map changes; drop the floating-doll override here so
		// the next match starts with the engine's own gravity.
		features::misc::g_other.restore_floating_doll( );

		// clear all local player data on level shutdown
		systems::g_local.reset();

		m_level_shutdown.call<void>( a1 );
	}

	void __fastcall cheat::read_frame_input( std::uintptr_t a1, std::uint32_t a2 )
	{
		m_read_frame_input.call<void>( a1, a2 );
	}

	void __fastcall cheat::process_input_event( std::uintptr_t csgo_input, int slot, float frametime )
	{
		systems::g_legit_input.on_process_input_event( csgo_input, slot );
		features::combat::g_misc.antiaim( ).begin_process_input_event( csgo_input, slot );
		m_process_input_event.call<void>( csgo_input, slot, frametime );

		// Harvest the engine-applied mouse delta and restore the view captured
		// before the call, including the first frame of a steering hold.
		features::combat::g_misc.antiaim( ).end_process_input_event( csgo_input, slot );

		// FrameStageNotify is dormant in the main menu. ProcessInputEvent remains
		// on the client/UI thread there, after Panorama has advanced for this tick.
		// This gives the native preview a valid lobby root without crossing into
		// the render thread from Present.
		rendering::g_model_preview_renderer.on_frame_stage_notify( );
		features::misc::g_fake_vac.on_process_input_event( );
	}

	std::uintptr_t __fastcall cheat::render_decals( std::uintptr_t render_context, std::uintptr_t** render_view, bool pass_flag_a, bool pass_flag_b )
	{
		if ( settings::g_misc.m_removals.decals.value )
		{
			return 0;
		}

		return m_render_decals.call<std::uintptr_t>( render_context, render_view, pass_flag_a, pass_flag_b );
	}

	void __fastcall cheat::render_smoke( std::uintptr_t a1, std::uintptr_t a2, int a3, int a4, std::uintptr_t a5, std::uintptr_t a6 )
	{
		static std::once_flag alright;
		std::call_once( alright, [ & ]
			{
				if ( !a2 )
				{
					return;
				}

				if ( m_render_smoke_map.create( reinterpret_cast< void* >( memory::get_vfunc( a2, 32 ) ), &render_smoke_map ) )
				{
					m_render_smoke_map.enable( );
				}

				if ( m_render_smoke_unmap.create( reinterpret_cast< void* >( memory::get_vfunc( a2, 33 ) ), &render_smoke_unmap ) )
				{
					m_render_smoke_unmap.enable( );
				}
			} );

		if ( settings::g_misc.m_removals.smoke.value )
		{
			return;
		}

		m_render_smoke.call<void>( a1, a2, a3, a4, a5, a6 );
	}

	std::uintptr_t __fastcall cheat::render_smoke_map( std::uintptr_t thisptr, std::size_t size, std::uintptr_t* out_ptr )
	{
		const auto result = m_render_smoke_map.call<std::uintptr_t>( thisptr, size, out_ptr );

		features::world::g_smoke.on_map( result, size, out_ptr && *out_ptr ? *out_ptr : 0 );

		return result;
	}

	void __fastcall cheat::render_smoke_unmap( std::uintptr_t thisptr, std::uintptr_t ctx, std::size_t size )
	{
		features::world::g_smoke.on_unmap( ctx );
		m_render_smoke_unmap.call<void>( thisptr, ctx, size );
	}

	char __fastcall cheat::set_info( std::uintptr_t rcx, std::uintptr_t a2 )
	{
		const auto& cfg = settings::g_misc.m_name_changer;
		const auto should_override = cfg.clantag.value || cfg.override_name.value || features::misc::other::s_name_change_pending;
		if ( should_override && a2 )
		{
			const auto arg_list = memory::safe_read<std::uintptr_t>( a2 + 0x440 ).value_or( 0 );
			const auto key = arg_list
				? memory::safe_read<const char*>( arg_list + 0x8 ).value_or( nullptr )
				: nullptr;

			if ( key && _stricmp( key, "name" ) == 0 )
			{
				constexpr std::uint64_t fcvar_protected = 1ull << 5;
				constexpr std::uint64_t fcvar_userinfo = 1ull << 9;
				constexpr std::uint64_t fcvar_registry_restricted = 1ull << 10;

				if ( addresses::globals::cvar )
				{
					if ( const auto name_cvar = addresses::globals::cvar->find( "name"_hash ) )
					{
						const auto flags_address = reinterpret_cast<std::uintptr_t>( name_cvar ) + offsetof( c_convar, m_flags );
						if ( const auto flags = memory::safe_read<std::uint64_t>( flags_address ) )
						{
							static_cast<void>( memory::safe_write<std::uint64_t>( flags_address,
								( *flags | fcvar_userinfo ) & ~( fcvar_protected | fcvar_registry_restricted ) ) );
						}
					}
				}

				const auto& display = features::misc::other::s_display_name;
				if ( !display.empty( ) )
				{
					static_cast<void>( memory::safe_write<const char*>( arg_list + 0x10, display.c_str( ) ) );
				}
			}
		}

		return m_set_info.call<char>( rcx, a2 );
	}

	void __fastcall cheat::draw_flash_effect( std::uintptr_t a1, int a2, std::uintptr_t* a3, std::uintptr_t a4, __m128* a5 )
	{
		if ( settings::g_misc.m_removals.flash_alpha.value < 100.0f && settings::g_misc.m_removals.flash_alpha.value != 0.0f )
		{
			const auto view_pawn = systems::g_local.get( ).view_pawn( );
			if ( view_pawn )
			{
				const auto max = settings::g_misc.m_removals.flash_alpha.value / 100.0f * 255.0f;
				memory::write<float>( view_pawn + SCHEMA( "C_CSPlayerPawnBase", "m_flFlashMaxAlpha"_hash ), max );
			}
		}

		if ( settings::g_misc.m_removals.flash_alpha.value != 0.0f )
		{
			m_draw_flash_effect.call<void>( a1, a2, a3, a4, a5 );
		}
	}

} // namespace hooks
