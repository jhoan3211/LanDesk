# LanCast

Escritorio remoto y streaming de pantalla **solo para LAN**, con conexión por IP, latencia mínima (objetivo: < 10 ms de extremo a extremo por cable) y transferencia de archivos entre PC y celular.

Implementado en **C++23** sobre Windows 10/11 (DXGI, Direct3D 11, Media Foundation, FFmpeg).

> Estado: **fase 1 completada** (base del proyecto + inventario de hardware). Ver la [hoja de ruta](#hoja-de-ruta).

## Requisitos

| Herramienta | Versión |
|---|---|
| Windows | 10 (1809+) u 11, x64 |
| Visual Studio 2022 Build Tools | 17.14 o superior, workload "Desarrollo de escritorio con C++" (MSVC v143, SDK de Windows 11, CMake) |
| CMake | 3.25 o superior (viene incluido en las Build Tools) |
| Git | cualquiera reciente |

Instalación rápida con `winget`:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

## Compilación

Desde una consola en la raíz del repositorio (si `cmake` no está en el `PATH`, usa la *Developer PowerShell for VS 2022*):

```powershell
cmake --preset vs2022            # configura (descarga GoogleTest la primera vez)
cmake --build --preset release   # o --preset debug
ctest --preset release           # ejecuta las pruebas
```

Los ejecutables quedan en `build\lancast-host\Release\lancast-host.exe` y `build\lancast-client\Release\lancast-client.exe`.

## Uso (fase 1)

```powershell
lancast-host --list              # GPUs, monitores, codificadores y decodificadores
lancast-host --list --verbose    # + LUIDs de los adaptadores y perfiles DXVA no reconocidos
lancast-host --list --json       # salida en JSON
lancast-client --list            # lo mismo, visto desde el cliente
```

Ejemplo de salida:

```
== Monitores ==
  [1] LG ULTRAGEAR  (principal)  \\.\DISPLAY1
      Modo actual: 2560x1440 @ 165.00 Hz   Nativa: 2560x1440
      Posición: (0, 0)   Escala: 125% (120 DPI)   Rotación: 0°
      Color: 10 bits   HDR: soportado (desactivado)   Conectado a: GPU 0 (AMD Radeon(TM) Graphics)
  [Todos] Escritorio virtual: 2560x1440 en (0, 0)

== Codificadores por hardware (host) ==
  h264_amf     H.264  AMF        GPU 0 (AMD Radeon(TM) Graphics)  [MFT: AMDh264Encoder]  runtime amfrt64.dll: OK
  hevc_amf     HEVC   AMF        GPU 0 (AMD Radeon(TM) Graphics)  [MFT: AMDh265Encoder]  runtime amfrt64.dll: OK
```

Qué detecta:

- **GPUs**: ordenadas por preferencia de rendimiento (`IDXGIFactory6::EnumAdapterByGpuPreference`), con fabricante, VRAM y LUID.
- **Monitores**: `EnumDisplayMonitors` + `DXGI_OUTPUT_DESC1` + `QueryDisplayConfig`. Nombre comercial, modo actual y nativo, Hz exactos (racional), posición en el escritorio virtual (incluidas **coordenadas negativas**), DPI y escala por monitor, rotación, bits de color y HDR. El proceso es **Per-Monitor DPI Aware v2** (manifiesto + API). Los monitores se numeran de izquierda a derecha: el número coincide con el atajo `Ctrl+Alt+Shift+N`.
- **Codificadores**: MFT de hardware expuestos por el driver de cada GPU (`MFTEnum2` filtrado por LUID), traducidos al codificador de FFmpeg (`h264_nvenc`, `hevc_amf`, `av1_qsv`…), más la comprobación de la DLL de runtime (`nvEncodeAPI64.dll`, `amfrt64.dll`). La validación definitiva (abrir el codificador con entrada D3D11) llega en la fase 2.
- **Decodificadores**: perfiles D3D11VA por GPU (H.264, HEVC Main/Main10/4:4:4, AV1) con la resolución máxima comprobada (1080p → 8K).
- **Bitrate automático** de referencia (ver `lancast-core/src/bitrate.cpp`).

## Estructura

```
CMakeLists.txt            Proyecto raíz (C++23, MSVC, /W4 /WX)
CMakePresets.json         Presets vs2022 / debug / release
cmake/                    Advertencias del compilador y manifiesto (DPI PMv2, UTF-8)
lancast-core/             Tipos compartidos, bitrate automático, JSON, formato de reportes (sin dependencias de Windows)
lancast-platform/         Capa Windows compartida por host y cliente: DXGI, D3D11, Media Foundation, DisplayConfig
lancast-host/             Ejecutable del host (captura, codificación, servidor; hoy: --list)
lancast-client/           Ejecutable del cliente (decodificación, presentación, entrada, UI; hoy: --list)
lancast-web/              Cliente web estático para el celular (fase 8)
tests/                    GoogleTest: core (unitarias) y platform (integración con el hardware real, etiqueta "hardware")
```

`lancast-platform` es un módulo extra respecto al plan original: host y cliente necesitan la misma enumeración de GPU/monitores, y así `lancast-core` sigue siendo portable (web/Android).

## Dependencias

| Fase | Dependencia | Versión | Uso / motivo |
|---|---|---|---|
| 1 | Windows SDK (DXGI, D3D11, Media Foundation, Shcore) | 10.0.22621+ | Enumeración de hardware; viene con las Build Tools |
| 1 | GoogleTest | v1.18.0 | Pruebas (descargado con `FetchContent`) |
| 2 | FFmpeg (build LGPL *shared* de BtbN) | n8.1.3 | `*_nvenc`, `*_amf`, `*_qsv` con `hwframes` D3D11 y decodificación `d3d11va` |
| 2 | C++/WinRT | 3.0.260818.1 (incluido en el SDK) | `Windows.Graphics.Capture` |
| 2 | spdlog | v1.17.0 | Logging |
| 3 | Reed-Solomon propio (Cauchy sobre GF(2⁸)) | — | FEC sin dependencias y con control total de la latencia (diseño inspirado en nanors, MIT) |
| 4 | Dear ImGui (backend D3D11 + Win32) | v1.92.9b | Interfaz de conexión y configuración (equivalente a egui) |
| 4 | toml++ | v3.4.0 | Configuración y perfiles en `%APPDATA%\LanCast` |
| 5 | libopus | v1.5.2 | Audio (WASAPI loopback, 48 kHz) |
| 6 | OpenSSL (LTS) | 3.5.9 | TLS 1.3 para control y archivos (SChannel no ofrece TLS 1.3 en Windows 10) y AES-GCM para el stream |
| 6 | DNS-SD nativo de Windows (`DnsServiceRegister`/`DnsServiceBrowse`) | Windows 10 1809+ | mDNS `_lancast._udp.local` sin dependencias |
| 7 | BLAKE3 (implementación C oficial) | 1.8.7 | Verificación de archivos |
| 8 | cpp-httplib | v0.60.1 | Servidor HTTPS del cliente web |
| 8 | libdatachannel | v0.24.6 | WebRTC (H.264) hacia el navegador del celular |
| 9 | WiX Toolset | v7.0.0 | Instalador MSI |

Las dependencias de fases futuras se fijarán con `FetchContent` (o binarios verificados) en la fase en la que se integren.

## Decisiones técnicas (criterio: menor latencia)

- **C++23 + MSVC**: acceso directo a D3D11/DXGI/FFmpeg sin capas de bindings; `std::expected` para errores sin excepciones en el camino caliente.
- **Detección de codificadores por MFT de hardware**: no necesita FFmpeg ni crear un dispositivo de codificación, es instantánea y refleja lo que el driver realmente expone por GPU.
- **Serialización binaria propia** (structs little-endian con versión) en lugar de un formato genérico: cero asignaciones y tamaño mínimo por paquete.
- **UDP con Winsock directo** (posible RIO más adelante): menos copias y syscalls que una capa asíncrona genérica.

## Solución de problemas

- **`cmake` no se reconoce**: abre la *Developer PowerShell for VS 2022* o añade al `PATH` `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- **No aparece ningún codificador**: actualiza el driver de la GPU. En portátiles híbridos el monitor interno suele estar conectado a la iGPU; LanCast lo indica en "Conectado a".
- **Aviso "falta nvEncodeAPI64.dll / amfrt64.dll"**: el driver está incompleto; reinstálalo (instalación limpia).
- **HDR "soportado (desactivado)"**: actívalo en Configuración → Pantalla → Usar HDR si quieres transmitir 10 bits HDR.
- **Las pruebas de plataforma fallan en una sesión sin pantalla** (RDP minimizado, servicio): se ejecutan contra el hardware real; usa `ctest --preset release -LE hardware` para excluirlas.

## Hoja de ruta

1. ✅ **Base**: estructura, enumeración de monitores y GPU, detección de codificadores/decodificadores, `lancast-host --list`.
2. Captura (WGC + fallback DXGI Desktop Duplication) + escalado/conversión en GPU + codificación a archivo `.h265`/`.ivf`.
3. Red UDP con FEC, cliente con D3D11VA y swapchain flip-model, conexión manual por IP, overlay de estadísticas.
4. Interfaz de configuración, perfiles, renegociación en vivo, multi-monitor.
5. Entrada, audio y portapapeles.
6. Seguridad: emparejamiento por PIN, TLS 1.3, cifrado del stream, mDNS.
7. Transferencia de archivos.
8. Cliente web para celular (WebRTC + archivos).
9. Empaquetado: MSI, servicio de Windows, bandeja del sistema.
10. *(Opcional)* App Android para compartir la pantalla del celular.
