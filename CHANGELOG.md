# Cambios

Cada versión publicada toma sus notas de la sección correspondiente de este archivo
(`.github/workflows/release.yml`). El SHA-256 de `LanCast.exe` se agrega solo al publicar.

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
