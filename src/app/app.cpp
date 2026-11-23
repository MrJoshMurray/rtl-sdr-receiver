/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "audio_output.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "screenshot_service.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "sdr/rtl_receiver.h"
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "rtl-sdr-receiver.conf"
#endif

namespace app {
namespace {

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

struct DarkModePersistence {
    std::string config_path;
    bool last_dark_mode;
    ApplicationConfig settings;
};

void save_settings(DarkModePersistence& persistence) {
    persistence.settings.dark_mode = persistence.last_dark_mode;
    std::string error;
    if (!save_application_config(persistence.config_path, persistence.settings, error)) {
        LOG_WARN("failed to save config {}: {}", persistence.config_path, error);
    }
}

class SimulatedAudioWorker {
public:
    ~SimulatedAudioWorker() {
        stop();
    }

    bool start(std::string& error) {
        if (!output_.open("", 48000, error)) {
            return false;
        }
        running_.store(true);
        worker_ = std::thread(&SimulatedAudioWorker::run, this);
        return true;
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
        output_.close();
    }

    void request_band_edge_tone() {
        output_.request_band_edge_tone();
    }

private:
    void run() {
        constexpr size_t kFramesPerWrite = 960;
        constexpr uint32_t kSampleRateHz = 48000;
        constexpr float kToneHz = 700.0f;
        constexpr float kPi = 3.14159265358979323846f;
        std::array<int16_t, kFramesPerWrite> samples{};
        uint64_t sample_index = 0;

        while (running_.load()) {
            for (size_t index = 0; index < samples.size(); ++index) {
                const float phase = 2.0f * kPi * kToneHz
                                  * static_cast<float>(sample_index + index)
                                  / static_cast<float>(kSampleRateHz);
                samples[index] = static_cast<int16_t>(std::lrint(std::sin(phase) * 4915.0f));
            }

            std::string error;
            if (!output_.write(samples.data(), samples.size(), error)) {
                LOG_WARN("simulated audio output stopped: {}", error);
                running_.store(false);
                break;
            }
            sample_index += samples.size();
#if USE_DESKTOP
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
        }
    }

    platform::audio::AudioOutput output_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

std::string writable_config_path() {
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "rtl-sdr-receiver" / "rtl-sdr-receiver.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "rtl-sdr-receiver" / "rtl-sdr-receiver.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

void persist_dark_mode_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<DarkModePersistence*>(lv_observer_get_user_data(observer));
    if (!persistence) {
        return;
    }
    const bool dark_mode = lv_subject_get_int(subject) != 0;
    if (dark_mode == persistence->last_dark_mode) {
        return;
    }
    persistence->last_dark_mode = dark_mode;

    save_settings(*persistence);
    LOG_INFO("saved config: {} (dark_mode={})",
             persistence->config_path,
             dark_mode ? "yes" : "no");
}

class ShortcutController {
public:
    ShortcutController(viewmodel::BaseViewModel& view_model, AssetManager& assets)
        : view_model_(view_model),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)),
                    help_popup_(std::make_unique<view::widgets::HelpPopup>(lv_layer_top(), view_model, assets)),
                    volume_overlay_(std::make_unique<view::widgets::VolumeOverlay>(lv_layer_top(), assets)) {
        toast_->build();
        help_popup_->build();
                volume_overlay_->build();
        tune_repeat_timer_ = lv_timer_create(tune_repeat_timer_cb, 120, this);
        lv_timer_pause(tune_repeat_timer_);
        volume_error_timer_ = lv_timer_create(volume_error_timer_cb, 250, this);
        platform::set_global_key_listener(global_key_listener, this);
        platform::set_key_release_listener(key_release_listener, this);
    }

    ~ShortcutController() {
        if (volume_error_timer_) {
            lv_timer_delete(volume_error_timer_);
        }
        if (tune_repeat_timer_) {
            lv_timer_delete(tune_repeat_timer_);
            tune_repeat_timer_ = nullptr;
        }
        platform::clear_key_release_listener(key_release_listener, this);
        platform::clear_global_key_listener(global_key_listener, this);
    }

    void set_band_edge_handler(std::function<void()> handler) {
        band_edge_handler_ = std::move(handler);
    }

    void show_warning(const std::string& text) {
        toast_->show(text, view::widgets::ToastTone::Warning, 2500);
    }

private:
    void tune(int direction) {
        if (view_model_.tune_frequency(direction) && band_edge_handler_) {
            band_edge_handler_();
        }
    }

    static void volume_error_timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<ShortcutController*>(lv_timer_get_user_data(timer));
        if (!controller) return;
        platform::audio::VolumeState state;
        if (controller->system_volume_.take_state(state) && !state.silent) {
            controller->user_muted_ = state.muted;
            controller->volume_overlay_->show(state.percent, state.muted);
        }
        std::string error;
        if (controller->system_volume_.take_error(error)) {
            LOG_WARN("system audio control failed: {}", error);
            controller->toast_->show("Audio control unavailable",
                                     view::widgets::ToastTone::Error,
                                     3000);
        }
    }

    static void tune_repeat_timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<ShortcutController*>(lv_timer_get_user_data(timer));
        if (!controller || controller->tune_repeat_key_ == 0
            || controller->view_model_.current_page() != model::AppPage::Apple) {
            if (controller) {
                lv_timer_pause(timer);
            }
            return;
        }

        if (controller->tune_repeat_delay_ticks_ > 0) {
            --controller->tune_repeat_delay_ticks_;
            return;
        }

        const bool left = controller->tune_repeat_key_ == LV_KEY_LEFT
                       || controller->tune_repeat_key_ == 'z'
                       || controller->tune_repeat_key_ == 'Z';
        controller->tune(left ? -1 : 1);
    }

    void start_tune_repeat(uint32_t key) {
        tune_repeat_key_ = key;
        tune_repeat_delay_ticks_ = 3;
        if (tune_repeat_timer_) {
            lv_timer_reset(tune_repeat_timer_);
            lv_timer_resume(tune_repeat_timer_);
        }
    }

    void stop_tune_repeat(uint32_t key) {
        if (key == tune_repeat_key_) {
            tune_repeat_key_ = 0;
            tune_repeat_delay_ticks_ = 0;
            if (tune_repeat_timer_) {
                lv_timer_pause(tune_repeat_timer_);
            }
        }
    }

    bool handle_key(uint32_t key, bool long_pressed) {
        if (key == platform::kKeyMute || key == platform::kKeyVolumeDown
            || key == platform::kKeyVolumeUp) {
            if (!long_pressed) {
                const auto action = key == platform::kKeyMute
                                  ? platform::audio::VolumeAction::ToggleMute
                                  : key == platform::kKeyVolumeDown
                                  ? platform::audio::VolumeAction::Down
                                  : platform::audio::VolumeAction::Up;
                if (!system_volume_.request(action)) {
                    toast_->show("Audio control unavailable", view::widgets::ToastTone::Error, 3000);
                }
            }
            return true;
        }

        if (key == platform::kKeyPrintScreen) {
            if (!long_pressed) {
                const auto result = platform::screenshot::capture_active_screen();
                if (result.success) {
                    LOG_INFO("screenshot saved: {}", result.path);
                    toast_->show_highlighted("Saved to ",
                                             result.path,
                                             "",
                                             view::widgets::ToastTone::Success,
                                             5200,
                                             true);
                }
                else {
                    LOG_WARN("screenshot failed: {}", result.error);
                    toast_->show("Screenshot failed: " + result.error,
                                 view::widgets::ToastTone::Error,
                                 3200);
                }
            }
            return true;
        }

        if (key == platform::kKeyHelp) {
            if (!long_pressed) {
                toast_->hide();
                if (help_popup_->visible()) {
                    help_popup_->hide();
                }
                else {
                    help_popup_->show(view_model_.current_page());
                }
            }
            return true;
        }

        if (help_popup_->visible()) {
            if (key == LV_KEY_ESC && !long_pressed) {
                help_popup_->hide();
            }
            return true;
        }

        if (!long_pressed) {
            if (key == 'u' || key == 'U') {
                view_model_.set_dark_mode(true);
                return true;
            }
            if (key == 'i' || key == 'I') {
                view_model_.set_dark_mode(false);
                return true;
            }
            const auto page = view_model_.current_page();
            const bool left = key == LV_KEY_LEFT || key == 'z' || key == 'Z';
            const bool right = key == LV_KEY_RIGHT || key == 'c' || key == 'C';

            if (page == model::AppPage::Apple) {
                if (left || right) {
                    tune(left ? -1 : 1);
                    start_tune_repeat(key);
                    return true;
                }
                if (key == 'n' || key == 'N') {
                    view_model_.toggle_auto_notch();
                    return true;
                }
                if (key == 'q' || key == 'Q') {
                    const int level = view_model_.cycle_squelch(1);
                    char text[24]{};
                    if (level == 0) std::snprintf(text, sizeof(text), "Squelch off");
                    else std::snprintf(text, sizeof(text), "Squelch %d dB", level);
                    toast_->show(text, view::widgets::ToastTone::Default, 1200);
                    return true;
                }
                if (key == 'r' || key == 'R') {
                    view_model_.toggle_rit();
                    return true;
                }
                if (view_model_.rit_active()) {
                    const bool up = key == LV_KEY_UP || key == 'f' || key == 'F';
                    const bool down = key == LV_KEY_DOWN || key == 'x' || key == 'X';
                    if (up || down) {
                        view_model_.adjust_rit(up ? 1 : -1);
                        return true;
                    }
                }
            }
            else {
                if (left || right) {
                    view_model_.adjust_setting(left ? -1 : 1);
                    return true;
                }
                const bool up = key == LV_KEY_UP || key == 'f' || key == 'F';
                const bool down = key == LV_KEY_DOWN || key == 'x' || key == 'X';
                if (up || down) {
                    view_model_.select_setting(up ? -1 : 1);
                    return true;
                }
                if ((key >= '0' && key <= '9') || key == '.' || key == ',' || key == 'b' || key == 'B') {
                    const bool point = key == ',' || key == 'b' || key == 'B';
                    view_model_.entry_append(point ? '.' : static_cast<char>(key));
                    return true;
                }
                if (key == LV_KEY_BACKSPACE) {
                    view_model_.entry_backspace();
                    return true;
                }
                if (key == LV_KEY_ENTER) {
                    if (view_model_.entry_text_empty()) {
                        view_model_.show_apple_page();
                    } else if (!view_model_.entry_commit()) {
                        toast_->show("Out of range (0.1 - 1766 MHz)",
                                     view::widgets::ToastTone::Warning, 2500);
                    }
                    return true;
                }
            }
        }

        const bool is_escape = key == LV_KEY_ESC;
        if (!is_escape) {
            return false;
        }

        if (long_pressed) {
            if (exit_key_pressed_ == key && exit_armed_) {
                toast_->hide();
                view_model_.request_quit();
            }
        }
        else {
            exit_key_pressed_ = key;
            exit_armed_ = view_model_.current_page() == model::AppPage::Apple;
            if (exit_armed_) {
                toast_->show_persistent_highlighted("Hold ",
                                                    "ESC",
                                                    " to exit",
                                                    view::widgets::ToastTone::Warning);
            }
        }
        return true;
    }

    void handle_release(uint32_t key) {
        stop_tune_repeat(key);
        if (key != exit_key_pressed_) {
            return;
        }

        if (exit_armed_) {
            toast_->hide();
        } else if (key == LV_KEY_ESC &&
                   view_model_.current_page() == model::AppPage::Butter) {
            view_model_.show_apple_page();
        }
        exit_key_pressed_ = 0;
        exit_armed_ = false;
    }

    static bool global_key_listener(uint32_t key,
                                    const char*,
                                    bool long_pressed,
                                    void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        return controller && controller->handle_key(key, long_pressed);
    }

    static void key_release_listener(uint32_t key, const char*, void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        if (controller) {
            controller->handle_release(key);
        }
    }

    viewmodel::BaseViewModel& view_model_;
    std::function<void()> band_edge_handler_;
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::unique_ptr<view::widgets::VolumeOverlay> volume_overlay_;
    platform::audio::SystemVolumeControl system_volume_;
    lv_timer_t* volume_error_timer_{nullptr};
    bool user_muted_{false};
    lv_timer_t* tune_repeat_timer_{nullptr};
    uint32_t tune_repeat_key_{0};
    uint8_t tune_repeat_delay_ticks_{0};
    uint32_t exit_key_pressed_{0};
    bool exit_armed_{false};
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("template");

#if !USE_DESKTOP
    if (setenv("RTL_SDR_LIVE_RADIO", "1", 0) != 0) {
        LOG_ERROR("failed to enable live radio mode");
        return 1;
    }
#endif

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    viewmodel::BaseViewModel view_model;
    const std::string user_config_path = writable_config_path();
    std::string loaded_config_path = APP_CONFIG_FILE;
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error)) {
            loaded_config_path = user_config_path;
        }
    }

    ApplicationConfig config;
    std::string config_error;
    if (load_application_config(loaded_config_path, config, config_error)) {
        view_model.set_dark_mode(config.dark_mode);
        LOG_INFO("loaded config: {} (dark_mode={})",
                 loaded_config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to load config {}: {}; using defaults", loaded_config_path, config_error);
    }

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    auto* standard_font = assets.load_standard_font(14);
    view::apply_lvgl_theme(display,
                           view_model.is_dark_mode(),
                           standard_font ? standard_font : LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    ScreenManager screen_manager(view_model, assets);
    screen_manager.start();
    ShortcutController shortcuts(view_model, assets);
    view::widgets::RadioDiscoveryNotice discovery_notice(lv_layer_top(), view_model, assets);
    discovery_notice.build();

    DarkModePersistence dark_mode_persistence{user_config_path, view_model.is_dark_mode(), {}};
    auto* dark_mode_observer = lv_subject_add_observer(view_model.dark_mode_subject(),
                                                        persist_dark_mode_observer,
                                                        &dark_mode_persistence);

    bool running = true;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);

    std::unique_ptr<SimulatedAudioWorker> simulated_audio;
    std::unique_ptr<platform::sdr::RtlReceiver> radio_audio;
    if (std::getenv("RTL_SDR_LIVE_RADIO")) {
        radio_audio = std::make_unique<platform::sdr::RtlReceiver>();
        radio_audio->set_frequency(view_model.frequency_hz());
        radio_audio->set_receive_settings(view_model.radio_mode(), view_model.filter_bandwidth_hz());
        radio_audio->set_squelch_threshold_db(view_model.squelch_threshold_db());
        radio_audio->start();
        LOG_INFO("RTL-SDR receive mode enabled");
    } else {
        simulated_audio = std::make_unique<SimulatedAudioWorker>();
        std::string audio_error;
        if (simulated_audio->start(audio_error)) {
            LOG_INFO("simulated audio output opened at 48 kHz");
        } else {
            LOG_WARN("simulated audio output unavailable: {}", audio_error);
        }
    }

    shortcuts.set_band_edge_handler([&] {
        if (radio_audio) radio_audio->request_band_edge_tone();
        if (simulated_audio) simulated_audio->request_band_edge_tone();
    });

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        if (radio_audio) {
            radio_audio->set_auto_notch(view_model.auto_notch());
            radio_audio->set_rit_hz(view_model.rit_hz());
            radio_audio->set_squelch_threshold_db(view_model.squelch_threshold_db());
            radio_audio->set_frequency(view_model.frequency_hz());
            radio_audio->set_receive_settings(view_model.radio_mode(), view_model.filter_bandwidth_hz());
            discovery_notice.update(radio_audio->discovery_state());
            view_model.set_radio_connection_state(radio_audio->status());
            view_model.set_radio_measurements(radio_audio->measurements());
        } else {
            view_model.set_radio_connection_state(model::RadioConnectionState::Disconnected);
        }
        lv_timer_handler();
        lv_delay_ms(5);
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    if (dark_mode_observer) {
        lv_observer_remove(dark_mode_observer);
    }

    return 0;
}

} // namespace app
