// Cliente Android de LanCast.
//   :core  Protocolo, cifrado, FEC y sesión (Kotlin puro: se prueba en el PC contra el host real).
//   :app   La app: pantallas, decodificador de video por hardware, audio, toque y teclado.
plugins {
    id("com.android.application") version "9.4.1" apply false
    kotlin("jvm") version "2.4.21" apply false
}
