#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/rendering/rendering.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include "../movement.hpp"

namespace features::movement {
	namespace {
		constexpr int k_max_steps = 32;
		constexpr auto k_jump = static_cast<std::uintptr_t>( cstypes::command_buttons::in_jump );
		constexpr float k_time_step = 1.0f / 1024.0f;
		constexpr float k_speed_tolerance = 0.5f;

		bool space_held( )
		{
			return !rendering::g_menu.is_open( ) && ( GetAsyncKeyState( VK_SPACE ) & 0x8000 ) != 0;
		}

		struct jump_layout
		{
			int modern = SCHEMA( "CCSPlayer_MovementServices", "m_ModernJump"_hash );
			int landed_tick = SCHEMA( "CCSPlayerModernJump", "m_nLastLandedTick"_hash );
			int landed_frac = SCHEMA( "CCSPlayerModernJump", "m_flLastLandedFrac"_hash );
			int landed_x = SCHEMA( "CCSPlayerModernJump", "m_flLastLandedVelocityX"_hash );
			int landed_y = SCHEMA( "CCSPlayerModernJump", "m_flLastLandedVelocityY"_hash );
			int jumped_tick = SCHEMA( "CCSPlayer_MovementServices", "m_nLastJumpTick"_hash );
			int jumped_frac = SCHEMA( "CCSPlayer_MovementServices", "m_flLastJumpFrac"_hash );

			bool valid( ) const
			{
				return modern > 0 && landed_tick > 0 && landed_frac > 0 && landed_x > 0
					&& landed_y > 0 && jumped_tick > 0 && jumped_frac > 0;
			}
		};

		struct movement_result
		{
			bool valid{};
			bool landed{};
			bool jumped{};
			float landing_when{};
			float jump_when{};
			float landing_speed{};
			float speed{};
		};

		// Engine movement is the timing oracle. No ray-height/quadratic estimate:
		// a high-speed fall, hull contact and ground categorization all run through
		// the same prediction transaction as the submitted command.
		movement_result predict( systems::input::usercmd* cmd, const systems::local::snapshot& local )
		{
			movement_result out{};
			const jump_layout layout{};
			const auto services = memory::read<std::uintptr_t>(
				local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			const auto base = cmd->csgo_user_cmd.mutable_base( );
			auto buttons = base ? const_cast<proto::in_button_state_pb*>( base->buttons_pb( ) ) : nullptr;
			if ( !services || !layout.valid( ) || !buttons ) return out;

			const auto modern = services + layout.modern;
			const auto old_land_tick = memory::read<int>( modern + layout.landed_tick );
			const auto old_land_frac = memory::read<float>( modern + layout.landed_frac );
			const auto old_jump_tick = memory::read<int>( services + layout.jumped_tick );
			const auto old_jump_frac = memory::read<float>( services + layout.jumped_frac );
			const auto expected_tick = memory::read<int>(
				local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) ) + 1;

			// Match input::apply without computing CRC or changing attack/history.
			const auto saved_buttons = *buttons;
			buttons->set_buttonstate1( cmd->buttons.value );
			buttons->set_buttonstate2( cmd->buttons.value_changed );
			buttons->set_buttonstate3( cmd->buttons.value_scroll );
			buttons->m_cached_size = 0;
			const auto simulated = systems::g_prediction.simulate( cmd, local, [ & ]
				{
					const auto land_tick = memory::read<int>( modern + layout.landed_tick );
					out.landing_when = memory::read<float>( modern + layout.landed_frac );
					const auto jump_tick = memory::read<int>( services + layout.jumped_tick );
					out.jump_when = memory::read<float>( services + layout.jumped_frac );
					const auto velocity = memory::read<math::vector3>(
						local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
					const auto flags = memory::read<std::uint32_t>(
						local.pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );
					out.speed = velocity.length_2d( );
					out.landing_speed = std::hypotf(
						memory::read<float>( modern + layout.landed_x ),
						memory::read<float>( modern + layout.landed_y ) );
					out.landed = land_tick == expected_tick
						&& ( land_tick != old_land_tick || out.landing_when != old_land_frac )
						&& std::isfinite( out.landing_when )
						&& out.landing_when >= 0.0f && out.landing_when < 1.0f;
					out.jumped = jump_tick == expected_tick
						&& ( jump_tick != old_jump_tick || out.jump_when != old_jump_frac )
						&& std::isfinite( out.jump_when ) && out.jump_when >= 0.0f && out.jump_when < 1.0f
						&& ( flags & cstypes::entity_flags::on_ground ) == 0
						&& std::isfinite( velocity.z ) && velocity.z > 1.0f;
				} );
			*buttons = saved_buttons;
			out.valid = simulated && std::isfinite( out.speed ) && std::isfinite( out.landing_speed );
			return out;
		}

		void sort_steps( proto::repeated_ptr_field<proto::subtick_move_step>* moves )
		{
			auto elements = moves->m_rep->elements;
			std::stable_sort( elements, elements + moves->size( ), []( void* a, void* b )
				{
					return proto::impl_ptr<proto::subtick_move_step>( a )->when( )
						< proto::impl_ptr<proto::subtick_move_step>( b )->when( );
				} );
		}

		bool owns( const std::array<proto::subtick_move_step*, 3>& events, const proto::subtick_move_step* step )
		{
			return std::find( events.begin( ), events.end( ), step ) != events.end( );
		}

		// Rotate removed slots into the reusable tail, preserving a permutation of
		// all arena pointers. Copy-compaction would alias pointers on the next add.
		void discard_events( proto::base_usercmd_pb* base, const std::array<proto::subtick_move_step*, 3>& events )
		{
			const auto moves = base ? base->mutable_subtick_moves( ) : nullptr;
			if ( !moves || !moves->m_rep ) return;
			auto elements = moves->m_rep->elements;
			for ( int i = 0; i < moves->size( ); )
			{
				if ( !owns( events, moves->mutable_at( i ) ) ) { ++i; continue; }
				std::rotate( elements + i, elements + i + 1, elements + moves->size( ) );
				--moves->m_current_size;
			}
			base->m_cached_size = 0;
		}

		bool reserve_events( proto::base_usercmd_pb* base, std::array<proto::subtick_move_step*, 3>& events )
		{
			const auto moves = base->mutable_subtick_moves( );
			if ( !moves || moves->size( ) < 0 || moves->size( ) > k_max_steps - 3 ) return false;
			for ( auto& event : events )
			{
				event = systems::g_input.acquire_subtick_step( moves );
				if ( !event ) { discard_events( base, events ); return false; }
				// Zero-time releases reserve capacity without extending airstrafe's
				// scheduling horizon or masquerading as analog movement.
				event->set_button( k_jump );
				event->set_pressed( false );
				event->set_when( 0.0f );
			}
			base->m_cached_size = 0;
			return true;
		}

		void set_pulse( const std::array<proto::subtick_move_step*, 3>& events, float when )
		{
			events[ 0 ]->set_when( 0.0f );
			events[ 0 ]->set_pressed( false );
			events[ 1 ]->set_when( when );
			events[ 1 ]->set_pressed( true );
			events[ 2 ]->set_when( std::min( 1.0f, when + k_time_step ) );
			events[ 2 ]->set_pressed( false );
			for ( const auto event : events ) event->m_cached_size = 0;
		}

		struct command_trajectory
		{
			std::array<void*, k_max_steps> pointers{};
			std::array<proto::subtick_move_step, k_max_steps> values{};
			std::array<math::vector2, k_max_steps> angles{};
			int count{};

			explicit command_trajectory( proto::repeated_ptr_field<proto::subtick_move_step>* moves )
				: count( moves->size( ) )
			{
				math::vector2 angle{};
				for ( int i = 0; i < count; ++i )
				{
					pointers[ i ] = moves->m_rep->elements[ i ];
					values[ i ] = *moves->mutable_at( i );
					angle.x += values[ i ].pitch_delta( );
					angle.y += values[ i ].yaw_delta( );
					angles[ i ] = angle;
				}
			}

			void restore( proto::repeated_ptr_field<proto::subtick_move_step>* moves ) const
			{
				for ( int i = 0; i < count; ++i )
				{
					moves->m_rep->elements[ i ] = pointers[ i ];
					*moves->mutable_at( i ) = values[ i ];
				}
			}

			void apply( proto::repeated_ptr_field<proto::subtick_move_step>* moves,
				const std::array<proto::subtick_move_step*, 3>& events, float when ) const
			{
				restore( moves );
				set_pulse( events, when );
				sort_steps( moves );
				math::vector2 previous{};
				for ( int i = 0; i < count; ++i )
				{
					const auto step = moves->mutable_at( i );
					auto target = count ? angles[ count - 1 ] : math::vector2{};
					for ( int j = 0; j < count; ++j )
					{
						const auto source = proto::impl_ptr<proto::subtick_move_step>( pointers[ j ] );
						if ( owns( events, source ) ) continue;
						if ( source == step || ( owns( events, step ) && values[ j ].when( ) >= step->when( ) ) )
						{
							target = angles[ j ];
							break;
						}
					}
					// Movement uses the angle at the END of each interval. A new
					// jump boundary must borrow that interval's endpoint angle, not
					// the preceding angle. Return the borrowed delta at the next
					// original record; its time, button and analog values stay intact.
					step->set_pitch_delta( target.x - previous.x );
					step->set_yaw_delta( target.y - previous.y );
					step->m_cached_size = 0;
					previous = target;
				}
			}
		};
	}

	void bhop::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_prediction_command = -1;
		this->m_prediction_pawn = 0;
		this->m_prediction_pending = false;
		if ( !cmd || !settings::g_movement.bhop.value ) return;
		const auto autojump = CONVAR( "sv_autobunnyhopping" );
		if ( !autojump || autojump->get<bool>( ) ) return;
		const auto perfect = settings::g_movement.perfect_bunny_hop.value;
		const auto holding = perfect ? space_held( ) : ( cmd->buttons.value & k_jump ) != 0 || space_held( );
		if ( !holding ) return;
		const auto local = systems::g_local.get( );
		if ( !local.pawn || !local.controller || !local.is_alive ) return;
		const auto type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( type == cstypes::move_type::ladder || type == cstypes::move_type::noclip
			|| type == cstypes::move_type::observer ) return;
		const auto& pre = systems::g_prediction.pre( );
		if ( pre.flags & cstypes::entity_flags::on_ground )
		{
			cmd->buttons.value |= k_jump;
			cmd->buttons.value_changed |= k_jump;
			cmd->buttons.value_scroll &= ~k_jump;
			return;
		}
		cmd->buttons.value &= ~k_jump;
		cmd->buttons.value_changed &= ~k_jump;
		cmd->buttons.value_scroll &= ~k_jump;
		const auto legacy = CONVAR( "sv_legacy_jump" );
		if ( !perfect || !legacy || legacy->get<bool>( )
			|| !std::isfinite( pre.velocity.z ) || pre.velocity.z >= 0.0f ) return;

		// Reserve only on a predicted landing command, not throughout the fall.
		// This first probe is a capacity hint; final movement is probed again.
		const auto landing = predict( cmd, local );
		if ( !landing.valid || !landing.landed ) return;
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto moves = base ? base->mutable_subtick_moves( ) : nullptr;
		if ( !moves || ( moves->size( ) > 0 && !moves->m_rep ) ) return;
		for ( int i = 0; i < moves->size( ); ++i )
			if ( moves->mutable_at( i )->button( ) & k_jump ) return;
		// Reserve capacity, not button events: landing trials run after final movement.
		this->m_prediction_command = cmd->command_number;
		this->m_prediction_pawn = local.pawn;
		this->m_prediction_pending = true;
	}

	void bhop::finish_prediction( systems::input::usercmd* cmd )
	{
		if ( !this->m_prediction_pending ) return;
		const auto number = this->m_prediction_command;
		const auto pawn = this->m_prediction_pawn;
		std::array<proto::subtick_move_step*, 3> events{};
		this->m_prediction_pending = false;
		this->m_prediction_command = -1;
		this->m_prediction_pawn = 0;
		if ( !cmd || cmd->command_number != number ) return;
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		const auto moves = base ? base->mutable_subtick_moves( ) : nullptr;
		const auto cancel = [ & ] { discard_events( base, events ); };
		const auto local = systems::g_local.get( );
		if ( !base || !moves || !moves->m_rep || moves->size( ) > k_max_steps
			|| local.pawn != pawn || !local.controller || !local.is_alive
			|| !settings::g_movement.bhop.value || !settings::g_movement.perfect_bunny_hop.value
			|| !space_held( ) || g_edgebug.active_this_tick( ) || g_jumpbug.active_this_tick( )
			|| ( cmd->buttons.value & k_jump ) != 0 ) { cancel( ); return; }
		if ( !reserve_events( base, events ) ) return;
		int found = 0;
		for ( int i = 0; i < moves->size( ); ++i )
		{
			const auto step = moves->mutable_at( i );
			if ( !step || !std::isfinite( step->when( ) ) ) { cancel( ); return; }
			if ( owns( events, step ) ) ++found;
			else if ( step->button( ) & k_jump ) { cancel( ); return; }
		}
		if ( found != 3 ) { cancel( ); return; }

		// Materialize the same fallback input anchor used by the final command.
		systems::g_input.prepare_subtick_movement( );
		if ( moves->size( ) > k_max_steps ) { cancel( ); return; }
		sort_steps( moves );
		const auto baseline = predict( cmd, local );
		if ( !baseline.valid || !baseline.landed ) { cancel( ); return; }
		const command_trajectory trajectory{ moves };

		const auto saved_buttons = cmd->buttons;
		cmd->buttons.value &= ~k_jump;
		cmd->buttons.value_changed |= k_jump;
		cmd->buttons.value_scroll |= k_jump;
		float best_when = -1.0f;
		float best_speed = -1.0f;
		// Contact itself and its adjacent representable-time neighborhood. Select
		// by actual jump acceptance AND exit speed, never by positive Z alone.
		for ( const float offset : { 0.0f, k_time_step, -k_time_step } )
		{
			const auto when = baseline.landing_when + offset;
			if ( when < 0.0f || when >= 1.0f ) continue;
			trajectory.apply( moves, events, when );
			const auto trial = predict( cmd, local );
			if ( !trial.valid || !trial.jumped
				|| std::fabsf( trial.jump_when - when ) > k_time_step
				|| trial.speed + k_speed_tolerance < baseline.speed ) continue;
			if ( trial.speed > best_speed )
			{
				best_when = when;
				best_speed = trial.speed;
			}
		}
		if ( best_when < 0.0f )
		{
			trajectory.restore( moves );
			cmd->buttons = saved_buttons;
			cancel( );
			return;
		}
		trajectory.apply( moves, events, best_when );
		base->m_cached_size = 0;
	}
}
