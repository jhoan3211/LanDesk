# Firma de código de LanCast.exe

Sin firma digital, Windows muestra **"Windows protegió su PC"** (SmartScreen) al abrir `LanCast.exe` y algunos
antivirus lo tratan con desconfianza. Con firma, el aviso desaparece o se reduce rápido, y la **actualización
automática** exige además que cada versión nueva esté firmada por el mismo editor.

El workflow `.github/workflows/release.yml` ya firma solo si encuentra las credenciales. Hay que conseguir una de
estas dos cosas y configurarla en GitHub (*Settings → Secrets and variables → Actions*).

## Opción A: Azure Artifact Signing (antes "Trusted Signing")

La más barata y cómoda: unos **9,99 USD/mes** (plan Basic) y no hay que manejar archivos de certificado.

**Requisito importante**: Microsoft solo valida **personas** ubicadas en **Estados Unidos o Canadá**. Para
**empresas** acepta Estados Unidos, Canadá, la Unión Europea, el Reino Unido y algunos países más. Para una persona o
empresa en **Colombia** hoy no aplica: usa la opción B. Verifica la lista actual en la documentación de Microsoft,
porque cambia.

Si eres elegible:
1. Crea una suscripción de Azure de pago (no sirve la gratuita) y un recurso **Artifact Signing** con un perfil de
   certificado *Public Trust*. Microsoft valida tu identidad (unos días).
2. Crea un *App registration* (service principal) con el rol **Artifact Signing Certificate Profile Signer** sobre el
   recurso.
3. En GitHub, crea los **secretos** `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` y `AZURE_CLIENT_SECRET`, y las
   **variables** `ARTIFACT_SIGNING_ENDPOINT` (por ejemplo `https://eus.codesigning.azure.net`),
   `ARTIFACT_SIGNING_ACCOUNT` y `ARTIFACT_SIGNING_PROFILE`.

## Opción B: certificado OV de una autoridad (DigiCert, Sectigo, GlobalSign, Certum…)

Válido para personas y empresas de cualquier país. Desde 2023 la clave privada debe vivir en hardware (token USB o
HSM en la nube), así que:

- Si el proveedor entrega un **.pfx** exportable (algunos servicios en la nube lo permiten): en GitHub crea el secreto
  `SIGNING_PFX_BASE64` (el .pfx en Base64: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("cert.pfx"))`) y
  `SIGNING_PFX_PASSWORD`.
- Si el certificado está en un **token USB** o en un servicio de firma en la nube con su propia herramienta, el
  workflow no puede usarlo directamente: publica el release, firma `LanCast.exe` en tu PC con `signtool` y súbelo
  reemplazando el del release, actualizando la línea `SHA-256:` de las notas con el hash del archivo firmado.

## App Android

Android exige que **todas las versiones de la app se firmen con la misma clave**; si la clave se pierde, los teléfonos
no podrán actualizar la app (habría que desinstalarla e instalar de nuevo).

- La clave está en `%USERPROFILE%\.lancast\lancast-android-release.jks` y su contraseña en `android/keystore.properties`
  (fuera de git). **Haz una copia de seguridad de los dos archivos** en un lugar seguro.
- Para que el workflow publique `LanCast.apk` firmado en cada versión, crea en GitHub los secretos
  `ANDROID_KEYSTORE_BASE64` (el .jks en Base64: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("lancast-android-release.jks"))`)
  y `ANDROID_KEYSTORE_PASSWORD`. Sin ellos, el release sale sin APK y se puede subir a mano.

## Verificar

En el resumen del workflow aparece "Firmado por: …". En el PC: clic derecho en `LanCast.exe` → *Propiedades* →
*Firmas digitales*, o en LanCast: *Diagnóstico* (arriba dice "Firma digital válida: …").

## Publicar una versión

1. Cambia `VERSION` en `CMakeLists.txt` y agrega la sección `## X.Y.Z` en `CHANGELOG.md`.
2. Sube los cambios a `main` y crea el tag: `git tag vX.Y.Z` y `git push origin vX.Y.Z`. También puedes ir a
   *Actions → Release → Run workflow* y escribir la versión.
3. El workflow compila, prueba, firma, agrega `SHA-256:` a las notas y publica `LanCast.exe` y `LanCast.pdb`.
