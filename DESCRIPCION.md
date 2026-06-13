# Rufus Linux

`rufus-linux` es la versión Linux de Rufus, la conocida utilidad para crear discos USB booteables.

## ¿De qué se trata?

Este proyecto permite compilar Rufus en Linux para generar un ejecutable nativo (`rufus-linux`) que puede crear y formatear unidades USB de arranque. El código fuente contiene herramientas y librerías necesarias para crear medios de instalación, incluyendo soporte para DOS, BIOS/UEFI, imagen ISO y varios formatos de archivo.

## Características principales

- Formateo de unidades USB, tarjetas y discos virtuales.
- Creación de medios booteables a partir de ISOs de Windows, Linux y otras imágenes.
- Soporte para BIOS y UEFI.
- Soporte de sistemas de archivos FAT/FAT32/NTFS/UDF/exFAT.
- Soporte para archivos y particiones persistentes de Linux.
- Validación de medios de arranque y comprobación de errores.
- Compilación con GTK3 y OpenSSL en Linux.

## Compilación en Linux

En Linux se compila con los comandos:

```sh
./configure --disable-debug
make -j$(nproc)
```

El binario resultante se genera en `src/rufus-linux`.

## Integración con GitHub Actions

Este proyecto ahora incluye un workflow de GitHub Actions que compila el binario en contenedores Linux para:

- Debian
- Arch Linux
- openSUSE

El workflow también publica los paquetes resultantes en las releases de GitHub cuando se crea una etiqueta (`tag`).

## ¿Cómo usarlo?

1. Clona el repositorio.
2. Ejecuta `./configure --disable-debug`.
3. Ejecuta `make -j$(nproc)`.
4. El ejecutable `src/rufus-linux` estará listo para usarse.

## Notas

- Esta descripción es para el proyecto de compilación en Linux.
- La publicación automática en GitHub Releases requiere que el repositorio tenga el workflow habilitado y que el token `GITHUB_TOKEN` esté presente en la acción.
