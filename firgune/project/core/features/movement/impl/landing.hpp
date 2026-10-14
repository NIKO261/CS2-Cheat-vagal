#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

// Command-local landing trials. These helpers never persist pawn state:
// prediction::simulate owns the engine-state rollback.
namespace features::movement::landing {

	constexpr int max_steps = 32;
	constexpr float time_step = 1.0f / 1024.0f;
	constexpr float speed_tolerance = 0.5f;
	constexpr auto jump = static_cast<std::uintptr_t>( cstypes::command_buttons::in_jump );
	constexpr auto duck = static_cast<std::uintptr_t>( cstypes::command_buttons::in_duck );

	inline int available_steps( const proto::repeated_ptr_field<proto::subtick_move_step>* moves )
	{
		if ( !moves || moves->size( ) < 0 || moves->size( ) > max_steps ) return 0;
		// Only one landing owner commits. Keep one additional slot for the final
		// movement anchor; provisional landing requests do not emit input events.
		const auto reserve = std::max( { g_edgebug.subtick_reserve( ), g_jumpbug.subtick_reserve( ), g_bhop.subtick_reserve( ) } );
		return std::max( 0, max_steps - moves->size( ) - reserve - 1 );
	}

	struct result
	{
		bool valid{};
		bool landed{};
		bool jumped{};
		std::uint32_t flags{};
		math::vector3 velocity{};
		float speed{};
		float landing_when{};
		float jump_when{};
	};

	inline result predict( systems::input::usercmd* cmd, const systems::local::snapshot& local )
	{
		result out{};
		const auto services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		const auto modern_offset = SCHEMA( "CCSPlayer_MovementServices", "m_ModernJump"_hash );
		const auto land_tick_offset = SCHEMA( "CCSPlayerModernJump", "m_nLastLandedTick"_hash );
		const auto land_frac_offset = SCHEMA( "CCSPlayerModernJump", "m_flLastLandedFrac"_hash );
		const auto jump_tick_offset = SCHEMA( "CCSPlayer_MovementServices", "m_nLastJumpTick"_hash );
		const auto jump_frac_offset = SCHEMA( "CCSPlayer_MovementServices", "m_flLastJumpFrac"_hash );
		const auto base = cmd->csgo_user_cmd.mutable_base( );
		auto buttons = base ? const_cast<proto::in_button_state_pb*>( base->buttons_pb( ) ) : nullptr;
		if ( !services || !local.controller || !buttons || modern_offset <= 0 || land_tick_offset <= 0
			|| land_frac_offset <= 0 || jump_tick_offset <= 0 || jump_frac_offset <= 0 ) return out;
		const auto modern = services + modern_offset;
		const auto old_land_tick = memory::read<int>( modern + land_tick_offset );
		const auto old_land_frac = memory::read<float>( modern + land_frac_offset );
		const auto old_jump_tick = memory::read<int>( services + jump_tick_offset );
		const auto old_jump_frac = memory::read<float>( services + jump_frac_offset );
		// Native jump/land writers convert the move-data clock before storing it.
		// A record changed inside this rolled-back trial belongs to this trial;
		// its integer tick is not necessarily controller TickBase + 1.
		const auto saved_buttons = *buttons;
		buttons->set_buttonstate1( cmd->buttons.value );
		buttons->set_buttonstate2( cmd->buttons.value_changed );
		buttons->set_buttonstate3( cmd->buttons.value_scroll );
		buttons->m_cached_size = 0;
		const auto simulated = systems::g_prediction.simulate( cmd, local, [ & ]
			{
				const auto land_tick = memory::read<int>( modern + land_tick_offset );
				const auto jump_tick = memory::read<int>( services + jump_tick_offset );
				out.landing_when = memory::read<float>( modern + land_frac_offset );
				out.jump_when = memory::read<float>( services + jump_frac_offset );
				out.flags = memory::read<std::uint32_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );
				out.velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
				out.speed = out.velocity.length_2d( );
				out.landed = land_tick >= 0 && ( land_tick != old_land_tick || out.landing_when != old_land_frac )
					&& std::isfinite( out.landing_when ) && out.landing_when >= 0.0f && out.landing_when < 1.0f;
				out.jumped = jump_tick >= 0 && ( jump_tick != old_jump_tick || out.jump_when != old_jump_frac )
					&& std::isfinite( out.jump_when ) && out.jump_when >= 0.0f && out.jump_when < 1.0f
					&& ( out.flags & cstypes::entity_flags::on_ground ) == 0 && out.velocity.z > 1.0f;
			} );
		*buttons = saved_buttons;
		out.valid = simulated && std::isfinite( out.speed ) && std::isfinite( out.velocity.z );
		return out;
	}

	inline void set_event( proto::subtick_move_step* step, std::uintptr_t button, bool pressed, float when )
	{
		*step = {};
		step->set_button( button );
		step->set_pressed( pressed );
		step->set_when( when );
		// No zero analog fields: a duck/jump event is not a movement anchor.
	}

	class transaction
	{
	public:
		explicit transaction( systems::input::usercmd* command, int needed ) : cmd( command ), buttons( command->buttons )
		{
			base = cmd->csgo_user_cmd.mutable_base( );
			moves = base ? base->mutable_subtick_moves( ) : nullptr;
			if ( !moves || moves->size( ) < 0 || needed < 1 || needed > 12
				|| moves->size( ) > max_steps - needed || ( moves->size( ) && !moves->m_rep ) ) return;
			original_count = moves->size( );
			for ( int i = 0; i < original_count; ++i )
			{
				const auto step = moves->mutable_at( i );
				if ( !step || ( step->button( ) & ( jump | duck ) )
					|| !std::isfinite( step->when( ) ) || !std::isfinite( step->yaw_delta( ) )
					|| !std::isfinite( step->pitch_delta( ) ) ) return;
			}
			for ( int i = 0; i < original_count; ++i )
			{
				pointers[ i ] = moves->m_rep->elements[ i ];
				values[ i ] = *moves->mutable_at( i );
				order[ i ] = i;
			}
			// Record endpoint angles in timeline order, while preserving the raw
			// pointer order for byte-exact rollback of the original active chain.
			std::stable_sort( order.begin( ), order.begin( ) + original_count, [ & ]( int a, int b )
				{ return values[ a ].when( ) < values[ b ].when( ); } );
			math::vector2 angle{};
			for ( int k = 0; k < original_count; ++k )
			{
				const auto i = order[ k ];
				angle.x += values[ i ].pitch_delta( );
				angle.y += values[ i ].yaw_delta( );
				angles[ i ] = angle;
			}
			captured = true;
			for ( int i = 0; i < needed; ++i )
			{
				const auto previous_size = moves->size( );
				const auto step = systems::g_input.acquire_subtick_step( moves );
				if ( !step ) return;
				added_pointers[ added_count ] = moves->m_rep->elements[ previous_size ];
				events[ added_count++ ] = step;
				if ( moves->size( ) != previous_size + 1 ) return;
				set_event( step, 0, false, 0.0f );
			}
			ready = true;
		}

		~transaction( ) { if ( !committed ) restore( ); }
		transaction( const transaction& ) = delete;
		transaction& operator=( const transaction& ) = delete;
		bool valid( ) const { return ready; }
		proto::subtick_move_step* event( int i ) const { return events[ i ]; }

		void reset( )
		{
			restore( );
			moves->m_current_size = original_count + added_count;
			for ( int i = 0; i < added_count; ++i ) set_event( events[ i ], 0, false, 0.0f );
		}

		void encode_angles( )
		{
			sort( );
			math::vector2 previous{};
			for ( int i = 0; i < moves->size( ); ++i )
			{
				const auto step = moves->mutable_at( i );
				const auto own = std::find( events.begin( ), events.begin( ) + added_count, step ) != events.begin( ) + added_count;
				// Beyond the final original record, retain its endpoint instead of
				// snapping back to zero (including the final jump release boundary).
				auto target = original_count ? angles[ order[ original_count - 1 ] ] : math::vector2{};
				for ( int k = 0; k < original_count; ++k )
				{
					const auto j = order[ k ];
					if ( proto::impl_ptr<proto::subtick_move_step>( pointers[ j ] ) == step
						|| ( own && values[ j ].when( ) >= step->when( ) ) )
					{
						target = angles[ j ];
						break;
					}
				}
				step->set_pitch_delta( target.x - previous.x );
				step->set_yaw_delta( target.y - previous.y );
				step->m_cached_size = 0;
				previous = target;
			}
			base->m_cached_size = 0;
		}

		void commit( ) { committed = true; base->m_cached_size = 0; }

	private:
		void sort( )
		{
			if ( !moves->m_rep ) return;
			auto elements = moves->m_rep->elements;
			std::stable_sort( elements, elements + moves->size( ), []( void* a, void* b )
				{
					return proto::impl_ptr<proto::subtick_move_step>( a )->when( ) < proto::impl_ptr<proto::subtick_move_step>( b )->when( );
				} );
		}

		void restore( )
		{
			if ( !captured ) return;
			cmd->buttons = buttons;
			for ( int i = 0; i < original_count; ++i )
			{
				moves->m_rep->elements[ i ] = pointers[ i ];
				*moves->mutable_at( i ) = values[ i ];
			}
			// Restore newly allocated pointers to the reusable tail too. Restoring
			// only the active prefix would leave aliases after a sorted trial.
			for ( int i = 0; i < added_count; ++i ) moves->m_rep->elements[ original_count + i ] = added_pointers[ i ];
			moves->m_current_size = original_count;
			base->m_cached_size = 0;
		}

		systems::input::usercmd* cmd{};
		systems::input::in_button_state buttons{};
		proto::base_usercmd_pb* base{};
		proto::repeated_ptr_field<proto::subtick_move_step>* moves{};
		std::array<void*, max_steps> pointers{};
		std::array<proto::subtick_move_step, max_steps> values{};
		std::array<math::vector2, max_steps> angles{};
		std::array<int, max_steps> order{};
		std::array<void*, 12> added_pointers{};
		std::array<proto::subtick_move_step*, 12> events{};
		int original_count{};
		int added_count{};
		bool captured{};
		bool ready{};
		bool committed{};
	};

} // namespace features::movement::landing
