# God of War PSP en Linux

Chains of Olympus y Ghost of Sparta nativos en Linux, para jugar desde Steam. Es un port de [gow-psp-recomp](https://github.com/DrummingBird1/gow-psp-recomp), que es para Windows.

Proyecto no oficial, sin relación con Sony ni con Santa Monica Studio. No trae ningún juego: necesitás tu propio disco en `.iso` o `.chd`.

## Instalar

1. Bajá el AppImage de [Releases](https://github.com/AngeloAscheri/portgowpsplinux/releases) y hacelo ejecutable.
2. Al lado del AppImage creá `roms/chains-of-olympus/` y `roms/ghost-of-sparta/`, y poné ahí tus discos.
3. Cerrá Steam y abrí el AppImage. La primera vez tarda unos minutos.
4. Abrí Steam: vas a ver "God of War PSP" en la biblioteca.

Sirven los discos de EE. UU. y los de Europa. Los logros de RetroAchievements funcionan solo con los europeos.

Más detalles en [docs/linux.md](docs/linux.md).

## Créditos

- [gow-psp-recomp](https://github.com/DrummingBird1/gow-psp-recomp) de DrummingBird1: el proyecto original, para Windows. Su README está en [README-original.md](README-original.md).
- [PPSSPP](https://www.ppsspp.org/): referencia del hardware de la PSP; partes portadas al runtime, su decodificador de audio, PrxDecrypter y xBRZ.
- libkirk, libchdr, SDL3, OpenH264, [rcheevos](https://github.com/RetroAchievements/rcheevos) y [stb_truetype](https://github.com/nothings/stb).
- Fuente [Cinzel](https://fonts.google.com/specimen/Cinzel) (SIL Open Font License).

## Licencia

GPL-3.0 o posterior ([LICENSE](LICENSE)). Las bibliotecas de terceros se bajan al compilar y conservan sus licencias; las de lo que trae el AppImage están en [packaging/linux/licenses](packaging/linux/licenses/README.txt).
