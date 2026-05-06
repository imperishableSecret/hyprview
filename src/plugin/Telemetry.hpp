#pragma once

#include "HyprviewConfig.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <string_view>

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprview {

    inline bool telemetryEnabled() {
        return g_hyprviewConfig.debug.telemetry;
    }

    inline std::string boolToken(bool value) {
        return value ? "1" : "0";
    }

    inline std::string formatRawPtr(const void* ptr) {
        if (!ptr)
            return "0";

        return std::format("{:x}", reinterpret_cast<uintptr_t>(ptr));
    }

    inline std::string formatVector(const Vector2D& vector) {
        return std::format("{:.2f},{:.2f}", vector.x, vector.y);
    }

    inline std::string formatBox(const CBox& box) {
        return std::format("{:.2f},{:.2f} {:.2f}x{:.2f}", box.x, box.y, box.w, box.h);
    }

    inline std::string formatRegion(const CRegion& region) {
        if (region.empty())
            return "empty";

        auto copy = region.copy();
        return formatBox(copy.getExtents());
    }

    inline void telemetryLog(std::string_view message) {
        if (!telemetryEnabled())
            return;

        static std::mutex mutex;
        std::lock_guard   lock{mutex};

        const auto        PATH = g_hyprviewConfig.debug.telemetryPath.empty() ? "/tmp/hyprview-telemetry.log" : g_hyprviewConfig.debug.telemetryPath;
        std::ofstream     out{PATH, std::ios::app};
        if (!out)
            return;

        const auto NOW    = std::chrono::system_clock::now();
        const auto MS     = std::chrono::duration_cast<std::chrono::milliseconds>(NOW.time_since_epoch()) % std::chrono::seconds{1};
        const auto TIME_T = std::chrono::system_clock::to_time_t(NOW);

        std::tm    tm = {};
        localtime_r(&TIME_T, &tm);

        out << std::put_time(&tm, "%F %T") << "." << std::setw(3) << std::setfill('0') << MS.count() << " " << message << "\n";
    }

}
