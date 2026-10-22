// Firma digital y registro de volcados de memoria.

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "lancast/platform/crash.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/signature.hpp"

namespace fs = std::filesystem;

TEST(Signature, UnsignedExecutableIsReported) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const lancast::sig::Info info = lancast::sig::verify(self);  // Las pruebas no se firman.
    EXPECT_FALSE(info.valid);
    EXPECT_EQ(info.error, "sin firma");
    EXPECT_TRUE(info.subject.empty());

    EXPECT_FALSE(lancast::sig::verify(fs::temp_directory_path() / L"no-existe-lancast.exe").valid);
}

TEST(CrashDumps, ListsNewestFirstAndRemembersSeen) {
    const fs::path dir = fs::temp_directory_path() / L"lancast-crash-list-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    EXPECT_FALSE(lancast::crash::unseen(dir).has_value());

    std::ofstream(dir / L"a.dmp") << "x";
    fs::last_write_time(dir / L"a.dmp", fs::file_time_type::clock::now() - std::chrono::hours(1));
    std::ofstream(dir / L"b.dmp") << "y";
    std::ofstream(dir / L"notas.txt") << "z";

    const auto files = lancast::crash::list(dir);
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].filename(), L"b.dmp");

    ASSERT_TRUE(lancast::crash::unseen(dir).has_value());
    lancast::crash::mark_seen(dir);
    EXPECT_FALSE(lancast::crash::unseen(dir).has_value());

    std::ofstream(dir / L"c.dmp") << "w";  // Un cierre nuevo vuelve a avisar.
    fs::last_write_time(dir / L"c.dmp", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    EXPECT_EQ(lancast::crash::unseen(dir)->filename(), L"c.dmp");
    fs::remove_all(dir);
}

TEST(WakeOnLan, RejectsInvalidMacsAndSendsValidOnes) {
    lancast::net::WinsockScope winsock;
    EXPECT_FALSE(lancast::net::wake_on_lan(""));
    EXPECT_FALSE(lancast::net::wake_on_lan("00:00:00:00:00:00"));
    EXPECT_FALSE(lancast::net::wake_on_lan("zz:11:22:33:44:55"));
    EXPECT_FALSE(lancast::net::wake_on_lan("11:22:33:44:55"));
    // Una MAC de documentación: el paquete sale por broadcast (nadie la tiene).
    EXPECT_TRUE(lancast::net::wake_on_lan("02:00:5e:10:00:01"));
    EXPECT_TRUE(lancast::net::wake_on_lan("02-00-5E-10-00-01", 0x7F000001));
}
