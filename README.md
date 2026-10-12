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
6. (Opcional, en el PC que vas a controlar) *Este equipo (host)* → **Instalar como servicio**: el host funciona también en la **pantalla de bloqueo**, en los avisos de **UAC** y permite enviar **Ctrl+Alt+Supr**. Ver [Host como servicio](#host-como-servicio-de-windows).

**Actualizaciones**: LanCast revisa al iniciar si hay una versión nueva en GitHub y muestra un aviso arriba; *Actualizar ahora* descarga el `.exe`, verifica su SHA-256 y se reinicia solo. También desde *Ajustes* → *Buscar actualizaciones*.

Requisitos: Windows 10 (1809+) u 11 de 64 bits y una GPU con codificación/decodificación por hardware (NVIDIA, AMD o Intel) con el driver actualizado. Para HEVC en el equipo cliente, Windows necesita la extensión **"Extensiones de video HEVC"** de Microsoft Store; si no está, LanCast usa H.264 automáticamente.

## App para Android (ver y controlar el PC desde el celular)

1. En el celular, abre la página de *Releases* (<https://github.com/jhoan3211/LanDesk/releases>) y descarga **`LanCast.apk`**.
2. Ábrelo para instalarlo. Android pedirá permitir *instalar apps de origen desconocido* para el navegador (o el administrador de archivos): actívalo solo para esta instalación.
3. El celular debe estar en la **misma red** que los PC (por ejemplo, Wi-Fi del access point conectado al switch, IP `192.168.40.x`).
4. Abre LanCast en el celular: los PC aparecen solos en **Equipos en la red** (o escribe la IP). La primera vez escribe el **PIN** que muestra el PC.

En el PC hace falta LanCast **1.5** o superior para escribir texto desde el teclado del celular (el resto funciona desde la 1.4).

| Gesto (modo touchpad) | Acción en el PC |
|---|---|
| Deslizar un dedo | Mover el cursor (con aceleración, como un touchpad) |
| Tocar | Clic |
| Tocar con dos dedos | Clic derecho |
| Deslizar dos dedos | Rueda (vertical y horizontal) |
| Pellizcar | Zoom de la vista (la vista sigue al cursor) |
| Arrastrar con tres dedos | Mover la vista con zoom (también en modo directo) |
| Mantener presionado, o doble toque y arrastrar | Arrastrar (botón izquierdo presionado) |
| Tocar con tres dedos | Abrir / cerrar el teclado |

La **bolita del teclado** a la derecha abre y cierra el teclado (arrástrala para cambiarla de altura; se quita en la barra: *Teclado flotante*). La app gira con el teléfono: en vertical se ve el escritorio completo y con el zoom se llena la pantalla.

En **modo directo** el clic cae donde tocas. La **pestaña** de arriba al centro abre la barra: teclado, modo, monitor, calidad, Ctrl+Alt+Supr, zoom 1:1, estadísticas y desconectar. Con el teclado abierto aparece una barra con Esc, Tab, Ctrl, Alt, Mayús, Win, flechas, Inicio, Fin, Supr y F1–F12 (Ctrl/Alt/Mayús quedan fijados para la siguiente tecla: Ctrl → C = copiar).

La app usa el decodificador de **hardware** del teléfono en modo de baja latencia, pide al PC la resolución y los Hz de la pantalla del celular, mantiene el Wi-Fi en modo de baja latencia mientras transmite y se reconecta sola si se corta.

## Primer uso: conectar un PC a otro

En el **PC que vas a controlar** (host), por ejemplo `192.168.40.7`:

1. Abre LanCast. En *Este equipo (host)* debe decir **"Esperando conexiones en: 192.168.40.7"**.

En el **PC desde el que controlas** (cliente):

1. Abre LanCast → *Conectar a un equipo*.
2. El host aparece solo en **"Equipos encontrados en la red"**; pulsa *Conectar*. También puedes escribir la IP (`192.168.40.7`) en *Conexión manual por IP*.
3. La primera vez el host muestra un **PIN de 6 dígitos** (en su ventana y en una notificación). Escríbelo en el cliente. Desde ese momento los dos equipos quedan **emparejados** y ya no se pide PIN.

Cuando alguien se conecta, el host muestra un aviso de Windows y lo indica en el ícono de la bandeja; para cortarlo: *Este equipo (host)* → **Desconectar**. (La barra roja "Sesión activa desde…" se puede activar en *Este equipo → Permisos*.)

Si la red se corta, el cliente **se reconecta solo** (reintenta cada 2 s durante 1 minuto, sin pedir PIN) y retoma el stream con la misma configuración.

## Cambiar la configuración durante la sesión

Lleva el mouse al **borde superior** de la ventana del stream: aparece una barra con **resolución, FPS, monitor, bitrate, códec y audio** (frecuencia, estéreo/mono y volumen), más *Abrir otro monitor…*, *Latencia mínima*, *Medir latencia*, *Estadísticas*, *Más opciones*, *Pantalla completa* y *Desconectar*. Los cambios se aplican al momento sin reconectar; el bitrate, el FEC y el audio ni siquiera reinician el video.

Por defecto la resolución y los FPS son **"Igual a esta pantalla"**: el cliente pide la resolución y los Hz del monitor donde está su ventana (y los vuelve a pedir si la mueves a otro monitor). Así el host no codifica más píxeles ni más cuadros de los que se pueden ver.

**Ocultar la barra**: debajo de la barra sobresale una **✕** en el centro. Al pulsarla la barra ya no aparece al pasar el mouse por arriba; solo se ve una **flecha pequeña** en el centro del borde superior, y al pulsarla vuelve la barra. El tamaño de la ✕ y la flecha se cambia en *Ajustes → Cliente*.

### Dos monitores a la vez

*Abrir otro monitor…* (barra superior o menú) muestra otro monitor del host en una **segunda ventana**, con su propio stream en paralelo. Si el cliente tiene dos monitores, la ventana se abre a pantalla completa en el otro. Doble clic o `F11` alterna pantalla completa; el mouse funciona en las dos ventanas y el teclado va a la que esté activa.

### Medir la latencia real

*Medir latencia* hace que el host muestre un cuadrito blanco/negro en una esquina y el cliente mide cuánto tarda en verlo en su pantalla (de pantalla a pantalla, con los relojes sincronizados). Muestra la mediana de 10 muestras.

## Atajos durante la sesión

| Atajo | Acción |
|---|---|
| `Ctrl+Alt+Shift+M` | Menú: configuración del stream en vivo, monitores, archivos, desconectar |
| `Ctrl+Alt+Shift+S` | Estadísticas (FPS, bitrate, latencias por etapa, pérdidas, FEC) |
| `Ctrl+Alt+Shift+G` | Gráficos en tiempo real: latencia por cuadro (por etapa), bitrate, pérdidas, FPS y RTT |
| `Ctrl+Alt+Shift+F` | Pantalla completa |
| `Ctrl+Alt+Shift+1…9` | Cambiar al monitor 1…9 del host (`0` = todos) |
| `Ctrl+Alt+Shift+R` | Mouse relativo (para juegos) |
| `Ctrl+Alt+Shift+V` | Modo solo ver |
| `Ctrl+Alt+Shift+Q` | Desconectar |
| `Ctrl+Alt+Fin` | Enviar `Ctrl+Alt+Supr` al host (requiere el host como servicio) |

Las teclas especiales (Win, Alt+Tab, etc.) se envían al host mientras la ventana de LanCast está activa. `Ctrl+Alt+Supr` en el teclado del cliente lo reserva Windows: usa `Ctrl+Alt+Fin` o el botón del menú.

## Funciones

- **Configuración del stream** (antes y durante la sesión, sin reconectar): monitor (o todos), resolución (igual a esta pantalla, 720p, 1080p, 1440p, 4K o personalizada), FPS (igual a esta pantalla, 30–240 o personalizado), códec (automático, H.264, HEVC, AV1), bitrate (automático o 5–1000 Mbps), CBR/VBR, escalado (ajustar, estirar, 1:1), V-Sync, audio y FEC (0–50 %). **Perfiles** con nombre ("Trabajo 1440p 4:4:4", "Juego 144 Hz", "Celular" y los tuyos).
- **4:4:4 real** (texto e íconos nítidos, sin bordes de color) con cualquier códec y decodificador: el host empaca el color completo en un cuadro de doble alto y el cliente lo reconstruye con un shader (como AVC444 de Escritorio remoto). Cuesta el doble de trabajo al codificador.
- **HDR de 10 bits** (HDR10, BT.2020 PQ) con HEVC o AV1: requiere el monitor del host en modo HDR y una GPU AMD o NVIDIA en el host. En una pantalla HDR se ve en HDR; en una SDR se adapta (tone mapping).
- **Ajuste automático**: si el codificador del host no da abasto (se salta más del 10 % de los cuadros o se le acumulan), baja la resolución (hasta 720p) y luego los FPS, y lo avisa. Nunca deja que se acumulen cuadros esperando (cada uno sumaría latencia).
- **Multi-monitor**: lista de monitores del host con resolución, Hz y posición (incluidas coordenadas negativas); cambio en vivo y **dos monitores a la vez** en dos ventanas.
- **Control remoto**: mouse absoluto o relativo, rueda, teclado por scancodes, cursor dibujado localmente (sin latencia) y modo solo ver.
- **Audio** del host (WASAPI loopback, PCM sin compresión: cero latencia de códec). Frecuencia elegible en vivo (la del sistema, 48 000, 44 100, 32 000, 24 000 o 16 000 Hz), estéreo o mono, volumen y búfer de reproducción (5–200 ms).
- **Portapapeles** de texto bidireccional.
- **Archivos**: arrastra archivos o carpetas a la ventana para enviarlos; desde el host, menú de la bandeja → *Enviar archivos al cliente*. Bloques de 4 MB, **reanudable**, verificado con SHA-256, con progreso, velocidad y tiempo restante. Nunca sobrescribe (guarda como `nombre (1).ext`) salvo que lo actives. Carpeta por defecto: `Descargas\LanCast`.
- **Seguridad**: emparejamiento con PIN (PBKDF2) y después secreto compartido de 256 bits por equipo (TOFU, guardado con DPAPI); intercambio ECDH P-256; todo el tráfico (control, video, audio y archivos) cifrado con **AES-256-GCM**. El host solo acepta IP privadas (RFC1918) y admite lista blanca de IP. Bloqueo de 10 minutos tras 5 PIN incorrectos.

## Rendimiento

- Camino de video sin copias a CPU: captura DXGI Desktop Duplication → escalado y conversión a NV12 con el procesador de video D3D11 → codificador por hardware → UDP con FEC Reed-Solomon → decodificador DXVA → swapchain *flip-model* con tearing.
- **Codificadores directos**: en GPU AMD usa **AMF** y en NVIDIA **NVENC** directamente (modo de ultra baja latencia, sin pasar por Media Foundation); en Intel u otros, el codificador de Media Foundation. *Diagnóstico* y las estadísticas indican cuál se usa.
- **Envío por slices**: cada cuadro se codifica en varias porciones que salen por la red apenas están listas, sin esperar el cuadro completo (se puede desactivar en *Ajustes*).
- **Intra-refresh** opcional: tras una pérdida, la imagen se repara en franjas en lugar de pedir un keyframe completo (apagado por defecto: algunos decodificadores de Windows no lo toleran).
- **En paralelo**: la captura entrega cada frame al codificador y sigue con el siguiente; los frames codificados se recogen en otro hilo y un tercer hilo los paquetiza, cifra y envía en lote (UDP Send Offload: un solo llamado al sistema por cuadro). Hilos con prioridad de tiempo real (MMCSS) y GPU con prioridad alta.
- Medido en loopback con una GPU integrada AMD (720p a 165 fps, HEVC, AMF directo): 165 fps sostenidos, ~5,7 ms de extremo a extremo (codificación 1,5–3 ms, red ~0,05 ms).
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

## Host como servicio de Windows

Una aplicación normal no puede ver ni controlar la **pantalla de bloqueo**, los avisos de **UAC** (escritorio seguro) ni generar **Ctrl+Alt+Supr**. Para eso LanCast se instala como servicio:

1. En el PC que vas a controlar: *Este equipo (host)* → **Instalar como servicio** → acepta el aviso de administrador.
2. Se copia `LanCast.exe` a `C:\Program Files\LanCast`, junto con tu configuración y los equipos emparejados (a `C:\ProgramData\LanCast`, accesible solo por el sistema y los administradores). Se crea el servicio **LanCast Host** (inicio automático) y las reglas del firewall.
3. Desde ese momento el host corre en el servicio aunque cierres LanCast o la sesión. El PIN de emparejamiento aparece en una ventana pequeña abajo a la derecha.
4. En el cliente: menú → **Ctrl+Alt+Supr**, o `Ctrl+Alt+Fin`.

Si cambias la configuración del host o actualizas LanCast, pulsa **Copiar configuración al servicio** / **Actualizar servicio a esta versión**. **Desinstalar servicio** quita el servicio, la copia en Archivos de programa y las reglas del firewall (la carpeta de ProgramData se conserva) y el host vuelve a ejecutarse dentro de la app. Registros: `C:\ProgramData\LanCast\service.log` y `agent.log`.

## Red recomendada

Todos los PC conectados por cable al mismo switch y en la misma subred (por ejemplo `192.168.40.x`). Si usas un router Wi-Fi detrás del router principal, ponlo en modo **punto de acceso** (o usa un access point conectado al switch) para que los dispositivos inalámbricos queden en la misma red.

Puertos (configurables en *Ajustes*): TCP **47984** (control), TCP **47985** (archivos), UDP **47998** (video y audio), UDP **47999** (descubrimiento).

## Solución de problemas

- **El cliente no encuentra el host**: escribe la IP a mano. Comprueba que el host diga "Esperando conexiones" y que la red sea *Privada*. Pulsa *Crear reglas del firewall* en el host. Si tienes un antivirus con firewall propio (Kaspersky, ESET, etc.), permite `LanCast.exe` en él.
- **"No se pudo conectar"**: haz ping a la IP del host. Si el ping funciona, casi siempre es el firewall del host.
- **Pantalla negra o "Esperando video"**: actualiza el driver de la GPU del host. Revisa *Diagnóstico* en ambos equipos (codificadores en el host y decodificadores en el cliente).
- **Volver a pedir PIN**: en el host, *Este equipo (host)* → *Equipos emparejados* → *Olvidar*.
- **"Ajuste automático: el codificador no daba abasto…"**: la GPU del host no da abasto con esa resolución y FPS. Baja la resolución, usa "Igual a esta pantalla" o quita 4:4:4. Se puede desactivar en *Ajustes → Ajuste automático*.
- **Pedí HDR pero llega SDR**: el aviso al conectar dice por qué (monitor del host sin HDR activado, cliente sin HEVC/AV1, GPU sin 10 bits, monitor girado…).
- **LanCast se cerró solo**: al volver a abrirlo avisa y guarda un informe en *Diagnóstico → Cierres inesperados*. Sirve para encontrar el error (junto con `LanCast.pdb` del mismo release).
- **Registro**: *Ajustes* → *Registro*, o el archivo `%APPDATA%\LanCast\lancast.log` (con el servicio: `C:\ProgramData\LanCast\agent.log`).
- **"Ese monitor ya se ve en la ventana principal"**: la segunda ventana debe mostrar un monitor distinto (Windows no permite capturar dos veces la misma pantalla).

## Línea de comandos

```
LanCast.exe --list [--json] [--verbose]   Inventario de GPU, monitores y códecs
LanCast.exe --connect 192.168.40.7        Conectar al iniciar
LanCast.exe --minimized                   Iniciar oculto en la bandeja
LanCast.exe --no-host                     Solo cliente (segunda instancia en el mismo PC)
LanCast.exe --measure-latency             Medir la latencia al conectar
LanCast.exe --install-service             (Administrador) Instalar el host como servicio
LanCast.exe --uninstall-service           (Administrador) Desinstalarlo
LanCast.exe --crash-test                  Provoca un cierre para probar los informes de fallos
```

La variable de entorno `LANCAST_DATA_DIR` cambia la carpeta de configuración (útil para pruebas).

## Compilar desde el código

Requisitos: Visual Studio 2022 Build Tools (C++ y CMake). GoogleTest y Dear ImGui se descargan solos en la configuración.

```powershell
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

El ejecutable queda en `build\lancast-app\Release\LanCast.exe`. GitHub Actions (`.github/workflows/ci.yml`) compila y ejecuta las pruebas sin hardware en cada cambio y publica el `.exe` como artefacto; las pruebas de GPU y extremo a extremo (etiqueta `hardware`) se corren en un PC real.

Las pruebas incluyen **fuzzing por mutación** (semillas fijas) de todos los lectores del protocolo, la cabecera de video, el reensamblador, TOML y JSON.

**Publicar una versión** (`.github/workflows/release.yml`): cambia `VERSION` en `CMakeLists.txt`, agrega la sección en `CHANGELOG.md`, súbelo y crea el tag `vX.Y.Z` (o *Actions → Release → Run workflow*). El workflow compila, prueba, firma si hay certificado configurado, agrega `SHA-256:` a las notas (la actualización automática lo exige) y publica `LanCast.exe` y `LanCast.pdb`. Firma de código: ver [docs/firma-de-codigo.md](docs/firma-de-codigo.md).

### Estructura

```
lancast-core/       Protocolo, FEC Reed-Solomon, paquetización, ajustes TOML, geometría (portable, sin Windows)
lancast-platform/   Windows: DXGI/D3D11, captura, procesador de video, Media Foundation, WASAPI, SendInput,
                    criptografía CNG, sockets, canal seguro, archivos
lancast-host/       Servicio de host: emparejamiento, sesión, tubería de video y audio
lancast-client/     Sesión de cliente: conexión, recepción con FEC, reloj, audio, archivos
lancast-app/        LanCast.exe: interfaz (Dear ImGui), presentación, entrada, bandeja
android/            App Android: core (protocolo, cifrado y FEC en Kotlin, probado contra el host real) y app
tests/              Unitarias (core), integración con el hardware (platform) y extremo a extremo (e2e)
```

### Dependencias

| Dependencia | Versión | Uso |
|---|---|---|
| Windows SDK (D3D11, DXGI, Media Foundation, WASAPI, CNG, Winsock) | 10.0.22621+ | Todo lo de plataforma (incluido en Windows) |
| Dear ImGui | v1.92.9 | Interfaz gráfica (compilada dentro del .exe) |
| AMD AMF SDK (encabezados, MIT) | 1.5.2 | Codificador AMD directo (`third_party/amf`; el runtime viene con el driver) |
| NVIDIA Video Codec SDK (encabezados, MIT) | 13.1 | Codificador NVIDIA directo (`third_party/nvenc`; el runtime viene con el driver) |
| GoogleTest | v1.18.0 | Pruebas |

## Pendiente

- Cliente web para el celular (HTTPS, video, táctil y archivos).
- App Android para compartir la pantalla del celular hacia el PC (opcional).
