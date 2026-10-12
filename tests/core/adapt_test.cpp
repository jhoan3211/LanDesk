// Ajuste automático de calidad: detección de sobrecarga y escalones.

#include <gtest/gtest.h>

#include "lancast/core/adapt.hpp"

using namespace lancast;

TEST(Adapt, DetectsOverload) {
    // Codificador en cadena: 8 ms por cuadro a 165 fps (6,06 ms entre cuadros) sin saltos NO es sobrecarga
    // (antes bajaba la calidad sin ganar latencia).
    EXPECT_FALSE(adapt::overloaded(8.1, 1000.0 / 165, 0, 160));
    EXPECT_FALSE(adapt::overloaded(10.7, 1000.0 / 60, 0, 60));
    // Los cuadros se acumulan (latencia > 2,5 intervalos): sobrecarga.
    EXPECT_TRUE(adapt::overloaded(16.0, 1000.0 / 165, 0, 120));
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

TEST(Adapt, RestoresFpsThenResolution) {
    // Recortado a 720p (escala 0,5) y 60 fps de 165 pedidos: primero vuelven los FPS, después la resolución.
    adapt::Level l{0.5, 60};
    auto r = adapt::restore_step(l, 165);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->fps_cap, 81u);
    EXPECT_DOUBLE_EQ(r->scale, 0.5);
    for (int i = 0; i < 10 && r->fps_cap != 0; ++i) r = adapt::restore_step(*r, 165);
    EXPECT_EQ(r->fps_cap, 0u) << "Llega a los FPS pedidos";
    r = adapt::restore_step(*r, 165);
    EXPECT_NEAR(r->scale, 0.625, 1e-9);
    for (int i = 0; i < 10 && r->scale < 1.0; ++i) r = adapt::restore_step(*r, 165);
    EXPECT_DOUBLE_EQ(r->scale, 1.0);
    EXPECT_FALSE(adapt::restore_step(*r, 165).has_value()) << "Ya en la calidad pedida";
}

TEST(Adapt, CalmWindows) {
    EXPECT_TRUE(adapt::calm(7.0, 1000.0 / 165, 0, 160));    // Codificador en cadena, sin saltos.
    EXPECT_TRUE(adapt::calm(0, 1000.0 / 60, 0, 2));          // Pantalla quieta.
    EXPECT_FALSE(adapt::calm(7.0, 1000.0 / 165, 3.0, 160));  // Saltando cuadros.
}
