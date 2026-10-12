// Mando virtual (ViGEmBus): si el driver está instalado, el mando creado debe verse por XInput con el estado enviado.

#include <gtest/gtest.h>

#include <windows.h>
#include <xinput.h>

#include "lancast/platform/gamepad.hpp"

#pragma comment(lib, "xinput.lib")

using namespace lancast;

TEST(Gamepad, VirtualPadIsSeenByXInput) {
    if (!gamepad::driver_available()) GTEST_SKIP() << "Driver ViGEmBus no instalado";
    bool before[4]{};
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        before[i] = XInputGetState(i, &st) == ERROR_SUCCESS;
    }
    gamepad::VirtualPads pads;
    proto::GamepadState g;
    g.buttons = 0x1000;  // A
    g.right_trigger = 255;
    g.left_x = 12345;
    pads.update(g);
    bool seen = false;
    for (int attempt = 0; attempt < 40 && !seen; ++attempt) {
        Sleep(50);
        pads.update(g);
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE st{};
            if (before[i] || XInputGetState(i, &st) != ERROR_SUCCESS) continue;
            if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0 && st.Gamepad.bRightTrigger == 255 && st.Gamepad.sThumbLX == 12345) seen = true;
        }
    }
    EXPECT_TRUE(seen) << "El mando virtual no apareció en XInput";
    g.connected = false;
    pads.update(g);
}
