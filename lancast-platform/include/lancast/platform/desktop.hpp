#pragma once

// Seguir al escritorio de entrada: cuando aparece la pantalla de bloqueo o un aviso de UAC, Windows cambia al
// escritorio "Winlogon". Un proceso SYSTEM puede capturarlo y mandarle entrada si el hilo se cambia a él.
// Para un proceso de usuario normal OpenInputDesktop falla y estas funciones no hacen nada.

#include <string>

namespace lancast::desk {

// Cambia el hilo actual al escritorio de entrada si es distinto del suyo. true si cambió.
// (El hilo no debe tener ventanas ni hooks propios.)
bool attach_to_input_desktop();

// Nombre del escritorio de entrada ("Default", "Winlogon"...) o vacío si no se puede abrir.
[[nodiscard]] std::wstring input_desktop_name();

}  // namespace lancast::desk
