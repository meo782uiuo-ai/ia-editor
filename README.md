# IA Editor (mod de Geode)

Boton negro "IA" en el editor -> menu de chat -> la IA (Gemini) construye objetos en tu nivel y puede ver un mapa de lo que hay.

## Compilar desde el movil (sin PC)
1. Crea cuenta en github.com y un repositorio nuevo (publico o privado), por ejemplo `ia-editor`.
2. Sube estos archivos respetando las rutas:
   - `mod.json`, `CMakeLists.txt`, `logo.png`, `README.md`
   - `src/main.cpp`
   - `.github/workflows/build.yml`
   (En la web: Add file > Create new file, escribe la ruta completa en el nombre, por ejemplo `src/main.cpp`, y pega el contenido. El `logo.png` se sube con Upload files.)
3. Abre la pestana **Actions**. Se ejecuta solo al subir; espera unos minutos.
4. Si sale verde, entra a la ejecucion y baja el artefacto **Mod compilado**. Dentro esta el archivo `.geode`.
5. Copia el `.geode` a la carpeta `Android/media/com.geode.launcher/game/geode/mods/` (o instalalo desde Geode) y abre el juego.

## Configurar
1. Consigue una API key gratis en Google AI Studio (aistudio.google.com).
2. En el juego: Geode > mods > IA Editor > ajustes. Pega la key en "API key de Gemini".
3. Abre el editor de un nivel (usa una copia, no tu nivel importante) y toca el boton negro.

## Si falla al compilar
Copia las lineas rojas del log de Actions y mandaselas a Claude para que las corrija.
Lo mas probable: el campo `"geode"` de `mod.json` debe coincidir con la version de Geode que usas.
