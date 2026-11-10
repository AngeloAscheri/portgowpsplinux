# Linux (experimental)

Port nativo en desarrollo para Bazzite y SteamOS. Se instala con un AppImage o se compila en un contenedor. Probado solo en Bazzite con una Radeon RX 6700 XT (RADV): **en SteamOS no se probó**; el AppImage está pensado para correr ahí (glibc 2.38, nada instalado aparte de `python3`), pero no está verificado.

Se necesita tu propio disco, en `.iso` o `.chd`: de EE. UU., `UCUS98653` (Chains of Olympus) o `UCUS98737` (Ghost of Sparta), o europeo, `UCES00842` o `UCES01401`. Va en `roms/chains-of-olympus/` o `roms/ghost-of-sparta/` (lo explica `roms/LEEME.txt`); git no sube nada de ahí salvo esos textos.

## Con el AppImage

`God_of_War_PSP-x86_64.AppImage` trae todo lo necesario para compilar: Zig, CMake, Ninja y SDL3 ya compilado. Usa el `python3` del sistema, que Bazzite y SteamOS traen. No hace falta contenedor.

1. Poné tus discos en una carpeta `roms/` al lado del AppImage, con `chains-of-olympus/` y `ghost-of-sparta/` adentro, o en `~/.local/share/gow-psp/roms/`.
2. Cerrá Steam y ejecutá el AppImage, por ejemplo desde una terminal: `./God_of_War_PSP-x86_64.AppImage`. Busca los discos, compila los juegos (la primera vez tarda unos minutos) y agrega "God of War PSP" a Steam.
3. Abrí Steam: "God of War PSP" está en la biblioteca, con sus imágenes.

Todo lo que escribe va en `~/.local/share/gow-psp/` (`work/` con lo compilado, una carpeta por juego con partidas y opciones). Acepta los mismos comandos que el launcher (`--menu`, `--play JUEGO`, `--build JUEGO`…).

Armarlo hace falta con Zig, CMake, Ninja y `appimagetool` oficiales ya desempaquetados. Antes hay que configurar una vez cualquier build (por ejemplo `--build coo` con el launcher) para que CMake baje las fuentes fijadas a `build/deps/`, y compilar SDL3 aparte, en el contenedor de "Compilar". SDL3 queda estático, y X11, Wayland y el audio se cargan al ejecutar, así que corre en SteamOS sin sus paquetes de desarrollo. El último paso va fuera del contenedor, porque `appimagetool` necesita el comando `file`, que Ubuntu no trae instalado y Bazzite sí. De Zig mete solo lo que usa `x86_64-linux-gnu`:

```bash
# en el contenedor
cmake -S build/deps/SDL3-* -B build/sdl3_build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
ninja -C build/sdl3_build
cmake --install build/sdl3_build --prefix build/sdl3_prebuilt

# fuera del contenedor
python3 -I packaging/linux/build_appimage.py --zig ZIG --cmake CMAKE --ninja NINJA/ninja --sdl3 build/sdl3_prebuilt --appimagetool APPIMAGETOOL
```

## Con el launcher

Dentro del contenedor (ver "Compilar"), desde la carpeta del repositorio:

```bash
python3 -I tools/launcher.pyw --scan-roms        # reconoce los discos de roms/ (también ~/.local/share/gow-psp/roms/)
python3 -I tools/launcher.pyw --build coo        # coo, gos, coo_eu o gos_eu: descifra, traduce y compila
python3 -I tools/launcher.pyw --menu             # el selector con los juegos compilados
python3 -I tools/launcher.pyw --add-to-steam     # con Steam cerrado: agrega "God of War PSP", que abre el selector
```

`--play JUEGO` abre un juego directo. Las partidas y las opciones van en `~/.local/share/gow-psp/<juego>/`. Si tenés los dos discos de un juego, el selector abre el europeo, que tiene logros; para el de EE. UU., poné `"prefer": {"gos": "gos"}` en `build/launcher.json` (`coo` para Chains). Con un solo disco abre ese. Toma el idioma del sistema si es inglés, francés o español. El acceso de Steam y los juegos corren fuera del contenedor, con el Python del sistema.

## Compilar

El contenedor Ubuntu 24.04 mantiene la glibc en 2.39 o menos. El binario corre después fuera de él.

```bash
distrobox create --name gow-dev --image docker.io/library/ubuntu:24.04
distrobox enter gow-dev -- sudo apt-get install -y clang lld cmake ninja-build pkg-config python3 python3-venv \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev \
  libxfixes-dev libxinerama-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libdrm-dev libgbm-dev libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libudev-dev libdbus-1-dev libibus-1.0-dev libvulkan-dev mesa-vulkan-drivers vulkan-tools
```

Dentro del contenedor, para Chains of Olympus. Para Ghost of Sparta, cambiar el ID por `UCUS98737` y la carpeta por `build/linux_gos`:

```bash
export CC=clang CXX=clang++
cmake -S . -B build/linux_tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DGOW_TOOLS_ONLY=ON
ninja -C build/linux_tools gow_disc

mkdir -p build/eboot
build/linux_tools/gow_disc --disc TU_DISCO.iso --id --decrypt-eboot build/eboot/UCUS98653_EBOOT.BIN

python3 -I recomp/recomp.py --elf build/eboot/UCUS98653_EBOOT.BIN \
  --ghidra re/out/recomp/UCUS98653_EBOOT.BIN.recomp.json --nids re/out/psp_nids_ppsspp.tsv \
  --out build/gen/UCUS98653_full --difftest 0 --boot 0 --all

cmake -S . -B build/linux_coo -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAME=UCUS98653_full \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
ninja -C build/linux_coo
```

Esto genera `gow_rt_UCUS98653_full` y el selector `gow_menu`.

### Discos europeos

El repositorio trae los datos de traducción de los discos europeos (`re/out/recomp/UCES*.recomp.json`), así que se compilan igual que los de EE. UU., con su ID y otra carpeta de build: con el launcher, `--build coo_eu` o `--build gos_eu`. Los dos traen español: `--language es`.

Esos datos salen de los de EE. UU.; para rehacerlos hace falta también el EBOOT de EE. UU. del mismo juego. Ejemplo con Chains, `UCES00842`; para Ghost of Sparta, `UCUS98737` y `UCES01401`:

```bash
build/linux_tools/gow_disc --disc DISCO_EUROPEO.iso --id --decrypt-eboot build/eboot/UCES00842_EBOOT.BIN
mkdir -p build/eu
python3 -I tools/eu_match.py build/eboot/UCUS98653_EBOOT.BIN build/eboot/UCES00842_EBOOT.BIN \
  re/out/recomp/UCUS98653_EBOOT.BIN.recomp.json build/eu/map_UCES00842.json
python3 -I tools/eu_recomp_json.py re/out/recomp/UCUS98653_EBOOT.BIN.recomp.json \
  build/eu/map_UCES00842.json build/eboot/UCES00842_EBOOT.BIN build/eu/UCES00842_EBOOT.BIN.recomp.json
```

El resultado tiene que ser igual al de `re/out/recomp/`.

## Jugar

El launcher arma estos comandos solo. A mano:

```bash
build/linux_coo/gow_rt_UCUS98653_full \
  --image build/gen/UCUS98653_full/image.bin --boot --disc TU_DISCO.iso \
  --ms0 "$HOME/.local/share/gow-psp/coo/ms0" \
  --settings "$HOME/.local/share/gow-psp/coo/settings.ini" \
  --window --fullscreen --renderer gpu --gpu-driver vulkan --res-scale 4 --vsync --fps 60
```

Creá antes la carpeta de `--ms0`. En Ghost of Sparta se agrega `--language es` para español (también `en` y `fr`).

### Desde Steam

Lo más simple es `--add-to-steam` del launcher (o el AppImage), que agrega "God of War PSP" con sus imágenes y abre el selector. A mano: agregá un script con ese comando como juego que no es de Steam. En un escritorio Wayland, el script tiene que exportar `SDL_VIDEODRIVER=x11` antes de abrir el juego: sin eso Steam activa la configuración de otra app y el joystick no responde. El launcher ya lo hace.

### Selector

```bash
build/linux_coo/gow_menu --entry "Chains of Olympus" IMAGEN.png SCRIPT_DEL_JUEGO --settings AJUSTES_COO.ini \
                         --entry "Ghost of Sparta" IMAGEN.png SCRIPT_DEL_JUEGO --settings AJUSTES_GOS.ini
```

Stick o cruceta para elegir, A para jugar, B para salir. Al cerrar el juego vuelve al selector. X muestra los logros del juego, si tiene (ver "Logros"). Y abre las opciones del juego elegido, las mismas del menú del juego: arriba y abajo eligen, izquierda y derecha cambian el valor, B vuelve. `--settings` tiene que ser el mismo archivo que recibe el juego. El selector escribe solo las opciones que cambiaste; las demás siguen con los valores del script. La imagen sale de tu disco:

```bash
build/linux_tools/gow_disc --disc TU_DISCO.iso --extract disc0:/PSP_GAME/PIC1.PNG IMAGEN.png
```

### Opciones dentro del juego

**Back + clic del stick izquierdo,** o F10. Resolución, FPS, V-Sync, MSAA, FXAA, anisotrópico, nitidez, aspecto, CRT, suavizar HUD y pantalla completa. "Suavizar HUD" agranda los iconos, el texto y las barras con xBRZ para que no se vean los píxeles a resoluciones altas. Se guardan en el archivo de `--settings`. MSAA se aplica al volver a abrir el juego; lo demás, al cerrar el menú.

Salir del juego: Back + Start durante 2 segundos, o desde este menú.

### Logros (RetroAchievements)

Solo en modo normal (softcore) y solo con los **discos europeos**, que son los que RetroAchievements tiene registrados (los de EE. UU. avisan que no tienen logros). Hace falta una cuenta de [RetroAchievements](https://retroachievements.org) y la `libcurl` del sistema, que Bazzite y SteamOS traen; sin ella o sin internet el juego abre igual.

```bash
python3 -I tools/ra_login.py   # pide usuario y contraseña; guarda solo el token en ~/.local/share/gow-psp/retroachievements.ini
```

Con ese archivo, el launcher abre los juegos con `--retroachievements`. Al ganar un logro aparece un aviso arriba, con su ícono, sin pausar. La lista (ganados y pendientes) está en el menú del juego, en "Logros", y en el selector con X. Los íconos se bajan una vez a `~/.local/share/gow-psp/ra_images/`.

Con `--warp`, `--load-level`, `--god` o una repetición con `--input`, o con `--ra-spectator`, el juego solo mira: evalúa los logros pero no envía nada.

## Problemas conocidos

- **HUD e iconos de botones en baja resolución:** son imágenes de la PSP, y la resolución interna no les agrega detalle. "Suavizar HUD" les quita lo pixelado.
- **Chains of Olympus en inglés con el disco de EE. UU.:** no trae otros idiomas. El europeo trae español.
- **Ghost of Sparta europeo:** pierde 1 jump table de 174 al generar sus datos; ese salto pasa por el camino lento, sin cambiar lo que hace el juego.
- **SteamOS sin probar:** el AppImage está pensado para correr ahí, pero solo se probó en Bazzite.

## Verificar un cambio

El renderer por software tiene que dar los mismos píxeles antes y después. Sin `--window` el resultado es determinista:

```bash
build/linux_coo/gow_rt_UCUS98653_full --image build/gen/UCUS98653_full/image.bin --boot --disc TU_DISCO.iso \
  --ms0 CARPETA_VACIA --frames-dir SALIDA --frames-every 200 --max-frames 3000 \
  --cpu-ms 600000 --wall-ms 600000 --wav SALIDA/mix.wav
python3 -I tools/check_frames.py REFERENCIA SALIDA
sha256sum SALIDA/mix.wav
```

Linux y Windows pueden no coincidir entre sí, así que la referencia tiene que ser una corrida de Linux anterior al cambio.
