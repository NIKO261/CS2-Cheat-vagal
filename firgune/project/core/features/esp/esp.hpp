#pragma once

#include "primitive_buffer.hpp"

namespace features::esp {

	namespace player {

		class chams
		{
		public:
			bool on_generate_primitives( std::uintptr_t owner_entity, std::uint32_t owner_hash, std::uintptr_t scene_object, std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_view );
			bool on_preview_generate_primitives( const settings::esp::chams_config& cfg, std::uintptr_t scene_object, std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_view );
			void on_sort_primitives( std::uintptr_t entries, std::uint32_t count );

			class backtrack
			{
			public:
				void update( );
				void shutdown( );

				[[nodiscard]] bool is_active( std::uintptr_t scene_object ) const;
				[[nodiscard]] bool has_active( std::uintptr_t pawn ) const;
				[[nodiscard]] std::uintptr_t get_scene_object( std::uintptr_t pawn ) const;

				struct object {
					std::uintptr_t scene_object {};
					std::uintptr_t pawn {};
					bool active {};
					float spawn_time {};
					int last_bone_tick { -1 };

					void create (std::uintptr_t pawn);
					void destroy ();
					void setup_bones (systems::bones::data* bones, int count) const;
				};

			private:
				void erase_object( std::uintptr_t pawn );

				std::unordered_map<std::uintptr_t, object> m_objects{};
				// scene_object -> pawn 反向索引，is_active 由全表扫描降为 O(1)。
				// generate_primitives 对每个 scene object（含地图静态对象）调用。
				std::unordered_map<std::uintptr_t, std::uintptr_t> m_scene_index{};
			};

			class onshot {
			public:
				void push (std::uintptr_t pawn);
				void update ();
				void shutdown ();

				[[nodiscard]] bool has_active (std::uintptr_t pawn) const;
				[[nodiscard]] bool is_active (std::uintptr_t scene_object) const;
				[[nodiscard]] std::uintptr_t get_scene_object (std::uintptr_t pawn) const;
				[[nodiscard]] float get_alpha (std::uintptr_t pawn) const;

			private:
				struct pending_entry {
					std::array<systems::bones::data, 27> bones {};
					int bone_count {};
				};

				void erase_entry( std::uintptr_t pawn );

				// Scene objects must be created from the frame-stage callback, not CreateMove.
				std::unordered_map<std::uintptr_t, pending_entry> m_pending {};
				std::unordered_map<std::uintptr_t, backtrack::object> m_entries {};
				// scene_object -> pawn 反向索引，is_active O(1)。
				std::unordered_map<std::uintptr_t, std::uintptr_t> m_scene_index {};
			};

			[[nodiscard]] onshot& os () { return this->m_onshot; }
			[[nodiscard]] backtrack& bt( ) { return this->m_backtrack; }

		private:
			void apply_layer( std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_object, std::uintptr_t scene_view, const xdraw::color& color, settings::esp::cham_ids material_id );
			void apply_overlay( std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_object, std::uintptr_t scene_view, const xdraw::color& color, settings::esp::cham_ids material_id );
			void apply_clone( std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_object, std::uintptr_t scene_view, systems::materials::clone_type type );

			void add_overlay_material( std::uintptr_t mat );

			backtrack m_backtrack{};
			onshot m_onshot {};

			// on_sort_primitives 每帧复用：只收集 overlay 项，容量跨帧保留。
			std::vector<detail::mesh_primitive> m_sort_scratch{};

			static constexpr auto k_max_overlay_materials{ 16 };
			std::array<std::atomic<std::uintptr_t>, k_max_overlay_materials> m_overlay_materials{};
			std::atomic<int> m_overlay_material_count{ 0 };
		};

		class glow
		{
		public:
			bool on_is_glowing( std::uintptr_t owner_entity, std::uint32_t owner_hash ) const;
			bool on_get_glow_color( std::uintptr_t owner_entity, std::uint32_t owner_hash, float* color ) const;
		};

		class overlay
		{
		public:
			void on_render( xdraw::draw_list& draw_list );

		private:
			struct draw_offsets
			{
				float left{};
				float top{};
				float bottom{};
			};

			struct info
			{
				std::uintptr_t controller{};
				std::uintptr_t pawn{};
				int health{};
				int team{};
				int money{};
				int armor{};
				int ping{};
				float distance{};
				bool is_other_team{};
				bool is_visible{};
				bool has_helmet{};
				bool has_defuser{};
				bool is_scoped{};
				bool is_defusing{};
				bool is_flashed{};
				char name[ 128 ]{};
				math::vector3 head{};
				std::array<systems::bones::data, 27> bones{};
				math::vector3 origin{};

				struct
				{
					std::uintptr_t ptr{};
					std::uintptr_t vdata{};
					int ammo{};
					int max_ammo{};
					char name[ 64 ]{};

					[[nodiscard]] bool valid( ) const { return this->ptr && this->vdata; }
				} weapon{};

				[[nodiscard]] bool valid( ) const { return this->controller && this->pawn && this->health > 0; }
			};

			void add_box( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const settings::esp::player::overlay::box& cfg, bool visible );
			void add_skeleton( xdraw::draw_list& draw_list, const info& info, const settings::esp::player::overlay::skeleton& cfg, bool visible, const systems::local::snapshot& local );
			void add_health_bar( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const info& info, const settings::esp::player::overlay::health_bar& cfg, draw_offsets& offsets );
			void add_ammo_bar( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const info& info, const settings::esp::player::overlay::ammo_bar& cfg, draw_offsets& offsets );
			void add_name( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const info& info, const settings::esp::player::overlay::name& cfg, draw_offsets& offsets );
			void add_weapon( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const info& info, const settings::esp::player::overlay::weapon& cfg, draw_offsets& offsets );
			void add_flags( xdraw::draw_list& draw_list, const systems::bounds::data& bounds, const info& info, const settings::esp::player::overlay::info_flags& cfg, draw_offsets& offsets );
			void add_oof_arrow( xdraw::draw_list& draw_list, const info& info, const settings::esp::player::overlay::oof_arrow& cfg );
			[[nodiscard]] info get_info( const systems::entities::cached& player, const systems::local::snapshot& local );

			struct animation_data
			{
				animation::spring health{};
				animation::spring ammo{};
				bool initialized{ false };
			};

			std::unordered_map<std::uintptr_t, animation_data> m_animations{};
			// 每帧复用：接收实体快照中 player 类型的条目并按距离排序。
			std::vector<systems::entities::cached> m_scratch_players{};
			// 骨架绘制复用缓冲：跨玩家跨帧保留容量，消除每链/每段的小堆分配。
			std::vector<math::vector2> m_skeleton_screen_scratch{};
			std::vector<float> m_skeleton_spline_scratch{};
		};

	} // namespace player

	namespace item {

		class chams
		{
		public:
			bool on_generate_primitives( std::uintptr_t owner_entity, std::uint32_t owner_hash, std::uintptr_t scene_object, std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_view );

		private:
			void apply_layer( std::uintptr_t primitive_buffer, void( __fastcall* original_fn )( std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t ), std::uintptr_t a1, std::uintptr_t scene_object, std::uintptr_t scene_view, const xdraw::color& color, settings::esp::cham_ids material_id );
			[[nodiscard]] std::uint32_t get_item_group( std::uint32_t schema_hash );
		};

		class glow
		{
		public:
			bool on_is_glowing( std::uintptr_t owner_entity, std::uint32_t owner_hash ) const;
			bool on_get_glow_color( std::uintptr_t owner_entity, std::uint32_t owner_hash, float* color ) const;

		private:
			[[nodiscard]] std::uint32_t get_item_group( std::uint32_t schema_hash ) const;
		};

		class overlay
		{
		public:
			void on_render( xdraw::draw_list& draw_list );

		private:
			struct info
			{
				std::uintptr_t entity{};
				std::uintptr_t vdata{};
				std::uint32_t schema_hash{};
				char name[ 64 ]{};
				math::vector3 origin{};
				float distance{};

				[[nodiscard]] bool valid( ) const { return this->entity && this->name[ 0 ] != '\0'; }
			};

			void add_label( xdraw::draw_list& draw_list, const info& info, const settings::esp::item::overlay::group& cfg );

			[[nodiscard]] info get_info( const systems::entities::cached& entity );
			[[nodiscard]] std::uint32_t get_item_group( std::uint32_t schema_hash );
		};

	} // namespace item

	namespace projectile {

		class overlay
		{
		public:
			void on_render( xdraw::draw_list& draw_list, xdraw::draw_list& middle_draw_list );

		private:
			struct info
			{
				std::uintptr_t entity{};
				std::uint32_t schema_hash{};
				std::uint32_t group_id{};
				math::vector3 origin{};
				float distance{};
				bool detonated{};
				bool smoke_active{};
				int effect_tick_begin{};

				[[nodiscard]] bool valid( ) const { return this->entity && group_id != UINT32_MAX; }
			};

			struct inferno_patch
			{
				math::vector3 center{};
				math::vector3 normal{};
			};

			struct inferno_surface
			{
				std::vector<math::vector3> points{};
				std::vector<std::pair<math::vector3, math::vector3>> edges{};
			};

			struct inferno_state
			{
				std::array<inferno_patch, 64> patches{};
				std::vector<inferno_surface> surfaces{};
				int patch_count{};
				int begin_tick{};
				std::int16_t entity_index{};
				float fade_alpha{};
				std::chrono::steady_clock::time_point spawn_time{};
				math::vector3 last_avg_pos{};
			};

			struct smoke_state
			{
				float fade_alpha{};
				std::chrono::steady_clock::time_point start_time{};
			};

			struct indicator_state
			{
				float fade_alpha{};
				bool was_active{};
			};

			void add_landing_indicators( xdraw::draw_list& draw_list );
			void add_indicator( xdraw::draw_list& draw_list, float cx, float cy, float dir_angle, bool has_arrow, float timer_frac, float alpha, bool is_fire, const settings::esp::projectile::overlay::indicator::group& cfg );

			void add_inferno( xdraw::draw_list& draw_list, xdraw::draw_list& middle_draw_list, const systems::entities::cached& entity, const settings::esp::projectile::overlay::infernos& cfg );
			void add_smoke_area( xdraw::draw_list& draw_list, const systems::entities::cached& entity, const settings::esp::projectile::overlay::smoke_areas& cfg );
			void add_label( xdraw::draw_list& draw_list, const math::vector2& screen, const info& info, const settings::esp::projectile::overlay::group& cfg );

			[[nodiscard]] info get_info( const systems::entities::cached& entity );
			[[nodiscard]] std::uint32_t get_projectile_group( std::uint32_t schema_hash );

			std::unordered_map<std::uintptr_t, inferno_state> m_inferno_states{};
			std::unordered_map<std::uintptr_t, smoke_state> m_smoke_states{};
			std::unordered_map<std::uintptr_t, indicator_state> m_indicator_states{};

			// World-space cells are rebuilt only when the replicated fire patches change.
			std::vector<math::vector3> m_inferno_polygon_scratch{};
			std::vector<math::vector3> m_inferno_clip_scratch{};
			std::vector<math::vector2> m_inferno_points_scratch{};
			// 烟区圆环与指示器弧线复用缓冲。
			std::vector<math::vector2> m_smoke_points_scratch{};
			std::vector<float> m_arc_scratch{};
			std::vector<float> m_arrow_scratch{};
		};

		class tracers
		{
		public:

		};

	} // namespace projectile

	namespace other {

		class overlay
		{
		public:
			void on_render( xdraw::draw_list& draw_list );

		private:
			void add_bomb( xdraw::draw_list& draw_list );
			void add_spectators( xdraw::draw_list& draw_list );
		};

	} // namespace other

} // namespace features::esp
