#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/diag.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {
	namespace {
		constexpr std::size_t k_max_accuracy_state_size{ 0x100 };

		void clear_attack( systems::input::usercmd* cmd )
		{
			if ( !cmd ) return;
			const auto attacks = static_cast<std::uintptr_t>( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
			cmd->buttons.value &= ~attacks;
			cmd->buttons.value_changed |= attacks;
			cmd->buttons.value_scroll &= ~attacks;
			cmd->csgo_user_cmd.set_attack1_start_history_index( -1 );
			cmd->csgo_user_cmd.set_attack2_start_history_index( -1 );
		}

		struct accuracy_state_range
		{
			std::uintptr_t address{};
			int offset{};
			std::size_t size{};
		};

		[[nodiscard]] std::optional<accuracy_state_range> get_accuracy_state_range( std::uintptr_t weapon )
		{
			const auto begin = SCHEMA( "C_CSWeaponBase", "m_flTurningInaccuracyDelta"_hash );
			const auto end = SCHEMA( "C_CSWeaponBase", "m_flRecoilIndex"_hash );
			if ( !weapon || begin <= 0 || end < begin ) return std::nullopt;

			const auto size = static_cast<std::size_t>( end - begin ) + sizeof( float );
			if ( size == 0 || size > k_max_accuracy_state_size ) return std::nullopt;

			return accuracy_state_range{ weapon + begin, begin, size };
		}

		class scoped_accuracy_state
		{
		public:
			scoped_accuracy_state( const accuracy_state_range& range, const std::vector<std::uint8_t>& state )
				: m_range( range )
			{
				if ( state.size( ) != range.size ) return;
				this->m_original.resize( range.size );
				std::memcpy( this->m_original.data( ), reinterpret_cast<const void*>( range.address ), range.size );
				std::memcpy( reinterpret_cast<void*>( range.address ), state.data( ), range.size );
				this->m_active = true;
			}

			~scoped_accuracy_state( )
			{
				if ( this->m_active )
				{
					std::memcpy( reinterpret_cast<void*>( this->m_range.address ), this->m_original.data( ), this->m_range.size );
				}
			}

			[[nodiscard]] bool active( ) const noexcept { return this->m_active; }

		private:
			accuracy_state_range m_range{};
			std::vector<std::uint8_t> m_original{};
			bool m_active{};
		};

		class scoped_bool_override
		{
		public:
			scoped_bool_override( bool& value, bool replacement )
				: m_value( value ), m_original( value )
			{
				this->m_value = replacement;
			}

			~scoped_bool_override( ) { this->m_value = this->m_original; }

		private:
			bool& m_value;
			bool m_original{};
		};

	}

	shared::shot_spread_state rage::capture_no_spread_state( )
	{
		const auto range = get_accuracy_state_range( g_shared.ctx( ).weapon );
		if ( !range )
		{
			this->m_no_spread_accuracy.reset( );
			return g_shared.capture_shot_spread_state( );
		}

		std::vector<std::uint8_t> live( range->size );
		std::memcpy( live.data( ), reinterpret_cast<const void*>( range->address ), range->size );

		const auto matches_live = this->m_no_spread_accuracy.valid &&
			this->m_no_spread_accuracy.weapon == g_shared.ctx( ).weapon &&
			this->m_no_spread_accuracy.offset == range->offset &&
			this->m_no_spread_accuracy.baseline.size( ) == range->size &&
			this->m_no_spread_accuracy.predicted.size( ) == range->size &&
			std::memcmp( live.data( ), this->m_no_spread_accuracy.baseline.data( ), range->size ) == 0;
		if ( !matches_live )
		{
			this->m_no_spread_accuracy.reset( );
			return g_shared.capture_shot_spread_state( );
		}

		scoped_accuracy_state scoped{ *range, this->m_no_spread_accuracy.predicted };
		if ( !scoped.active( ) )
		{
			this->m_no_spread_accuracy.reset( );
			return g_shared.capture_shot_spread_state( );
		}

		// The cached bytes are immediately after the previous shot. Let the
		// engine advance them to this command's time before reading its cone.
		memory::call<void>( PATTERN( patterns::weapon_update_accuracy ), g_shared.ctx( ).weapon );
		std::memcpy( this->m_no_spread_accuracy.predicted.data( ), reinterpret_cast<const void*>( range->address ), range->size );
		return g_shared.capture_shot_spread_state( );
	}

	void rage::advance_no_spread_state( systems::input::usercmd* cmd, const systems::local::snapshot& local )
	{
		const auto range = get_accuracy_state_range( g_shared.ctx( ).weapon );
		if ( !cmd || !local.pawn || !range )
		{
			this->m_no_spread_accuracy.reset( );
			return;
		}

		std::vector<std::uint8_t> live( range->size );
		std::memcpy( live.data( ), reinterpret_cast<const void*>( range->address ), range->size );

		std::vector<std::uint8_t> predicted = this->m_no_spread_accuracy.valid &&
			this->m_no_spread_accuracy.weapon == g_shared.ctx( ).weapon &&
			this->m_no_spread_accuracy.offset == range->offset &&
			this->m_no_spread_accuracy.predicted.size( ) == range->size
			? this->m_no_spread_accuracy.predicted
			: live;

		scoped_accuracy_state scoped{ *range, predicted };
		if ( !scoped.active( ) )
		{
			this->m_no_spread_accuracy.reset( );
			return;
		}

		std::vector<std::uint8_t> post_shot{};
		scoped_bool_override prediction_scope{ this->m_no_spread_state_prediction, true };
		const auto predicted_fire = systems::g_prediction.simulate( cmd, local, [ & ]
			{
				post_shot.resize( range->size );
				std::memcpy( post_shot.data( ), reinterpret_cast<const void*>( range->address ), range->size );
			} );
		if ( !predicted_fire || post_shot.size( ) != range->size )
		{
			this->m_no_spread_accuracy.reset( );
			return;
		}

		this->m_no_spread_accuracy.weapon = g_shared.ctx( ).weapon;
		this->m_no_spread_accuracy.offset = range->offset;
		this->m_no_spread_accuracy.baseline = std::move( live );
		this->m_no_spread_accuracy.predicted = std::move( post_shot );
		this->m_no_spread_accuracy.valid = true;
	}

	void rage::finalize_no_spread_state( systems::input::usercmd* cmd, const systems::local::snapshot& local )
	{
		if ( !this->m_no_spread_state_pending ) return;
		this->m_no_spread_state_pending = false;

		const auto& weapon = g_shared.ctx( );
		const auto is_gun = weapon.weapon_type >= cstypes::weapon_type::pistol && weapon.weapon_type <= cstypes::weapon_type::lmg;
		if ( !cmd || !local.is_alive || !local.pawn || !local.controller || !weapon.valid || !is_gun ||
			!settings::g_combat.m_ragebot.enabled || !settings::g_combat.m_ragebot.get_group( weapon.weapon_type ).no_spread.value )
		{
			this->m_no_spread_accuracy.reset( );
			return;
		}

		const auto attacks = static_cast<std::uintptr_t>( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack );
		if ( ( cmd->buttons.value & attacks ) == 0 ) return;

		this->advance_no_spread_state( cmd, local );
	}

	void rage::stamp_history( systems::input::usercmd* cmd, const scan_hit& hit )
	{
		if ( !cmd ) return;
		const auto record_time = cstypes::tick_fraction::from_value( hit.record ? hit.record->simulation_time / cstypes::tick_interval : 0.0f );
		for ( auto i = 0; i < cmd->csgo_user_cmd.input_history_size( ); ++i )
		{
			const auto entry = cmd->csgo_user_cmd.mutable_input_history( i );
			if ( !entry ) continue;
			if ( const auto angles = entry->mutable_view_angles( ) ) { angles->set_x( hit.aim_angle.x ); angles->set_y( hit.aim_angle.y ); angles->set_z( hit.aim_angle.z ); }
			entry->set_render_tick_count( record_time.tick + 1 );
			entry->set_render_tick_fraction( 0.0f );
			if ( !hit.source_eye.is_uninterpolated )
			{
				const auto frac = hit.source_eye.player_frac + hit.source_eye.lerp_ticks_frac;
				entry->set_player_tick_count( hit.source_eye.player_tick + hit.source_eye.lerp_ticks_int + static_cast<int>( frac ) );
				entry->set_player_tick_fraction( frac - std::floorf( frac ) );
			}
			if ( entry->has_sv_interp0( ) ) { auto* x = entry->mutable_sv_interp0( ); x->set_src_tick( -1 ); x->set_dst_tick( -1 ); x->set_frac( 0.0f ); }
			if ( entry->has_sv_interp1( ) ) { auto* x = entry->mutable_sv_interp1( ); x->set_src_tick( -1 ); x->set_dst_tick( -1 ); x->set_frac( 0.0f ); }
			if ( entry->has_cl_interp( ) ) entry->mutable_cl_interp( )->set_frac( 0.0f );
		}
	}

	bool rage::fire_gun( systems::input::usercmd* cmd, const target& target, bool forced, const math::vector3& shoot_eye, const systems::local::snapshot& local )
	{
		if ( !cmd || !target.valid || !target.hit.record || !target.hit.record->valid || g_shared.ctx( ).clip <= 0 || g_shared.ctx( ).in_reload ) return false;
		auto* base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base || !base->viewangles( ) ) return false;
		const auto revolver = g_shared.ctx( ).item_def_idx == cstypes::item_definition_index::weapon_r8_revolver;
		if ( revolver )
		{
			if ( !g_shared.can_shoot( cmd, local.controller ) || !this->revolver_ready_to_fire( cmd ) ) return false;
		}
		else
		{
			const auto pending_second = this->m_double_tap.second_pending( );
			if ( !g_shared.can_shoot( cmd, local.controller, !pending_second ) ) return false;
			// Do not guess a cycle-derived claim. A queued follow-up uses the live
			// next-primary-attack claim exposed for this command.
			if ( pending_second && !this->m_double_tap.second_ready( ) ) return false;
		}
		const auto& config = settings::g_combat.m_ragebot.get_group( g_shared.ctx( ).weapon_type );
		const auto punch = g_shared.get_aim_punch( local.pawn );
		const auto aim = config.no_spread.value ? math::helpers::calculate_angle( shoot_eye, target.hit.position ) : target.hit.aim_angle;

		// Final angles written to the command. Both paths end up punch-subtracted:
		// the server re-adds aim punch when it builds the bullet direction.
		auto transmitted = math::vector3{ aim.x - punch.x, aim.y - punch.y, 0.0f };
		bool history_created{};
		if ( cmd->csgo_user_cmd.input_history_size( ) == 0 )
		{
			// Native CreateMove can emit no history without a physical input frame.
			// An automatic shot still needs an attack-time entry, before seed selection.
			const auto record_time = cstypes::tick_fraction::from_value( target.hit.record->simulation_time / cstypes::tick_interval );
			systems::input::input_history_params params{};
			params.view_angles = transmitted;
			params.shoot_position = shoot_eye;
			params.render_tick = record_time.tick + 1;
			params.player_tick = base->client_tick( );
			params.player_interp_src = params.player_tick;
			params.player_interp_dst = params.player_tick;
			params.sv_interp0_src = params.sv_interp0_dst = -1;
			params.sv_interp1_src = params.sv_interp1_dst = -1;
			history_created = systems::g_input.push_input_history( cmd, params ) != nullptr;
		}
		const auto attack_history_index = cmd->csgo_user_cmd.input_history_size( ) - 1;
		auto* attack_history = attack_history_index >= 0 ? cmd->csgo_user_cmd.mutable_input_history( attack_history_index ) : nullptr;
		if ( !attack_history || !attack_history->mutable_view_angles( ) || !attack_history->has_player_tick_count( ) || attack_history->player_tick_count( ) < 0 )
		{
			clear_attack( cmd );
			cmd->csgo_user_cmd.m_has_bits.clear( 0x60u );
			static unsigned failures{};
			if ( failures < 4 )
			{
				++failures;
				diag::writef( diag::level::warning, "[rage-fire] rejected invalid history: cmd=%d count=%d index=%d",
					cmd->command_number, cmd->csgo_user_cmd.input_history_size( ), attack_history_index );
			}
			return false;
		}

		const auto dt_claim_tick = config.no_spread.value ? this->m_double_tap.attack_claim_tick( ) : -1;
		const auto apply_dt_claim_tick = [ & ]
		{
			if ( dt_claim_tick < 0 ) return;
			for ( auto i = 0; i < cmd->csgo_user_cmd.input_history_size( ); ++i )
			{
				if ( const auto entry = cmd->csgo_user_cmd.mutable_input_history( i ) )
				{
					entry->set_player_tick_count( dt_claim_tick );
					entry->set_player_tick_fraction( 0.0f );
				}
			}
		};

		if ( config.no_spread.value )
		{
			// Server-side gun fire resolves an attack-time history entry and uses
			// that entry's local player tick in the seed hash. Lag-comp render
			// time describes the target only; it is not a valid spread-seed tick.
			stamp_history( cmd, target.hit );
			apply_dt_claim_tick( );

			auto seed_tick = dt_claim_tick;
			if ( seed_tick < 0 && cmd->csgo_user_cmd.input_history_size( ) > 0 )
			{
				if ( const auto entry = cmd->csgo_user_cmd.mutable_input_history( cmd->csgo_user_cmd.input_history_size( ) - 1 ); entry && entry->has_player_tick_count( ) )
				{
					seed_tick = entry->player_tick_count( );
				}
			}

			const auto spread_state = this->capture_no_spread_state( );
			const auto correction = g_shared.find_spread_correction( aim, seed_tick, punch, spread_state );
			if ( !correction )
			{
				// A failed closure must not leak the original manual command as a
				// raw-cone shot.
				clear_attack( cmd );
				return false;
			}
			transmitted = *correction;
		}
		auto committed_hit = target.hit;
		committed_hit.aim_angle = transmitted;
		stamp_history( cmd, committed_hit );
		apply_dt_claim_tick( );

		// fire_gun always writes a complete ordinary shot first. DT may replace
		// that expression with a claimed press/release pulse only in the final
		// create_move stage; if pulse allocation fails this path remains valid.
		{
			clear_attack( cmd );
			// Native CreateMove clears presence bits for -1 (unused) attack indices.
			cmd->csgo_user_cmd.m_has_bits.clear( 0x60u );
			// The R8 releases the hauled-back hammer on the secondary channel;
			// primary is the wind-up and clear_attack has just released it.
			const auto attack = revolver ? cstypes::command_buttons::in_second_attack : cstypes::command_buttons::in_attack;
			cmd->buttons.value |= attack;
			cmd->buttons.value_changed |= attack;
			cmd->buttons.value_scroll |= attack;
			if ( revolver ) cmd->csgo_user_cmd.set_attack2_start_history_index( attack_history_index );
			else cmd->csgo_user_cmd.set_attack1_start_history_index( attack_history_index );
		}
		const auto& command_aim = transmitted;
		auto base_aim = command_aim;
		if ( base->viewangles( ) )
		{
			math::vector3 forward{};
			math::helpers::angle_vectors_left( { base->viewangles( )->x( ), base->viewangles( )->y( ), base->viewangles( )->z( ) }, &forward );
			if ( settings::g_combat.m_antiaim.hide_shots.value &&
				forward.dot( ( target.hit.record->origin - systems::g_prediction.pre( ).networked_origin ).normalized( ) ) < 0.707107f )
			{
				base_aim.x = 179.9f;
				base_aim.y = std::remainderf( command_aim.y + 180.0f, 360.0f );
			}
		}
		// The roll carries the no-spread lateral compensation. The seed hash
		// uses pitch/yaw, while roll still has to reach both command angle
		// copies so the weapon's spread basis is rotated consistently.
		base->mutable_viewangles( )->set_x( base_aim.x );
		base->mutable_viewangles( )->set_y( base_aim.y );
		base->mutable_viewangles( )->set_z( base_aim.z );
		// Keep the local camera roll-free; the command already carries the roll.
		if ( !config.silent.value ) systems::g_input.set_view_angles( { command_aim.x, command_aim.y, 0.0f } );
		static unsigned shot_reports{};
		if ( shot_reports < 8 )
		{
			++shot_reports;
			diag::writef( diag::level::info,
				"[rage-fire] cmd=%d count=%d index=%d mask=%x player=%d+%.5f render=%d+%.5f base=%d buttons=%llx/%llx/%llx created=%d",
				cmd->command_number, cmd->csgo_user_cmd.input_history_size( ), attack_history_index, cmd->csgo_user_cmd.m_has_bits.bits[ 0 ],
				attack_history->player_tick_count( ), attack_history->player_tick_fraction( ),
				attack_history->render_tick_count( ), attack_history->render_tick_fraction( ), base->client_tick( ),
				static_cast<unsigned long long>( cmd->buttons.value ), static_cast<unsigned long long>( cmd->buttons.value_changed ),
				static_cast<unsigned long long>( cmd->buttons.value_scroll ), static_cast<int>( history_created ) );
		}
		if ( config.no_spread.value ) this->m_no_spread_state_pending = true;
		// Submission is not a discharged bullet. Native CreateMove may rebuild
		// this command before it is sent; consuming the local two-tick delay here
		// would suppress its attack on that rebuild. Keep the weapon cooldown
		// check above authoritative until the weapon actually advances it.
		features::esp::player::g_chams.os( ).push( target.hit.pawn );
		features::misc::g_impacts.on_boom( target.hit.pawn, target.hit.hitgroup, target.hit.damage, target.hitchance,
			g_shared.ctx( ).inaccuracy, g_shared.ctx( ).spread, aim, shoot_eye, target.hit.record->tick,
			g_shared.lc( ).get_skeleton( *target.hit.record ), forced );
		if ( settings::g_misc.m_impacts.hit_display.value && settings::g_misc.m_impacts.console_log.value )
			logging::console::print( xs( "[rage] shot target hp {} for {:.0f} (hc {:.0f}%{})" ), target.hit.health, target.hit.damage, target.hitchance * 100.0f, forced ? xs( ", forced" ) : xs( "" ) );
		this->m_firing_this_tick = true;
		return true;
	}

	bool rage::fire_melee( systems::input::usercmd* cmd, const target& target, const systems::local::snapshot& local )
	{
		if ( !cmd || !target.valid || !target.hit.record || !target.hit.record->valid || !g_shared.can_shoot( cmd, local.controller ) ) return false;
		auto* base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base || !base->viewangles( ) ) return false;
		stamp_history( cmd, target.hit );
		clear_attack( cmd );
		const auto attack = target.hit.attack_type == 1 ? cstypes::command_buttons::in_second_attack : cstypes::command_buttons::in_attack;
		cmd->buttons.value |= attack;
		cmd->buttons.value_changed |= attack;
		cmd->buttons.value_scroll |= attack;
		if ( cmd->csgo_user_cmd.input_history_size( ) > 0 )
		{
			if ( target.hit.attack_type == 1 ) cmd->csgo_user_cmd.set_attack2_start_history_index( cmd->csgo_user_cmd.input_history_size( ) - 1 );
			else cmd->csgo_user_cmd.set_attack1_start_history_index( cmd->csgo_user_cmd.input_history_size( ) - 1 );
		}
		base->mutable_viewangles( )->set_x( target.hit.aim_angle.x );
		base->mutable_viewangles( )->set_y( target.hit.aim_angle.y );
		g_shared.last_shoot_tick( ) = memory::read<int>( local.controller + SCHEMA( "CBasePlayerController", "m_nTickBase"_hash ) );
		this->m_firing_this_tick = true;
		return true;
	}
}
