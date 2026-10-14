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

	private:
		std::intptr_t m_prediction_command{ -1 };
		std::uintptr_t m_prediction_pawn{};
		bool m_prediction_pending{};
	};

	class airstrafe
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_angles( );

	private:
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		void rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const;
		void rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const;

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		bool m_side_switch{};
		math::vector3 m_angles{};
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
		float m_hint_when{};
		int m_needed{};
		int m_passes{};
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

	class test_strafer
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool is_active( ) const;
		[[nodiscard]] bool handled_this_tick( ) const { return this->m_handled_this_tick; }

	private:
		[[nodiscard]] bool apply_yaw_subtick( proto::base_usercmd_pb* base, float when, float yaw_delta, float forward_delta = 0.0f, float left_delta = 0.0f ) const;
		[[nodiscard]] bool apply_analog_subtick( proto::base_usercmd_pb* base, float when, float forward_delta, float left_delta ) const;
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		[[nodiscard]] static math::vector2 movement_from_buttons( std::uintptr_t pressed );

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		int m_substep_counter{};
		bool m_handled_this_tick{};
	};

} // namespace features::movement
