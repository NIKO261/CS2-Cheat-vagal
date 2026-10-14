#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <protection/game_addresses.hpp>
#include <protection/offsets.hpp>
#include "../addresses.hpp"

namespace addresses::globals {

	bool initialize () {
		source2client              = INTERFACE_ (sdk::interfaces::source2client);
		panorama = INTERFACE_ (sdk::interfaces::panorama);
		source2engine_to_client    = INTERFACE_ (sdk::interfaces::source2engine_to_client);
		scene_system               = INTERFACE_ (sdk::interfaces::scene_system);
		material_system            = INTERFACE_ (sdk::interfaces::material_system);
		schema_system              = INTERFACE_ (sdk::interfaces::schema_system);
		input_system               = INTERFACE_ (sdk::interfaces::input_system);
		particle_system_mgr        = INTERFACE_ (sdk::interfaces::particle_system_mgr);
		cvar                       = (interfaces::c_engine_cvar*)INTERFACE_ (sdk::interfaces::cvar);
		source2client_prediction   = INTERFACE_ (sdk::interfaces::source2client_prediction);
		network_client_service     = INTERFACE_ (sdk::interfaces::network_client_service);
		resource_system            = INTERFACE_ (sdk::interfaces::resource_system);
		localize                   = INTERFACE_ (sdk::interfaces::localize);
		mesh_system                = INTERFACE_ (sdk::interfaces::mesh_system);
		file_system                = INTERFACE_ (sdk::interfaces::file_system);

		// Build-pinned dumper RVAs; runtime Schema fields remain dynamically resolved.
		csgo_input             = modules::client + sdk::offsets::client::dwCSGOInput;
		entity_list            = modules::client + sdk::offsets::client::dwEntityList;
		local_player_controller = modules::client + sdk::offsets::client::dwLocalPlayerController;
		global_vars            = modules::client + sdk::offsets::client::dwGlobalVars;
		view_matrix            = modules::client + sdk::offsets::client::dwViewMatrix;
		game_rules             = modules::client + sdk::offsets::client::dwGameRules;
		light_data_queue       = PATTERN (patterns::light_data_queue);
		particle_manager       = PATTERN (patterns::particle_manager);
		game_event_manager     = PATTERN (patterns::game_event_manager);
		game_trace_manager     = PATTERN (patterns::game_trace_manager);
		render_game_system_storage = PATTERN (patterns::render_game_system_storage);
		material_manager       = PATTERN (patterns::material_manager);
		game_entity_system     = modules::client + sdk::offsets::client::dwGameEntitySystem;
		weapon_recoil_data     = PATTERN (patterns::weapon_recoil_data);
		hud                    = PATTERN (patterns::hud);
		main_menu_panel        = PATTERN (patterns::main_menu_panel);
		prediction_seed        = PATTERN (patterns::prediction_seed);
		// Distinct native slots: simulation owns a pawn; SetupContext owns a controller.
		simulation_player      = PATTERN (patterns::simulation_player);
		prediction_controller  = PATTERN (patterns::prediction_controller);
		planted_c4             = modules::client + sdk::offsets::client::dwPlantedC4;
		item_system            = PATTERN (patterns::item_system);
		frame_input_ring_idx   = PATTERN (patterns::frame_input_ring_idx);
		frame_input_ring_base  = PATTERN (patterns::frame_input_ring_base);
		// CGameEntitySystem alias, not the CPrediction interface object.
		prediction_state       = PATTERN (patterns::prediction_state);

		if (const auto ptr = MODULE_EXPORT("tier0.dll:g_pMemAlloc"))
			mem_alloc = *reinterpret_cast<std::uintptr_t*>(ptr);

		const std::pair<std::string_view, std::uintptr_t> required_globals[] {
			{ "csgo_input", csgo_input },
			{ "entity_list", entity_list },
			{ "local_player_controller", local_player_controller },
			{ "global_vars", global_vars },
			{ "view_matrix", view_matrix },
			{ "game_rules", game_rules },
			{ "light_data_queue", light_data_queue },
			{ "particle_manager", particle_manager },
			{ "game_event_manager", game_event_manager },
			{ "game_trace_manager", game_trace_manager },
			{ "render_game_system_storage", render_game_system_storage },
			{ "mem_alloc", mem_alloc },
			{ "material_manager", material_manager },
			{ "game_entity_system", game_entity_system },
			{ "weapon_recoil_data", weapon_recoil_data },
			{ "hud", hud },
			{ "main_menu_panel", main_menu_panel },
			{ "prediction_seed", prediction_seed },
			{ "simulation_player", simulation_player },
			{ "prediction_controller", prediction_controller },
			{ "planted_c4", planted_c4 },
			{ "item_system", item_system },
			{ "network_client_service", network_client_service },
			{ "frame_input_ring_idx", frame_input_ring_idx },
			{ "frame_input_ring_base", frame_input_ring_base },
			{ "prediction_state", prediction_state },
		};

		auto initialized = true;
		for (const auto& [name, address] : required_globals) {
			if (!address) {
				logging::console::print (xs ("[error] global address not initialized | {}"), name);
				initialized = false;
			}
		}

		return initialized;
	}

} // namespace addresses::globals
