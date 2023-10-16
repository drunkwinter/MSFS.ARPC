#define WIN32_LEAN_AND_MEAN

#pragma comment(lib, "user32.lib")

#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>

#include "Hooking.Patterns\Hooking.Patterns.h"
#include "ini_file.hpp"

static HINSTANCE dll_module;

std::atomic hotkey_pressed(false);
std::atomic ini_file_changed(false);

void show_warning(const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), "MSFS2020.ARPC", MB_OK | MB_ICONWARNING | MB_SYSTEMMODAL);
}

namespace Patch {
    using Coefficients = std::array<float, 3>;
    enum Status { SUCCESS, ERROR_PATTERN_NOT_FOUND };
    auto is_enabled = false;

    namespace {
        float* ptr_coefficients;
        Coefficients default_coefficients;
        Coefficients new_coefficients;
    }

    auto init(Coefficients custom_coefficients) {
        auto pattern = hook::pattern("C3 64 2A 3A E3 8B F6 3A 07 42 B2 38");

        if (pattern.empty()) {
            return Status::ERROR_PATTERN_NOT_FOUND;
        }

        ptr_coefficients = pattern.get_first<float>();
        std::copy(ptr_coefficients, ptr_coefficients + 3, default_coefficients.begin());
        new_coefficients = custom_coefficients;

        return Status::SUCCESS;
    }

    void update(Coefficients custom_coefficients) {
        new_coefficients = custom_coefficients;
    }

    void apply() {
        std::copy(new_coefficients.begin(), new_coefficients.end(), ptr_coefficients);
    }

    void enable() {
        std::copy(new_coefficients.begin(), new_coefficients.end(), ptr_coefficients);
        is_enabled = true;
    }

    void disable() {
        std::copy(default_coefficients.begin(), default_coefficients.end(), ptr_coefficients);
        is_enabled = false;
    }

    void toggle() {
        is_enabled ? disable() : enable();
    }
}

void listen_for_hotkey(int key, int modifiers) {
    if (RegisterHotKey(nullptr, 1, modifiers, key)) {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_HOTKEY) {
                hotkey_pressed = true;
            }
        }
    }
}

void listen_for_filechange(const std::string& ini_path) {
    const auto handle = FindFirstChangeNotificationA(".", FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);

    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    auto last_modified_time = std::filesystem::last_write_time(ini_path);

    while (WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0) {
        const auto current_modified_time = std::filesystem::last_write_time(ini_path);

        if (current_modified_time != last_modified_time) {
            last_modified_time = current_modified_time;
            ini_file_changed = true;
        }

        FindNextChangeNotification(handle);
    }
}

auto get_ini_path() {
    char module_path[MAX_PATH];
    GetModuleFileNameA(dll_module, module_path, sizeof(module_path));
    return std::filesystem::path(module_path).replace_extension(".ini").string();
}

auto get_ini_coefficients(IniFile ini) {
    const Patch::Coefficients coefficients = {
        ini.get("COEFFICIENTS", "RED", 0.002723f),
        ini.get("COEFFICIENTS", "GREEN", 0.001831f),
        ini.get("COEFFICIENTS", "BLUE", -0.000083096f),
    };
    return coefficients;
}

void init() {
    const auto ini_path = get_ini_path();

    IniFile ini(ini_path);

    const auto status = Patch::init(get_ini_coefficients(ini));

    if (status == Patch::ERROR_PATTERN_NOT_FOUND) {
        show_warning("Could not find the coefficients pattern! No changes were made." + std::string());
        return;
    }

    Patch::enable();

    const auto live_editing_enabled = ini.get("COEFFICIENTS", "LIVE_EDITING_ENABLED", false);
    const auto toggle_enabled = ini.get("TOGGLE", "ENABLED", false);

    if (live_editing_enabled) {
        std::thread file_thread(listen_for_filechange, ini_path);
        file_thread.detach();
    }

    if (toggle_enabled) {
        const auto hot_key = ini.get("TOGGLE", "KEY_CODE", 0x2D);
        const auto modifiers = MOD_NOREPEAT
            | (ini.get("TOGGLE", "HOLD_ALT", false) * MOD_ALT)
            | (ini.get("TOGGLE", "HOLD_CTRL", false) * MOD_CONTROL)
            | (ini.get("TOGGLE", "HOLD_SHIFT", false) * MOD_SHIFT);

        std::thread input_thread(listen_for_hotkey, hot_key, modifiers);
        input_thread.detach();
    }

    if (!live_editing_enabled && !toggle_enabled) {
        return;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (hotkey_pressed) {
            hotkey_pressed = false;
            Patch::toggle();
        }

        if (ini_file_changed) {
            ini_file_changed = false;

            Patch::update(get_ini_coefficients(ini));

            if (Patch::is_enabled) {
                Patch::apply();
            }
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        dll_module = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        std::thread main_thread(init);
        main_thread.detach();
    }

    return true;
}
