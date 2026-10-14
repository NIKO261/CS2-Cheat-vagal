#include <pch/pch.hpp>
#include <utilities/logging/logging.hpp>
#include <utilities/steam/steam.hpp>
#include "../systems.hpp"
#include "auth_channel.hpp"
#include <external/nlohmann/json.hpp>

// 账号数据模块。数据来源与语义：
// - 注入器（NikoLoader）在注入前创建命名共享内存（auth_channel.hpp 契约），
//   交付 username / hwid / access_token；启动即读不到 = 未走注入器，直接
//   进入失联态（仅显示降级，无功能影响）。
// - 复验复用 user_status.php（Bearer access_token），仅刷新 sub_until 并
//   探测服务器可达性；到期/封禁不做任何提示或门禁（用户明确要求）。
// - access_token 不刷新不落盘（token.php 单 token 制，DLL 侧刷新会踢掉
//   注入器的缓存令牌）；401/解析失败按普通失败计数。

namespace systems {

	namespace {

		constexpr auto k_api_base{ "https://www.nikosense.top/api/v1/user_status.php?username=" };
		constexpr auto k_revalidate_seconds{ 60 };
		constexpr auto k_max_failures{ 3 };
		constexpr auto k_network_timeout_seconds{ 10 };
		constexpr auto k_request_wait_ms{ 15000 };

		// username 可能含非 ASCII/保留字符，hwid 为固定十六进制串——统一编码无妨。
		[[nodiscard]] std::string percent_encode( std::string_view value )
		{
			static constexpr char hex[]{ "0123456789ABCDEF" };
			std::string out{};
			out.reserve( value.size( ) );
			for ( const auto byte : value )
			{
				const auto c = static_cast<unsigned char>( byte );
				if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ||
					c == '-' || c == '_' || c == '.' || c == '~' )
				{
					out.push_back( static_cast<char>( c ) );
				}
				else
				{
					out.push_back( '%' );
					out.push_back( hex[ c >> 4 ] );
					out.push_back( hex[ c & 0x0F ] );
				}
			}
			return out;
		}

		[[nodiscard]] std::string date_part( const std::string& sub_until )
		{
			return sub_until.size( ) >= 10 ? sub_until.substr( 0, 10 ) : sub_until;
		}

	} // namespace

	bool account::initialize( )
	{
		std::string username{};
		std::string hwid{};
		std::string token{};
		this->m_channel = auth_channel::consume( username, hwid, token );

		{
			std::lock_guard lock( this->m_mtx );
			if ( this->m_channel )
			{
				this->m_username = std::move( username );
				this->m_hwid = std::move( hwid );
				this->m_access_token = std::move( token );
				this->m_snap.status = state::linked;
				this->m_snap.username = this->m_username;
			}
			else
			{
				// 未走注入器（直接启动游戏）：无凭证可复验，保持失联显示。
				this->m_snap.status = state::lost;
			}
		}

		if ( !this->m_channel ) return true;

		try
		{
			this->m_thread = std::thread( [ this ] { this->worker( ); } );
		}
		catch ( const std::system_error& )
		{
			logging::console::print( xs( "warning: account worker thread unavailable" ) );
			return false;
		}
		return true;
	}

	void account::shutdown( )
	{
		this->m_stop.store( true, std::memory_order_relaxed );
		if ( this->m_thread.joinable( ) ) this->m_thread.join( );
	}

	account::snapshot account::get( ) const
	{
		std::lock_guard lock( this->m_mtx );
		return this->m_snap;
	}

	void account::worker( )
	{
		while ( !this->m_stop.load( std::memory_order_relaxed ) )
		{
			this->revalidate( );
			for ( auto second = 0; second < k_revalidate_seconds && !this->m_stop.load( std::memory_order_relaxed ); ++second )
				std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
		}
	}

	bool account::revalidate( )
	{
		const auto url = std::format( "{}{}&hwid={}", k_api_base,
			percent_encode( this->m_username ), percent_encode( this->m_hwid ) );
		const auto bearer = std::format( "Bearer {}", this->m_access_token );

		const auto request = steam::http::create_get( url.c_str( ) );
		if ( !request ) return this->on_failure( );
		steam::http::set_header( request, "Authorization", bearer.c_str( ) );
		steam::http::set_timeout( request, k_network_timeout_seconds );

		const auto sent = steam::http::send_and_wait( request, k_request_wait_ms );
		std::vector<std::uint8_t> body{};
		const auto got_body = sent && steam::http::get_response_body( request, body );
		steam::http::release( request );
		if ( !got_body || body.empty( ) ) return this->on_failure( );

		try
		{
			const auto doc = nlohmann::json::parse( body.begin( ), body.end( ), nullptr, false );
			if ( !doc.is_object( ) ) return this->on_failure( );
			const auto sub_until = doc.value( "sub_until", std::string{} );
			if ( sub_until.empty( ) ) return this->on_failure( );

			std::lock_guard lock( this->m_mtx );
			this->m_snap.sub_until = date_part( sub_until );
			this->m_snap.status = state::linked;
			this->m_failures = 0;
			return true;
		}
		catch ( const std::exception& )
		{
			return this->on_failure( );
		}
	}

	bool account::on_failure( )
	{
		std::lock_guard lock( this->m_mtx );
		++this->m_failures;
		if ( this->m_failures >= k_max_failures ) this->m_snap.status = state::lost;
		return false;
	}

} // namespace systems
