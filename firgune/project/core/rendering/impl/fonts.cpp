#include <pch/pch.hpp>
#include <core/resources/fonts/inter.hpp>
#include <core/resources/fonts/pixel7.hpp>
#include "../rendering.hpp"

namespace rendering {

	namespace detail {

		std::vector<std::byte>& cjk_font_data( )
		{
			static std::vector<std::byte> data{};
			return data;
		}

		bool try_load_font_file( const char* path )
		{
			auto& data = cjk_font_data( );
			std::ifstream file( path, std::ios::binary | std::ios::ate );
			if ( !file )
			{
				return false;
			}

			const auto size = file.tellg( );
			if ( size <= 0 )
			{
				return false;
			}

			const auto byte_count = static_cast< std::size_t >( size );
			data.resize( byte_count );
			file.seekg( 0, std::ios::beg );
			if ( !file.read( reinterpret_cast< char* >( data.data( ) ), static_cast< std::streamsize >( byte_count ) ) )
			{
				data.clear( );
				return false;
			}

			return true;
		}

		std::span<const std::byte> load_cjk_font_data( )
		{
			auto& data = cjk_font_data( );
			if ( !data.empty( ) )
			{
				return { data.data( ), data.size( ) };
			}

			static constexpr const char* k_font_paths[ ]
			{
				R"(C:\Windows\Fonts\msyh.ttc)",
				R"(C:\Windows\Fonts\msyh.ttf)",
				R"(C:\Windows\Fonts\simhei.ttf)",
				R"(C:\Windows\Fonts\simsun.ttc)",
				R"(C:\Windows\Fonts\NotoSansCJK-Regular.ttc)",
				R"(C:\Windows\Fonts\NotoSansSC-Regular.otf)"
			};

			for ( const auto* path : k_font_paths )
			{
				if ( try_load_font_file( path ) )
				{
					return { data.data( ), data.size( ) };
				}
			}

			return {};
		}

	} // namespace detail

	void fonts::initialize( )
	{
		this->load_family( this->inter_medium, std::as_bytes( std::span{ resources::fonts::inter::regular } ), { 13.0f, 16.0f, 20.0f } );
		this->load_family( this->inter_bold, std::as_bytes( std::span{ resources::fonts::inter::bold } ), { 13.0f, 16.0f, 20.0f } );
		this->load_family( this->smallest_pixel7, std::as_bytes( std::span{ resources::fonts::pixel7::smallest } ), { 9.0f, 10.5f, 14.0f } );

		if ( const auto cjk_data = detail::load_cjk_font_data( ); !cjk_data.empty( ) )
		{
			this->load_family( this->cjk_regular, cjk_data, { 13.0f, 16.0f, 20.0f } );
			this->attach_fallback( this->inter_medium, this->cjk_regular );
			this->attach_fallback( this->inter_bold, this->cjk_regular );
			this->attach_fallback( this->smallest_pixel7, this->cjk_regular );
		}
	}

	void fonts::load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes )
	{
		for ( auto i = 0ull; i < sizes.size( ); ++i )
		{
			family.sizes[ i ] = xdraw::load_font( data, sizes[ i ] );
		}
	}

	void fonts::attach_fallback( family_t& family, const family_t& fallback ) const
	{
		for ( auto i = 0ull; i < family.sizes.size( ); ++i )
		{
			if ( family.sizes[ i ] && fallback.sizes[ i ] )
			{
				family.sizes[ i ]->fallback = fallback.sizes[ i ];
			}
		}
	}

} // namespace rendering
