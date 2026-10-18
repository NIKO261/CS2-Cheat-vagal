#pragma once

namespace features::movement {

	class movement_fix
	{
	public:
		void begin( systems::input::usercmd* cmd );
		void finish( systems::input::usercmd* cmd );

		[[nodiscard]] const math::vector2& source_impulses( ) const { return this->m_source_impulses; }
		[[nodiscard]] float source_yaw( ) const { return this->m_source_yaw; }

	private:
		math::vector2 m_source_impulses{};
		math::vector2 m_previous_impulses{};
		math::vector2 m_persisted_impulses{};
		float m_source_yaw{};
		bool m_active{};
	};

	class bhop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void finish_prediction( systems::input::usercmd* cmd );
		[[nodiscard]] int subtick_reserve( ) const { return this->m_prediction_pending ? 3 : 0; }
		[[nodiscard]] bool suppressed_jump( const systems::input::usercmd* cmd ) const;

	private:
		const systems::input::usercmd* m_suppressed_jump_command{};
		std::intptr_t m_suppressed_jump_number{ -1 };
		std::intptr_t m_prediction_command{ -1 };
		std::uintptr_t m_prediction_pawn{};
		bool m_prediction_pending{};
	};

	class airstrafe
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool finish( systems::input::usercmd* cmd );
		void cancel_pending( ) { this->m_pending = false; }

	private:
		void rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const;

		systems::input::usercmd* m_command{};
		std::intptr_t m_command_number{ -1 };
		std::uintptr_t m_pawn{};
		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		std::uintptr_t m_expected_buttons{};
		math::vector2 m_expected_move{};
		int m_expected_steps{};
		float m_target_yaw{};
		bool m_pending{};
		bool m_side_switch{};
	};

	class jumpbug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void finish_prediction( systems::input::usercmd* cmd );
		[[nodiscard]] int subtick_reserve( ) const { return this->m_prediction_pending ? 4 : 0; }
		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }

	private:

		bool m_active_this_tick{ false };
		bool m_prediction_pending{};
		bool m_candidate_active{};
		float m_landing_fraction{ 1.0f };
		std::intptr_t m_prediction_command{ -1 };
		std::uintptr_t m_prediction_pawn{};
	};

	class fastladder
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgejump
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgestop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgebug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void finish_prediction( systems::input::usercmd* cmd );
		[[nodiscard]] int subtick_reserve( ) const { return this->m_prediction_pending ? this->m_needed : 0; }
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }

	private:
		bool m_active_this_tick{ false };
		bool m_prediction_pending{};
		std::intptr_t m_prediction_command{ -1 };
		std::uintptr_t m_prediction_pawn{};
		int m_needed{};
		int m_passes{};
		bool m_include_jump{};
		float m_landing_fraction{ 1.0f };
	};

	class slowwalk
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class fast_stop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};


} // namespace features::movement
