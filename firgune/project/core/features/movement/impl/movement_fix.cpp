#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

#include "../movement.hpp"

namespace features::movement {

	namespace {

		constexpr auto k_move_epsilon{ 1.0e-6f };
		constexpr auto k_max_subtick_moves{ 32 };
		constexpr auto k_movement_mask = static_cast<std::uintptr_t>(
			cstypes::command_buttons::in_forward
			| cstypes::command_buttons::in_back
			| cstypes::command_buttons::in_moveleft
			| cstypes::command_buttons::in_moveright );

		// 过滤非法输入，并把向量收敛到长度 <= 1（保持方向）。
		// 引擎按 wish 方向解算并截断 wish 速度，超长输入没有增益。
		[[nodiscard]] math::vector2 normalize_impulse( math::vector2 move )
		{
			if ( !std::isfinite( move.x ) || !std::isfinite( move.y ) )
			{
				return {};
			}

			const auto length = std::sqrtf( move.x * move.x + move.y * move.y );
			if ( length > 1.0f )
			{
				move /= length;
			}

			return move;
		}

		// 把 (forwardmove, leftmove) 基准下的输入向量重新表达在 target_yaw
		// 基准下，世界方向不变。
		// 引擎解算 wish = forward * forwardmove + left * leftmove，left 为玩家
		// 左侧（几何左 = (-sin yaw, cos yaw)，即 angle_vectors_2d 的 right 参数），
		// leftmove 正 = 左移。世界向量 w = forward(source) * f + left(source) * l，
		// 在新基准里取 (w . forward(target), w . left(target)) 即为重投影：
		//   f' = f * cos(d) + l * sin(d)
		//   l' = l * cos(d) - f * sin(d)        d = target_yaw - source_yaw
		// 该形式与 subtick 复杂路径的 fold 编码逐位自洽：reframe(v, fold, 0)
		// 等价于直接 reframe(v, source_yaw + sum_yaw, target_yaw)。
		// 2026-08-29 曾把 sin 项符号反过来（按 leftmove 正 = 右移 推导），实测
		// 地面方向错乱且空中无法转向与加速，已回滚：符号翻转会破坏 subtick
		// 复杂路径上的 airstrafe 编码，airstrafe 的两次镜像使其对 leftmove
		// 符号不敏感，不能拿来反推本函数的方向。
		[[nodiscard]] math::vector2 reframe_move( const math::vector2& move, float source_yaw, float target_yaw )
		{
			if ( !std::isfinite( source_yaw ) || !std::isfinite( target_yaw ) )
			{
				return {};
			}

			const auto delta = std::remainderf( target_yaw - source_yaw, 360.0f )
				* ( std::numbers::pi_v<float> / 180.0f );
			const auto cosine = std::cosf( delta );
			const auto sine = std::sinf( delta );

			return {
				move.x * cosine + move.y * sine,
				move.y * cosine - move.x * sine
			};
		}

		[[nodiscard]] math::vector2 read_analog_delta( const proto::subtick_move_step* step )
		{
			if ( !step )
			{
				return {};
			}

			const auto forward = step->m_has_bits.test( 0x8u ) ? step->analog_forward_delta( ) : 0.0f;
			const auto left = step->m_has_bits.test( 0x10u ) ? step->analog_left_delta( ) : 0.0f;
			return {
				std::isfinite( forward ) ? forward : 0.0f,
				std::isfinite( left ) ? left : 0.0f
			};
		}

		[[nodiscard]] bool has_analog_fields( const proto::subtick_move_step* step )
		{
			return step && ( step->m_has_bits.test( 0x8u ) || step->m_has_bits.test( 0x10u ) );
		}

		void write_analog_delta( proto::subtick_move_step* step, const math::vector2& delta )
		{
			if ( !step )
			{
				return;
			}

			const auto forward = std::isfinite( delta.x ) ? delta.x : 0.0f;
			const auto left = std::isfinite( delta.y ) ? delta.y : 0.0f;
			step->m_analog_forward_delta = forward;
			step->m_analog_left_delta = left;

			if ( std::fabsf( forward ) > k_move_epsilon )
			{
				step->m_has_bits.set( 0x8u );
			}
			else
			{
				step->m_has_bits.clear( 0x8u );
			}

			if ( std::fabsf( left ) > k_move_epsilon )
			{
				step->m_has_bits.set( 0x10u );
			}
			else
			{
				step->m_has_bits.clear( 0x10u );
			}

			step->m_cached_size = 0;
		}

		[[nodiscard]] float step_when( const proto::subtick_move_step* step )
		{
			if ( !step || !step->m_has_bits.test( 0x4u ) || !std::isfinite( step->when( ) ) )
			{
				return 0.0f;
			}

			return std::clamp( step->when( ), 0.0f, 1.0f );
		}

		void stable_sort_subticks( proto::repeated_ptr_field<proto::subtick_move_step>* moves )
		{
			if ( !moves || !moves->m_rep )
			{
				return;
			}

			auto elements = moves->m_rep->elements;
			for ( auto i = 1; i < moves->size( ); ++i )
			{
				const auto current = elements[ i ];
				const auto current_when = step_when( proto::impl_ptr<proto::subtick_move_step>( current ) );
				auto j = i;
				while ( j > 0
					&& step_when( proto::impl_ptr<proto::subtick_move_step>( elements[ j - 1 ] ) ) > current_when )
				{
					elements[ j ] = elements[ j - 1 ];
					--j;
				}
				elements[ j ] = current;
			}
		}

		[[nodiscard]] proto::subtick_move_step* prepend_anchor( proto::repeated_ptr_field<proto::subtick_move_step>* moves )
		{
			if ( !moves || moves->size( ) >= k_max_subtick_moves )
			{
				return nullptr;
			}

			const auto previous_size = moves->size( );
			const auto anchor = systems::g_input.acquire_subtick_step( moves );
			if ( !anchor || !moves->m_rep || moves->size( ) != previous_size + 1 )
			{
				return nullptr;
			}

			auto elements = moves->m_rep->elements;
			const auto raw_anchor = elements[ previous_size ];
			std::move_backward( elements, elements + previous_size, elements + previous_size + 1 );
			elements[ 0 ] = raw_anchor;
			anchor->set_button( 0 );
			anchor->set_pressed( false );
			anchor->set_when( 0.0f );
			return anchor;
		}

		// 让移动按钮位与模拟输入一致。
		// 引擎 wish = forward * forwardmove + left * leftmove，left 为玩家左侧，
		// 因此 leftmove 正 = 左（IN_MOVELEFT），负 = 右（IN_MOVERIGHT）。
		// 与 antiaim::correct_movement（misc.cpp）的映射一致；旧写法把左右
		// 写反了（leftmove 正 -> IN_MOVERIGHT）。AA 基础翻转 180 度时
		// reframe 会把 (f, l) 整体取反，取反恰好掩盖了这个反向；一旦 yaw
		// 偏移不是 180 度（横滚补偿 +33、manual ±90、body angle、鼠标控制），
		// 取反不再成立，按钮位就会把移动拉到错误的一侧。
		// 状态对应的移动按钮位（leftmove 正 = 左，见 sync_movement_buttons 注释）。
		[[nodiscard]] std::uint64_t movement_button_mask( const math::vector2& move )
		{
			auto bits{ 0ull };
			if ( move.x > k_move_epsilon )
			{
				bits |= cstypes::command_buttons::in_forward;
			}
			else if ( move.x < -k_move_epsilon )
			{
				bits |= cstypes::command_buttons::in_back;
			}

			if ( move.y > k_move_epsilon )
			{
				bits |= cstypes::command_buttons::in_moveleft;
			}
			else if ( move.y < -k_move_epsilon )
			{
				bits |= cstypes::command_buttons::in_moveright;
			}

			return bits;
		}

		struct previous_button_state
		{
			std::uintptr_t buttons{};
			bool valid{};
		};

		[[nodiscard]] previous_button_state previous_movement_buttons( const systems::input::usercmd* cmd )
		{
			if ( !cmd || cmd->command_number <= 0 )
			{
				return {};
			}

			const auto local = systems::g_local.get( );
			if ( !local.controller )
			{
				return {};
			}

			const auto previous = systems::g_input.get_command_by_sequence(
				local.controller, static_cast<int>( cmd->command_number - 1 ) );
			if ( !previous || previous == cmd || previous->command_number != cmd->command_number - 1 )
			{
				return {};
			}

			return { previous->buttons.value & k_movement_mask, true };
		}

		void sync_movement_buttons(
			systems::input::usercmd* cmd,
			const math::vector2& move,
			const previous_button_state& previous )
		{
			const auto before = cmd->buttons.value;
			const auto buttons = ( before & ~k_movement_mask ) | movement_button_mask( move );

			// value_changed/value_scroll 是相对上一条线上命令的边沿，不是相对
			// 本条命令被重投影前的物理 WASD。旧代码拿 before 做基准，AA 让
			// 输出方向不同于物理按键时便会每 tick 重复发送假释放/假按下；
			// 非直角补偿会同时涉及两轴，因此最容易被这些伪边沿拉偏或翻向。
			const auto target_buttons = buttons & k_movement_mask;
			const auto changed = previous.valid
				? ( previous.buttons ^ target_buttons )
				: ( ( before & k_movement_mask ) ^ target_buttons );
			cmd->buttons.value = buttons;
			cmd->buttons.value_changed = ( cmd->buttons.value_changed & ~k_movement_mask ) | changed;
			cmd->buttons.value_scroll = ( cmd->buttons.value_scroll & ~k_movement_mask )
				| ( target_buttons & changed );
		}

		// 量化路径用：AA 基准下 45° 倍数方向的数字状态与单位向量。
		// 数字状态是整数 (forward, left) 对——对角按两轴同时满键表示，引擎把
		// wish 速度截断到最大速度后与正轴向同速，与数字按钮输入语义一致；
		// 无论服务器把模拟量取整到什么粒度，整数状态都逐位存活。
		[[nodiscard]] math::vector2 cardinal_state( float degrees, math::vector2& unit )
		{
			const auto radians = degrees * ( std::numbers::pi_v<float> / 180.0f );
			unit = { std::cosf( radians ), std::sinf( radians ) };
			return { std::roundf( unit.x ), std::roundf( unit.y ) };
		}

		[[nodiscard]] float cross_2d( const math::vector2& a, const math::vector2& b )
		{
			return a.x * b.y - a.y * b.x;
		}

	} // namespace

	void movement_fix::begin( systems::input::usercmd* cmd )
	{
		this->m_active = false;
		this->m_source_impulses = {};
		this->m_previous_impulses = {};
		this->m_persisted_impulses = {};
		this->m_source_yaw = 0.0f;

		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		const auto angles = base ? base->viewangles( ) : nullptr;
		if ( !base || !angles || !std::isfinite( angles->y( ) ) )
		{
			return;
		}

		// This command's base fields are the only authoritative input intent.
		// MovementServices belongs to an already processed command and is only
		// used later as the starting point for serialized analog deltas.
		this->m_source_impulses = normalize_impulse( { base->forwardmove( ), base->leftmove( ) } );
		this->m_source_yaw = std::remainderf( angles->y( ), 360.0f );

		const auto& prestate = systems::g_prediction.pre( );
		this->m_previous_impulses = normalize_impulse( {
			prestate.last_movement_impulses.x,
			prestate.last_movement_impulses.y
		} );

		// 量化分支的锚点用未归一化的持久化分量：上一命令收尾若是对角状态
		// （长度 > 1），归一化会让锚点增量无法把链起点精确搬到目标状态。
		this->m_persisted_impulses = {
			std::isfinite( prestate.last_movement_impulses.x ) ? prestate.last_movement_impulses.x : 0.0f,
			std::isfinite( prestate.last_movement_impulses.y ) ? prestate.last_movement_impulses.y : 0.0f
		};
		this->m_active = true;
	}

	void movement_fix::finish( systems::input::usercmd* cmd )
	{
		const auto base = cmd ? cmd->csgo_user_cmd.mutable_base( ) : nullptr;
		const auto angles = base ? base->viewangles( ) : nullptr;
		if ( !this->m_active || !base || !angles || !std::isfinite( angles->y( ) ) )
		{
			this->m_active = false;
			return;
		}

		const auto pawn = systems::g_local.get( ).pawn;
		const auto move_type = pawn
			? memory::read<std::uint8_t>( pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) )
			: cstypes::move_type::none;
		const auto is_3d_movement = move_type == cstypes::move_type::ladder
			|| move_type == cstypes::move_type::noclip
			|| move_type == cstypes::move_type::observer;

		const auto target_yaw = std::remainderf( angles->y( ), 360.0f );
		const auto yaw_shift = std::remainderf( target_yaw - this->m_source_yaw, 360.0f );

		auto moves = base->mutable_subtick_moves( );
		const auto step_count = moves ? moves->size( ) : 0;

		// 通用输入链的角度与模拟增量；统一空中轨迹在下方单独提交。
		auto has_yaw_steps = false;
		auto has_analog_steps = false;
		for ( auto i = 0; i < step_count; ++i )
		{
			const auto step = moves->mutable_at( i );
			if ( !step )
			{
				continue;
			}

			if ( step->m_has_bits.test( 0x40u ) && std::fabsf( step->yaw_delta( ) ) > k_move_epsilon )
			{
				has_yaw_steps = true;
			}

			if ( has_analog_fields( step ) )
			{
				has_analog_steps = true;
			}
		}

		auto quantized{ true };
		if ( const auto cvar = CONVAR( "sv_quantize_movement_input" ) )
		{
			quantized = cvar->get<bool>( );
		}

		if ( is_3d_movement )
		{
			base->m_cached_size = 0;
			this->m_active = false;
			return;
		}

		// The unified strafer emits its complete trajectory in the FINAL yaw
		// basis. Do not quantize its wish again or fold away its yaw endpoints.
		if ( g_airstrafe.finish( cmd ) )
		{
			base->m_cached_size = 0;
			this->m_active = false;
			return;
		}

		const auto previous_buttons = previous_movement_buttons( cmd );

		if ( quantized )
		{
			// 量化服务器（官方匹配）在 SetupMove 里对 forward/side 逐项取整
			// （V_roundf，见 _experiences/2026-08-28-slowwalk-quantize-rounding.md）。
			// 把输入旋转到 AA 基准的空间换算在任意偏转角下产生非整数分量，
			// 取整直接把方向偏到最近的基准斜向——180/±90 旋转后仍是整数，
			// 因此只有这两个角度无恙。
			// 改用时间分割：把目标方向表达为基准下相邻两个可精确表示的数字
			// 状态（整数 (f,l) 对），按 2D 几何解出的时间权重在一个 tick 内用
			// subtick 步切换，时间加权平均方向即目标方向。相邻 45° 扇区的数字
			// 状态恰差一个按钮位，切换步与原生键盘事件步（按钮事件 + 单轴 ±1
			// 模拟增量）同构；扇区边界退化为单状态，与无 AA 行为一致。
			if ( std::fabsf( yaw_shift ) <= k_move_epsilon )
			{
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			const auto intent = normalize_impulse( { base->forwardmove( ), base->leftmove( ) } );

			const auto reoriented = reframe_move( intent, this->m_source_yaw, target_yaw );
			const auto reoriented_length = std::sqrtf( reoriented.x * reoriented.x + reoriented.y * reoriented.y );
			if ( !( reoriented_length > k_move_epsilon ) )
			{
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			// base 写数字状态并同步按钮位（两条通道语义一致，服务器无论
			// 消费哪个通道结果相同）。
			const auto write_base_state = [&]( const math::vector2& state )
			{
				base->set_forwardmove( state.x );
				base->set_leftmove( state.y );
				sync_movement_buttons( cmd, state, previous_buttons );
			};

			// 通用量化路径需要给数字状态切换腾出槽位：移除不携带 yaw
			// 字段的纯模拟记录，保留其他写入者的角度记录。统一空中轨迹
			// 已在上方单独提交，不会进入此分支。分数 base 输入会被逐轴
			// roundf 吸到错误方向，因此这里继续使用原来的数字时间分割。
			if ( has_analog_steps && moves && moves->m_rep )
			{
				auto elements = moves->m_rep->elements;
				auto kept = 0;
				const auto total = moves->size( );
				for ( auto index = 0; index < total; ++index )
				{
					const auto step = static_cast< proto::subtick_move_step* >( elements[ index ] );
					const auto pure_analog = has_analog_fields( step ) && !step->m_has_bits.test( 0x40u );
					if ( !pure_analog )
					{
						elements[ kept ] = elements[ index ];
						++kept;
					}
				}
				moves->m_current_size = kept;
			}

			// 目标方向在最终（AA）基准下的单位向量（正向为左）与所在 45° 扇区。
			const auto wish = reoriented / reoriented_length;
			auto wish_angle = std::atan2f( wish.y, wish.x ) * ( 180.0f / std::numbers::pi_v<float> );
			if ( wish_angle < 0.0f )
			{
				wish_angle += 360.0f;
			}

			const auto sector = std::min( static_cast<int>( wish_angle / 45.0f ), 7 );
			const auto sector_angle = static_cast<float>( sector ) * 45.0f;
			math::vector2 first_unit{};
			math::vector2 second_unit{};
			const auto first_state = cardinal_state( sector_angle, first_unit );
			const auto second_state = cardinal_state( sector_angle + 45.0f, second_unit );

			// 状态一时间占比 p：cross(p·u1 + (1-p)·u2, wish) = 0 解出
			// p = cross(u2,wish) / (cross(u2,wish) - cross(u1,wish))，
			// 使两状态按时间加权平均后恰为目标方向。
			const auto first_cross = cross_2d( first_unit, wish );
			const auto second_cross = cross_2d( second_unit, wish );
			const auto cross_span = second_cross - first_cross;
			if ( !( std::fabsf( cross_span ) > k_move_epsilon ) )
			{
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			const auto first_share = std::clamp( second_cross / cross_span, 0.0f, 1.0f );

			// 扇区边界：目标方向恰为数字状态，单状态输出。
			if ( first_share >= 1.0f - k_move_epsilon )
			{
				write_base_state( first_state );
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}
			if ( first_share <= k_move_epsilon )
			{
				write_base_state( second_state );
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			// 链起点锚点把上一命令收尾的持续模拟状态搬到状态一（input::apply
			// 的对账步只在链上无模拟步时写，本分支自带模拟步，须自备锚点）。
			const auto original_move_count = moves ? moves->size( ) : 0;
			const auto anchor = prepend_anchor( moves );
			if ( !anchor )
			{
				// 容量不足等异常：吸附最近数字状态（≤22.5° 方向误差）。分数
				// base 会被 SetupMove 逐项 roundf 到任意错误状态（可反向），
				// 不可作兜底。
				write_base_state( first_share >= 0.5f ? first_state : second_state );
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			// prepend_anchor 把原有指针整体右移一格；后续任一步分配失败时
			// 原子撤回本分支新增的所有步骤，避免发送半条按钮时间线。
			const auto rollback_generated_steps = [ & ]
			{
				if ( !moves || !moves->m_rep || moves->size( ) <= original_move_count )
				{
					return;
				}

				auto elements = moves->m_rep->elements;
				std::move( elements + 1, elements + 1 + original_move_count, elements );
				moves->m_current_size = original_move_count;
			};

			write_analog_delta( anchor, first_state - this->m_persisted_impulses );

			const auto first_buttons = movement_button_mask( first_state );
			const auto second_buttons = movement_button_mask( second_state );
			const auto initial_buttons = previous_buttons.valid
				? previous_buttons.buttons
				: ( cmd->buttons.value & k_movement_mask );
			const auto start_changed = initial_buttons ^ first_buttons;
			auto pending_start_events = start_changed;

			// 命令以第二状态结束时，第一状态不能再靠 base buttons 冒充。
			// 明确写出 when=0 的起始按钮边沿；稳定输入时相邻扇区只需一个
			// 边沿，输入/角度突变时可用同一时刻的额外步骤补齐。
			if ( pending_start_events != 0 )
			{
				const auto button = pending_start_events & ( ~pending_start_events + 1ull );
				anchor->set_button( button );
				anchor->set_pressed( ( first_buttons & button ) != 0 );
				pending_start_events &= ~button;
			}
			while ( pending_start_events != 0 && moves->size( ) < k_max_subtick_moves - 1 )
			{
				const auto step = systems::g_input.acquire_subtick_step( moves );
				if ( !step )
				{
					break;
				}

				const auto button = pending_start_events & ( ~pending_start_events + 1ull );
				step->set_button( button );
				step->set_pressed( ( first_buttons & button ) != 0 );
				step->set_when( 0.0f );
				pending_start_events &= ~button;
			}

			const auto switch_step = pending_start_events == 0
				? systems::g_input.acquire_subtick_step( moves )
				: nullptr;
			if ( !switch_step )
			{
				rollback_generated_steps( );
				write_base_state( first_share >= 0.5f ? first_state : second_state );
				base->m_cached_size = 0;
				this->m_active = false;
				return;
			}

			// 相邻扇区状态恰差一个按钮位：按钮事件与模拟增量同步切换
			// （按钮位与模拟量一致，服务器无论消费哪个通道语义都相同）。
			write_analog_delta( switch_step, second_state - first_state );
			switch_step->set_when( first_share );

			const auto changed_bits = second_buttons ^ first_buttons;
			if ( changed_bits != 0 && ( changed_bits & ( changed_bits - 1 ) ) == 0 )
			{
				switch_step->set_button( changed_bits );
				switch_step->set_pressed( ( second_buttons & changed_bits ) != 0 );
			}

			// base forward/left 与 buttons 表示命令处理完成后的绝对状态，必须
			// 等于 subtick 链终点。旧代码写 first_state，导致任意角分段后
			// base/按钮终态与 switch_step 到达的 second_state 相互矛盾；下一
			// 命令会从错误方向续接，表现为地面偏斜和空中转向反向。
			write_base_state( second_state );

			// 汇总位必须覆盖 tick 内短暂出现后又释放的按钮，否则按钮时间线
			// 与绝对终态仍会互相否定。
			const auto timeline_changed = start_changed | changed_bits;
			const auto timeline_pressed = ( first_buttons & start_changed )
				| ( second_buttons & changed_bits );
			cmd->buttons.value_changed = ( cmd->buttons.value_changed & ~k_movement_mask )
				| timeline_changed;
			cmd->buttons.value_scroll = ( cmd->buttons.value_scroll & ~k_movement_mask )
				| timeline_pressed;
			stable_sort_subticks( moves );
			base->m_cached_size = 0;
			this->m_active = false;
			return;
		}

		if ( !( std::fabsf( yaw_shift ) > k_move_epsilon || has_yaw_steps || has_analog_steps ) )
		{
			// 非量化且无事发生：不动。
			base->m_cached_size = 0;
			this->m_active = false;
			return;
		}

		stable_sort_subticks( moves );

		// 意图轨迹（yaw 未被改写时已验证可用的语义）：模拟状态从玩家
		// 输入起累计各步增量，移动基准从 source yaw 起随 yaw 增量旋转。
		//   world_j = rot(source_yaw + sum_yaw_j) * intent_state_j
		struct trajectory_point
		{
			proto::subtick_move_step* step{};
			math::vector2 state{};
		};
		std::array<trajectory_point, k_max_subtick_moves> points{};
		auto point_count{ 0 };

		auto intent_state = this->m_source_impulses;
		auto sum_yaw = 0.0f;
		for ( auto i = 0; i < step_count && point_count < k_max_subtick_moves; ++i )
		{
			const auto step = moves->mutable_at( i );
			if ( !step )
			{
				continue;
			}

			const auto carries_trajectory = has_analog_fields( step )
				|| ( step->m_has_bits.test( 0x40u ) && std::fabsf( step->yaw_delta( ) ) > k_move_epsilon );
			if ( !carries_trajectory )
			{
				continue;
			}

			intent_state = intent_state + read_analog_delta( step );
			if ( step->m_has_bits.test( 0x40u ) )
			{
				sum_yaw = std::remainderf( sum_yaw + step->yaw_delta( ), 360.0f );
			}

			// 最终基准下的重编码值：基准扫掠与 yaw 改写都折叠进模拟值
			// 本身，满足 rot(target_yaw) * state' = rot(source_yaw +
			// sum_yaw) * state。reframe(v, fold, 0) 即 rot(fold) * v。
			const auto fold = std::remainderf( sum_yaw - yaw_shift, 360.0f );
			points[ point_count++ ] = { step, reframe_move( intent_state, fold, 0.0f ) };
		}

		auto final_state = normalize_impulse( { base->forwardmove( ), base->leftmove( ) } );
		if ( point_count > 0 )
		{
			// when=0 锚点把引擎的持续模拟状态强制搬到意图轨迹起点，
			// 链不再依赖上一命令的收尾向量。
			auto chain_state = this->m_previous_impulses;
			if ( const auto anchor = prepend_anchor( moves ) )
			{
				write_analog_delta( anchor, points[ 0 ].state - chain_state );
				chain_state = points[ 0 ].state;
			}

			for ( auto i = 0; i < point_count; ++i )
			{
				write_analog_delta( points[ i ].step, points[ i ].state - chain_state );
				chain_state = points[ i ].state;

				// 基准旋转已折叠进模拟值，yaw 增量清零
				//（步骤保留按钮与时机字段）。
				points[ i ].step->m_yaw_delta = 0.0f;
				points[ i ].step->m_has_bits.clear( 0x40u );
				points[ i ].step->m_cached_size = 0;
			}

			final_state = normalize_impulse( chain_state );
		}
		else
		{
			// 无轨迹步骤：只有 base 移动量需要换算到最终基准。
			final_state = reframe_move( final_state, this->m_source_yaw, target_yaw );
		}

		// base 与链终点一致：无论服务器读 base、积分链还是两者对账，
		// 解析出的 wish 方向相同。
		base->set_forwardmove( final_state.x );
		base->set_leftmove( final_state.y );
		sync_movement_buttons( cmd, final_state, previous_buttons );

		base->m_cached_size = 0;
		this->m_active = false;
	}

} // namespace features::movement
