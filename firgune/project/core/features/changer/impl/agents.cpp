#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include <utilities/game_path.hpp>

namespace features::changer {

	void agents::on_frame_stage_notify( )
	{
		const auto local = systems::g_local.get( );
		if ( !local.is_alive || systems::g_local.is_in_cinematic( ) || !local.pawn )
		{
			return;
		}

		const auto team = memory::read<int>( local.pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
		const auto selected_def_index = ( team == 3 ) ? settings::g_changer.agents.ct_def : ( team == 2 ) ? settings::g_changer.agents.t_def : static_cast< std::int16_t >( 0 );
		const auto& custom_model_path = settings::g_changer.custom_model_path.value;

		const econ_item_system::item_def* selected{ nullptr };
		if ( custom_model_path.empty( ) && selected_def_index != 0 )
		{
			selected = g_econ_item_system.find_def( selected_def_index );
		}

		const auto selected_model = !custom_model_path.empty( )
			? custom_model_path
			: selected ? selected->model_player : std::string{};

		if ( this->m_tracked_pawn != local.pawn )
		{
			this->m_original_model.clear( );
			this->m_applied_model.clear( );
			this->m_overridden = false;
			this->m_applied_handle = 0;
			this->m_tracked_team = 0;
			this->m_tracked_pawn = local.pawn;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return;
		}

		const auto model_state = game_scene_node + SCHEMA( "CSkeletonInstance", "m_modelState"_hash );

		if ( selected_model.empty( ) )
		{
			if ( this->m_overridden && !this->m_original_model.empty( ) )
			{
				memory::call<void>(PATTERN (patterns::set_player_model), local.pawn, this->m_original_model.c_str( ) );

				this->cycle_weapon_owners( local.pawn );

				this->m_applied_handle = 0;
				this->m_applied_model.clear( );
				this->m_tracked_team = 0;
				this->m_overridden = false;
			}
			return;
		}

		if ( !this->m_overridden && this->m_original_model.empty( ) )
		{
			const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
			if ( model_name_ptr )
			{
				this->m_original_model = memory::read_string( model_name_ptr );
			}
		}

		const auto current_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		const auto selection_matches = this->m_applied_model == selected_model;
		const auto team_matches = ( this->m_tracked_team == team );
		const auto handle_matches = ( this->m_applied_handle != 0 && current_handle == this->m_applied_handle );

		if ( this->m_overridden && selection_matches && team_matches && handle_matches )
		{
			return;
		}

		if ( this->m_overridden && !team_matches )
		{
			this->m_original_model.clear( );
			this->m_overridden = false;

			const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
			if ( model_name_ptr )
			{
				this->m_original_model = memory::read_string( model_name_ptr );
			}
		}

		if ( !custom_model_path.empty( ) )
		{
			this->precache_model( custom_model_path.c_str( ) );
		}

		memory::call<void>(PATTERN (patterns::set_player_model), local.pawn, selected_model.c_str( ) );

		const auto collision = local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ), math::vector3( -16.0f, -16.0f, 0.0f ) );
		memory::write<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ), math::vector3( 16.0f, 16.0f, 72.0f ) );

		this->cycle_weapon_owners( local.pawn );

		this->m_applied_handle = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_hModel"_hash ) );
		this->m_applied_model = selected_model;
		this->m_tracked_team = team;
		this->m_overridden = true;
	}

	void agents::refresh_custom_models( )
	{
		this->m_custom_models.clear( );
		this->m_custom_models.push_back( { "off", {} } );

		const auto csgo_directory = game_path::csgo_directory( );
		if ( !csgo_directory )
		{
			return;
		}

		const auto root = *csgo_directory / L"characters" / L"models";
		std::error_code error{};
		if ( !std::filesystem::is_directory( root, error ) )
		{
			return;
		}

		const auto options = std::filesystem::directory_options::skip_permission_denied;
		for ( std::filesystem::recursive_directory_iterator it{ root, options, error }, end; it != end; it.increment( error ) )
		{
			if ( error )
			{
				error.clear( );
				continue;
			}

			if ( !it->is_regular_file( error ) || it->path( ).extension( ) != L".vmdl_c" )
			{
				continue;
			}

			auto resource = std::filesystem::relative( it->path( ), *csgo_directory, error );
			if ( error )
			{
				error.clear( );
				continue;
			}

			resource.replace_extension( L".vmdl" );
			auto display = std::filesystem::relative( resource, std::filesystem::path{ L"characters" } / L"models", error );
			if ( error )
			{
				error.clear( );
				display = resource.filename( );
			}

			display.replace_extension( );
			this->m_custom_models.push_back( { display.generic_string( ), resource.generic_string( ) } );
		}

		std::sort( this->m_custom_models.begin( ) + 1, this->m_custom_models.end( ), [ ]( const custom_model& lhs, const custom_model& rhs )
			{
				return lhs.name < rhs.name;
			} );
	}

	void agents::precache_model( const char* path ) const
	{
		if ( !path || !*path || !addresses::globals::resource_system )
		{
			return;
		}

		struct buffer_string
		{
			std::int32_t length{};
			std::uint32_t allocated_size{ 0xc0000008u };
			union
			{
				char* string_ptr;
				char inline_string[ 8 ];
			};

			buffer_string( ) : string_ptr{ nullptr } {}
		} buffer;

		const auto insert_string = MODULE_EXPORT( "tier0.dll:?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z" );
		const auto precache_resource = PATTERN (patterns::resource_system_precache);
		if ( !insert_string || !precache_resource )
		{
			return;
		}

		memory::call<const char*>( insert_string, &buffer, 0, path, -1, false );
		memory::call<void>( precache_resource, addresses::globals::resource_system, &buffer, "" );
	}

	void agents::cycle_weapon_owners( std::uintptr_t pawn )
	{
		const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
		if ( !weapon_services )
		{
			return;
		}

		const auto weapons_base = weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hMyWeapons"_hash );
		const auto weapons_size = memory::read<int>( weapons_base );
		const auto weapons_data = memory::read<std::uintptr_t>( weapons_base + 0x8 );

		if ( !weapons_data || weapons_size <= 0 )
		{
			return;
		}

		for ( auto i = 0; i < weapons_size; ++i )
		{
			const auto handle = memory::read<std::uint32_t>( weapons_data + i * sizeof( std::uint32_t ) );
			const auto weapon = systems::g_entities.lookup( handle );

			if ( !weapon )
			{
				continue;
			}

			const auto owner_off = SCHEMA( "C_BaseEntity", "m_hOwnerEntity"_hash );
			const auto saved_owner = memory::read<std::uint32_t>( weapon + owner_off );

			memory::write<std::uint32_t>( weapon + owner_off, 0xffffffff );
			memory::write<std::uint32_t>( weapon + owner_off, saved_owner );
		}
	}

} // namespace features::changer
