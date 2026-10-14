#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <utilities/logging/logging.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

namespace features::misc {

	void invalid_steam_logon::on_frame_stage_notify ( )
	{
		const auto enabled = settings::g_misc.m_invalid_steam_logon.enabled.value;

		// Edge-triggered arm: false->true transition starts a fresh burst.
		if ( enabled != this->m_was_enabled )
		{
			this->m_was_enabled = enabled;
			if ( enabled )
			{
				this->m_remaining = k_burst_sends;
			}
		}

		if ( !enabled || this->m_remaining <= 0 )
		{
			return;
		}

		if ( !this->m_resolved && !this->resolve ( ) )
		{
			this->m_remaining = 0;
			return;
		}

		this->send ( k_logon_reason );
		this->m_remaining--;
	}

	bool invalid_steam_logon::resolve ( )
	{
		const auto engine = addresses::globals::source2engine_to_client;
		if ( !engine )
		{
			logging::console::print ( xs ( "[error] invalid steam logon: Source2EngineToClient001 missing" ) );
			return false;
		}

		const auto dispatch = *reinterpret_cast<void**>(
			*reinterpret_cast<std::uintptr_t*>( engine ) + k_dispatch_vtable_offset );

		// tier0 exports; primary set with the PoC's fallback mangled names for
		// older tier0 builds.
		auto kv_new     = MODULE_EXPORT ( "tier0.dll:??2KeyValues@@SAPEAX_K@Z" );
		const auto kv_ctor = MODULE_EXPORT ( "tier0.dll:??0KeyValues@@QEAA@PEBDPEAVIKeyValuesSystem@@_N@Z" );
		auto kv_set_int = MODULE_EXPORT ( "tier0.dll:?SetInt@KeyValues@@QEAAXPEBDH@Z" );
		const auto kv_dtor = MODULE_EXPORT ( "tier0.dll:??1KeyValues@@QEAA@XZ" );

		if ( !kv_new )
		{
			kv_new = MODULE_EXPORT ( "tier0.dll:??2KeyValues@@SAPEAX_KHPEBDH@Z" );
		}
		if ( !kv_set_int )
		{
			kv_set_int = MODULE_EXPORT ( "tier0.dll:?SetInt@KeyValues@@QAEXPEBDH@Z" );
		}

		if ( !dispatch || !kv_new || !kv_ctor || !kv_set_int || !kv_dtor )
		{
			logging::console::print ( xs ( "[error] invalid steam logon: KeyValues dispatch not resolved" ) );
			return false;
		}

		this->m_engine     = reinterpret_cast<void*>( engine );
		this->m_dispatch   = reinterpret_cast<dispatch_fn>( dispatch );
		this->m_kv_new     = reinterpret_cast<kv_new_fn>( kv_new );
		this->m_kv_ctor    = reinterpret_cast<kv_ctor_fn>( kv_ctor );
		this->m_kv_set_int = reinterpret_cast<kv_set_int_fn>( kv_set_int );
		this->m_kv_dtor    = reinterpret_cast<kv_dtor_fn>( kv_dtor );
		this->m_resolved   = true;

		return true;
	}

	void invalid_steam_logon::send ( int reason )
	{
		if ( !this->m_engine || !this->m_dispatch )
		{
			return;
		}

		const auto kv = this->m_kv_new ( 0x1C );
		if ( !kv )
		{
			return;
		}

		this->m_kv_ctor ( kv, xs ( "InvalidSteamLogon" ), nullptr, false );
		this->m_kv_set_int ( kv, xs ( "reason" ), reason );
		this->m_dispatch ( this->m_engine, kv );
		this->m_kv_dtor ( kv );
	}

} // namespace features::misc
