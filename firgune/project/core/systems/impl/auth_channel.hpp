#pragma once

// NikoLoader → vagal.dll 认证数据通道（命名共享内存，AES-256-GCM 加密+认证）。
// 双端契约：firgune/project/core/systems/impl/auth_channel.hpp 与
// deepseek_inject/src/auth_channel.hpp 必须保持逐字节一致——改动字段布局、
// 密钥或流程时两端同步更新并递增 k_version。
//
// 威胁模型说明：密钥内嵌于两端二进制，逆向任一端可提取后伪造通道；本通道
// 定位为抬高脚本小子门槛（显示数据无功能门禁，订阅的真正防线在服务器侧
// 的 DLL 下载校验）。bcrypt 动态加载，不进两端导入表。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

namespace auth_channel {

	inline constexpr std::uint32_t k_magic{ 0x4C4B4F4Eu }; // "NOKL"
	inline constexpr std::uint32_t k_version{ 1u };
	// 映射内时间戳允许的年龄窗口（秒）：publish 发生在注入前数秒，
	// 超窗即视为拷贝/重放。
	inline constexpr long long k_max_age_seconds{ 3600 };
	inline constexpr long long k_future_tolerance_seconds{ 60 };

	inline constexpr wchar_t k_mapping_prefix[]{ L"Local\\NikoLoader_Auth_" };

	inline constexpr std::size_t k_username_max{ 64u };
	inline constexpr std::size_t k_hwid_max{ 64u };
	inline constexpr std::size_t k_token_max{ 512u };

	inline constexpr std::uint8_t k_key[ 32 ]{
		0x9d, 0x3f, 0x71, 0xc4, 0x2a, 0xe8, 0x15, 0x66, 0xb0, 0x4d, 0xf9, 0x83,
		0x1c, 0x57, 0xae, 0x02, 0x7b, 0xd3, 0x69, 0x98, 0xe4, 0x10, 0x46, 0xcd,
		0x8f, 0x25, 0xba, 0x73, 0x0e, 0xd1, 0x5c, 0xa9
	};

#pragma pack( push, 1 )
	struct payload
	{
		std::uint32_t magic;
		std::uint32_t version;
		std::uint64_t timestamp; // GetSystemTimeAsFileTime，publish 时刻
		char username[ k_username_max ];
		char hwid[ k_hwid_max ];
		char access_token[ k_token_max ];
	};

	struct wire
	{
		std::uint8_t nonce[ 12 ];
		std::uint8_t tag[ 16 ];
		std::uint8_t data[ sizeof( payload ) ];
	};
#pragma pack( pop )

	namespace detail {

		struct bcrypt_api
		{
			decltype( &BCryptOpenAlgorithmProvider ) open{};
			decltype( &BCryptSetProperty ) set_property{};
			decltype( &BCryptGenerateSymmetricKey ) generate_key{};
			decltype( &BCryptDestroyKey ) destroy_key{};
			decltype( &BCryptEncrypt ) encrypt{};
			decltype( &BCryptDecrypt ) decrypt{};
			decltype( &BCryptGenRandom ) gen_random{};
			decltype( &BCryptCloseAlgorithmProvider ) close{};
		};

		[[nodiscard]] inline const bcrypt_api& api( )
		{
			static const bcrypt_api value = [ ] {
				bcrypt_api out{};
				// 有意不 FreeLibrary：与进程同生命周期。
				const auto module = LoadLibraryW( L"bcrypt.dll" );
				if ( !module ) return out;
				const auto resolve = [ module ]( const char* name )
				{
					return reinterpret_cast< void* >( GetProcAddress( module, name ) );
				};
				out.open = reinterpret_cast< decltype( out.open ) >( resolve( "BCryptOpenAlgorithmProvider" ) );
				out.set_property = reinterpret_cast< decltype( out.set_property ) >( resolve( "BCryptSetProperty" ) );
				out.generate_key = reinterpret_cast< decltype( out.generate_key ) >( resolve( "BCryptGenerateSymmetricKey" ) );
				out.destroy_key = reinterpret_cast< decltype( out.destroy_key ) >( resolve( "BCryptDestroyKey" ) );
				out.encrypt = reinterpret_cast< decltype( out.encrypt ) >( resolve( "BCryptEncrypt" ) );
				out.decrypt = reinterpret_cast< decltype( out.decrypt ) >( resolve( "BCryptDecrypt" ) );
				out.gen_random = reinterpret_cast< decltype( out.gen_random ) >( resolve( "BCryptGenRandom" ) );
				out.close = reinterpret_cast< decltype( out.close ) >( resolve( "BCryptCloseAlgorithmProvider" ) );
				return out;
			}( );
			return value;
		}

		// AES-256-GCM 单块处理：encrypting=true 加密并产出 tag，false 时校验 tag
		// 后解密（tag 不匹配返回 false，输出不可信）。
		[[nodiscard]] inline bool aes_gcm( const std::uint8_t* key, std::size_t key_size, const std::uint8_t* nonce,
			std::uint8_t* tag, const std::uint8_t* in, std::uint8_t* out, std::size_t size, bool encrypting )
		{
			const auto& fn = api( );
			if ( !fn.open || !fn.set_property || !fn.generate_key || !fn.encrypt || !fn.decrypt || !fn.destroy_key || !fn.close )
				return false;

			BCRYPT_ALG_HANDLE alg{};
			if ( fn.open( &alg, BCRYPT_AES_ALGORITHM, nullptr, 0 ) != 0 ) return false;
			const auto close_alg = [ & ]( ) { fn.close( alg, 0 ); };

			static constexpr wchar_t chaining_mode[]{ BCRYPT_CHAIN_MODE_GCM };
			if ( fn.set_property( alg, BCRYPT_CHAINING_MODE, reinterpret_cast< PUCHAR >( const_cast< wchar_t* >( chaining_mode ) ),
				sizeof( chaining_mode ), 0 ) != 0 )
			{
				close_alg( );
				return false;
			}

			BCRYPT_KEY_HANDLE key_handle{};
			if ( fn.generate_key( alg, &key_handle, nullptr, 0, const_cast< PUCHAR >( key ),
				static_cast< ULONG >( key_size ), 0 ) != 0 )
			{
				close_alg( );
				return false;
			}

			BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info{};
			BCRYPT_INIT_AUTH_MODE_INFO( auth_info );
			auth_info.pbNonce = const_cast< PUCHAR >( nonce );
			auth_info.cbNonce = 12;
			auth_info.pbTag = tag;
			auth_info.cbTag = 16;

			ULONG done{};
			const auto status = encrypting
				? fn.encrypt( key_handle, const_cast< PUCHAR >( in ), static_cast< ULONG >( size ), &auth_info,
					nullptr, 0, out, static_cast< ULONG >( size ), &done, 0 )
				: fn.decrypt( key_handle, const_cast< PUCHAR >( in ), static_cast< ULONG >( size ), &auth_info,
					nullptr, 0, out, static_cast< ULONG >( size ), &done, 0 );

			fn.destroy_key( key_handle );
			close_alg( );
			return status == 0 && done == size;
		}

		[[nodiscard]] inline std::wstring mapping_name( unsigned long pid )
		{
			return std::wstring( k_mapping_prefix ) + std::to_wstring( pid );
		}

		[[nodiscard]] inline std::uint64_t now_filetime( )
		{
			FILETIME ft{};
			GetSystemTimeAsFileTime( &ft );
			return ( static_cast< std::uint64_t >( ft.dwHighDateTime ) << 32 ) | ft.dwLowDateTime;
		}

	} // namespace detail

	// 注入器侧：构建 payload、加密并写入命名映射。target_process（CS2 句柄，
	// 需 PROCESS_DUP_HANDLE）非空时把映射句柄复制进目标进程——页文件后备
	// section 在最后一个句柄关闭时销毁，注入器"注入完成即关窗"会早于 DLL
	// 的初始化消费，远端句柄保活后时序无关。本地句柄常驻注入器进程。
	[[nodiscard]] inline bool publish( unsigned long pid, void* target_process, std::string_view username,
		std::string_view hwid, std::string_view access_token )
	{
		const auto& fn = detail::api( );
		if ( !fn.gen_random || !fn.encrypt || username.empty( ) || access_token.empty( ) ) return false;

		payload plain{};
		plain.magic = k_magic;
		plain.version = k_version;
		plain.timestamp = detail::now_filetime( );
		std::memcpy( plain.username, username.data( ), ( std::min )( username.size( ), k_username_max - 1 ) );
		std::memcpy( plain.hwid, hwid.data( ), ( std::min )( hwid.size( ), k_hwid_max - 1 ) );
		std::memcpy( plain.access_token, access_token.data( ), ( std::min )( access_token.size( ), k_token_max - 1 ) );

		wire out{};
		if ( fn.gen_random( nullptr, out.nonce, static_cast< ULONG >( sizeof( out.nonce ) ), BCRYPT_USE_SYSTEM_PREFERRED_RNG ) != 0 )
			return false;
		if ( !detail::aes_gcm( k_key, sizeof( k_key ), out.nonce, out.tag,
			reinterpret_cast< const std::uint8_t* >( &plain ), out.data, sizeof( plain ), true ) )
			return false;

		const auto name = detail::mapping_name( pid );
		const auto mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, static_cast< DWORD >( sizeof( wire ) ), name.c_str( ) );
		if ( !mapping ) return false;

		const auto view = MapViewOfFile( mapping, FILE_MAP_WRITE, 0, 0, sizeof( wire ) );
		if ( !view )
		{
			CloseHandle( mapping );
			return false;
		}
		std::memcpy( view, &out, sizeof( out ) );
		FlushViewOfFile( view, sizeof( out ) );
		UnmapViewOfFile( view );

		if ( target_process )
		{
			HANDLE remote{};
			if ( !DuplicateHandle( GetCurrentProcess( ), mapping, static_cast< HANDLE >( target_process ),
				&remote, 0, FALSE, DUPLICATE_SAME_ACCESS ) )
			{
				CloseHandle( mapping );
				return false;
			}
			// 远端句柄有意不关：CS2 持有的这一引用把 section 保活到进程退出。
		}
		return true;
	}

	// DLL 侧：打开本进程对应的映射，解密+验证后输出。任何一步失败返回
	// false 且输出保持空串。读取后即关闭句柄（数据一次性消费）。
	[[nodiscard]] inline bool consume( std::string& out_username, std::string& out_hwid, std::string& out_access_token )
	{
		const auto& fn = detail::api( );
		if ( !fn.decrypt ) return false;

		const auto name = detail::mapping_name( GetCurrentProcessId( ) );
		const auto mapping = OpenFileMappingW( FILE_MAP_READ, FALSE, name.c_str( ) );
		if ( !mapping ) return false;

		const auto view = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, sizeof( wire ) );
		if ( !view )
		{
			CloseHandle( mapping );
			return false;
		}
		wire in{};
		std::memcpy( &in, view, sizeof( in ) );
		UnmapViewOfFile( view );
		CloseHandle( mapping );

		payload plain{};
		if ( !detail::aes_gcm( k_key, sizeof( k_key ), in.nonce, in.tag, in.data,
			reinterpret_cast< std::uint8_t* >( &plain ), sizeof( plain ), false ) )
			return false;
		if ( plain.magic != k_magic || plain.version != k_version ) return false;

		const auto now = detail::now_filetime( );
		const auto age_seconds = ( static_cast< long long >( now ) - static_cast< long long >( plain.timestamp ) ) / 10'000'000LL;
		if ( age_seconds > k_max_age_seconds || age_seconds < -k_future_tolerance_seconds ) return false;

		out_username.assign( plain.username, strnlen_s( plain.username, k_username_max ) );
		out_hwid.assign( plain.hwid, strnlen_s( plain.hwid, k_hwid_max ) );
		out_access_token.assign( plain.access_token, strnlen_s( plain.access_token, k_token_max ) );
		return !out_username.empty( ) && !out_access_token.empty( );
	}

} // namespace auth_channel
