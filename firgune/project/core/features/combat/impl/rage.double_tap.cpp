#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

namespace features::combat {
	namespace {
		constexpr auto k_pair_window_ticks{ 4 };
		constexpr auto k_max_claim_lead_ticks{ 256 };
		constexpr auto k_max_subtick_moves{ 32 };
		constexpr auto k_attack_press_when{ 0.0f };
		constexpr auto k_attack_release_when{ 1.0f / 64.0f };

		[[nodiscard]] float subtick_when( void* raw ) noexcept
		{
			if ( !raw ) return 1.0f;
			const auto* step = proto::impl_ptr<proto::subtick_move_step>( raw );
			return step ? step->when( ) : 1.0f;
		}
	}

	void rage::double_tap::reset_pair( ) noexcept
	{
		this->m_phase = pair_phase::idle;
		this->m_intent_command = -1;
		this->m_first_command = -1;
		this->m_second_command = -1;
		this->m_first_claim_tick = -1;
		this->m_second_claim_tick = -1;
		this->m_first_clip = 0;
		this->m_first_observed = false;
		this->m_pair_deadline = -1;
		this->m_intent = false;
		this->m_intent_is_second = false;
	}

	void rage::double_tap::reset( ) noexcept
	{
		this->reset_pair( );
		this->m_state = charge_state::charging;
		this->m_fail_reason = fail_reason::none;
		this->m_weapon = 0;
		this->m_command = -1;
		this->m_tick_base = -1;
		this->m_claim_tick = -1;
		this->m_cycle_ticks = 1;
		this->m_clip = 0;
		this->m_supported = false;
		this->m_claim_valid = false;
		this->m_natural_ready = false;
		this->m_progress = 0.0f;
	}

	void rage::double_tap::on_create_move( systems::input::usercmd* cmd )
	{
		// Intent is command-local. A stale intent must never be finalized into a
		// later usercmd after another feature caused an early return.
		this->m_intent = false;
		this->m_intent_is_second = false;
		this->m_intent_command = -1;

		const auto& cfg = settings::g_combat.m_ragebot.m_double_tap;
		const auto& ctx = g_shared.ctx( );
		if ( !cfg.enabled.value || !settings::g_combat.m_ragebot.enabled.value || !cmd || !ctx.valid || !ctx.weapon )
		{
			this->reset( );
			return;
		}

		if ( ctx.weapon != this->m_weapon )
		{
			this->reset( );
			this->m_weapon = ctx.weapon;
		}
		this->m_command = cmd->command_number;

		const auto is_gun = ctx.weapon_type >= cstypes::weapon_type::pistol
			&& ctx.weapon_type <= cstypes::weapon_type::lmg;
		const auto is_revolver = ctx.item_def_idx == cstypes::item_definition_index::weapon_r8_revolver;
		this->m_supported = is_gun && !is_revolver;
		if ( !this->m_supported )
		{
			this->reset_pair( );
			this->m_claim_valid = false;
			this->m_natural_ready = false;
			this->m_state = charge_state::failed;
			this->m_fail_reason = fail_reason::unavailable;
			this->m_progress = 0.0f;
			return;
		}

		const auto local = systems::g_local.get( );
		const auto* base = cmd->csgo_user_cmd.base( );
		if ( !local.is_alive || !local.pawn || !local.controller || !base )
		{
			this->reset( );
			this->m_state = charge_state::failed;
			this->m_fail_reason = fail_reason::unavailable;
			this->m_progress = 0.0f;
			return;
		}

		this->m_tick_base = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		this->m_claim_tick = ctx.next_primary_attack_tick;
		this->m_clip = ctx.clip;
		this->m_cycle_ticks = std::max( 1, static_cast<int>( std::ceil(
			std::max( ctx.cycle_time, cstypes::tick_interval ) / cstypes::tick_interval ) ) );

		// The DT primitive uses the weapon's published integer claim. Do not
		// synthesize first_claim + cycle: that was the old scheduler and merely
		// waited one normal fire interval. A zero/old ready tick is legal; only
		// a negative or implausibly far-future value is rejected.
		// fire_gun creates missing attack history later in the same command.
		// An empty native history is not a reason to disable the charge.
		this->m_claim_valid = this->m_claim_tick >= 0
			&& this->m_claim_tick <= this->m_tick_base + k_max_claim_lead_ticks;
		this->m_natural_ready = this->m_claim_valid && base->client_tick( ) >= this->m_claim_tick;

		if ( this->m_phase != pair_phase::idle && ( ctx.in_reload || this->m_clip <= 0 ) )
		{
			this->reset_pair( );
		}
		else if ( this->m_phase != pair_phase::idle && this->m_command > this->m_first_command
			&& this->m_command != this->m_second_command && this->m_tick_base > this->m_pair_deadline )
		{
			logging::console::print( xs( "[dt] follow-up expired (first claim {}, live claim {})" ),
				this->m_first_claim_tick, this->m_claim_tick );
			this->reset_pair( );
		}
		else if ( this->m_phase != pair_phase::idle && this->m_command > this->m_first_command
			&& this->m_command != this->m_second_command )
		{
			if ( this->m_clip <= this->m_first_clip - 2 )
			{
				// This is live client prediction, not a server hit acknowledgement.
				// Do not turn two submissions or a rebuilt command into a success.
				logging::console::print( xs( "[dt] two rounds observed in live weapon state" ) );
				this->reset_pair( );
			}
			else if ( this->m_phase == pair_phase::second_queued )
			{
				this->m_first_observed = this->m_clip == this->m_first_clip - 1
					&& this->m_claim_tick > this->m_first_claim_tick;
			}
		}

		if ( this->m_phase == pair_phase::second_submitted && this->m_command == this->m_second_command )
		{
			// Rebuild the same second shot with its original claim, never the
			// newly advanced cooldown that would describe a third bullet.
			this->m_claim_tick = this->m_second_claim_tick;
			this->m_claim_valid = this->m_claim_tick >= 0
				&& this->m_claim_tick <= this->m_tick_base + k_max_claim_lead_ticks;
		}

		if ( ctx.in_reload )
		{
			this->m_claim_valid = false;
			this->m_state = charge_state::failed;
			this->m_fail_reason = fail_reason::reloading;
			this->m_progress = 0.0f;
			return;
		}
		if ( this->m_clip <= ( this->second_pending( ) ? 0 : 1 ) )
		{
			this->m_state = charge_state::failed;
			this->m_fail_reason = fail_reason::empty_clip;
			this->m_progress = 0.0f;
			return;
		}
		if ( !this->m_claim_valid )
		{
			this->m_state = charge_state::failed;
			this->m_fail_reason = fail_reason::unavailable;
			this->m_progress = 0.0f;
			return;
		}

		this->m_fail_reason = fail_reason::none;
		if ( this->m_phase != pair_phase::idle )
		{
			this->m_state = this->second_ready( ) ? charge_state::ready : charge_state::charging;
			this->m_progress = this->m_phase == pair_phase::second_queued ? 0.5f : 0.0f;
			return;
		}

		if ( this->m_natural_ready )
		{
			this->m_state = charge_state::ready;
			this->m_progress = 1.0f;
			return;
		}

		this->m_state = charge_state::charging;
		const auto remaining = std::max( this->m_claim_tick - base->client_tick( ), 0 );
		this->m_progress = std::clamp( 1.0f - static_cast<float>( remaining ) /
			static_cast<float>( this->m_cycle_ticks ), 0.0f, 1.0f );
	}

	void rage::double_tap::on_shot_intent( systems::input::usercmd* cmd, bool fired )
	{
		if ( !fired || !cmd || !this->can_pair( ) ) return;
		if ( cmd->command_number == this->m_first_command ) return;
		if ( this->second_pending( ) && !this->second_ready( ) ) return;

		this->m_intent = true;
		this->m_intent_is_second = this->second_pending( );
		this->m_intent_command = cmd->command_number;
	}

	bool rage::double_tap::commit_pulse( systems::input::usercmd* cmd, int claim_tick ) const
	{
		if ( !cmd || claim_tick < 0 || cmd->csgo_user_cmd.input_history_size( ) <= 0 ) return false;
		auto* base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base ) return false;

		bool has_history{};
		for ( auto i = 0; i < cmd->csgo_user_cmd.input_history_size( ); ++i )
		{
			if ( cmd->csgo_user_cmd.mutable_input_history( i ) )
			{
				has_history = true;
				break;
			}
		}
		if ( !has_history ) return false;

		auto* moves = base->mutable_subtick_moves( );
		if ( !moves ) return false;
		const auto original_size = moves->m_current_size;
		proto::subtick_move_step* press{};
		proto::subtick_move_step* release{};
		for ( auto i = 0; i < base->subtick_moves_size( ); ++i )
		{
			auto* step = base->mutable_subtick_moves( i );
			if ( !step || step->button( ) != cstypes::command_buttons::in_attack
				|| step->m_has_bits.test( 0x78u ) ) continue;
			if ( step->pressed( ) && step->when( ) == k_attack_press_when ) press = step;
			if ( !step->pressed( ) && step->when( ) == k_attack_release_when ) release = step;
		}
		const auto missing_edges = static_cast<int>( !press ) + static_cast<int>( !release );
		if ( original_size > k_max_subtick_moves - missing_edges ) return false;
		if ( !press ) press = systems::g_input.acquire_subtick_step( moves );
		if ( !release ) release = systems::g_input.acquire_subtick_step( moves );
		if ( !press || !release )
		{
			// acquire_subtick_step may consume one reusable slot before the
			// second allocation fails. Hide the incomplete pulse atomically.
			moves->m_current_size = original_size;
			return false;
		}

		for ( auto i = 0; i < cmd->csgo_user_cmd.input_history_size( ); ++i )
		{
			if ( auto* entry = cmd->csgo_user_cmd.mutable_input_history( i ) )
			{
				entry->set_player_tick_count( claim_tick );
				entry->set_player_tick_fraction( 0.0f );
			}
		}

		press->set_button( cstypes::command_buttons::in_attack );
		press->set_pressed( true );
		press->set_when( k_attack_press_when );
		release->set_button( cstypes::command_buttons::in_attack );
		release->set_pressed( false );
		release->set_when( k_attack_release_when );

		// DT finalizes after every movement/script stage, then restores
		// chronological order without disturbing equal-time movement events.
		// The release edge makes consecutive commands distinct clicks instead
		// of a held automatic-fire stream.
		if ( moves->m_rep && moves->m_current_size > 1 )
		{
			std::stable_sort( moves->m_rep->elements,
				moves->m_rep->elements + moves->m_current_size,
				[]( void* lhs, void* rhs ) { return subtick_when( lhs ) < subtick_when( rhs ); } );
		}

		const auto attack = static_cast<std::uintptr_t>( cstypes::command_buttons::in_attack );
		cmd->buttons.value |= attack;
		cmd->buttons.value_changed |= attack;
		cmd->buttons.value_scroll |= attack;
		cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
		return true;
	}

	void rage::double_tap::finalize_command( systems::input::usercmd* cmd )
	{
		if ( !this->m_intent || !cmd || cmd->command_number != this->m_intent_command ) return;

		const auto second = this->m_intent_is_second;
		this->m_intent = false;
		this->m_intent_is_second = false;
		this->m_intent_command = -1;

		const auto attack = static_cast<std::uintptr_t>( cstypes::command_buttons::in_attack );
		const auto cancel_followup = [ & ]
		{
			cmd->buttons.value &= ~attack;
			cmd->buttons.value_changed |= attack;
			cmd->buttons.value_scroll &= ~attack;
			cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			cmd->csgo_user_cmd.m_has_bits.clear( 0x20u );
			if ( auto* base = cmd->csgo_user_cmd.mutable_base( ) )
			{
				for ( auto i = 0; i < base->subtick_moves_size( ); ++i )
				{
					if ( auto* step = base->mutable_subtick_moves( i ); step && ( step->button( ) & attack ) != 0 )
						step->set_button( step->button( ) & ~attack );
				}
			}
			this->reset_pair( );
			this->m_state = charge_state::charging;
			this->m_progress = 0.0f;
		};
		const auto index = cmd->csgo_user_cmd.attack1_start_history_index( );
		if ( ( cmd->buttons.value & attack ) == 0 || index < 0
			|| index >= cmd->csgo_user_cmd.input_history_size( )
			|| !cmd->csgo_user_cmd.mutable_input_history( index ) )
		{
			if ( second ) cancel_followup( );
			return;
		}

		if ( !second )
		{
			if ( this->m_phase != pair_phase::idle ) return;
			// Keep the first shot's validated ordinary attack index, time and
			// NoSpread seed. No stale next-attack claim is stamped over it.
			this->m_phase = pair_phase::second_queued;
			this->m_first_command = cmd->command_number;
			this->m_first_claim_tick = this->m_claim_tick;
			this->m_first_clip = this->m_clip;
			this->m_pair_deadline = this->m_tick_base + this->m_cycle_ticks + k_pair_window_ticks;
			this->m_state = charge_state::charging;
			this->m_progress = 0.5f;
			logging::console::print( xs( "[dt] first submitted in command {}; waiting for weapon state" ),
				this->m_first_command );
			return;
		}

		if ( !this->commit_pulse( cmd, this->m_claim_tick ) )
		{
			// This intent bypassed the ordinary cooldown. Without a complete DT
			// pulse it is not a valid normal shot, so never transmit it as one.
			cancel_followup( );
			logging::console::print( xs( "[dt] follow-up cancelled: pulse unavailable" ) );
			return;
		}

		if ( this->m_phase == pair_phase::second_submitted ) return;

		this->m_phase = pair_phase::second_submitted;
		this->m_second_command = cmd->command_number;
		this->m_second_claim_tick = this->m_claim_tick;
		this->m_pair_deadline = this->m_tick_base + this->m_cycle_ticks + k_pair_window_ticks;
		this->m_state = charge_state::charging;
		this->m_progress = 0.0f;
		logging::console::print( xs( "[dt] follow-up submitted in command {} at live claim {}" ),
			this->m_second_command, this->m_second_claim_tick );
	}

	void rage::double_tap::on_render( xdraw::draw_list& draw_list ) const
	{
		const auto& cfg = settings::g_combat.m_ragebot.m_double_tap;
		if ( !cfg.enabled.value || !cfg.preview.value ) return;
		if ( systems::g_local.is_in_cinematic( ) || systems::g_local.is_in_time_freeze( ) ) return;

		const auto local = systems::g_local.get( );
		if ( !local.controller || local.team < 2 ) return;

		const auto [ screen_w, screen_h ] = xdraw::viewport_size( );
		const auto cx = static_cast<float>( screen_w ) * 0.5f;
		const auto& s = xui::ctx( ).style;

		xdraw::color state_col{ 255, 255, 255, 255 };
		switch ( this->m_state )
		{
		case charge_state::charging: state_col = cfg.charging_color; break;
		case charge_state::ready: state_col = cfg.ready_color; break;
		case charge_state::failed: state_col = cfg.failed_color; break;
		default: break;
		}

		constexpr auto bar_w{ 214.0f };
		constexpr auto bar_h{ 16.0f };
		constexpr auto pad{ 2.0f };
		constexpr auto r{ 8.0f };
		constexpr auto bottom_offset{ 170.0f };
		const auto bar_x = std::floor( cx - bar_w * 0.5f );
		const auto bar_y = std::floor( static_cast<float>( screen_h ) - bottom_offset - bar_h );

		draw_list.rect_filled_blurred( bar_x - pad, bar_y - pad, bar_w + pad * 2.0f, bar_h + pad * 2.0f, xdraw::corner_radius{ r } );
		draw_list.rect_filled( bar_x - pad, bar_y - pad, bar_w + pad * 2.0f, bar_h + pad * 2.0f, s.window_bg, xdraw::corner_radius{ r } );
		draw_list.rect_filled( bar_x, bar_y, bar_w, bar_h, s.child_bg, xdraw::corner_radius{ r - pad } );

		const auto fill_w = std::floor( bar_w * this->m_progress );
		if ( fill_w >= 1.0f )
		{
			draw_list.rect_filled( bar_x, bar_y, fill_w, bar_h, state_col.alpha( 220 ), xdraw::corner_radius{ r - pad } );
		}

		char pct_buf[ 8 ]{};
		std::snprintf( pct_buf, sizeof( pct_buf ), "%.0f%%", this->m_progress * 100.0f );
		const auto [ pct_w, pct_h ] = xdraw::measure_text( pct_buf );
		draw_list.text( bar_x + bar_w - pct_w - 6.0f, bar_y + ( bar_h - pct_h ) * 0.5f + 0.5f, pct_buf, s.text );
	}

} // namespace features::combat
