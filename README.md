# LanCast

Escritorio remoto y streaming de pantalla **para tu red local (LAN)**: ve y controla otro PC con latencia mínima, con audio, portapapeles y transferencia de archivos. Un único `LanCast.exe` portátil (sin instalador, sin DLL de terceros) que funciona a la vez como **host** (equipo compartido) y como **cliente** (equipo que se conecta).

## Instalación (en cada PC)

1. Descarga **`LanCast.exe`** desde la página de *Releases* del repositorio: <https://github.com/jhoan3211/LanDesk/releases>.
2. Crea una carpeta, por ejemplo `C:\LanCast`, y copia ahí `LanCast.exe`. No necesita instalación.
3. Ábrelo con doble clic.
   - Si aparece **"Windows protegió su PC"** (SmartScreen, porque el archivo no está firmado): *Más información* → *Ejecutar de todas formas*.
   - Si aparece el aviso del **Firewall de Windows**: marca **Redes privadas** y pulsa *Permitir acceso*.
     Si no apareció o lo cerraste, en LanCast ve a *Ajustes* → **Crear reglas del firewall (perfil Privado)**.
4. Comprueba que la red esté como **Privada**: *Configuración* → *Red e Internet* → *Ethernet* → *Tipo de perfil de red: Privada*.
5. (Opcional) *Ajustes* → **Iniciar con Windows** para que el equipo siempre acepte conexiones (arranca minimizado en la bandeja).

Requisitos: Windows 10 (1809+) u 11 de 64 bits y una GPU con codificación/decodificación por hardware (NVIDIA, AMD o Intel) con el driver actualizado. Para HEVC en el equipo cliente, Windows necesita la extensión **"Extensiones de video HEVC"** de Microsoft Store; si no está, LanCast usa H.264 automáticamente.

## Primer uso: conectar un PC a otro

En el **PC que vas a controlar** (host), por ejemplo `192.168.40.7`:

1. Abre LanCast. En *Este equipo (host)* debe decir **"Esperando conexiones en: 192.168.40.7"**.

En el **PC desde el que controlas** (cliente):

1. Abre LanCast → *Conectar a un equipo*.
2. El host aparece solo en **"Equipos encontrados en la red"**; pulsa *Conectar*. También puedes escribir la IP (`192.168.40.7`) en *Conexión manual por IP*.
3. La primera vez el host muestra un **PIN de 6 dígitos** (en su ventana y en una notificación). Escríbelo en el cliente. Desde ese momento los dos equipos quedan **emparejados** y ya no se pide PIN.

Mientras alguien está conectado, el host muestra un aviso rojo **"Sesión activa desde …"** con un botón **Desconectar**.

## Cambiar la configuración durante la sesión

Lleva el mouse al **borde superior** de la ventana del stream: aparece una barra con **resolución, FPS, monitor, bitrate, códec y audio** (frecuencia, estéreo/mono y volumen), más *Estadísticas*, *Más opciones*, *Pantalla completa* y *Desconectar*. Los cambios se aplican al momento sin reconectar; el bitrate, el FEC y el audio ni siquiera reinician el video.

## Atajos durante la sesión

| Atajo | Acción |
|---|---|
| `Ctrl+Alt+Shift+M` | Menú: configuración del stream en vivo, monitores, archivos, desconectar |
| `Ctrl+Alt+Shift+S` | Estadísticas (FPS, bitrate, latencias por etapa, pérdidas, FEC) |
| `Ctrl+Alt+Shift+F` | Pantalla completa |
| `Ctrl+Alt+Shift+1…9` | Cambiar al monitor 1…9 del host (`0` = todos) |
| `Ctrl+Alt+Shift+R` | Mouse relativo (para juegos) |
| `Ctrl+Alt+Shift+V` | Modo solo ver |
| `Ctrl+Alt+Shift+Q` | Desconectar |

Las teclas especiales (Win, Alt+Tab, etc.) se envían al host mientras la ventana de LanCast está activa. `Ctrl+Alt+Supr` no se puede capturar (lo reserva Windows).

## Funciones

- **Configuración del stream** (antes y durante la sesión, sin reconectar): monitor (o todos), resolución (nativa, 720p, 1080p, 1440p, 4K o personalizada), FPS (30–240 o personalizado), códec (automático, H.264, HEVC, AV1), bitrate (automático o 5–1000 Mbps), CBR/VBR, escalado (ajustar, estirar, 1:1), V-Sync, audio y FEC (0–50 %). **Perfiles** con nombre ("Trabajo 1440p 4:4:4", "Juego 144 Hz", "Celular" y los tuyos).
- **Multi-monitor**: lista de monitores del host con resolución, Hz y posición (incluidas coordenadas negativas); cambio en vivo.
- **Control remoto**: mouse absoluto o relativo, rueda, teclado por scancodes, cursor dibujado localmente (sin latencia) y modo solo ver.
- **Audio** del host (WASAPI loopback, PCM sin compresión: cero latencia de códec). Frecuencia elegible en vivo (la del sistema, 48 000, 44 100, 32 000, 24 000 o 16 000 Hz), estéreo o mono, volumen y búfer de reproducción (5–200 ms).
- **Portapapeles** de texto bidireccional.
- **Archivos**: arrastra archivos o carpetas a la ventana para enviarlos; desde el host, menú de la bandeja → *Enviar archivos al cliente*. Bloques de 4 MB, **reanudable**, verificado con SHA-256, con progreso, velocidad y tiempo restante. Nunca sobrescribe (guarda como `nombre (1).ext`) salvo que lo actives. Carpeta por defecto: `Descargas\LanCast`.
- **Seguridad**: emparejamiento con PIN (PBKDF2) y después secreto compartido de 256 bits por equipo (TOFU, guardado con DPAPI); intercambio ECDH P-256; todo el tráfico (control, video, audio y archivos) cifrado con **AES-256-GCM**. El host solo acepta IP privadas (RFC1918) y admite lista blanca de IP. Bloqueo de 10 minutos tras 5 PIN incorrectos.

## Rendimiento

- Camino de video sin copias a CPU: captura DXGI Desktop Duplication → escalado y conversión a NV12 con el procesador de video D3D11 → codificador por hardware del driver (NVENC/AMF/QuickSync vía Media Foundation) → UDP con FEC Reed-Solomon → decodificador DXVA → swapchain *flip-model* con tearing.
- **En paralelo**: la captura entrega cada frame al codificador y sigue con el siguiente; los frames codificados se recogen en otro hilo y un tercer hilo los paquetiza, cifra y envía (los paquetes de datos salen antes de calcular la paridad). Hilos con prioridad de tiempo real (MMCSS) y GPU con prioridad alta.
- Medido en loopback con una GPU integrada AMD (720p a 165 fps, HEVC): 165 fps sostenidos, ~6,5 ms de extremo a extremo (captura+codificación ~6 ms, red ~0,3 ms).
- FEC al 15 % recupera sin cortes un 1 % de pérdida aleatoria (prueba automatizada).
- Cambio de resolución, FPS, códec o monitor en ~130 ms sin reconectar; bitrate, FEC y audio sin reiniciar el video.

### Cómo conseguir la menor latencia

1. Botón **Latencia mínima** (barra superior del stream): FPS = Hz del monitor del host, V-Sync apagado, codificador rápido y pantalla completa.
2. **Pantalla completa** en el cliente: las estadísticas deben decir *Presentación directa (sin DWM)*; en ventana, Windows suma un frame de composición.
3. Monitores de **alta frecuencia** en ambos equipos (144–240 Hz): cada frame tarda menos en dibujarse y capturarse.
4. Windows (ambos PC): *Configuración → Sistema → Pantalla → Gráficos* → **Programación de GPU acelerada por hardware** y **Optimizaciones para juegos en ventana** activadas; **Modo de juego** activado; plan de energía de alto rendimiento.
5. Tarjeta de red (ambos PC, *Administrador de dispositivos → adaptador → Opciones avanzadas*): **Moderación de interrupciones: Deshabilitado**, **Ethernet de eficiencia energética (EEE): Deshabilitado**, **Green Ethernet: Deshabilitado**.
6. Antivirus con inspección de red (Kaspersky, etc.): excluye `LanCast.exe` o desactiva la inspección para la red local.
7. El ancho de banda no es un problema en 2.5 GbE: un bitrate alto no agrega latencia, solo calidad.

## Red recomendada

Todos los PC conectados por cable al mismo switch y en la misma subred (por ejemplo `192.168.40.x`). Si usas un router Wi-Fi detrás del router principal, ponlo en modo **punto de acceso** (o usa un access point conectado al switch) para que los dispositivos inalámbricos queden en la misma red.

Puertos (configurables en *Ajustes*): TCP **47984** (control), TCP **47985** (archivos), UDP **47998** (video y audio), UDP **47999** (descubrimiento).

## Solución de problemas

- **El cliente no encuentra el host**: escribe la IP a mano. Comprueba que el host diga "Esperando conexiones" y que la red sea *Privada*. Pulsa *Crear reglas del firewall* en el host. Si tienes un antivirus con firewall propio (Kaspersky, ESET, etc.), permite `LanCast.exe` en él.
- **"No se pudo conectar"**: haz ping a la IP del host. Si el ping funciona, casi siempre es el firewall del host.
- **Pantalla negra o "Esperando video"**: actualiza el driver de la GPU del host. Revisa *Diagnóstico* en ambos equipos (codificadores en el host y decodificadores en el cliente).
- **Volver a pedir PIN**: en el host, *Este equipo (host)* → *Equipos emparejados* → *Olvidar*.
- **Registro**: *Ajustes* → *Registro*, o el archivo `%APPDATA%\LanCast\lancast.log`.

## Línea de comandos

```
LanCast.exe --list [--json] [--verbose]   Inventario de GPU, monitores y códecs
LanCast.exe --connect 192.168.40.7        Conectar al iniciar
LanCast.exe --minimized                   Iniciar oculto en la bandeja
LanCast.exe --no-host                     Solo cliente (segunda instancia en el mismo PC)
```

La variable de entorno `LANCAST_DATA_DIR` cambia la carpeta de configuración (útil para pruebas).

## Compilar desde el código

Requisitos: Visual Studio 2022 Build Tools (C++ y CMake). GoogleTest y Dear ImGui se descargan solos en la configuración.

```powershell
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

El ejecutable queda en `build\lancast-app\Release\LanCast.exe`.

### Estructura

```
lancast-core/       Protocolo, FEC Reed-Solomon, paquetización, ajustes TOML, geometría (portable, sin Windows)
lancast-platform/   Windows: DXGI/D3D11, captura, procesador de video, Media Foundation, WASAPI, SendInput,
                    criptografía CNG, sockets, canal seguro, archivos
lancast-host/       Servicio de host: emparejamiento, sesión, tubería de video y audio
lancast-client/     Sesión de cliente: conexión, recepción con FEC, reloj, audio, archivos
lancast-app/        LanCast.exe: interfaz (Dear ImGui), presentación, entrada, bandeja
tests/              Unitarias (core), integración con el hardware (platform) y extremo a extremo (e2e)
```

### Dependencias

| Dependencia | Versión | Uso |
|---|---|---|
| Windows SDK (D3D11, DXGI, Media Foundation, WASAPI, CNG, Winsock) | 10.0.22621+ | Todo lo de plataforma (incluido en Windows) |
| Dear ImGui | v1.92.9 | Interfaz gráfica (compilada dentro del .exe) |
| GoogleTest | v1.18.0 | Pruebas |

## Pendiente

- Cliente web para el celular (HTTPS, video, táctil y archivos).
- App Android para compartir la pantalla del celular (opcional).
- Captura de la pantalla segura de Windows (UAC, Ctrl+Alt+Supr), que requiere ejecutar el host como servicio de sistema.
- 4:4:4 y HDR de 10 bits: la interfaz los permite elegir, pero los codificadores de Media Foundation transmiten 4:2:0 de 8 bits (el host lo indica al conectar).
