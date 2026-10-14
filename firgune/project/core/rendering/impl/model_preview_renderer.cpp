#include <pch/pch.hpp>
#include <core/rendering/rendering.hpp>
#include <core/features/misc/misc.hpp>
#include <core/systems/systems.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <protection/game_addresses.hpp>

namespace rendering {

namespace {
bool project_preview_bone( std::uintptr_t panel, const char* name, float& x, float& y )
{
	const auto address = PATTERN( patterns::get_bone_position_in_panel_space );
	if ( !address || !panel || !name ) return false;
	float output[ 2 ]{};
	__try
	{
		reinterpret_cast<void *( __fastcall* )( void*, float*, const char* )>( address )(
			reinterpret_cast<void*>( panel ), output, name );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		return false;
	}
	if ( !std::isfinite( output[ 0 ] ) || !std::isfinite( output[ 1 ] ) ) return false;
	if ( output[ 0 ] < -64.0f || output[ 0 ] > 576.0f || output[ 1 ] < -64.0f || output[ 1 ] > 576.0f ) return false;
	x = output[ 0 ] > 2.0f ? output[ 0 ] / 512.0f : output[ 0 ];
	y = output[ 1 ] > 2.0f ? output[ 1 ] / 512.0f : output[ 1 ];
	return x >= -0.125f && x <= 1.125f && y >= -0.125f && y <= 1.125f;
}

constexpr const char* k_create_script = R"PANORAMA(
(function () {
    var root = $.GetContextPanel();
    if (!root) return;
    var old = root.FindChildTraverse("VagalNativePreviewRootV6");
	if (old) {
		var existing = old.FindChildTraverse("VagalNativePreviewV6");
		if (existing) return;
		try { old.DeleteAsync(0.0); } catch (e) {}
	}
    var container = $.CreatePanel("Panel", root, "VagalNativePreviewRootV6", {
        hittest: false,
        style: "width: 512px; height: 512px; opacity: 0.01; brightness: 0.0; wash-color: #00000000; position: 0px 0px 0px; z-index: -99999;"
    });
    if (!container) return;
    var sequence = "ct_main_menu_rifle_awp_lookat";
    var model = "agents/models/ctm_st6/ctm_st6_variante.vmdl";
	var panel = $.CreatePanel("MapPlayerPreviewPanel", container, "VagalNativePreviewV6", {
		map: "ui/buy_menu", camera: "cam_loadoutmenu_ct",
		"require-composition-layer": true,
		"composition-layer-texture-name": "vagal_model_preview_v4",
		playermodel: model, playername: "vanity_character",
		animgraphcharactermode: "main-menu", pose_sequence: sequence,
		player: true, mouse_rotate: false, sync_spawn_addons: true,
		"transparent-background": true, "pin-fov": "vertical",
		csm_split_plane0_distance_override: "120.0", hittest: false,
		style: "width: 100%; height: 100%; position: 0px 0px 0px;"
	});
    if (!panel) return;
    var activate = function () {
		try {
			if (!panel || (panel.IsValid && !panel.IsValid())) return;
			if (panel.SetActiveCharacter) panel.SetActiveCharacter("vanity_character");
			if (panel.SetPlayerModel) panel.SetPlayerModel(model);
			if (panel.SetReadyForDisplay) panel.SetReadyForDisplay(true);
		} catch (e) {}
    };
	try {
		if (panel.EquipPlayerWithItem && typeof BigInt === "function")
			panel.EquipPlayerWithItem(BigInt("0xF000000000000000") | BigInt(9));
	} catch (e) {}
    var replay = function () {
		var nextDelay = 7.0 + Math.random() * 8.0;
		$.Schedule(nextDelay, replay);
		try {
			if (!panel || (panel.IsValid && !panel.IsValid())) return;
			if (panel.SetReadyForDisplay) panel.SetReadyForDisplay(true);
			if (panel.PlaySequence) panel.PlaySequence(sequence);
		} catch (e) {}
    };
    var keepAlive = function () {
		try {
			if (!panel || (panel.IsValid && !panel.IsValid())) return;
			if (panel.SetReadyForDisplay) panel.SetReadyForDisplay(true);
			panel.style.opacity = panel.style.opacity === "1.0" ? "0.999" : "1.0";
			$.Schedule(0.25, keepAlive);
		} catch (e) {}
    };
    // SetPlayerModel queues the native +0xA66 model refresh. Delay it until the
    // portrait scene owned by the panel has had a chance to initialize.
    $.Schedule(0.10, activate);
	$.Schedule(0.50, activate);
	$.Schedule(1.00, activate);
	$.Schedule(0.25, replay);
    $.Schedule(0.25, keepAlive);
})();
)PANORAMA";
}

bool model_preview_renderer::initialize( ID3D11Device*, ID3D11DeviceContext*, HMODULE )
{
    std::scoped_lock lock{ m_mutex };
    m_srv.Reset();
    m_script_injected = false;
	m_refresh_counter = 0;
	m_panorama_ready = false;
	m_script_root = 0;
	m_script_tick = 0;
	m_preview_panel = 0;
	m_preview_player = 0;
	m_bones = {{
		{ "head_0" }, { "neck_0" }, { "spine_3" }, { "pelvis" },
		{ "clavicle_L" }, { "arm_lower_L" }, { "hand_L" }, { "clavicle_R" },
		{ "arm_lower_R" }, { "hand_R" }, { "leg_upper_L" }, { "leg_lower_L" },
		{ "ankle_L" }, { "leg_upper_R" }, { "leg_lower_R" }, { "ankle_R" }
	}};
    m_initialized = true;
    return true;
}

void model_preview_renderer::shutdown( )
{
    std::scoped_lock lock{ m_mutex };
    m_srv.Reset();
    m_script_injected = false;
	m_refresh_counter = 0;
	m_panorama_ready = false;
	m_script_root = 0;
	m_script_tick = 0;
	m_preview_panel = 0;
	m_preview_player = 0;
	for ( auto& bone : m_bones ) bone.valid = false;
    m_initialized = false;
}

void model_preview_renderer::capture_texture( ID3D11ShaderResourceView* texture )
{
    if ( !texture ) return;
    std::scoped_lock lock{ m_mutex };
	// Lobby and HUD compositions can submit in the same frame while the game is
	// changing state. Keep the first SRV for this preview generation; replacing
	// it here makes the menu alternate between both render targets and flash.
	if ( m_srv ) return;
    m_srv = texture;
    logging::console::print( xs( "[model_preview] composition texture captured" ) );
}

ID3D11ShaderResourceView* model_preview_renderer::texture( ) const
{
    std::scoped_lock lock{ m_mutex };
    return m_srv.Get();
}

const char* model_preview_renderer::status_text( ) const
{
	std::scoped_lock lock{ m_mutex };
	if ( m_srv ) return "ready";
	if ( m_panorama_ready ) return "waiting for composition texture";
	if ( m_script_injected ) return "waiting for panorama";
	return "initializing panorama";
}

bool model_preview_renderer::waiting_for_texture( ) const
{
	std::scoped_lock lock{ m_mutex };
	return m_panorama_ready && !m_srv;
}

bool model_preview_renderer::fallback_capture_window( ) const
{
	std::scoped_lock lock{ m_mutex };
	if ( !m_panorama_ready || m_srv || !m_script_tick ) return false;
	const auto elapsed = GetTickCount64() - m_script_tick;
	return elapsed >= 180 && elapsed <= 5000;
}

bool model_preview_renderer::is_preview_player( std::uint32_t owner_hash, std::uintptr_t owner_entity )
{
	if ( !owner_entity )
		return false;
	bool primary{};
	{
		std::scoped_lock lock{ m_mutex };
		if ( owner_entity == m_preview_player )
			primary = true;
		else if ( owner_hash != "C_CSGO_PreviewPlayer"_hash )
			return false;
		else if ( !m_preview_player )
		{
			m_preview_player = owner_entity;
			primary = true;
			logging::console::print( xs( "[model_preview] primary preview player captured entity={}" ),
				reinterpret_cast<void*>( owner_entity ) );
		}
		else
			return false;
	}
	if ( !primary ) return false;

	std::uintptr_t preview_panel{};
	{
		std::scoped_lock lock{ m_mutex };
		preview_panel = m_preview_panel;
	}
	std::array<std::pair<float, float>, 16> points{};
	std::array<bool, 16> valid{};
	if ( preview_panel )
	{
		for ( std::size_t index{}; index < m_bones.size( ); ++index )
			valid[ index ] = project_preview_bone( preview_panel, m_bones[ index ].name,
				points[ index ].first, points[ index ].second );
	}

	std::scoped_lock lock{ m_mutex };
	for ( std::size_t index{}; index < m_bones.size( ); ++index )
	{
		m_bones[ index ].valid = valid[ index ];
		if ( valid[ index ] )
		{
			m_bones[ index ].x = points[ index ].first;
			m_bones[ index ].y = points[ index ].second;
		}
	}
	return true;
}

void model_preview_renderer::on_ui_transition( )
{
	std::scoped_lock lock{ m_mutex };
	m_srv.Reset();
	m_script_injected = false;
	m_panorama_ready = false;
	m_refresh_counter = 0;
	m_script_tick = 0;
	m_preview_panel = 0;
	m_preview_player = 0;
	for ( auto& bone : m_bones ) bone.valid = false;
}

void model_preview_renderer::on_frame_stage_notify( )
{
	bool should_inject{};
	{
		std::scoped_lock lock{ m_mutex };
		if ( !m_initialized ) return;
		static std::uint64_t last_update{};
		const auto now = GetTickCount64();
		if ( now - last_update < 50 ) return;
		last_update = now;
	}
	if ( !addresses::globals::panorama ) return;
	auto* engine = reinterpret_cast<features::misc::c_panorama_ui_engine*>( addresses::globals::panorama );
	auto* ui = engine->get_ui_engine();
	if ( !ui ) return;

	features::misc::c_ui_panel* panel{};
	const char* root_source = "none";
	const auto depth = memory::safe_read<std::int32_t>( reinterpret_cast<std::uintptr_t>( ui ) + 0xAE0 ).value_or( 0 );
	const auto panels = memory::safe_read<features::misc::panel_data_t*>(
		reinterpret_cast<std::uintptr_t>( ui ) + 0x228 ).value_or( nullptr );
	const auto panel_count = memory::safe_read<std::int32_t>(
		reinterpret_cast<std::uintptr_t>( ui ) + 0x230 ).value_or( 0 );
	const auto is_registered_visible = [ & ]( features::misc::c_ui_panel* target )
	{
		if ( !target || !panels || panel_count <= 0 || panel_count >= 65536 ) return false;
		for ( auto index = panel_count - 1; index >= 0; --index )
		{
			const auto entry = memory::safe_read<features::misc::panel_data_t>(
				reinterpret_cast<std::uintptr_t>( panels ) + static_cast<std::size_t>( index ) * sizeof( features::misc::panel_data_t ) );
			if ( entry && entry->m_panel == target ) return entry->m_visible != 0;
		}
		return false;
	};
	features::misc::c_ui_panel* main_menu_panel{};
	if ( addresses::globals::main_menu_panel )
	{
		const auto main_menu = memory::safe_read<std::uintptr_t>( addresses::globals::main_menu_panel ).value_or( 0 );
		main_menu_panel = main_menu
			? memory::safe_read<features::misc::c_ui_panel*>( main_menu + 0x8 ).value_or( nullptr )
			: nullptr;
	}
	const auto main_menu_visible = is_registered_visible( main_menu_panel );
	const auto in_match = !main_menu_visible && systems::g_local.get( ).is_valid( );
	// MainMenuPanel remains allocated after entering a map, but its composition
	// tree becomes dormant. Route match previews to HudPanel and lobby previews
	// to MainMenuPanel so both states own an actively rendered panel.
	if ( in_match && addresses::globals::hud )
	{
		const auto hud = memory::safe_read<std::uintptr_t>( addresses::globals::hud ).value_or( 0 );
		auto* candidate = hud
			? memory::safe_read<features::misc::c_ui_panel*>( hud + 0x8 ).value_or( nullptr )
			: nullptr;
		const auto vtable = candidate
			? memory::safe_read<std::uintptr_t>( reinterpret_cast<std::uintptr_t>( candidate ) ).value_or( 0 )
			: 0;
		if ( vtable )
		{
			panel = candidate;
			root_source = "hud-match";
		}
	}
	if ( !panel && main_menu_panel && ( main_menu_visible || !in_match ) )
	{
		auto* candidate = main_menu_panel;
		const auto vtable = candidate
			? memory::safe_read<std::uintptr_t>( reinterpret_cast<std::uintptr_t>( candidate ) ).value_or( 0 )
			: 0;
		if ( vtable )
		{
			panel = candidate;
			root_source = "main-menu";
		}
	}
	// Use the active script stack only as a fallback. It can briefly contain both
	// lobby and HUD roots during a transition, which is not stable enough to own
	// a composition texture.
	if ( !panel && depth > 0 && depth <= 63 )
	{
		for ( auto index = depth - 1; index >= 0; --index )
		{
			auto* candidate = memory::safe_read<features::misc::c_ui_panel*>(
				reinterpret_cast<std::uintptr_t>( ui ) + 0x8E8 + index * sizeof( void* ) ).value_or( nullptr );
			if ( !candidate ) continue;
			const auto vtable = memory::safe_read<std::uintptr_t>( reinterpret_cast<std::uintptr_t>( candidate ) ).value_or( 0 );
			if ( !vtable ) continue;
			panel = candidate;
			root_source = "active-stack-fallback";
			break;
		}
	}
	// Outside a match the script stack is commonly empty. The HUD pointer still
	// exists there, but its tree is dormant and cannot drive a composition layer.
	// Panorama appends live panels to this registry, so the newest visible entry
	// is a better main-menu context than the inactive HUD root.
	if ( !panel )
	{
		if ( panels && panel_count > 0 && panel_count < 65536 )
		{
			for ( auto index = panel_count - 1; index >= 0; --index )
			{
				const auto entry = memory::safe_read<features::misc::panel_data_t>(
					reinterpret_cast<std::uintptr_t>( panels ) + static_cast<std::size_t>( index ) * sizeof( features::misc::panel_data_t ) );
				if ( !entry || !entry->m_panel || !entry->m_visible ) continue;
				const auto address = reinterpret_cast<std::uintptr_t>( entry->m_panel );
				const auto vtable = memory::safe_read<std::uintptr_t>( address ).value_or( 0 );
				if ( !vtable ) continue;
				panel = entry->m_panel;
				root_source = "visible-registry";
				break;
			}
		}
	}
	if ( !panel && addresses::globals::hud )
	{
		const auto hud = memory::safe_read<std::uintptr_t>( addresses::globals::hud ).value_or( 0 );
		panel = hud ? memory::safe_read<features::misc::c_ui_panel*>( hud + 0x8 ).value_or( nullptr ) : nullptr;
		if ( panel ) root_source = "hud-fallback";
	}
	if ( !panel ) return;

	const auto active_root = reinterpret_cast<std::uintptr_t>( panel );
	{
		std::scoped_lock lock{ m_mutex };
		const auto root_changed = m_script_root != active_root;
		if ( root_changed )
		{
			m_srv.Reset();
			m_preview_panel = 0;
			m_preview_player = 0;
			for ( auto& bone : m_bones ) bone.valid = false;
			m_script_tick = GetTickCount64();
		}
		should_inject = root_changed || !m_script_injected || ++m_refresh_counter >= 300;
		if ( should_inject )
		{
			m_refresh_counter = 0;
			m_script_root = active_root;
		}
	}
	if ( should_inject )
		logging::console::print( xs( "[model_preview] root={} source={} stack_depth={}" ),
			reinterpret_cast<void*>( active_root ), root_source, depth );
	if ( should_inject )
	{
		// MapPlayerPreviewPanel only renders when parented below the currently
		// active Panorama root. Injecting into every top-level root leaves a valid
		// but culled panel in the lobby.
		ui->run_script( panel, k_create_script );
	}

	// The script-created Panorama panel links to CUI_Player3dPanel through +0x8.
	// Resolve it only below our current root and validate every pointer read.
	std::uintptr_t linked_preview_panel{};
	{
		std::scoped_lock lock{ m_mutex };
		linked_preview_panel = m_preview_panel;
	}
	std::array<features::misc::c_ui_panel*, 256> pending{};
	std::size_t pending_count{ 1 };
	pending[ 0 ] = panel;
	for ( std::size_t cursor{}; cursor < pending_count && !linked_preview_panel; ++cursor )
	{
		auto* current = pending[ cursor ];
		if ( !current ) continue;
		const auto current_address = reinterpret_cast<std::uintptr_t>( current );
		const auto name_ptr = memory::safe_read<const char*>( current_address + 0x10 ).value_or( nullptr );
		const auto name = name_ptr ? memory::read_string( reinterpret_cast<std::uintptr_t>( name_ptr ), 64 ) : std::string{};
		if ( name == "VagalNativePreviewV6" )
		{
			linked_preview_panel = memory::safe_read<std::uintptr_t>( current_address + 0x8 ).value_or( 0 );
			break;
		}
		const auto child_count = memory::safe_read<std::int32_t>( current_address + 0x28 ).value_or( 0 );
		const auto children = memory::safe_read<features::misc::c_ui_panel**>( current_address + 0x30 ).value_or( nullptr );
		if ( !children || child_count <= 0 || child_count > 128 ) continue;
		for ( auto child_index = 0; child_index < child_count && pending_count < pending.size( ); ++child_index )
		{
			auto* child = memory::safe_read<features::misc::c_ui_panel*>(
				reinterpret_cast<std::uintptr_t>( children ) + child_index * sizeof( void* ) ).value_or( nullptr );
			if ( child ) pending[ pending_count++ ] = child;
		}
	}
	if ( linked_preview_panel )
	{
		std::scoped_lock lock{ m_mutex };
		if ( m_preview_panel != linked_preview_panel )
		{
			m_preview_panel = linked_preview_panel;
			logging::console::print( xs( "[model_preview] native preview panel captured panel={}" ),
				reinterpret_cast<void*>( linked_preview_panel ) );
		}
	}
	{
		std::scoped_lock lock{ m_mutex };
		const auto first_injection = !m_script_injected;
		if ( should_inject )
		{
			m_script_injected = true;
			m_panorama_ready = true;
			if ( first_injection ) m_script_tick = GetTickCount64();
		}
		if ( should_inject && first_injection )
			logging::console::print( xs( "[model_preview] panorama script injected" ) );
	}

}

bool model_preview_renderer::bone_point( std::string_view name, float& x, float& y ) const
{
	std::scoped_lock lock{ m_mutex };
	for ( const auto& bone : m_bones )
	{
		if ( bone.valid && name == bone.name ) { x = bone.x; y = bone.y; return true; }
	}
    return false;
}

}