#pragma once

#include <cstdint>
#include <limits>
#include <utilities/addresses/addresses.hpp>
#include <utilities/memory/memory.hpp>
#include <protection/game_addresses.hpp>

namespace addresses {

	inline void client_command( const char* command )
	{
		// Current CEngineClient ABI: native callers use a NaN time and a zero context.
		memory::call<void>( PATTERN( patterns::engine_client_cmd ),
			globals::source2engine_to_client, 0, command, true,
			std::numeric_limits<double>::quiet_NaN( ), std::uint64_t{ 0 } );
	}

} // namespace addresses
