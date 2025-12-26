# Proyecto MSX PicoVerse 2040

|PicoVerse Frontal|PicoVerse Trasero|
|---|---|
|![PicoVerse Frontal](/images/2025-12-02_20-05.png)|![PicoVerse Trasero](/images/2025-12-02_20-06.png)|

El PicoVerse 2040 es un cartucho para MSX basado en Raspberry Pi Pico que utiliza firmware reemplazable para ampliar las capacidades del ordenador. Al cargar diferentes imágenes de firmware, el cartucho puede ejecutar juegos y aplicaciones de MSX y emular dispositivos de hardware adicionales (como mappers de ROM, RAM extra o interfaces de almacenamiento), añadiendo efectivamente periféricos virtuales al MSX. Uno de estos firmwares es el sistema MultiROM, que proporciona un menú en pantalla para navegar y lanzar múltiples títulos de ROM almacenados en el cartucho.

El cartucho también puede exponer el puerto USB-C de la Pico como un dispositivo de almacenamiento masivo, lo que permite copiar ROMs, DSKs y otros archivos directamente desde un PC con Windows o Linux al cartucho.

Estas son las características disponibles en la versión actual del cartucho PicoVerse 2040:

* Sistema de menú MultiROM para seleccionar y lanzar ROMs de MSX.
* Opción de menú en línea con soporte para Nextor OS.
* Soporte de dispositivo de almacenamiento masivo USB para cargar ROMs y DSKs.
* Soporte para varios mappers de ROM de MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidad con sistemas MSX, MSX2 y MSX2+.

## Manual del Creador de UF2 MultiROM

Esta sección documenta la herramienta de consola `multirom` utilizada para generar imágenes UF2 (`multirom.uf2`) que programan el cartucho PicoVerse 2040.

En su comportamiento predeterminado, la herramienta `multirom` escanea archivos ROM de MSX en un directorio y los empaqueta en una sola imagen UF2 que se puede flashear en la Raspberry Pi Pico. La imagen resultante suele contener el firmware de la Pico, la ROM del menú MSX de MultiROM, un área de configuración que describe cada entrada de ROM y los propios contenidos de las ROM.

> **Nota:** Se puede incluir un máximo de 128 ROMs en una sola imagen, con un límite de tamaño total de aproximadamente 16 MB.

Dependiendo de las opciones proporcionadas, `multirom` también puede producir imágenes UF2 que arrancan directamente en un firmware personalizado en lugar del menú MultiROM. Por ejemplo, se pueden usar opciones para producir archivos UF2 con un firmware que implementa el Nextor OS. En este modo, el puerto USB-C de la Pico se puede usar como un dispositivo de almacenamiento masivo para cargar ROMs, DSKs y otros archivos desde Nextor (por ejemplo, a través de SofaRun).

## Descripción general

Si se ejecuta sin ninguna opción, la herramienta `multirom.exe` escanea el directorio de trabajo actual en busca de archivos ROM de MSX (`.ROM` o `.rom`), analiza cada ROM para adivinar el tipo de mapper, construye una tabla de configuración que describe cada ROM (nombre, byte de mapper, tamaño, desplazamiento) e incrusta esta tabla en una sección de la ROM del menú de MSX. Luego, la herramienta concatena el binario del firmware de la Pico, la sección del menú, el área de configuración y los contenidos de las ROM, y serializa toda la imagen en un archivo UF2 llamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

El archivo UF2 (normalmente multirom.uf2) se puede copiar a un dispositivo de almacenamiento masivo USB de la Pico para flashear la imagen combinada. Debe conectar la Pico mientras presiona el botón BOOTSEL para entrar en el modo de flasheo UF2. Luego aparece una nueva unidad USB llamada `RPI-RP2`, y puede copiar `multirom.uf2` en ella. Una vez completada la copia, puede desconectar la Pico e insertar el cartucho en su MSX para arrancar el menú MultiROM.

Los nombres de los archivos ROM se utilizan para nombrar las entradas en el menú de MSX. Hay un límite de 50 caracteres por nombre. Se utiliza un efecto de desplazamiento para mostrar nombres más largos en el menú de MSX, pero si el nombre supera los 50 caracteres, se truncará.

![alt text](/images/multirom_2040_menu.png)

> **Nota:** Para usar una memoria USB, es posible que necesite un adaptador o cable OTG. Esto se puede usar para convertir el puerto USB-C en un puerto USB-A hembra estándar.

> **Nota:** La opción `-n` incluye una ROM Nextor integrada experimental que funciona en modelos específicos de MSX2. Esta versión utiliza un método de comunicación diferente (basado en puertos de E/S) y puede no funcionar en todos los sistemas.

## Uso de la línea de comandos

Por ahora solo se proporcionan ejecutables para Microsoft Windows (`multirom.exe`).

### Uso básico:

```
multirom.exe [opciones]
```

### Opciones:
- `-n`, `--nextor` : Incluye la ROM NEXTOR integrada beta de la configuración y las salidas. Esta opción es todavía experimental y en este momento solo funciona en modelos específicos de MSX2.
- `-h`, `--help`   : Muestra la ayuda de uso y sale.
- `-o <nombre_archivo>`, `--output <nombre_archivo>` : Establece el nombre del archivo UF2 de salida (el valor predeterminado es `multirom.uf2`).
- Si necesita forzar un tipo de mapper específico para un archivo ROM, puede añadir una etiqueta de mapper antes de la extensión `.ROM` en el nombre del archivo. La etiqueta no distingue entre mayúsculas y minúsculas. Por ejemplo, nombrar un archivo `Knight Mare.PL-32.ROM` fuerza el uso del mapper PL-32 para esa ROM. Las etiquetas como `SYSTEM` se ignoran. La lista de etiquetas posibles que se pueden usar es: `PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16`

### Ejemplos
- Produce el archivo multirom.uf2 con el menú MultiROM y todos los archivos `.ROM` en el directorio actual. Puede ejecutar la herramienta usando el símbolo del sistema o simplemente haciendo doble clic en el ejecutable:
  ```
  multirom.exe
  ```

## Cómo funciona (nivel alto)

1. La herramienta escanea el directorio de trabajo actual en busca de archivos que terminen en `.ROM` o `.rom`. Para cada archivo:
   - Extrae un nombre para mostrar (nombre de archivo sin extensión, truncado a 50 caracteres).
   - Obtiene el tamaño del archivo y valida que esté entre `MIN_ROM_SIZE` y `MAX_ROM_SIZE`.
   - Llama a `detect_rom_type()` para determinar heurísticamente el byte del mapper a usar en la entrada de configuración. Si hay una etiqueta de mapper en el nombre del archivo, esta anula la detección.
   - Si falla la detección del mapper, se omite el archivo.
   - Serializa el registro de configuración por ROM (nombre de 50 bytes, mapper de 1 byte, tamaño de 4 bytes LE, desplazamiento de flash de 4 bytes LE) en el área de configuración.
2. Después del escaneo, la herramienta concatena (en orden): el binario del firmware de la Pico integrado, una sección inicial de la ROM del menú MSX (bytes `MENU_COPY_SIZE`), el área de configuración completa (bytes `CONFIG_AREA_SIZE`), la ROM NEXTOR opcional y luego los contenidos de las ROM descubiertas en el orden de descubrimiento.
3. El contenido combinado se escribe como un archivo UF2 llamado `multirom.uf2` usando `create_uf2_file()`, que produce bloques UF2 de 256 bytes dirigidos a la dirección de flash de la Pico `0x10000000`.

## Heurística de detección de mapper
- `detect_rom_type()` implementa una combinación de comprobaciones de firma (cabecera "AB", etiquetas `ROM_NEO8` / `ROM_NE16`) y escaneo heurístico de códigos de operación y direcciones para elegir mappers comunes de MSX, incluyendo (pero no limitado a):
  - Plain 16KB (byte de mapper 1) — comprobación de cabecera AB de 16KB
  - Plain 32KB (byte de mapper 2) — comprobación de cabecera AB de 32KB
  - Mapper Linear0 (byte de mapper 4) — comprobación de diseño AB especial
  - NEO8 (byte de mapper 8) y NEO16 (byte de mapper 9)
  - Konami, Konami SCC, ASCII8, ASCII16 y otros mediante puntuación ponderada
- Si no se puede detectar de forma fiable ningún mapper, la herramienta omite la ROM e informa "mapper no compatible". Recuerde que puede forzar un mapper mediante una etiqueta en el nombre del archivo. Las etiquetas no distinguen entre mayúsculas y minúsculas y se enumeran a continuación.
- Solo se admiten los siguientes mappers en el área de configuración y el menú: `PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16`

## Uso del menú Selector de ROM de MSX

Cuando enciende el MSX con el cartucho PicoVerse 2040 insertado, aparece el menú MultiROM, mostrando la lista de ROMs disponibles. Puede navegar por el menú usando las teclas de flecha del teclado.

![alt text](/images/multirom_2040_menu.png)

Use las teclas de flecha Arriba y Abajo para mover el cursor de selección a través de la lista de ROMs. Si tiene más de 19 ROMs, use las teclas de flecha laterales (Izquierda y Derecha) para desplazarse por las páginas de entradas.

Presione la tecla Enter o Espacio para lanzar la ROM seleccionada. El MSX intentará arrancar la ROM usando los ajustes de mapper apropiados.

En cualquier momento mientras esté en el menú, puede presionar la tecla H para leer la pantalla de ayuda con instrucciones básicas. Presione cualquier tecla para volver al menú principal.

## Uso de Nextor con el cartucho PicoVerse 2040

La opción -n de la herramienta multirom incluye una ROM Nextor integrada experimental que se puede usar en modelos específicos de MSX2. Sin embargo, esta opción aún está en desarrollo y puede no funcionar en todos los sistemas. Esta versión utiliza un método diferente (basado en puertos de E/S) para comunicarse con el MSX, lo que le permite funcionar en sistemas donde la ranura del cartucho no es primaria, por ejemplo con expansores de ranuras.

Se pueden encontrar más detalles sobre el protocolo utilizado para comunicarse con el ordenador MSX en el documento Nextor-Pico-Bridge-Protocol.md. 

### Cómo preparar una memoria USB para Nextor

Para preparar una memoria USB para su uso con Nextor en el cartucho PicoVerse 2040, siga estos pasos:

1. Conecte la memoria USB a su PC.
2. Cree una partición FAT16 de 4 GB como máximo en la memoria USB. Puede usar la herramienta integrada de Nextor OS (CALL FDISK mientras está en MSX Basic) o software de particionamiento de terceros para hacer esto.
3. Copie los archivos del sistema Nextor al directorio raíz de la partición FAT16. Puede obtener los archivos del sistema Nextor de la distribución o repositorio oficial de Nextor. También necesita el archivo COMMAND2 para el intérprete de comandos:
   1. [Página de descarga de Nextor](https://github.com/Konamiman/Nextor/releases)
   2. [Página de descarga de Command2](http://www.tni.nl/products/command2.html)
4. Copie cualquier ROM de MSX (archivos `.ROM`) o imágenes de disco (archivos `.DSK`) que desee usar con Nextor al directorio raíz de la memoria USB.
5. Instale SofaRun o cualquier otro lanzador compatible con Nextor en la memoria USB si planea usarlo para lanzar ROMs y DSKs. Puede descargar SofaRun de su fuente oficial aquí: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Expulse de forma segura la memoria USB de su PC.
7. Conecte la memoria USB al cartucho PicoVerse 2040 usando un adaptador o cable USB OTG si es necesario.

> **Nota:** No todas las memorias USB son compatibles con el cartucho PicoVerse 2040. Si encuentra problemas, intente usar una marca o modelo diferente de memoria USB.
>
> **Nota:** Recuerde que Nextor necesita un mínimo de 128 KB de RAM para funcionar.

## Ideas de mejoras
- Mejorar la heurística de detección del tipo de ROM para cubrir más mappers y casos límite.
- Implementar una pantalla de configuración para cada entrada de ROM (nombre, anulación de mapper, etc.).
- Añadir soporte para más mappers de ROM.
- Implementar un menú gráfico con mejor navegación y visualización de información de la ROM.
- Añadir soporte para guardar/cargar la configuración del menú para preservar las preferencias del usuario.
- Soportar también archivos DSK, con las entradas de configuración adecuadas.
- Añadir soporte para temas personalizados para el menú.
- Permitir la descarga de ROMs desde URLs de Internet e incrustarlas directamente.
- Permitir el uso del joystick para navegar por el menú.

## Problemas conocidos

- La inclusión de la ROM Nextor integrada es todavía experimental y puede no funcionar en todos los modelos de MSX2.
- Algunas ROMs con mappers poco comunes pueden no detectarse correctamente y se omitirán a menos que se use una etiqueta de mapper válida para forzar la detección.
- La herramienta MultiROM actualmente solo es compatible con Windows. Las versiones para Linux y macOS aún no están disponibles.
- Actualmente, la herramienta no valida la integridad de los archivos ROM más allá del tamaño y las comprobaciones básicas de cabecera. Las ROMs corruptas pueden provocar un comportamiento inesperado.
- Debido a la naturaleza de la herramienta MultiROM (incrustar múltiples archivos en un solo UF2), algunos programas antivirus pueden marcar el ejecutable como sospechoso. Esto es un falso positivo; asegúrese de descargar la herramienta de una fuente de confianza.
- El menú MultiROM no admite archivos DSK; solo se enumeran y lanzan archivos ROM.
- Actualmente, la herramienta no admite subdirectorios; solo se procesan los archivos ROM en el directorio de trabajo actual.
- La memoria flash de la pico puede desgastarse después de muchos ciclos de escritura. Evite el flasheo excesivo del cartucho.

## Modelos de MSX probados

El cartucho PicoVerse 2040 con firmware MultiROM ha sido probado en los siguientes modelos de MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Funcionamiento verificado |
| Gradiente Expert | MSX1 | OK | Funcionamiento verificado |
| JFF MSX | MSX1 | OK | Funcionamiento verificado |
| MSX Book | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |
| MSX One | MSX1 | No OK | Cartucho no reconocido |
| National FS-4500 | MSX1 | OK | Funcionamiento verificado |
| Omega MSX | MSX2+ | OK | Funcionamiento verificado |
| Panasonic FS-A1GT | TurboR | OK | Funcionamiento verificado |
| Panasonic FS-A1ST | TurboR | OK | Funcionamiento verificado |
| Panasonic FS-A1WX | MSX2+ | OK | Funcionamiento verificado |
| Panasonic FS-A1WSX | MSX2+ | OK | Funcionamiento verificado |
| Sharp HotBit HB8000 | MSX1 | OK | Funcionamiento verificado |
| SMX-HB | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |
| Sony HB-F1XD | MSX2 | OK | Funcionamiento verificado |
| Sony HB-F1XDJ | MSX2 | OK | Funcionamiento verificado |
| Sony HB-F1XV | MSX2+ | OK | Funcionamiento verificado |
| TRHMSX | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |
| uMSX | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |
| Yamaha YIS604 | MSX1 | OK | Funcionamiento verificado |

La ROM Nextor integrada experimental (opción `-n`) ha sido probada en los siguientes modelos de MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | No OK | USB no detectado |
| Sony HB-F1XD | MSX2 | No OK | Error al leer sector de disco |
| TRHMSX | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |
| uMSX | MSX2+ (clon FPGA) | OK | Funcionamiento verificado |

Autor: Cristiano Almeida Goncalves
Última actualización: 25/12/2025
