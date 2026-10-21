// Calibración del codificador: mide la GPU real (4:2:0 y 4:4:4 a 2560x1440) y guarda/lee el resultado por tarjeta y
// versión del driver.

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>

#include "lancast/platform/calibration.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/platform.hpp"

using namespace lancast;

TEST(Calibration, MeasuresEncoderAndCachesByGpuAndDriver) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mf::MfScope mf;
    const auto dir = std::filesystem::temp_directory_path() / L"lancast-calib-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());

    auto dev = gpu::create_device(std::nullopt);
    ASSERT_TRUE(dev.has_value());
    const std::string key = calib::gpu_key(dev->luid);
    EXPECT_FALSE(key.empty()) << "Clave de la GPU (fabricante:modelo:driver)";

    std::atomic<bool> cancel{false};
    const auto t0 = std::chrono::steady_clock::now();
    auto r = calib::measure(dev->luid, Size{2560, 1440}, cancel);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r.has_value()) << r.error().message();
    std::printf("  GPU %s: %s 2560x1440 %.2f ms en 4:2:0, %.2f ms en 4:4:4 (medido en %.1f s)\n", key.c_str(),
                std::string{to_string(r->codec)}.c_str(), r->ms_420, r->ms_444, secs);
    EXPECT_GT(r->ms_420, 0.3);
    EXPECT_LT(r->ms_420, 100.0);
    if (r->ms_444 > 0) EXPECT_GT(r->ms_444, r->ms_420) << "4:4:4 empaquetado codifica el doble de píxeles";
    EXPECT_LT(secs, 8.0) << "La calibración debe ser corta";

    calib::save(*r);
    const auto back = calib::load(key, Size{2560, 1440});
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->ms_420, r->ms_420, 0.01);
    EXPECT_FALSE(calib::load(key, Size{1920, 1080}).has_value()) << "Otra resolución: hay que medir de nuevo";
    EXPECT_FALSE(calib::load(key + "x", Size{2560, 1440}).has_value()) << "Otro driver: hay que medir de nuevo";

    // Cancelada (p. ej. empezó una sesión): no devuelve un resultado a medias.
    cancel = true;
    EXPECT_FALSE(calib::measure(dev->luid, Size{2560, 1440}, cancel).has_value());
    std::filesystem::remove_all(dir);
}
