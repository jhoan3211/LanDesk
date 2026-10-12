// Ajuste automático de calidad: detección de sobrecarga y escalones.

#include <gtest/gtest.h>

#include "lancast/core/adapt.hpp"

using namespace lancast;

TEST(Adapt, DetectsOverload) {
    // 1440p a 165 fps (6,06 ms entre cuadros) con 10,7 ms por cuadro: sobrecarga.
    EXPECT_TRUE(adapt::overloaded(10.7, 1000.0 / 165, 0, 120));
    // Mismo tiempo a 60 fps (16,7 ms): da abasto.
    EXPECT_FALSE(adapt::overloaded(10.7, 1000.0 / 60, 0, 60));
    // Cuadros saltados por un codificador ocupado.
    EXPECT_TRUE(adapt::overloaded(5.0, 1000.0 / 60, 25.0, 45));
    // Pantalla quieta (pocos cuadros): no se juzga.
    EXPECT_FALSE(adapt::overloaded(30.0, 1000.0 / 60, 0, 3));
}

TEST(Adapt, StepsDownResolutionThenFps) {
    // 1440p: baja 20 % (1152 de alto).
    auto s = adapt::next_step({2560, 1440}, 165, 1.0);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->scale, 0.8, 1e-9);
    EXPECT_EQ(s->fps, 0u);
    // Cerca de 720p: baja justo hasta 720.
    s = adapt::next_step({1280, 800}, 165, 0.5);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(s->scale, 0.5 * 720.0 / 800.0, 1e-9);
    // En 720p: ahora los FPS (165 -> 123 -> ... -> 60 -> 45 -> 33 -> 30).
    s = adapt::next_step({1280, 720}, 165, 0.5);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->fps, 123u);
    EXPECT_EQ(adapt::next_step({1280, 720}, 70, 0.5)->fps, 60u);
    EXPECT_EQ(adapt::next_step({1280, 720}, 60, 0.5)->fps, 45u);
    EXPECT_EQ(adapt::next_step({1280, 720}, 40, 0.5)->fps, 30u);
    // Mínimo alcanzado.
    EXPECT_FALSE(adapt::next_step({1280, 720}, 30, 0.5).has_value());
}
