#pragma once

#include "racebox/core.hpp"
#include "racebox/crew_chief.hpp"
#include "racebox/driver_analysis.hpp"
#include "racebox/race_day.hpp"
#include "racebox/telemetry_plot_order.hpp"

#include <d3d11.h>
#include <imgui.h>
#include <windows.h>

#include <array>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace racebox::app {

struct Texture {
    ID3D11Texture2D* resource{};
    ID3D11ShaderResourceView* view{};
    int width{};
    int height{};
    void reset();
};

std::vector<std::filesystem::path> open_telemetry_files(HWND owner);
std::optional<std::filesystem::path> open_race_day_file(HWND owner);
std::optional<std::filesystem::path> open_image_file(HWND owner);
std::optional<std::filesystem::path> save_file(HWND owner, const wchar_t* title, const wchar_t* extension, const wchar_t* filter);
bool load_texture(ID3D11Device* device, const std::filesystem::path& path, Texture& texture, std::string& error);

class NativeApp {
public:
    NativeApp(HWND window, ID3D11Device* device, bool software_renderer);
    ~NativeApp();
    NativeApp(const NativeApp&) = delete;
    NativeApp& operator=(const NativeApp&) = delete;

    bool render();
    void open_files(const std::vector<std::filesystem::path>& files);
    void enable_soak_mode();

private:
    enum class WorkspaceSection { Overview, Analysis, Compare, Sectors, RaceDay };
    enum class TelemetryTab { Telemetry, Imu, Events, Sectors };

    struct PlotData {
        std::vector<double> time;
        std::vector<double> elapsed;
        std::vector<double> speed;
        std::vector<double> speed_mph;
        std::vector<double> lateral;
        std::vector<double> longitudinal;
        std::vector<double> altitude;
        std::vector<double> vertical;
        std::vector<double> gyro_x;
        std::vector<double> gyro_y;
        std::vector<double> gyro_z;
        std::vector<double> vehicle_yaw;
        std::vector<double> controls;
        std::vector<double> steering;
    };

    struct Annotation {
        int id{};
        std::string surface;
        std::string anchor_window;
        std::string plot_id;
        int plot_index{-1};
        float normalized_x{};
        float normalized_y{};
        Timestamp cursor_us{};
        int active_lap_index{};
        int compare_lap_index{};
        int compare_b_lap_index{};
        int playback_lap_index{};
        ViewMode view_mode{ViewMode::SingleLap};
        std::array<char, 256> note{};
    };

    void draw_app_header();
    bool build_overview_layout(ImGuiID dockspace, ImVec2 origin, ImVec2 size);
    void draw_playback();
    void draw_map();
    void draw_telemetry();
    void draw_laps();
    void draw_radio();
    void draw_analysis();
    void draw_diagnostics();
    void draw_gg_plot();
    void draw_altitude();
    void draw_imu_tab();
    void draw_annotations();
    void draw_crew_chief();
    void draw_race_day();
    void draw_dev_notes();
    void draw_events_tab();
    void draw_sectors_tab();
    void poll_loader();
    void begin_load(const std::vector<std::filesystem::path>& files);
    void save_session();
    void save_active_lap();
    void export_active_lap();
    void load_background();
    void load_triangulated_background();
    void export_annotations();
    void export_driver_analysis();
    void new_race_day();
    void open_race_day();
    void save_race_day(bool choose_path);
    void attach_race_day_telemetry();
    void analyze_race_day_runs();
    void handle_annotation_surface(const char* surface, int plot_index, float origin_x, float origin_y,
                                   float width, float height, bool hovered, const char* stable_plot_id = nullptr);
    void handle_annotation_window(const char* surface);
    void handle_annotation_workspace();
    void go_to_annotation(const Annotation& annotation);
    void refresh_driver_analysis(bool force = false);
    void auto_number_corners();
    bool place_start_finish_at_index(std::size_t telemetry_index);
    void navigate_to_analysis_target(const driver_analysis::NavigationTarget& target);
    void navigate_to_progress(double progress);
    [[nodiscard]] std::string serialize_workspace_state() const;
    void restore_workspace_state();
    void reset_workspace_state();
    PlotData build_plot_data(const LapInfo* lap, std::size_t max_points = 2200, bool normalized_progress = false) const;
    void refresh_plot_cache();
    void sync_continuous_lap();
    bool ensure_unique_complete_comparison_roles();
    void open_continuous_lap(int lap_index);
    void return_to_continuous();
    std::pair<std::size_t, std::size_t> active_range() const;
    const LapInfo* active_lap() const;
    const LapInfo* compare_lap() const;
    const LapInfo* compare_b_lap() const;
    const LapInfo* playback_lap() const;
    static const char* phase_name(LapPhase phase);

    HWND window_{};
    ID3D11Device* device_{};
    bool software_renderer_{};
    bool exit_requested_{};
    bool loading_{};
    bool soak_mode_{};
    std::future<LoadResult> load_future_;
    std::filesystem::path pending_vbo_;
    std::filesystem::path pending_racebox_csv_;
    std::filesystem::path pending_sanwa_csv_;
    std::optional<Session> session_;
    std::string status_{"Open any VBO, RaceBox CSV, GPX, session archive, or Sanwa file."};
    std::string error_;
    Texture background_;
    ViewMode view_mode_{ViewMode::SingleLap};
    int active_lap_index_{};
    int compare_lap_index_{1};
    int compare_b_lap_index_{2};
    int playback_lap_index_{};
    bool fastest_reference_{true};
    int continuous_begin_{};
    int continuous_end_{};
    bool continuous_drilldown_{};
    Timestamp continuous_resume_cursor_us_{};
    bool continuous_resume_playing_{};
    bool include_out_lap_{};
    bool include_in_lap_{};
    bool playing_{};
    bool repeat_{};
    double playback_speed_{1.0};
    Timestamp cursor_us_{};
    std::optional<Timestamp> hover_cursor_us_;
    bool hover_seen_this_frame_{};
    bool light_theme_{};
    bool metric_units_{true};
    bool show_speed_color_{true};
    bool show_average_track_guide_{true};
    bool separate_compare_maps_{};
    bool show_analysis_aligned_traces_{true};
    bool show_map_grid_{true};
    float map_grid_spacing_m_{10.0F};
    WorkspaceSection workspace_section_{WorkspaceSection::Overview};
    TelemetryTab requested_telemetry_tab_{TelemetryTab::Telemetry};
    bool telemetry_tab_request_pending_{};
    bool show_radio_panel_{};
    bool show_analysis_panel_{};
    bool show_gg_panel_{};
    bool show_altitude_panel_{};
    bool show_diagnostics_panel_{};
    bool compact_telemetry_{true};
    bool annotation_mode_{};
    bool start_finish_placement_mode_{};
    bool show_turn_numbers_{true};
    bool show_annotation_pins_{true};
    bool annotation_click_consumed_{};
    bool annotation_editor_hovered_{};
    int next_annotation_id_{1};
    int selected_annotation_id_{};
    std::vector<Annotation> annotations_;
    std::array<char, 4096> session_notes_{};
    std::array<char, 512> crew_chief_endpoint_{};
    std::array<char, 4096> crew_chief_setup_change_{};
    std::array<char, 4096> crew_chief_question_{};
    std::string crew_chief_bearer_token_;
    driver_analysis::ComparisonSlot crew_chief_after_slot_{driver_analysis::ComparisonSlot::CompareA};
    std::vector<crew_chief::ChatTurn> crew_chief_history_;
    std::optional<crew_chief::Report> crew_chief_report_;
    std::future<crew_chief::Response> crew_chief_future_;
    bool crew_chief_busy_{};
    std::string crew_chief_error_;
    race_day::Day race_day_;
    std::filesystem::path race_day_path_;
    int selected_race_day_run_{};
    int race_day_previous_run_{};
    int race_day_current_run_{1};
    int race_day_main_group_{};
    int race_day_main_legs_{};
    bool race_day_include_q4_{};
    bool race_day_triple_a_{};
    bool race_day_dirty_{};
    std::string race_day_question_{
        "Compare the previous run with the current run. Did the setup change improve the car, "
        "what evidence supports that, what may be a driver or condition effect, and what should I test next?"};
    std::future<crew_chief::Response> race_day_future_;
    std::optional<crew_chief::Report> race_day_report_;
    std::optional<race_day::SetupKnowledgeRecord> race_day_pending_knowledge_;
    int selected_setup_knowledge_record_{-1};
    std::size_t race_day_relevant_history_count_{};
    bool race_day_busy_{};
    std::string race_day_error_;
    driver_analysis::AnalysisRules analysis_rules_{driver_analysis::default_analysis_rules()};
    std::vector<driver_analysis::CornerZone> corner_zones_;
    driver_analysis::AnalysisResult driver_analysis_;
    imu::Analysis imu_analysis_;
    bool driver_analysis_dirty_{true};
    int selected_insight_index_{-1};
    int selected_corner_index_{-1};
    std::vector<TelemetryPlotId> telemetry_plot_order_{default_telemetry_plot_order()};
    bool build_default_layout_{};
    int loaded_layout_version_{};
    PlotData primary_plot_cache_;
    PlotData compare_plot_cache_;
    PlotData compare_b_plot_cache_;
    std::vector<double> relative_delta_progress_;
    std::vector<double> relative_delta_seconds_;
    std::vector<double> relative_delta_b_seconds_;
    std::vector<double> primary_distance_progress_;
    std::vector<double> primary_distance_elapsed_;
    std::vector<double> average_track_latitude_;
    std::vector<double> average_track_longitude_;
    int plot_cache_active_{-1};
    int plot_cache_compare_{-1};
    int plot_cache_compare_b_{-1};
    int plot_cache_continuous_begin_{-1};
    int plot_cache_continuous_end_{-1};
    ViewMode plot_cache_mode_{static_cast<ViewMode>(-1)};
    bool plot_fit_pending_{true};
    double linked_plot_x_min_{};
    double linked_plot_x_max_{1.0};
    std::vector<float> frame_times_ms_;
};

}  // namespace racebox::app
