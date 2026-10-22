# Cambios

Cada versión publicada toma sus notas de la sección correspondiente de este archivo
(`.github/workflows/release.yml`). El SHA-256 de `LanCast.exe` se agrega solo al publicar; el de `LanCast.apk` ("SHA-256 de LanCast.apk: …") se agrega al subir el APK (la app lo exige para actualizarse).

## 1.9.0

### Menos latencia
- **Decodificación el doble de rápida en el cliente de PC.** Algunas GPU (sobre todo las integradas de AMD) apagan su unidad de video entre cuadro y cuadro, y cada cuadro pagaba el despertar. LanCast la mantiene ocupada con un cuadro diminuto cada 3 ms en un dispositivo aparte. Medido a 1440p: HEVC baja de ~10 a ~5 ms por cuadro y H.264 de ~14 a ~6,5 ms. *Ajustes → Decodificación → Mantener despierta la unidad de video* (activado; gasta un poco más de energía).
- **FEC automático** (PC y Android): la paridad baja sola hasta 5 % por cable sin pérdidas (menos paquetes, los cuadros salen antes) y sube enseguida, hasta 40 %, si aparecen pérdidas (Wi-Fi). Cambiarlo no corta la imagen.
- **Calibración del codificador**: la primera vez, el host mide su codificador en segundo plano (~2 s, una vez por tarjeta y driver). Con eso, **Calidad máxima** decide sola si conviene sumar 4:4:4 (solo si cuesta 3 ms o menos de más).

### Funciones nuevas
- **Espectadores**: mientras alguien controla, hasta 4 equipos más pueden conectarse a **solo ver** (presentaciones, soporte). Reciben el mismo video y audio (se codifica una sola vez), no controlan, no hablan y no cambian la calidad. Se van cuando termina la sesión principal. En el host: *Permitir espectadores* (activado), la lista de quién mira y *Desconectar espectadores*.
- **Micrófono del cliente al host** (PC y Android): botón *Micrófono* en la barra del stream. En el host llega como un micrófono más ("CABLE Output") para Discord, Teams o un juego; requiere el cable de audio virtual gratuito **VB-Cable** en el host (vb-audio.com/Cable) y la opción *Recibir el micrófono del cliente*. Sin eco: el cliente de PC resta del micrófono el sonido del host que sale por sus parlantes (SpeexDSP; con un eco fuerte simulado lo baja 28 dB) y Android usa la cancelación del sistema. El host no lo usa si el cable está como salida predeterminada (habría eco). En **solo ver** no se habla.
- **Vibración del mando**: la vibración que pide el juego en el host llega al mando del cliente (PC: el mando de Xbox; Android: el mando conectado, o el teléfono con el mando en pantalla).
- **Despertar un PC desde otro PC** (Wake-on-LAN): *Conectar → Equipos emparejados que no responden → Despertar*.
- **Archivo de diagnóstico** para soporte: *Diagnóstico → Crear archivo para soporte* deja en el escritorio un .txt con la versión, el equipo, el estado del host y de la sesión, estadísticas y registro (sin contraseñas ni claves). En Android: *Enviar diagnóstico (si algo falla)*.

### Correcciones
- **El FEC automático subía sin pérdidas**, sobre todo con sonido en el PC: el host numera cada paquete al prepararlo y envía el video en tandas, así que un paquete de audio llega antes que video con número menor y eso se contaba como pérdida. En Android era peor: se contaban solo los paquetes de video pero la numeración incluye el audio, así que cualquier sonido parecía pérdida. Ahora un paquete se da por perdido solo si ya llegaron 64 posteriores sin él (PC y Android).
- **El host reintenta abrir sus puertos** durante 15 s si están ocupados al arrancar (por ejemplo, la instancia anterior todavía se está cerrando tras una actualización), en vez de quedarse sin escuchar.

### Compatibilidad
- Los campos nuevos del protocolo son opcionales: un cliente 1.8 se conecta a un host 1.9 y al revés (sin las funciones nuevas).

## 1.8.0

### Controlar un teléfono (desde un PC o desde otro teléfono)
- **Compartir este teléfono** (app Android, sección nueva en la pantalla principal): el teléfono pasa a ser un *host* como el PC. Otro teléfono o un PC con LanCast lo encuentran en *Equipos en la red*, se emparejan con el PIN que muestra el teléfono y lo ven y controlan.
  - Imagen: captura de pantalla de Android directo al codificador por hardware (H.264 o HEVC, sin copias), en vertical u horizontal (si el teléfono gira, el stream se reacomoda solo). Audio de las apps en Android 10 o superior.
  - Control: con el servicio de accesibilidad **«LanCast: control remoto»** (Android no permite tocar la pantalla de otra forma; se activa una vez en *Ajustes → Accesibilidad*). Clic = toque, mantener = toque largo, arrastrar = deslizar en vivo, rueda = desplazar, clic derecho = Atrás, botón del medio = Inicio. El teclado escribe en el campo de texto activo; Ctrl+A/C/V/X, flechas, Inicio/Fin, Borrar, Enter, Esc (= Atrás) y la tecla Windows (= Inicio).
  - Lo que se copia en el PC pasa al portapapeles del teléfono.
- **Cliente de PC con un teléfono**: manda el texto ya traducido con la distribución de teclado de Windows (ñ, tildes con tecla muerta, @ con AltGr) y dibuja un puntero sobre la pantalla del teléfono.
- **Cliente Android con un teléfono**: usa el toque directo (se toca donde se quiere tocar en el otro teléfono).

### PC
- **Botón «Calidad máxima»** (junto a «Latencia mínima», en la barra y en el menú): resolución nativa del host (sin escalar) y 50 % más bitrate, con el codificador rápido. Medido en una GPU AMD integrada: 2560x1440 a 127 Mbps cuesta ~2,6 ms más por cuadro que 1080p. El 4:4:4 no se activa solo porque ahí costaba ~7,7 ms más: queda como opción aparte para trabajar con mucho texto.
- Medición por etapa de la latencia del host (`LANCAST_BENCH=1`, *Bench.HostLatencyBreakdown*): la conversión de color tarda 0,6–1,7 ms y el resto es el codificador por hardware, que crece con los píxeles codificados (el 4:4:4 empaquetado codifica el doble).
- **La barra superior del stream se acomoda al ancho**: en una fila si cabe y, si no (por ejemplo en una pantalla de 1280x720 o en una ventana angosta), en dos o más filas, sin cortar ni esconder ningún control.

### Si el router cambia la IP (DHCP)
- **Ya no hay que volver a escribir la IP ni el PIN.** El emparejamiento siempre fue por el identificador del equipo, no por la IP. Ahora, si un equipo emparejado no responde en su IP de siempre (se le da 1,5 s), el cliente lo busca en la red por ese identificador, se conecta en la IP nueva sin PIN y la guarda. En el PC y en el teléfono. Probado: ~3 s la primera vez después del cambio.

## 1.7.1

- **App Android: botón *Buscar actualizaciones*** en los ajustes, además de la búsqueda automática al abrir la app (cada 6 horas como mucho). Si el teléfono no tiene la pantalla de *instalar apps desconocidas*, la app explica dónde activarlo.
- **Mandos: el driver ViGEmBus está descontinuado.** Los textos y avisos ahora lo dicen y enlazan a su última versión, la 1.22.0 (<https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0>). Su autor lo archivó en 2023 y no tendrá más actualizaciones, pero sigue funcionando en Windows 10 y 11 y es el mismo que usan otros programas de streaming. Windows no trae otra forma de crear un mando de Xbox sin un driver: se probó la inyección de mando de Windows (InputInjector) y ni XInput ni Windows.Gaming.Input la ven. Si ya tenías una versión anterior de ViGEmBus, puede que haya que ejecutar su instalador dos veces.

## 1.7.0

### App Android
- **Funciona aunque el Wi-Fi no tenga internet**: la app usa siempre el Wi-Fi para hablar con los PC (antes Android podía mandar el tráfico por los datos móviles y no los encontraba).
- **Gráficos en tiempo real** (botón *Gráficos*): latencia de red (promedio y picos), decodificación, bitrate y FPS de los últimos 30 segundos, con marcas de cuadros perdidos y reparados.
- **Imagen más fluida**: la pantalla del teléfono se pone a la frecuencia del stream (por ejemplo 120 Hz) en vez de que Android la baje a 60 Hz.
- **S Pen, mouse y teclado Bluetooth**: el S Pen y el mouse apuntan exactamente donde están (punta = clic, botón del lápiz o botón derecho = clic derecho, rueda); el teclado físico manda cada tecla con pulsar y soltar, así funcionan los atajos y los juegos.
- **Mandos**: un mando Bluetooth/USB conectado al teléfono, o el **mando en pantalla** (botón *Mando*), aparece en el PC como un mando de Xbox 360. Requiere el driver gratuito **ViGEmBus** en el PC (<https://github.com/nefarius/ViGEmBus/releases>).
- **Portapapeles** en los dos sentidos: lo copiado en el PC queda en el teléfono; lo copiado en el teléfono pasa al PC al volver a LanCast.
- **Archivos**: botón *Archivos* para enviar al PC; lo que manda el PC se guarda en *Descargas/LanCast*. Con verificación SHA-256, igual que entre PC.
- **Se actualiza sola**: avisa cuando hay una versión nueva en GitHub, la descarga, comprueba su SHA-256 y la instala (Android pide confirmación).
- **Despertar un PC** (Wake-on-LAN): en *Equipos emparejados* → ⋯ → *Despertar*. El PC debe tener Wake-on-LAN activado en la BIOS y en la tarjeta de red.
- **Ritmo de envío** en *Calidad*, por si hay cortes por Wi-Fi.

### PC
- **Mandos virtuales**: el host crea mandos de Xbox 360 para los mandos del cliente (teléfono o PC). Necesita el driver ViGEmBus; sin él, avisa y los ignora.
- **El cliente de PC envía sus mandos** (XInput) al host. Se puede desactivar en *Ajustes → Cliente*.
- **Ritmo de envío** (*Calidad*): el host reparte los paquetes de cada cuadro a un ritmo máximo en lugar de mandarlos de golpe. Por cable no hace falta; por Wi-Fi o pasando por un router de 1 Gb evita que las ráfagas se pierdan.
- El host informa su MAC al cliente (para Wake-on-LAN).
- Se midió subir la prioridad de GPU del host con la GPU ocupada por otro proceso: en una GPU AMD integrada no cambia nada (hay que esperar igual a que termine el trabajo que ya está en la GPU), así que no se incluye.

## 1.6.1

### PC (host)
- **Sin la barra roja "Sesión activa desde… [Desconectar]"** en el equipo controlado: la desconexión ya está en la barra del cliente. Siguen el aviso de Windows al conectarse, el ícono de la bandeja y el botón *Desconectar* en *Este equipo*. Se puede volver a activar en *Este equipo → Permisos*.
- **Arreglo: el ajuste automático bajaba la calidad sin necesidad.** Tomaba como "sobrecarga" que un cuadro tardara más en codificarse que el tiempo entre cuadros, pero el codificador trabaja en cadena (varios cuadros a la vez): en una GPU AMD tarda ~7-8 ms tanto a 1080p como a 1440p y aun así entrega 165 fps. Resultado: bajaba a 720p y menos FPS sin ganar latencia. Ahora solo baja si se saltan más del 10 % de los cuadros o si se acumulan (latencia mayor a 2,5 intervalos).
- **El ajuste automático recupera la calidad**: si todo va bien durante 20 segundos, sube un escalón (primero los FPS, después la resolución) hasta volver a lo pedido. Si al subir vuelve a sobrecargarse enseguida, espera el doble antes de intentarlo de nuevo (hasta ~5 minutos), para no oscilar.

## 1.6.0

### PC (cliente)
- **Gráficos en tiempo real** de los últimos 10 segundos (botón *Gráficos* en la barra del stream, en el menú o `Ctrl+Alt+Shift+G`):
  - **Latencia por cuadro** desglosada: host (captura + codificación), red, decodificación + dibujo y espera hasta la pantalla. Se marcan los picos y los eventos: cuadro perdido (rojo), reparado por FEC (naranja) y keyframes o pedidos de keyframe (violeta).
  - **Bitrate** cada 250 ms con la **pérdida de paquetes** en barras rojas.
  - **FPS** y **RTT**.

## 1.5.3

### PC (host)
- **Menos latencia en la conversión de color**: el host convierte la captura a NV12 con un shader propio en lugar del procesador de video de la GPU. Medido en una GPU AMD: 0,7 ms contra 2,5 ms (1440p a 1080p) y 0,8 contra 1,2 ms (1440p a 1440p). En la prueba de extremo a extremo, captura + codificación bajó de ~5,6 a ~4,4 ms. Con un monitor girado se sigue usando el procesador de video.
- Medición de la tubería (`LANCAST_BENCH=1`): conversión y codificación por códec, resolución y FPS.

## 1.5.2

### PC
- **Arreglo: la ventana del stream quedaba negra según su tamaño.** Con el escalado en *1:1*, si la ventana era más chica que la imagen del stream (por ejemplo, ver un PC de 1080p en una ventana menor a 1920x1080), la imagen se salía de la ventana y el procesador de video de la GPU se negaba a dibujarla. Ahora se recorta a la ventana y se ve la parte central, pixel por pixel. Una prueba recorre 2016 combinaciones de tamaño de ventana y modo de escalado.

## 1.5.1

### App para Android
- **Arreglo: la conexión se cortaba y reconectaba al tocar la pantalla.** El toque y el teclado enviaban desde el hilo de la interfaz, Android lo bloqueaba y el contador del cifrado quedaba desfasado: el PC rechazaba el siguiente mensaje y cerraba la sesión. Ahora todo sale por una cola en su propio hilo y el contador solo avanza cuando el mensaje sale.
- **Mover la vista con zoom arrastrando con tres dedos**, en los dos modos (en modo directo antes no había cómo moverse con el zoom). Tocar con tres dedos sin moverlos sigue abriendo el teclado.
- **Teclado flotante**: una bolita a la derecha abre y cierra el teclado; se arrastra para subirla o bajarla y se activa o desactiva en la barra de opciones (*Teclado flotante*).
- **Gira con el teléfono**: horizontal o vertical según la rotación automática; la vista y el zoom se reacomodan sin cortar la sesión.
- **Con el teclado abierto la vista sube** a la zona que queda libre (al cerrarlo vuelve a su lugar), y arrastrar con tres dedos también la mueve hacia arriba y abajo aunque no haya zoom.
- **Arreglo: los botones de volumen del teléfono no funcionaban** durante la sesión (la vista del teclado se quedaba con ellos). Ahora controlan el volumen del audio del PC.
- **Bitrate para Wi-Fi**: 40 Mbps por defecto (antes el automático, que a 1440p y 120 fps superaba 150 Mbps y las ráfagas se perdían entre el router de 1 Gb y el Wi-Fi). Se cambia en *Ajustes* o en *Calidad* durante la sesión (20, 40, 60, 100 Mbps o automático).

## 1.5.0

### App para Android (nueva)
- **LanCast para Android**: ve y controla el PC desde el celular. Instala `LanCast.apk` (en este release) en el teléfono.
- Encuentra los PC solos en la red (o por IP), se empareja con el mismo PIN de 6 dígitos y después se conecta sin PIN.
- Video por el decodificador de hardware del teléfono en modo de baja latencia (HEVC, H.264 o AV1), audio del PC y Wi-Fi de baja latencia.
- **Touchpad**: un dedo mueve el cursor, toque = clic, dos dedos = clic derecho y rueda, pellizcar = zoom (la vista sigue al cursor), mantener = arrastrar, tres dedos = teclado. También modo de toque directo.
- **Teclado** del teléfono con tildes y ñ, más una barra con Esc, Tab, Ctrl, Alt, Mayús, Win, flechas, Supr y F1–F12.
- Barra con pestaña arriba al centro: monitor, calidad, Ctrl+Alt+Supr, estadísticas y desconectar. Se reconecta sola si se corta el Wi-Fi.

### PC
- Nuevo mensaje de **texto** para escribir desde teclados de celular (tildes, ñ, emojis). Necesario para escribir desde la app.

## 1.4.0

### Novedades
- **Resolución y FPS "Igual a esta pantalla"** por defecto: el cliente pide la resolución y los Hz del monitor donde está su ventana, y vuelve a pedirlos si la ventana pasa a otro monitor. Las configuraciones anteriores se migran una vez.
- **Ajuste automático**: si el codificador del host no da abasto (tarda más que el tiempo entre cuadros), baja la resolución y luego los FPS y avisa. Además, como mucho 2 cuadros esperan en el codificador: si está ocupado se salta uno en vez de acumular latencia. Se puede desactivar en *Ajustes*.
- **4:4:4 real** (texto nítido, sin bordes de color) con cualquier códec y decodificador, y **HDR de 10 bits** con HEVC o AV1.
- **Barra del stream con ✕**: la oculta y deja solo una flecha pequeña arriba al centro. El tamaño se ajusta en *Ajustes → Cliente*.
- **Informes de cierres inesperados** (volcados de memoria) en *Diagnóstico*.
- **Releases automáticos** con firma digital opcional. Si LanCast está firmado, la actualización automática exige que la versión nueva esté firmada por el mismo editor.

## 1.3.0

- Codificadores AMD AMF y NVIDIA NVENC directos, envío por slices, intra-refresh opcional y envío UDP por lotes.
- Medición de latencia real de pantalla a pantalla.
- Reconexión automática y actualización automática desde GitHub.
- Dos monitores a la vez en dos ventanas.
- Host como servicio de Windows: pantalla de bloqueo, UAC y Ctrl+Alt+Supr.
- Pruebas de fuzzing y CI en GitHub Actions.

## 1.2.0

- Cambio en vivo de resolución, FPS, monitor, bitrate, códec y audio durante la sesión.
- Botón "Latencia mínima" y codificación en paralelo (captura, codificación y envío en hilos separados).

## 1.1.0

- Correcciones: pantalla negra al maximizar, atajos con el control activo y botón del PIN.

## 1.0.0

- Primera versión: host y cliente en un solo `LanCast.exe`, video por hardware con FEC, audio, portapapeles, archivos y emparejamiento cifrado.
