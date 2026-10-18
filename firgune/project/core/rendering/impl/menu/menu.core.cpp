#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/settings.hpp>
#include <utilities/security/security.hpp>
#include <external/config.hpp>
#include <core/systems/systems.hpp>
#include <core/resources/images/menu_header_background.hpp>
#include <core/resources/images/vagal_watermark_logo.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr auto k_menu_header_h{ 150.0f };

		[[nodiscard]] static std::string to_lower_copy( std::string_view v )
		{
			std::string out( v );
			std::transform( out.begin( ), out.end( ), out.begin( ), [ ]( unsigned char c )
				{
					return static_cast< char >( std::tolower( c ) );
				} );
			return out;
		}

		// 配置名以宽字符存储在 config 注册表，顶栏下拉需要 UTF-8 窄字符串。
		[[nodiscard]] static std::string to_utf8( const std::wstring& wide )
		{
			if ( wide.empty( ) )
			{
				return {};
			}

			const auto needed = WideCharToMultiByte( CP_UTF8, 0, wide.c_str( ), static_cast< int >( wide.size( ) ), nullptr, 0, nullptr, nullptr );
			if ( needed <= 0 )
			{
				return {};
			}

			std::string out( static_cast< std::size_t >( needed ), '\0' );
			WideCharToMultiByte( CP_UTF8, 0, wide.c_str( ), static_cast< int >( wide.size( ) ), out.data( ), needed, nullptr, nullptr );
			return out;
		}

		// 订阅到期标签：日期来自账号快照（注入器共享内存 + 周期复验），
		// 空数据显示占位。前缀走 xui 翻译（"expires"→"到期"）。
		[[nodiscard]] static std::string subscription_label( )
		{
			const auto account_data = systems::g_account.get( );
			const auto date = account_data.sub_until.size( ) >= 10 ? account_data.sub_until.substr( 0, 10 ) : account_data.sub_until;
			return std::format( "{} {}", xui::translate( "expires" ), date.empty( ) ? std::string{ "--" } : date );
		}

		[[nodiscard]] static int parse_weapon_subtab( const std::string& category_lower )
		{
			if ( category_lower.find( "pistols" ) != std::string::npos ) return 0;
			if ( category_lower.find( "smg" ) != std::string::npos ) return 1;
			if ( category_lower.find( "rifles" ) != std::string::npos ) return 2;
			if ( category_lower.find( "shotguns" ) != std::string::npos ) return 3;
			if ( category_lower.find( "snipers" ) != std::string::npos ) return 4;
			if ( category_lower.find( "lmg" ) != std::string::npos ) return 5;
			return 0;
		}

		[[nodiscard]] static std::pair<int, int> map_category_to_tab( const std::string& category_lower )
		{
			if ( category_lower.find( "ragebot" ) != std::string::npos || category_lower.find( "anti aim" ) != std::string::npos || category_lower.find( "peek assistance" ) != std::string::npos || category_lower.find( "zeusbot" ) != std::string::npos || category_lower.find( "knifebot" ) != std::string::npos || category_lower.find( "autos" ) != std::string::npos )
			{
				return { 0, parse_weapon_subtab( category_lower ) };
			}

			if ( category_lower.find( "legitbot" ) != std::string::npos )
			{
				return { 1, parse_weapon_subtab( category_lower ) };
			}

			if ( category_lower.find( "enemy" ) != std::string::npos || category_lower.find( "player enemies" ) != std::string::npos )
			{
				return { 2, 0 };
			}
			if ( category_lower.find( "allies" ) != std::string::npos || category_lower.find( "teammate" ) != std::string::npos || category_lower.find( "friendly" ) != std::string::npos )
			{
				return { 2, 1 };
			}
			if ( category_lower.find( "local" ) != std::string::npos )
			{
				return { 2, 2 };
			}

			if ( category_lower.find( "scene" ) != std::string::npos )
			{
				return { 3, 1 };
			}
			if ( category_lower.find( "camera" ) != std::string::npos || category_lower.find( "viewmodel" ) != std::string::npos )
			{
				return { 3, 1 };
			}
			if ( category_lower.find( "weather" ) != std::string::npos )
			{
				return { 3, 2 };
			}
			if ( category_lower.find( "world" ) != std::string::npos || category_lower.find( "projectile" ) != std::string::npos || category_lower.find( "item" ) != std::string::npos || category_lower.find( "bomb" ) != std::string::npos || category_lower.find( "spectator" ) != std::string::npos )
			{
				return { 3, 0 };
			}

			if ( category_lower.find( "knife" ) != std::string::npos )
			{
				return { 4, 1 };
			}
			if ( category_lower.find( "glove" ) != std::string::npos )
			{
				return { 4, 2 };
			}
			if ( category_lower.find( "agent" ) != std::string::npos )
			{
				return { 4, 3 };
			}
			if ( category_lower.find( "skin" ) != std::string::npos || category_lower.find( "paint" ) != std::string::npos || category_lower.find( "sticker" ) != std::string::npos || category_lower.find( "wear" ) != std::string::npos )
			{
				return { 4, 0 };
			}

			if ( category_lower.find( "config" ) != std::string::npos )
			{
				return { 6, 0 };
			}

			return { 5, 0 };
		}

		[[nodiscard]] static bool is_profile_category( const std::string& category_lower )
		{
			constexpr std::string_view categories[] {
				"crosshair", "scope overlay", "hat", "velocity hud", "dt indicator",
				"fake crouch indicator", "slow indicator", "hc indicator", "md indicator", "widgets"
			};

			return std::ranges::any_of( categories, [ & ]( const std::string_view category )
				{
					return category_lower.find( category ) != std::string::npos;
				} );
		}

	} // namespace detail

	namespace svgs {

		constexpr auto search = R"(<svg width="16" height="16" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M2 6.66667C2 7.2795 2.12071 7.88634 2.35523 8.45252C2.58975 9.01871 2.93349 9.53316 3.36683 9.9665C3.80017 10.3998 4.31462 10.7436 4.88081 10.9781C5.447 11.2126 6.05383 11.3333 6.66667 11.3333C7.2795 11.3333 7.88634 11.2126 8.45252 10.9781C9.01871 10.7436 9.53316 10.3998 9.9665 9.9665C10.3998 9.53316 10.7436 9.01871 10.9781 8.45252C11.2126 7.88634 11.3333 7.2795 11.3333 6.66667C11.3333 6.05383 11.2126 5.447 10.9781 4.88081C10.7436 4.31462 10.3998 3.80017 9.9665 3.36683C9.53316 2.93349 9.01871 2.58975 8.45252 2.35523C7.88634 2.12071 7.2795 2 6.66667 2C6.05383 2 5.447 2.12071 4.88081 2.35523C4.31462 2.58975 3.80017 2.93349 3.36683 3.36683C2.93349 3.80017 2.58975 4.31462 2.35523 4.88081C2.12071 5.447 2 6.05383 2 6.66667Z" stroke="white" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/><path d="M14 14L10 10" stroke="white" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto settings = R"(<svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M11.5937 3.65661C11.7942 3.77063 11.9607 3.93601 12.0761 4.13573C12.1914 4.33546 12.2514 4.56231 12.25 4.79294V9.04194C12.25 9.51386 11.9916 9.94903 11.5745 10.1783L7.637 12.6691C7.44179 12.7763 7.2227 12.8325 7 12.8325C6.7773 12.8325 6.55821 12.7763 6.363 12.6691L2.4255 10.1783C2.22143 10.0668 2.05107 9.90245 1.93224 9.70255C1.81341 9.50265 1.75047 9.2745 1.75 9.04194V4.79236C1.75 4.32044 2.00842 3.88586 2.4255 3.65661L6.363 1.33494C6.56398 1.22413 6.78975 1.16602 7.01925 1.16602C7.24875 1.16602 7.47452 1.22413 7.6755 1.33494L11.613 3.65661H11.5937Z" stroke="#111111" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/><path d="M5.25 7C5.25 7.46413 5.43437 7.90925 5.76256 8.23744C6.09075 8.56563 6.53587 8.75 7 8.75C7.46413 8.75 7.90925 8.56563 8.23744 8.23744C8.56563 7.90925 8.75 7.46413 8.75 7C8.75 6.53587 8.56563 6.09075 8.23744 5.76256C7.90925 5.43437 7.46413 5.25 7 5.25C6.53587 5.25 6.09075 5.43437 5.76256 5.76256C5.43437 6.09075 5.25 6.53587 5.25 7Z" stroke="#111111" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_rage = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M3.75 12C3.75 13.0834 3.96339 14.1562 4.37799 15.1571C4.7926 16.1581 5.40029 17.0675 6.16637 17.8336C6.93245 18.5997 7.84193 19.2074 8.84286 19.622C9.8438 20.0366 10.9166 20.25 12 20.25M20.25 12C20.25 10.9166 20.0366 9.8438 19.622 8.84286C19.2074 7.84193 18.5997 6.93245 17.8336 6.16637C17.0675 5.40029 16.1581 4.7926 15.1571 4.37799C14.1562 3.96339 13.0834 3.75 12 3.75C10.9166 3.75 9.8438 3.96339 8.84286 4.37799C7.84193 4.7926 6.93245 5.40029 6.16637 6.16637C5.40029 6.93245 4.7926 7.84193 4.37799 8.84286C3.96339 9.8438 3.75 10.9166 3.75 12M3.75 12H7.41667M12 20.25C13.0834 20.25 14.1562 20.0366 15.1571 19.622C16.1581 19.2074 17.0675 18.5997 17.8336 17.8336C18.5997 17.0675 19.2074 16.1581 19.622 15.1571C20.0366 14.1562 20.25 13.0834 20.25 12M12 20.25V17.5M20.25 12H17.5M12 3.75V7.41667M12 12V12.01" stroke="white" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_legit = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M12 6.25C8.82436 6.25 6.25 8.82436 6.25 12C6.25 15.1756 8.82436 17.75 12 17.75C15.1756 17.75 17.75 15.1756 17.75 12C17.75 8.82436 15.1756 6.25 12 6.25Z" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M12 4.25V6.25M12 17.75V19.75M4.25 12H6.25M17.75 12H19.75M12 12H12.01" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_player = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M9.34933 15.375C7.00449 14.7241 4.98599 12.9889 3.29199 10.1665C5.49199 6.49979 8.24199 4.66646 11.542 4.66646C14.842 4.66646 17.592 6.49979 19.792 10.1665M9.70866 10.1665C9.70866 10.6527 9.90181 11.119 10.2456 11.4628C10.5894 11.8066 11.0558 11.9998 11.542 11.9998C12.0282 11.9998 12.4945 11.8066 12.8384 11.4628C13.1822 11.119 13.3753 10.6527 13.3753 10.1665C13.3753 9.68023 13.1822 9.21391 12.8384 8.8701C12.4945 8.52628 12.0282 8.33313 11.542 8.33313C11.0558 8.33313 10.5894 8.52628 10.2456 8.8701C9.90181 9.21391 9.70866 9.68023 9.70866 10.1665ZM16.8587 18.2487L14.8677 19.2919C14.8086 19.3226 14.7421 19.3364 14.6757 19.3315C14.6092 19.3267 14.5455 19.3035 14.4915 19.2645C14.4374 19.2255 14.3953 19.1723 14.3698 19.1107C14.3443 19.0492 14.3364 18.9818 14.347 18.916L14.7274 16.706L13.1168 15.1412C13.0688 15.0947 13.0347 15.0357 13.0186 14.9708C13.0025 14.9059 13.0049 14.8378 13.0257 14.7742C13.0464 14.7107 13.0846 14.6542 13.1359 14.6113C13.1872 14.5684 13.2495 14.5408 13.3157 14.5316L15.5414 14.209L16.5369 12.1987C16.5667 12.1388 16.6126 12.0885 16.6694 12.0532C16.7263 12.018 16.7918 11.9994 16.8587 11.9994C16.9255 11.9994 16.9911 12.018 17.0479 12.0532C17.1047 12.0885 17.1506 12.1388 17.1804 12.1987L18.1759 14.209L20.4016 14.5316C20.4676 14.5411 20.5297 14.5689 20.5807 14.6118C20.6318 14.6548 20.6698 14.7112 20.6905 14.7746C20.7112 14.838 20.7138 14.906 20.6979 14.9707C20.6819 15.0355 20.6482 15.0946 20.6005 15.1412L18.9899 16.706L19.3694 18.9151C19.3808 18.981 19.3735 19.0488 19.3483 19.1107C19.3231 19.1726 19.2811 19.2262 19.227 19.2655C19.1728 19.3047 19.1088 19.328 19.0421 19.3327C18.9754 19.3373 18.9087 19.3232 18.8497 19.2919L16.8587 18.2487Z" stroke="white" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_world = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M12 20.25C16.5563 20.25 20.25 16.5563 20.25 12C20.25 7.44365 16.5563 3.75 12 3.75C7.44365 3.75 3.75 7.44365 3.75 12C3.75 16.5563 7.44365 20.25 12 20.25Z" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M4.25 12H19.75M12 3.75C14.1 6.1 15.15 8.85 15.15 12C15.15 15.15 14.1 17.9 12 20.25M12 3.75C9.9 6.1 8.85 8.85 8.85 12C8.85 15.15 9.9 17.9 12 20.25" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_skins = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M9.34933 15.375C7.00449 14.7241 4.98599 12.9889 3.29199 10.1665C5.49199 6.49979 8.24199 4.66646 11.542 4.66646C14.842 4.66646 17.592 6.49979 19.792 10.1665M9.70866 10.1665C9.70866 10.6527 9.90181 11.119 10.2456 11.4628C10.5894 11.8066 11.0558 11.9998 11.542 11.9998C12.0282 11.9998 12.4945 11.8066 12.8384 11.4628C13.1822 11.119 13.3753 10.6527 13.3753 10.1665C13.3753 9.68023 13.1822 9.21391 12.8384 8.8701C12.4945 8.52628 12.0282 8.33313 11.542 8.33313C11.0558 8.33313 10.5894 8.52628 10.2456 8.8701C9.90181 9.21391 9.70866 9.68023 9.70866 10.1665ZM16.8587 18.2487L14.8677 19.2919C14.8086 19.3226 14.7421 19.3364 14.6757 19.3315C14.6092 19.3267 14.5455 19.3035 14.4915 19.2645C14.4374 19.2255 14.3953 19.1723 14.3698 19.1107C14.3443 19.0492 14.3364 18.9818 14.347 18.916L14.7274 16.706L13.1168 15.1412C13.0688 15.0947 13.0347 15.0357 13.0186 14.9708C13.0025 14.9059 13.0049 14.8378 13.0257 14.7742C13.0464 14.7107 13.0846 14.6542 13.1359 14.6113C13.1872 14.5684 13.2495 14.5408 13.3157 14.5316L15.5414 14.209L16.5369 12.1987C16.5667 12.1388 16.6126 12.0885 16.6694 12.0532C16.7263 12.018 16.7918 11.9994 16.8587 11.9994C16.9255 11.9994 16.9911 12.018 17.0479 12.0532C17.1047 12.0885 17.1506 12.1388 17.1804 12.1987L18.1759 14.209L20.4016 14.5316C20.4676 14.5411 20.5297 14.5689 20.5807 14.6118C20.6318 14.6548 20.6698 14.7112 20.6905 14.7746C20.7112 14.838 20.7138 14.906 20.6979 14.9707C20.6819 15.0355 20.6482 15.0946 20.6005 15.1412L18.9899 16.706L19.3694 18.9151C19.3808 18.981 19.3735 19.0488 19.3483 19.1107C19.3231 19.1726 19.2811 19.2262 19.227 19.2655C19.1728 19.3047 19.1088 19.328 19.0421 19.3327C18.9754 19.3373 18.9087 19.3232 18.8497 19.2919L16.8587 18.2487Z" stroke="white" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_misc = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M5 7H10M14 7H19M5 12H7M11 12H19M5 17H13M17 17H19M10 5.25V8.75M11 10.25V13.75M17 15.25V18.75" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_config = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M4.25 7.25C4.25 6.42157 4.92157 5.75 5.75 5.75H9L11 7.75H18.25C19.0784 7.75 19.75 8.42157 19.75 9.25V17.25C19.75 18.0784 19.0784 18.75 18.25 18.75H5.75C4.92157 18.75 4.25 18.0784 4.25 17.25V7.25Z" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M8 12.25H16M8 15.25H13" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto tab_script = R"(<svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M8 18L2 12L8 6" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M16 6L22 12L16 18" stroke="white" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M14 4L10 20" stroke="white" stroke-width="1.6" stroke-linecap="round"/></svg>)";

		constexpr auto cfg_folder_black = R"(<svg width="15" height="13" viewBox="0 0 15 13" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M2.25 0.75H5.25L7.5 3H12.75C13.1478 3 13.5294 3.15804 13.8107 3.43934C14.092 3.72064 14.25 4.10218 14.25 4.5V10.5C14.25 10.8978 14.092 11.2794 13.8107 11.5607C13.5294 11.842 13.1478 12 12.75 12H2.25C1.85218 12 1.47064 11.842 1.18934 11.5607C0.908035 11.2794 0.75 10.8978 0.75 10.5V2.25C0.75 1.85218 0.908035 1.47064 1.18934 1.18934C1.47064 0.908035 1.85218 0.75 2.25 0.75Z" stroke="#111111" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto cfg_folder_dim = R"(<svg width="15" height="13" viewBox="0 0 15 13" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M2.25 0.75H5.25L7.5 3H12.75C13.1478 3 13.5294 3.15804 13.8107 3.43934C14.092 3.72064 14.25 4.10218 14.25 4.5V10.5C14.25 10.8978 14.092 11.2794 13.8107 11.5607C13.5294 11.842 13.1478 12 12.75 12H2.25C1.85218 12 1.47064 11.842 1.18934 11.5607C0.908035 11.2794 0.75 10.8978 0.75 10.5V2.25C0.75 1.85218 0.908035 1.47064 1.18934 1.18934C1.47064 0.908035 1.85218 0.75 2.25 0.75Z" stroke="#ADC0FF" stroke-opacity="0.32" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto cfg_cloud_black = R"(<svg width="17" height="12" viewBox="0 0 17 12" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M4.24275 10.4998C2.31375 10.4998 0.75 8.99454 0.75 7.13754C0.75 5.28129 2.31375 3.77604 4.24275 3.77604C4.5375 2.45454 5.58825 1.37604 6.999 0.946291C8.409 0.517291 9.966 0.801541 11.082 1.69629C12.198 2.58879 12.7035 3.95154 12.4095 5.27304H13.152C14.5867 5.27304 15.75 6.44304 15.75 7.88754C15.75 9.33279 14.5867 10.5028 13.1512 10.5028H4.24275" stroke="#111111" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto cfg_cloud_dim = R"(<svg width="17" height="12" viewBox="0 0 17 12" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M4.24275 10.4998C2.31375 10.4998 0.75 8.99454 0.75 7.13754C0.75 5.28129 2.31375 3.77604 4.24275 3.77604C4.5375 2.45454 5.58825 1.37604 6.999 0.946291C8.409 0.517291 9.966 0.801541 11.082 1.69629C12.198 2.58879 12.7035 3.95154 12.4095 5.27304H13.152C14.5867 5.27304 15.75 6.44304 15.75 7.88754C15.75 9.33279 14.5867 10.5028 13.1512 10.5028H4.24275" stroke="#ADC0FF" stroke-opacity="0.32" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>)";
		constexpr auto cfg_plus = R"(<svg width="16" height="15" viewBox="0 0 16 15" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M8 4V11M4 7.5H12" stroke="#ADC0FF" stroke-width="1.5" stroke-linecap="round"/></svg>)";

	} // namespace svgs

	void menu::initialize_graphics( )
	{
		if ( this->m_graphics_initialized )
		{
			return;
		}

		constexpr auto icon_target{ 16.0f };
		this->m_textures.menu_header.resource = xdraw::load_texture(
			std::as_bytes( std::span{ resources::images::menu_header_background::data } ),
			&this->m_textures.menu_header.width,
			&this->m_textures.menu_header.height );

		this->m_textures.startup_logo.resource = xdraw::load_texture(
			std::as_bytes( std::span{ resources::images::vagal_watermark_logo::data } ),
			&this->m_textures.startup_logo.width,
			&this->m_textures.startup_logo.height );

		this->m_textures.search.resource = xdraw::load_svg( svgs::search, icon_target / 16.0f, &this->m_textures.search.width, &this->m_textures.search.height );
		this->m_textures.settings.resource = xdraw::load_svg( svgs::settings, icon_target / 14.0f, &this->m_textures.settings.width, &this->m_textures.settings.height );

		constexpr float k_cfg_icon_scale{ 1.0f };
		this->m_textures.cfg_folder_on.resource = xdraw::load_svg( svgs::cfg_folder_black, k_cfg_icon_scale, &this->m_textures.cfg_folder_on.width, &this->m_textures.cfg_folder_on.height );
		this->m_textures.cfg_folder_off.resource = xdraw::load_svg( svgs::cfg_folder_dim, k_cfg_icon_scale, &this->m_textures.cfg_folder_off.width, &this->m_textures.cfg_folder_off.height );
		this->m_textures.cfg_cloud_on.resource = xdraw::load_svg( svgs::cfg_cloud_black, k_cfg_icon_scale, &this->m_textures.cfg_cloud_on.width, &this->m_textures.cfg_cloud_on.height );
		this->m_textures.cfg_cloud_off.resource = xdraw::load_svg( svgs::cfg_cloud_dim, k_cfg_icon_scale, &this->m_textures.cfg_cloud_off.width, &this->m_textures.cfg_cloud_off.height );
		this->m_textures.cfg_plus.resource = xdraw::load_svg( svgs::cfg_plus, 1.0f, &this->m_textures.cfg_plus.width, &this->m_textures.cfg_plus.height );

		constexpr auto tab_target{ 22.0f };
		constexpr std::array<const char*, static_cast< std::size_t >( tab::count )> tab_svgs{
			svgs::tab_rage,
			svgs::tab_legit,
			svgs::tab_player,
			svgs::tab_world,
			svgs::tab_skins,
			svgs::tab_misc,
			svgs::tab_config,
			svgs::tab_script
		};
		for ( auto i = 0; i < static_cast< int >( tab::count ); ++i )
		{
			this->m_textures.tabs[ i ].resource = xdraw::load_svg( tab_svgs[ i ], tab_target / 24.0f, &this->m_textures.tabs[ i ].width, &this->m_textures.tabs[ i ].height );
		}

		this->m_graphics_initialized = true;
	}

	void menu::rebuild_search_index( )
	{
		this->m_search_entries.clear( );

		const auto all_settings = xui::binds::all( );
		this->m_search_entries.reserve( all_settings.size( ) );

		for ( const auto* setting : all_settings )
		{
			if ( !setting || setting->name.empty( ) )
			{
				continue;
			}

			search_entry entry{};
			entry.name = setting->name;
			entry.category = setting->category;
			entry.name_lower = detail::to_lower_copy( entry.name );
			entry.category_lower = detail::to_lower_copy( entry.category );
			std::tie( entry.tab, entry.subtab ) = detail::map_category_to_tab( entry.category_lower );
			entry.profile_page = detail::is_profile_category( entry.category_lower );
			if ( entry.name_lower == "bullet impacts" || entry.name_lower == "bullet tracers" || entry.name_lower == "reveal radar" )
			{
				entry.tab = 3;
				entry.subtab = 1;
			}
			entry.bind_key = setting->bind.key;

			this->m_search_entries.emplace_back( std::move( entry ) );
		}
	}

	void menu::close_search( )
	{
		this->m_search_open = false;
		this->m_search_query.clear( );
		this->m_search_visible_indices.clear( );
	}

	void menu::activate_search_result( const std::size_t index )
	{
		if ( index >= this->m_search_visible_indices.size( ) )
		{
			return;
		}

		const auto& chosen = this->m_search_entries[ this->m_search_visible_indices[ index ] ];
		this->m_tab = std::clamp( chosen.tab, 0, 6 );
		this->m_subtab = std::clamp( chosen.subtab, 0, k_subtab_defs[ this->m_tab ].count - 1 );
		this->m_profile_page = chosen.profile_page;
		xui::set_highlight_target( chosen.name, 1.2f );
		this->close_search( );
	}

	void menu::draw_search_results( float x, float y, float w, float h )
	{
		xui::layout::set_cursor( x - this->m_x, y - this->m_y );
		this->m_search_visible_indices.clear( );

		auto query_lower = detail::to_lower_copy( this->m_search_query );
		for ( std::size_t i = 0; i < this->m_search_entries.size( ); ++i )
		{
			const auto& it = this->m_search_entries[ i ];
			if ( query_lower.empty( ) || it.name_lower.find( query_lower ) != std::string::npos || it.category_lower.find( query_lower ) != std::string::npos )
			{
				this->m_search_visible_indices.emplace_back( i );
				if ( this->m_search_visible_indices.size( ) >= 50 )
				{
					break;
				}
			}
		}

		if ( xui::begin_child( "##menu_search_results", w, h, true ) )
		{
			for ( std::size_t i = 0; i < this->m_search_visible_indices.size( ); ++i )
			{
				const auto& item = this->m_search_entries[ this->m_search_visible_indices[ i ] ];

				const auto avail_w = xui::layout::avail( ).first;
				const auto row_h = 44.0f;
				const auto row = xui::layout::item( avail_w, row_h );
				const auto hovered = xui::ctx( ).input.in_rect( row );
				const auto row_anim = xui::anim::lerp( xui::fnv1a( "search_row" ) + static_cast< std::uintptr_t >( i ), hovered ? 1.0f : 0.0f, 12.0f );

				auto& dl = xui::draw::current( );
				const auto row_bg = xui::lerp( tokens::col_card, tokens::col_accent.alpha( 90 ), row_anim * 0.35f );
				dl.rect_filled( row.x, row.y, row.w, row.h, row_bg, xdraw::corner_radius{ 8.0f } );

				const auto label_col = xui::lerp( tokens::col_text, tokens::col_dark, row_anim * 0.2f );
				const auto sub_col = tokens::col_text_dim;
				const auto item_name = xui::translate( item.name );
				const auto name_th = xdraw::measure_text( item_name ).second;
				const auto subtitle = xui::translate( item.category.empty( ) ? std::string_view{ "misc" } : std::string_view{ item.category } );
				const auto sub_th = xdraw::measure_text( subtitle ).second;
				const auto block_h = name_th + 3.0f + sub_th;
				const auto text_start_y = row.y + ( row.h - block_h ) * 0.5f;
				const auto name_y = text_start_y;
				dl.text( row.x + 8.0f, name_y, item_name, label_col );

				const auto sub_y = name_y + name_th + 3.0f;
				dl.text( row.x + 8.0f, sub_y, subtitle, sub_col.alpha( 170 ) );

				if ( item.bind_key != 0 )
				{
					const auto key = xui::translate( xui::vk_name( item.bind_key ) );
					const auto [kw, kh] = xdraw::measure_text( key );
					const auto bh = 20.0f;
					const auto bw = kw + 16.0f;
					const auto bx = row.right( ) - bw - 6.0f;
					const auto by = row.y + ( row.h - bh ) * 0.5f;
					dl.rect_filled( bx, by, bw, bh, tokens::col_card, xdraw::corner_radius{ 5.0f } );
					dl.text( bx + 8.0f, by + ( bh - kh ) * 0.5f, key, tokens::col_text_dim );
				}

				if ( hovered && xui::ctx( ).input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
				{
					this->activate_search_result( i );
					break;
				}
			}

			xui::end_child( );
		}

		if ( !this->m_search_visible_indices.empty( ) )
		{
			for ( const auto vk : xui::ctx( ).input.key_presses( ) )
			{
				if ( vk == VK_RETURN )
				{
					this->activate_search_result( 0 );
					break;
				}
			}
		}
	}

	bool menu::draw_intro( )
	{
		if ( this->m_intro_finished )
		{
			return false;
		}

		const auto dt = std::clamp( xdraw::delta_time( ), 0.0f, 0.1f );
		this->m_intro_visible_elapsed += dt;

		const auto state = g_startup_progress.snapshot( );
		const auto stage = startup_progress::stage_from( state );
		const auto generation = startup_progress::generation_from( state );
		if ( generation != this->m_intro_stage_generation )
		{
			this->m_intro_stage_generation = generation;
			this->m_intro_stage_elapsed = 0.0f;
		}

		const auto describe_stage = [ ]( const startup_stage value ) -> std::pair< std::string_view, float >
		{
			switch ( value )
			{
			case startup_stage::bootstrap:        return { "Preparing display", 0.03f };
			case startup_stage::configuration:    return { "Loading configuration", 0.10f };
			case startup_stage::runtime_services: return { "Starting services", 0.25f };
			case startup_stage::addresses:        return { "Resolving addresses", 0.40f };
			case startup_stage::systems:          return { "Starting systems", 0.55f };
			case startup_stage::economy:          return { "Loading inventory", 0.68f };
			case startup_stage::hooks:            return { "Installing hooks", 0.82f };
			case startup_stage::cvars:            return { "Unlocking cvars", 0.90f };
			case startup_stage::skyboxes:         return { "Discovering skyboxes", 0.96f };
			case startup_stage::complete:         return { "Initialization complete", 1.00f };
			}

			return { "Preparing display", 0.03f };
		};

		const auto [stage_label, target_progress] = describe_stage( stage );
		this->m_intro_displayed_progress = std::min(
			target_progress,
			this->m_intro_displayed_progress + dt * 0.78f );
		this->m_intro_stage_elapsed += dt;

		if ( this->m_intro_stage_elapsed >= 0.18f &&
			this->m_intro_displayed_progress >= target_progress - 0.001f )
		{
			g_startup_progress.acknowledge_display( generation );
		}

		constexpr float k_complete_hold_duration{ 1.0f };
		constexpr float k_complete_fade_duration{ 0.8f };
		if ( stage == startup_stage::complete && this->m_intro_displayed_progress >= 0.999f )
		{
			this->m_intro_complete_elapsed += dt;
		}
		else
		{
			this->m_intro_complete_elapsed = 0.0f;
		}

		if ( this->m_intro_complete_elapsed >= k_complete_hold_duration + k_complete_fade_duration &&
			g_startup_progress.is_runtime_ready( ) )
		{
			this->m_intro_finished = true;
			this->m_open = true;
			return false;
		}

		const auto fade_in = std::clamp( this->m_intro_visible_elapsed / 0.18f, 0.0f, 1.0f );
		const auto fade_out = this->m_intro_complete_elapsed <= k_complete_hold_duration
			? 1.0f
			: std::clamp(
				1.0f - ( this->m_intro_complete_elapsed - k_complete_hold_duration ) / k_complete_fade_duration,
				0.0f,
				1.0f );
		const auto visibility = fade_in * fade_out;
		const auto alpha = [ visibility ]( const int value )
		{
			return static_cast< std::uint8_t >( std::clamp(
				static_cast< int >( std::lround( static_cast< float >( value ) * visibility ) ),
				0,
				255 ) );
		};

		// ── Startup intro: local frosted-glass plate ──
		// Keep the original 340x226 bounds and choreography. Only the surface
		// treatment changes: the scene is blurred inside the plate, the glass
		// fades with visibility, and its sampling radius breathes in/out.
		const auto viewport = xdraw::viewport_size( );
		const auto cx = std::floorf( static_cast< float >( viewport.first ) * 0.5f );
		const auto cy = std::floorf( static_cast< float >( viewport.second ) * 0.5f );

		// Completion keeps the existing 14% contraction and 0.8s fade.
		const auto shrink = 1.0f - ( 1.0f - fade_out ) * 0.14f;
		constexpr float plate_w{ 340.0f };
		constexpr float plate_h{ 226.0f };
		const auto plate_draw_w = plate_w * shrink;
		const auto plate_draw_h = plate_h * shrink;
		const auto plate_x = cx - plate_draw_w * 0.5f;
		const auto plate_y = cy - plate_draw_h * 0.5f;
		const auto glass_rounding = 16.0f * shrink;

		const auto smoothstep = [ ]( const float value )
		{
			const auto t = std::clamp( value, 0.0f, 1.0f );
			return t * t * ( 3.0f - 2.0f * t );
		};
		const auto glass_presence = smoothstep( fade_in ) * smoothstep( fade_out );
		const auto glass_blur_scale = std::lerp( 0.18f, 1.30f, glass_presence );

		auto& dl = xdraw::get( xdraw::layer::top );
		dl.rect_filled_blurred(
			plate_x,
			plate_y,
			plate_draw_w,
			plate_draw_h,
			xdraw::corner_radius{ glass_rounding },
			xdraw::color{ 226, 235, 255, alpha( 220 ) },
			true,
			glass_blur_scale );

		// Two translucent tones retain contrast without hiding the scene.
		dl.rect_filled_gradient(
			plate_x,
			plate_y,
			plate_draw_w,
			plate_draw_h,
			xdraw::color{ 27, 34, 49, alpha( 142 ) },
			xdraw::color{ 23, 29, 43, alpha( 148 ) },
			xdraw::color{ 9, 12, 19, alpha( 176 ) },
			xdraw::color{ 12, 16, 24, alpha( 168 ) },
			xdraw::corner_radius{ glass_rounding } );

		// Soft top reflection and restrained double edge make the material read
		// as glass at both bright and dark scene backgrounds.
		dl.rect_filled_gradient(
			plate_x + 1.0f,
			plate_y + 1.0f,
			plate_draw_w - 2.0f,
			54.0f * shrink,
			xdraw::color{ 255, 255, 255, alpha( 22 ) },
			xdraw::color{ 205, 220, 255, alpha( 12 ) },
			xdraw::color{ 205, 220, 255, alpha( 0 ) },
			xdraw::color{ 255, 255, 255, alpha( 0 ) },
			xdraw::corner_radius::top( std::max( 0.0f, glass_rounding - 1.0f ) ) );
		dl.rect(
			plate_x,
			plate_y,
			plate_draw_w,
			plate_draw_h,
			xdraw::color{ 223, 232, 255, alpha( 54 ) },
			xdraw::corner_radius{ glass_rounding } );
		dl.rect(
			plate_x + 1.0f,
			plate_y + 1.0f,
			plate_draw_w - 2.0f,
			plate_draw_h - 2.0f,
			xdraw::color{ 255, 255, 255, alpha( 18 ) },
			xdraw::corner_radius{ std::max( 0.0f, glass_rounding - 1.0f ) } );

		// Watermark logo: fade in with a settle around its own center.
		constexpr auto logo_size{ 64.0f };
		const auto logo_in = std::clamp( this->m_intro_visible_elapsed / 0.45f, 0.0f, 1.0f );
		const auto logo_ease = 1.0f - std::powf( 1.0f - logo_in, 3.0f );
		const auto logo_full = logo_size * shrink;
		const auto logo_draw = logo_full * ( 0.9f + 0.1f * logo_ease );
		const auto logo_shell = 84.0f * shrink;
		const auto logo_shell_x = cx - logo_shell * 0.5f;
		const auto logo_shell_y = plate_y + 16.0f * shrink;
		dl.rect_filled_gradient(
			logo_shell_x,
			logo_shell_y,
			logo_shell,
			logo_shell,
			xdraw::color{ 255, 255, 255, alpha( static_cast< int >( 24.0f * logo_ease ) ) },
			xdraw::color{ 197, 216, 255, alpha( static_cast< int >( 15.0f * logo_ease ) ) },
			xdraw::color{ 116, 143, 214, alpha( static_cast< int >( 9.0f * logo_ease ) ) },
			xdraw::color{ 225, 235, 255, alpha( static_cast< int >( 14.0f * logo_ease ) ) },
			xdraw::corner_radius{ 24.0f * shrink } );
		dl.rect(
			logo_shell_x,
			logo_shell_y,
			logo_shell,
			logo_shell,
			xdraw::color{ 221, 232, 255, alpha( static_cast< int >( 34.0f * logo_ease ) ) },
			xdraw::corner_radius{ 24.0f * shrink } );
		if ( this->m_textures.startup_logo.resource )
		{
			dl.image(
				cx - logo_draw * 0.5f,
				plate_y + 26.0f * shrink + ( logo_full - logo_draw ) * 0.5f,
				logo_draw,
				logo_draw,
				this->m_textures.startup_logo.resource.Get( ),
				xdraw::color{ 255, 255, 255, alpha( static_cast< int >( 255.0f * logo_ease ) ) } );
		}

		// Wordmark: letters surface one by one with a small rise.
		xdraw::push_font( g_fonts.inter_bold[ fonts::size::big ] );
		const auto word = "VAGAL";
		float letter_widths[ 5 ]{};
		float word_width = 0.0f;
		for ( auto i = 0; i < 5; ++i )
		{
			const char glyph[]{ word[ i ], '\0' };
			letter_widths[ i ] = xdraw::measure_text( glyph ).first;
			word_width += letter_widths[ i ];
		}
		word_width += 4.0f * 10.0f;

		const auto word_y = plate_y + 106.0f * shrink;
		auto letter_x = cx - word_width * 0.5f;
		for ( auto i = 0; i < 5; ++i )
		{
			const auto appear = 0.50f + static_cast< float >( i ) * 0.10f;
			const auto k = std::clamp( ( this->m_intro_visible_elapsed - appear ) / 0.22f, 0.0f, 1.0f );
			if ( k > 0.0f )
			{
				const auto ease_k = 1.0f - std::powf( 1.0f - k, 3.0f );
				const char glyph[]{ word[ i ], '\0' };
				dl.text(
					letter_x,
					word_y + ( 1.0f - ease_k ) * 6.0f,
					glyph,
					xdraw::color{ 240, 243, 251, alpha( static_cast< int >( 255.0f * ease_k ) ) } );
			}
			letter_x += letter_widths[ i ] + 10.0f;
		}
		xdraw::pop_font( );

		// Hairline progress + stage caption + percent.
		const auto line_x = cx - 130.0f * shrink;
		const auto line_y = plate_y + 152.0f * shrink;
		const auto line_w = 260.0f * shrink;
		constexpr auto line_h{ 2.0f };
		dl.rect_filled(
			line_x,
			line_y,
			line_w,
			line_h,
			xdraw::color{ 255, 255, 255, alpha( 30 ) },
			xdraw::corner_radius{ line_h * 0.5f } );
		dl.rect_filled_gradient(
			line_x,
			line_y,
			line_w * this->m_intro_displayed_progress,
			line_h,
			xdraw::color{ 203, 220, 255, alpha( 255 ) },
			xdraw::color{ 139, 170, 255, alpha( 255 ) },
			xdraw::color{ 113, 145, 242, alpha( 255 ) },
			xdraw::color{ 181, 202, 255, alpha( 255 ) },
			xdraw::corner_radius{ line_h * 0.5f } );

		xdraw::push_font( g_fonts.inter_medium[ fonts::size::petite ] );
		const auto stage_text = xui::translate( stage_label );
		const auto [stage_w, stage_h] = xdraw::measure_text( stage_text );
		dl.text(
			cx - stage_w * 0.5f,
			plate_y + 166.0f * shrink,
			stage_text,
			xdraw::color{ 205, 213, 232, alpha( 225 ) } );

		const auto progress_text = std::format(
			"{}%",
			static_cast< int >( std::lround( this->m_intro_displayed_progress * 100.0f ) ) );
		const auto [pct_w, pct_h] = xdraw::measure_text( progress_text );
		dl.text(
			cx - pct_w * 0.5f,
			plate_y + 188.0f * shrink,
			progress_text,
			xdraw::color{ 150, 158, 176, alpha( 210 ) } );
		xdraw::pop_font( );

		return true;
	}

	void menu::draw( )
	{
		xui::set_english( settings::g_misc.language.value == settings::misc::menu_language::english );
		if ( !g_context.ui_assets_ready( ) || this->draw_intro( ) )
		{
			return;
		}

		if ( !g_startup_progress.is_runtime_ready( ) )
		{
			return;
		}

		if ( this->m_last_open != this->m_open )
		{
			if ( this->m_open )
			{
				this->m_saved_relative_mouse = memory::read<std::uint8_t>( addresses::globals::input_system + 84 );
				memory::call_vfunc<void>( addresses::globals::input_system, 76, false );
				this->apply_saved_cursor( );
			}
			else
			{
				POINT pt{};
				if ( GetCursorPos( &pt ) )
				{
					this->m_saved_cursor_x = pt.x;
					this->m_saved_cursor_y = pt.y;
					this->m_has_saved_cursor = true;
				}

				memory::call_vfunc<void>( addresses::globals::input_system, 76, this->m_saved_relative_mouse != 0 );
			}

			this->m_last_open = this->m_open;
		}

		xui::begin( );
		this->sync_theme_style( );
		{
			const auto dt = xdraw::delta_time( );
			const auto anim_speed = this->m_open ? 14.0f : 16.0f;
			const auto anim_target = this->m_open ? 1.0f : 0.0f;
			this->m_open_anim += ( anim_target - this->m_open_anim ) * std::min( anim_speed * dt, 1.0f );

			if ( this->m_open_anim < 0.01f && !this->m_open )
			{
				xui::end( );
				return;
			}

			const auto menu_reveal = xui::ease::out_cubic( this->m_open_anim );

			if ( !xui::begin_window( "##menu", this->m_x, this->m_y, this->m_w, this->m_h, true, 680.0f, 480.0f, menu_reveal ) )
			{
				return;
			}

			xdraw::push_font( rendering::g_fonts.inter_bold[ rendering::fonts::size::normal ] );
			auto& dl = xui::draw::current( );
			const auto draw_resize_grip = [ & ]
			{
				const xui::rect grip{ this->m_x + this->m_w - 16.0f, this->m_y + this->m_h - 16.0f, 16.0f, 16.0f };
				const auto col = tokens::col_accent.alpha( xui::ctx( ).input.in_rect( grip ) ? 230 : 130 );
				dl.line( this->m_x + this->m_w - 12.0f, this->m_y + this->m_h - 3.0f, this->m_x + this->m_w - 3.0f, this->m_y + this->m_h - 12.0f, col, 1.0f );
				dl.line( this->m_x + this->m_w - 7.0f, this->m_y + this->m_h - 3.0f, this->m_x + this->m_w - 3.0f, this->m_y + this->m_h - 7.0f, col, 1.0f );
			};
			const auto wx = this->m_x;
			const auto wy = this->m_y;
			const auto ww = this->m_w;
			const auto wh = this->m_h;

			const auto sb_x = wx + tokens::gap;
			const auto sb_y = wy + tokens::gap;
			const auto sb_w = tokens::sidebar_w;
			const auto sb_h = wh - tokens::gap * 2.0f;

			const auto header_h = detail::k_menu_header_h;
			dl.rect_filled( sb_x, sb_y, sb_w, header_h, tokens::col_card, xdraw::corner_radius{ tokens::card_rounding } );

			const auto tabs_y = sb_y + header_h + tokens::gap;
			const auto tabs_h = sb_h - header_h - tokens::gap;
			dl.rect_filled( sb_x, tabs_y, sb_w, tabs_h, tokens::col_card, xdraw::corner_radius{ tokens::card_rounding } );

			this->draw_side_bar( tabs_h );

			const auto content_x = sb_x + sb_w + tokens::gap;
			const auto content_y = sb_y;
			const auto content_w = ww - tokens::gap * 2.0f - sb_w - tokens::gap;
			const auto content_h = sb_h;

			this->draw_top_bar( content_w );

			const auto body_y = content_y + tokens::subtab_bar_h + tokens::gap;
			const auto body_h = content_h - tokens::subtab_bar_h - tokens::gap;
			const auto col_w = ( content_w - tokens::gap ) * 0.5f;

			this->m_body_x = content_x;
			this->m_body_y = body_y;
			this->m_body_w = content_w;
			this->m_body_h = body_h;
			xui::layout::set_cursor( content_x - wx, body_y - wy );

			if ( this->m_search_open )
			{
				// While search is open, block the regular top-bar interactions in this frame.
				xui::ctx( ).inside_overlay = xui::fnv1a( "menu_search_mode" );
				this->draw_search_results( content_x, body_y, content_w, body_h );
				xui::ctx( ).inside_overlay = xui::null_id;

				draw_resize_grip( );
				xui::end_window( );
				xui::end( );
				xdraw::pop_font( );
				return;
			}

			if ( this->m_profile_page )
			{
				this->draw_profile_page( content_w );
			}
			else switch ( this->m_tab )
			{
			case 0: this->draw_ragebot( col_w ); break;
			case 1: this->draw_legitbot( col_w ); break;
			case 2: this->draw_player( col_w ); break;
			case 3: this->draw_world( col_w ); break;
			case 4: this->draw_skins( col_w ); break;
			case 5: this->draw_misc( col_w ); break;
			case 6: this->draw_config( col_w ); break;
			case 7: this->draw_script( col_w ); break;
			}

			draw_resize_grip( );
			xui::end_window( );
		}
		xui::end( );
		xdraw::pop_font( );
	}

	void menu::shutdown( ) const
	{
		if ( this->m_open )
		{
			memory::call_vfunc<void>( addresses::globals::input_system, 76, this->m_saved_relative_mouse != 0 );
		}
	}

	void menu::apply_saved_cursor( )
	{
		if ( !this->m_has_saved_cursor )
		{
			return;
		}

		const auto vx = GetSystemMetrics( SM_XVIRTUALSCREEN );
		const auto vy = GetSystemMetrics( SM_YVIRTUALSCREEN );
		const auto vw = GetSystemMetrics( SM_CXVIRTUALSCREEN );
		const auto vh = GetSystemMetrics( SM_CYVIRTUALSCREEN );

		const auto x = std::clamp( this->m_saved_cursor_x, vx, vx + std::max( 1, vw ) - 1 );
		const auto y = std::clamp( this->m_saved_cursor_y, vy, vy + std::max( 1, vh ) - 1 );

		SetCursorPos( x, y );
		SetCursor( LoadCursor( nullptr, IDC_ARROW ) );
	}

		void menu::draw_side_bar( float h )
	{
		auto& dl = xui::draw::current( );
		const auto& input = xui::ctx( ).input;

		const auto sb_x = this->m_x + tokens::gap;
		const auto header_h = detail::k_menu_header_h;
		const auto tabs_y = this->m_y + tokens::gap + header_h + tokens::gap;

		{
			const auto pill_x = sb_x + 4.0f;
			const auto pill_y = this->m_y + tokens::gap + 4.0f;
			const auto pill_w = tokens::sidebar_w - 8.0f;
			const auto pill_h = header_h - 8.0f;

			if ( this->m_textures.menu_header.resource )
			{
				dl.image(
					pill_x,
					pill_y,
					pill_w,
					pill_h,
					this->m_textures.menu_header.resource.Get( ),
					xdraw::corner_radius{ tokens::btn_rounding } );
			}
			else
			{
				dl.rect_filled( pill_x, pill_y, pill_w, pill_h, tokens::col_dark, xdraw::corner_radius{ tokens::btn_rounding } );
			}

			dl.rect_filled( pill_x, pill_y, pill_w, pill_h, tokens::col_dark.alpha( 24 ), xdraw::corner_radius{ tokens::btn_rounding } );
			constexpr std::string_view brand{ "Vagal" };
			dl.text( pill_x + 12.0f, pill_y + 12.0f, brand, tokens::col_text );
			dl.text( pill_x + 12.0f, pill_y + 29.0f, "Counter-Strike 2", tokens::col_text.alpha( 175 ) );
		}

		constexpr auto tab_count{ static_cast< int >( tab::count ) };
		constexpr const char* tab_names[ tab_count ]{ "rage", "legit", "players", "world", "skins", "misc", "configs", "scripts" };
		const auto icon_pad{ 4.0f };
		const auto tabs_top = tabs_y + icon_pad;

		// 子标签嵌在当前主标签下方（截图样式）：先按总行数把行高收进可用高度，避免压到资料卡。
		const auto profile_h = 58.0f;
		const auto profile_x = sb_x + 4.0f;
		const auto profile_y = this->m_y + tokens::gap + header_h + h - profile_h - 6.0f;
		const auto tabs_avail = std::max( 0.0f, profile_y - 6.0f - tabs_top );
		const auto active_subs = ( !this->m_profile_page && this->m_tab >= 0 && this->m_tab < tab_count ) ? k_subtab_defs[ this->m_tab ].count : 0;
		const auto row_units = static_cast< float >( tab_count ) + static_cast< float >( active_subs ) * 0.75f;
		const auto row_h = std::clamp( tabs_avail / std::max( 1.0f, row_units ), 28.0f, tokens::tab_icon_size + 4.0f );
		const auto sub_row_h = row_h * 0.75f;
		constexpr auto sub_indent{ 26.0f };

		auto row_y = tabs_top;

		for ( auto i = 0; i < tab_count; ++i )
		{
			const auto ix = sb_x + 4.0f;
			const auto btn = xui::rect{ ix, row_y, tokens::sidebar_w - 8.0f, row_h };

			const auto hovered = input.in_rect( btn );
			const auto is_active = !this->m_profile_page && ( this->m_tab == i );

			if ( !this->m_search_open && hovered && input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
			{
				this->m_tab = i;
				this->m_subtab = 0;
				this->m_profile_page = false;
			}

			const auto hover_anim = xui::anim::lerp( xui::fnv1a( "sidebar" ) + i, hovered ? 1.0f : 0.0f, 12.0f );
			const auto active_anim = xui::anim::lerp( xui::fnv1a( "sidebar_active" ) + i, is_active ? 1.0f : 0.0f, 10.0f );

			if ( active_anim > 0.01f )
			{
				auto bg = tokens::col_accent;
				bg.a = static_cast< std::uint8_t >( bg.a * active_anim );
				dl.rect_filled( btn.x, btn.y, btn.w, btn.h, bg, xdraw::corner_radius{ tokens::btn_rounding } );
			}
			else if ( hover_anim > 0.01f )
			{
				dl.rect_filled( btn.x, btn.y, btn.w, btn.h, tokens::col_elevated.alpha( static_cast< std::uint8_t >( 80.0f * hover_anim ) ), xdraw::corner_radius{ tokens::btn_rounding } );
			}

			auto icon_col = xui::lerp( tokens::col_text_dim, tokens::col_accent, hover_anim );
			if ( is_active )
			{
				icon_col = xui::lerp( icon_col, tokens::col_dark, active_anim );
			}

			{
				const auto& tex = this->m_textures.tabs[ i ];
				const auto iw = static_cast< float >( tex.width );
				const auto ih = static_cast< float >( tex.height );
				const auto icon_x = std::floor( btn.x + ( tokens::tab_icon_size - iw ) * 0.5f );
				const auto icon_y = std::floor( btn.y + ( btn.h - ih ) * 0.5f );

				dl.image( icon_x, icon_y, iw, ih, tex.resource.Get( ), icon_col );
			}

			const auto label = xui::translate( tab_names[ i ] );
			const auto label_h = xdraw::measure_text( label ).second;
			const auto label_col = is_active ? xui::lerp( tokens::col_text, tokens::col_dark, active_anim ) : xui::lerp( tokens::col_text_dim, tokens::col_text, hover_anim );
			dl.text( btn.x + tokens::tab_icon_size + 4.0f, btn.y + ( btn.h - label_h ) * 0.5f, label, label_col );

			row_y += row_h;

			const auto& def = k_subtab_defs[ i ];

			if ( !is_active || def.count <= 0 )
			{
				continue;
			}

			for ( auto j = 0; j < def.count; ++j )
			{
				const auto sub_btn = xui::rect{ ix + sub_indent, row_y, tokens::sidebar_w - 8.0f - sub_indent, sub_row_h };
				const auto sub_hovered = input.in_rect( sub_btn );
				const auto sub_active = ( this->m_subtab == j );

				if ( !this->m_search_open && sub_hovered && input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
				{
					this->m_subtab = j;
				}

				const auto sub_hover_anim = xui::anim::lerp( xui::fnv1a( "sidebar_sub" ) + i * 8 + j, sub_hovered ? 1.0f : 0.0f, 12.0f );
				const auto sub_active_anim = xui::anim::lerp( xui::fnv1a( "sidebar_sub_active" ) + i * 8 + j, sub_active ? 1.0f : 0.0f, 10.0f );

				if ( sub_active_anim > 0.01f )
				{
					dl.rect_filled( sub_btn.x, sub_btn.y, sub_btn.w, sub_btn.h, tokens::col_accent.alpha( static_cast< std::uint8_t >( 46.0f * sub_active_anim ) ), xdraw::corner_radius{ tokens::btn_rounding } );
				}
				else if ( sub_hover_anim > 0.01f )
				{
					dl.rect_filled( sub_btn.x, sub_btn.y, sub_btn.w, sub_btn.h, tokens::col_elevated.alpha( static_cast< std::uint8_t >( 70.0f * sub_hover_anim ) ), xdraw::corner_radius{ tokens::btn_rounding } );
				}

				const auto sub_label = xui::translate( def.names[ j ] );
				const auto sub_label_h = xdraw::measure_text( sub_label ).second;
				const auto sub_col = sub_active
					? xui::lerp( tokens::col_text_dim, tokens::col_accent, sub_active_anim )
					: xui::lerp( tokens::col_text_dim, tokens::col_text, sub_hover_anim );
				dl.text( sub_btn.x + 12.0f, sub_btn.y + ( sub_btn.h - sub_label_h ) * 0.5f, sub_label, sub_col );

				row_y += sub_row_h;
			}

			row_y += 4.0f;
		}

		// 资料卡：头像圆盘 + 昵称 + 订阅到期（截图样式）。账号数据未就绪时退化为占位。
		// 卡片背景仍由 button 绘制，同时保留点击进入资料页的交互。
		xui::layout::set_cursor( profile_x - this->m_x, profile_y - this->m_y );
		if ( xui::button( "##profile_card", tokens::sidebar_w - 8.0f, profile_h ) )
		{
			this->m_profile_page = true;
			this->m_search_open = false;
		}

		const auto account_data = systems::g_account.get( );
		const auto has_account = !account_data.username.empty( );
		const auto avatar_cx = profile_x + 16.0f + 14.0f;
		const auto avatar_cy = profile_y + profile_h * 0.5f;

		dl.circle_filled( avatar_cx, avatar_cy, 14.0f, tokens::col_accent.alpha( 60 ) );
		dl.circle_filled( avatar_cx, avatar_cy, 12.0f, tokens::col_elevated.alpha( 200 ) );

		{
			const auto initial = has_account ? account_data.username.substr( 0, 1 ) : std::string{ "?" };
			const auto [ iw, ih ] = xdraw::measure_text( initial );
			dl.text( std::floor( avatar_cx - iw * 0.5f ), std::floor( avatar_cy - ih * 0.5f ), initial, tokens::col_text );
		}

		const auto name_x = profile_x + 16.0f + 28.0f + 8.0f;
		const auto name_w = profile_x + tokens::sidebar_w - 30.0f - name_x;
		const std::string user_label = has_account ? account_data.username : std::string{ "--" };
		const std::string_view user_name = xui::truncate( user_label, std::max( 24.0f, name_w ) );
		const auto [ uw, uh ] = xdraw::measure_text( user_name );
		const std::string sub_label = detail::subscription_label( );
		const std::string_view sub_text = xui::truncate( sub_label, std::max( 24.0f, name_w ) );
		const auto [ sw2, sh2 ] = xdraw::measure_text( sub_text );
		const auto text_block_h = uh + 3.0f + sh2;

		dl.text( name_x, std::floor( profile_y + ( profile_h - text_block_h ) * 0.5f ), user_name, tokens::col_text );
		dl.text( name_x, std::floor( profile_y + ( profile_h - text_block_h ) * 0.5f + uh + 3.0f ), sub_text, tokens::col_text_dim );

		const auto chevron_x = profile_x + tokens::sidebar_w - 25.0f;
		const auto chevron_y = profile_y + profile_h * 0.5f;
		const auto chevron_col = tokens::col_text_dim.alpha( input.in_rect( { chevron_x - 8.0f, chevron_y - 10.0f, 18.0f, 20.0f } ) ? 255 : 180 );
		dl.line( chevron_x, chevron_y - 4.0f, chevron_x + 4.0f, chevron_y, chevron_col, 1.5f );
		dl.line( chevron_x + 4.0f, chevron_y, chevron_x, chevron_y + 4.0f, chevron_col, 1.5f );

	}

	void menu::draw_profile_page( float group_w )
	{
		const auto col_w = ( group_w - tokens::gap ) * 0.5f;
		const auto right_x = this->m_body_x + col_w + tokens::gap;
		auto& misc = settings::g_misc;
		auto& hud = misc.m_hud;
		constexpr const char* hat_types[] { "kasa", "bucket" };

		xui::layout::set_cursor( this->m_body_x - this->m_x, this->m_body_y - this->m_y );
		if ( xui::begin_child( "interface", col_w ) )
		{
			xui::text( detail::subscription_label( ), tokens::col_text_dim );
			xui::layout::separator( );
			if ( xui::button( "back", col_w - 24.0f, 28.0f ) )
			{
				this->m_profile_page = false;
			}

			xui::layout::separator( );
			xui::checkbox( "crosshair overlay", hud.m_crosshair.enabled );
			if ( xui::begin_popup( "##xhair_popup", 220.0f ) )
			{
				xui::slider_float( "size##xhair", hud.m_crosshair.size, 0.5f, 10.0f, "%.1f" );
				xui::slider_float( "outline##xhair", hud.m_crosshair.outline, 0.0f, 4.0f, "%.1f" );
				xui::color_picker( "color##xhair", hud.m_crosshair.color );
				xui::color_picker( "outline color##xhair", hud.m_crosshair.outline_color );
				xui::end_popup( );
			}

			xui::checkbox( "scope overlay", hud.m_scope.enabled );
			if ( xui::begin_popup( "##scope_popup", 220.0f ) )
			{
				xui::slider_float( "line length", hud.m_scope.line_length, 10.0f, 500.0f, "%.0f" );
				xui::slider_float( "gap##scope", hud.m_scope.gap, 0.0f, 50.0f, "%.0f" );
				xui::slider_float( "thickness##scope", hud.m_scope.thickness, 0.5f, 5.0f, "%.2f" );
				xui::slider_float( "anim speed", hud.m_scope.anim_speed, 1.0f, 30.0f, "%.0f" );
				xui::color_picker( "color##scope", hud.m_scope.color );
				xui::checkbox( "fade in##scope", hud.m_scope.fade_in );
				xui::layout::separator( );
				xui::checkbox( "glow##scope", hud.m_scope.glow );
				xui::slider_float( "glow strength##scope", hud.m_scope.glow_strength, 0.1f, 1.0f, "%.2f" );
				xui::end_popup( );
			}

			xui::checkbox( "velocity counter", hud.m_velocity.counter );
			xui::checkbox( "velocity chart", hud.m_velocity.chart );
			if ( xui::begin_popup( "##velocity_hud_popup", 220.0f ) )
			{
				xui::color_picker( "color##velocity", hud.m_velocity.color );
				xui::slider_float( "bottom offset", hud.m_velocity.bottom_offset, 20.0f, 200.0f, "%.0f" );
				xui::slider_float( "chart width", hud.m_velocity.chart_width, 120.0f, 320.0f, "%.0f" );
				xui::slider_float( "chart height", hud.m_velocity.chart_height, 24.0f, 80.0f, "%.0f" );
				xui::end_popup( );
			}

			xui::checkbox( "combat indicators", misc.m_widgets.combat_indicators_enabled );
			if ( xui::begin_popup( "##combat_indicators_popup", 240.0f ) )
			{
				xui::checkbox( "fake crouch indicator", hud.m_duck_indicator.enabled );
				xui::color_picker( "color##fake_crouch", hud.m_duck_indicator.color );
				xui::color_picker( "active color##fake_crouch", hud.m_duck_indicator.active_color );
				xui::slider_float( "scale##fake_crouch", hud.m_duck_indicator.scale, 0.72f, 1.8f, "%.2f" );

				xui::checkbox( "HC indicator", hud.m_hitchance_indicator.enabled );
				xui::color_picker( "color##hc", hud.m_hitchance_indicator.color );
				xui::slider_float( "scale##hc", hud.m_hitchance_indicator.scale, 0.72f, 1.8f, "%.2f" );

				xui::checkbox( "MD indicator", hud.m_min_damage_indicator.enabled );
				xui::color_picker( "color##md", hud.m_min_damage_indicator.color );
				xui::slider_float( "scale##md", hud.m_min_damage_indicator.scale, 0.72f, 1.8f, "%.2f" );
				xui::end_popup( );
			}

			xui::checkbox( "slow indicator", hud.m_slow_indicator.enabled );
			if ( xui::begin_popup( "##slow_indicator_popup", 220.0f ) )
			{
				xui::color_picker( "color##slow", hud.m_slow_indicator.color );
				xui::color_picker( "active color##slow", hud.m_slow_indicator.active_color );
				xui::slider_float( "bottom offset##slow", hud.m_slow_indicator.bottom_offset, 40.0f, 360.0f, "%.0f" );
				xui::slider_float( "scale##slow", hud.m_slow_indicator.scale, 0.72f, 1.8f, "%.2f" );
				xui::end_popup( );
			}

			xui::checkbox( "low fps indicator", hud.m_low_fps_indicator.enabled );
			if ( xui::begin_popup( "##low_fps_indicator_popup", 220.0f ) )
			{
				xui::color_picker( "color##low_fps", hud.m_low_fps_indicator.color );
				xui::slider_float( "scale##low_fps", hud.m_low_fps_indicator.scale, 0.72f, 1.8f, "%.2f" );
				xui::end_popup( );
			}

			xui::checkbox( "keybind list", misc.m_widgets.keybinds_enabled );
			xui::slider_float( "keybind scale", misc.m_widgets.keybinds_scale, 0.72f, 1.8f, "%.2f" );
			xui::slider_float( "indicator vertical position", misc.m_widgets.indicator_stack_y, 40.0f, 1400.0f, "%.0f" );
			xui::end_child( );
		}

		xui::layout::set_cursor( right_x - this->m_x, this->m_body_y - this->m_y );
		if ( xui::begin_child( "appearance", col_w ) )
		{
			auto& theme = misc.m_ui_theme;
			xui::color_picker( "accent color", theme.accent );
			xui::color_picker( "main background", theme.main_background );
			xui::color_picker( "panel background", theme.panel_background );
			xui::layout::separator( );

			xui::checkbox( "hat", hud.m_hat.enabled );
			if ( xui::begin_popup( "##hat_popup", 220.0f ) )
			{
				xui::combo( "type##hat", hud.m_hat.type.value, hat_types, 2 );
				xui::color_picker( "color##hat", hud.m_hat.color );
				xui::color_picker( "secondary color##hat", hud.m_hat.secondary_color );
				xui::checkbox( "glow##hat", hud.m_hat.glow );
				xui::slider_float( "glow strength##hat", hud.m_hat.glow_strength, 0.1f, 1.0f, "%.2f" );
				xui::end_popup( );
			}
			xui::end_child( );
		}
	}

	void menu::apply_theme_preset( int preset )
	{
		this->m_theme_preset = std::clamp( preset, 0, 4 );

		switch ( this->m_theme_preset )
		{
		default:
		case 0:
			tokens::col_accent = xdraw::color{ 255, 126, 156, 255 };
			tokens::col_dark = xdraw::color{ 18, 18, 21, 255 };
			tokens::col_text = xdraw::color{ 246, 242, 245, 238 };
			tokens::col_text_dim = xdraw::color{ 184, 190, 198, 120 };
			tokens::col_card = xdraw::color{ 18, 18, 22, 120 };
			tokens::col_elevated = xdraw::color{ 22, 22, 26, 165 };
			break;
		case 1:
			tokens::col_accent = xdraw::color{ 190, 164, 255, 255 };
			tokens::col_dark = xdraw::color{ 18, 17, 23, 255 };
			tokens::col_text = xdraw::color{ 235, 229, 255, 235 };
			tokens::col_text_dim = xdraw::color{ 190, 164, 255, 88 };
			tokens::col_card = xdraw::color{ 18, 17, 23, 88 };
			tokens::col_elevated = xdraw::color{ 34, 31, 43, 125 };
			break;
		case 2:
			tokens::col_accent = xdraw::color{ 122, 224, 181, 255 };
			tokens::col_dark = xdraw::color{ 14, 20, 19, 255 };
			tokens::col_text = xdraw::color{ 222, 246, 238, 235 };
			tokens::col_text_dim = xdraw::color{ 122, 224, 181, 88 };
			tokens::col_card = xdraw::color{ 14, 20, 19, 88 };
			tokens::col_elevated = xdraw::color{ 25, 39, 35, 125 };
			break;
		case 3:
			tokens::col_accent = xdraw::color{ 255, 171, 112, 255 };
			tokens::col_dark = xdraw::color{ 22, 17, 15, 255 };
			tokens::col_text = xdraw::color{ 255, 232, 214, 235 };
			tokens::col_text_dim = xdraw::color{ 255, 171, 112, 88 };
			tokens::col_card = xdraw::color{ 22, 17, 15, 88 };
			tokens::col_elevated = xdraw::color{ 43, 32, 26, 125 };
			break;
		case 4:
			tokens::col_accent = xdraw::color{ 210, 214, 224, 255 };
			tokens::col_dark = xdraw::color{ 18, 18, 20, 255 };
			tokens::col_text = xdraw::color{ 232, 235, 242, 235 };
			tokens::col_text_dim = xdraw::color{ 210, 214, 224, 82 };
			tokens::col_card = xdraw::color{ 18, 18, 20, 92 };
			tokens::col_elevated = xdraw::color{ 35, 35, 39, 126 };
			break;
		}

		auto& theme = settings::g_misc.m_ui_theme;
		theme.accent = tokens::col_accent;
		theme.main_background = tokens::col_elevated;
		theme.panel_background = tokens::col_card;
	}

	void menu::sync_theme_style( ) const
	{
		const auto& theme = settings::g_misc.m_ui_theme;
		tokens::col_accent = theme.accent.value;
		tokens::col_elevated = theme.main_background.value;
		tokens::col_card = theme.panel_background.value;
		auto& style = xui::ctx( ).style;
		style.window_bg = tokens::col_elevated.alpha( 175 );
		style.child_bg = tokens::col_card;
		style.checkbox_bg = tokens::col_card;
		style.checkbox_mark = tokens::col_accent;
		style.checkbox_mark_icon = tokens::col_dark;
		style.slider_track = tokens::col_card;
		style.slider_fill = tokens::col_accent;
		style.button_bg = tokens::col_card;
		style.button_hovered = tokens::col_card.alpha( 120 );
		style.button_active = tokens::col_accent;
		style.keybind_bg = tokens::col_card;
		style.keybind_waiting = tokens::col_accent;
		style.combo_bg = tokens::col_card;
		style.combo_arrow = tokens::col_text_dim;
		style.combo_hovered = tokens::col_card.alpha( 120 );
		style.combo_popup_bg = tokens::col_elevated.alpha( 170 );
		style.combo_popup_item_hovered = tokens::col_card.alpha( 125 );
		style.combo_popup_item_selected = tokens::col_accent.alpha( 42 );
		style.popup_bg = tokens::col_elevated.alpha( 170 );
		style.picker_bg = tokens::col_card;
		style.picker_popup_bg = tokens::col_elevated.alpha( 170 );
		style.text_input_bg = tokens::col_card;
		style.separator = tokens::col_accent.alpha( 30 );
		style.text = tokens::col_text;
		style.text_dim = tokens::col_text_dim;
		style.accent = tokens::col_accent;
	}

	void menu::draw_top_bar( float w )
	{
		auto& dl = xui::draw::current( );
		const auto& input = xui::ctx( ).input;
		const auto& style = xui::ctx( ).style;

		const auto content_x = this->m_x + tokens::gap + tokens::sidebar_w + tokens::gap;
		const auto bar_y = this->m_y + tokens::gap;
		const auto bar_h = tokens::subtab_bar_h;
		constexpr auto inner_pad{ 4.0f };
		const auto field_y = bar_y + inner_pad;
		const auto field_h = bar_h - inner_pad * 2.0f;

		dl.rect_filled( content_x, bar_y, w, bar_h, tokens::col_card, xdraw::corner_radius{ tokens::card_rounding } );

		// 顶栏右侧配置下拉与配置页共用 detail::config_list / detail::selected，两边保持一致。
		if ( detail::needs_refresh )
		{
			detail::config_list = config::registry::list( );
			detail::needs_refresh = false;
			this->m_config_items_dirty = true;

			if ( detail::selected >= static_cast< int >( detail::config_list.size( ) ) )
			{
				detail::selected = -1;
			}
		}

		if ( this->m_config_items_dirty )
		{
			this->m_config_names.clear( );
			this->m_config_items.clear( );
			this->m_config_names.reserve( detail::config_list.size( ) );

			for ( const auto& name : detail::config_list )
			{
				this->m_config_names.emplace_back( detail::to_utf8( name ) );
			}

			this->m_config_items.reserve( this->m_config_names.size( ) );

			for ( const auto& name : this->m_config_names )
			{
				this->m_config_items.emplace_back( name.c_str( ) );
			}

			this->m_config_items_dirty = false;
		}

		constexpr auto save_w{ 72.0f };
		const auto save_x = content_x + w - inner_pad - save_w;
		const auto combo_w = std::clamp( w * 0.2f, 96.0f, 170.0f );
		const auto combo_x = save_x - tokens::gap - combo_w;
		const auto field_x = content_x + inner_pad;
		const auto field_w = std::max( 120.0f, combo_x - tokens::gap - field_x );

		dl.rect_filled( field_x, field_y, field_w, field_h, tokens::col_elevated.alpha( 110 ), xdraw::corner_radius{ tokens::btn_rounding } );

		if ( this->m_textures.search.resource )
		{
			const auto iw = static_cast< float >( this->m_textures.search.width );
			const auto ih = static_cast< float >( this->m_textures.search.height );
			dl.image( std::floor( field_x + 9.0f ), std::floor( field_y + ( field_h - ih ) * 0.5f ), iw, ih, this->m_textures.search.resource.Get( ), tokens::col_text_dim );
		}

		if ( auto* win = xui::layout::current_window( ) )
		{
			const auto saved_bounds = win->bounds;
			const auto saved_cursor_x = win->cursor_x;
			const auto saved_cursor_y = win->cursor_y;
			const auto saved_line_h = win->line_h;

			win->bounds.w = field_x - win->bounds.x + field_w + style.window_pad_x;
			xui::layout::set_cursor( field_x - this->m_x, field_y - this->m_y );

			xui::push_style_var( xui::style_var::text_input_h, field_h );
			xui::push_style_var( xui::style_var::text_input_rounding, tokens::btn_rounding );
			xui::push_style_var( xui::style_var::frame_pad_x, 26.0f );
			xui::push_style_color( xui::style_col::text_input_bg, tokens::col_card.alpha( 0 ) );
			xui::push_style_color( xui::style_col::text_input_border, tokens::col_card.alpha( 0 ) );

			const auto typed = xui::text_input( "##menu_search_input", this->m_search_query, 96, "i want to find..." );

			xui::pop_style_color( 2 );
			xui::pop_style_var( 3 );

			win->bounds = saved_bounds;
			win->cursor_x = saved_cursor_x;
			win->cursor_y = saved_cursor_y;
			win->line_h = saved_line_h;

			if ( typed && !this->m_search_query.empty( ) && !this->m_search_open )
			{
				this->m_search_open = true;
				this->rebuild_search_index( );
			}
		}

		if ( this->m_search_open )
		{
			const auto close_w = field_h;
			const auto close_x = field_x + field_w - inner_pad - close_w;
			const auto close_rect = xui::rect{ close_x, field_y, close_w, field_h };
			const auto close_hovered = input.in_rect( close_rect );
			const auto close_anim = xui::anim::lerp( xui::fnv1a( "menu_search_close" ), close_hovered ? 1.0f : 0.0f, 14.0f );

			dl.rect_filled( close_x, field_y, close_w, field_h, xui::lerp( tokens::col_card.alpha( 0 ), tokens::col_accent.alpha( 80 ), close_anim ), xdraw::corner_radius{ tokens::btn_rounding } );

			const auto cx = close_x + close_w * 0.5f;
			const auto cy = field_y + field_h * 0.5f;
			constexpr auto span{ 4.0f };
			const auto close_col = xui::lerp( tokens::col_text_dim, tokens::col_text, close_anim );
			dl.line( cx - span, cy - span, cx + span, cy + span, close_col, 1.3f );
			dl.line( cx - span, cy + span, cx + span, cy - span, close_col, 1.3f );

			if ( close_hovered && input.mouse_clicked )
			{
				this->close_search( );
			}

			for ( const auto vk : input.key_presses( ) )
			{
				if ( vk == VK_ESCAPE )
				{
					this->close_search( );
					break;
				}
			}
		}

		const auto has_selection = detail::selected >= 0 && detail::selected < static_cast< int >( detail::config_list.size( ) );
		const auto save_rect = xui::rect{ save_x, field_y, save_w, field_h };
		const auto save_hovered = has_selection && input.in_rect( save_rect );
		const auto save_anim = xui::anim::lerp( xui::fnv1a( "menu_topbar_save" ), save_hovered ? 1.0f : 0.0f, 14.0f );

		auto save_bg = tokens::col_accent;
		save_bg.a = has_selection ? static_cast< std::uint8_t >( 210.0f + 45.0f * save_anim ) : 60;
		dl.rect_filled( save_x, field_y, save_w, field_h, save_bg, xdraw::corner_radius{ tokens::btn_rounding } );

		const auto save_label = xui::translate( "save" );
		const auto [ save_label_w, save_label_h ] = xdraw::measure_text( save_label );
		dl.text(
			std::floor( save_x + ( save_w - save_label_w ) * 0.5f ),
			std::floor( field_y + ( field_h - save_label_h ) * 0.5f ),
			save_label,
			tokens::col_text.alpha( has_selection ? 255 : 140 ) );

		if ( save_hovered && input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
		{
			config::registry::save( detail::config_list[ detail::selected ] );
			detail::needs_refresh = true;
		}

		if ( !this->m_config_items.empty( ) )
		{
			const auto label_h = xdraw::measure_text( "" ).second;
			const auto combo_spacing = style.item_spacing_y * 0.25f;
			const auto combo_y = field_y + ( field_h - style.combo_h ) * 0.5f - label_h - combo_spacing;

			xui::layout::set_cursor( combo_x - this->m_x, combo_y - this->m_y );

			if ( xui::combo( "##cfg_combo", detail::selected, this->m_config_items.data( ), static_cast< int >( this->m_config_items.size( ) ), combo_w ) && has_selection )
			{
				config::registry::load( detail::config_list[ detail::selected ] );
				settings::finalize_binds( );
			}
		}
	}

} // namespace rendering
