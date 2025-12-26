# Proyecto MSX PicoVerse 2040

|PicoVerse Frontal|PicoVerse Trasera|
|---|---|
|![PicoVerse Frontal](/images/2025-12-02_20-05.png)|![PicoVerse Trasera](/images/2025-12-02_20-06.png)|

El PicoVerse 2040 es un cartucho para MSX basado en Raspberry Pi Pico que utiliza firmware reemplazable para ampliar las capacidades del ordenador. Al cargar diferentes imágenes de firmware, el cartucho puede ejecutar juegos y aplicaciones de MSX y emular dispositivos de hardware adicionales (como mappers de ROM, RAM extra o interfaces de almacenamiento), añadiendo efectivamente periféricos virtuales al MSX. Uno de esos firmwares es el sistema MultiROM, que proporciona un menú en pantalla para explorar y lanzar múltiples títulos ROM almacenados en el cartucho.

El cartucho también puede exponer el puerto USB‑C del Pico como un dispositivo de almacenamiento masivo, permitiendo copiar ROMs, DSKs y otros archivos directamente desde un PC con Windows o Linux al cartucho.

Estas son las características disponibles en la versión actual del cartucho PicoVerse 2040:

* Sistema de menú MultiROM para seleccionar y lanzar ROMs de MSX.
* Opción de menú en línea con soporte Nextor.
* Soporte de dispositivo de almacenamiento masivo USB para cargar ROMs y DSKs.
* Soporte para varios mappers de ROM de MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidad con sistemas MSX, MSX2 y MSX2+.

## Manual del creador de UF2 de MultiROM

Esta sección documenta la herramienta de consola `multirom` utilizada para generar imágenes UF2 (`multirom.uf2`) que programan el cartucho PicoVerse 2040.

En su comportamiento por defecto, la herramienta `multirom` escanea archivos ROM de MSX en un directorio y los empaqueta en una sola imagen UF2 que puede ser flasheada al Raspberry Pi Pico. La imagen resultante típicamente contiene el binario del firmware del Pico, la ROM del menú MultiROM, un área de configuración que describe cada entrada de ROM y las cargas útiles de las ROMs.

> **Nota:** Se puede incluir un máximo de 128 ROMs en una sola imagen, con un límite de tamaño total de aproximadamente 16 MB.

Dependiendo de las opciones provistas, `multirom` también puede producir imágenes UF2 que arranquen directamente en un firmware personalizado en lugar del menú MultiROM. Por ejemplo, las opciones pueden usarse para producir archivos UF2 con un firmware que implemente Nextor. En este modo, el puerto USB‑C del Pico puede usarse como un dispositivo de almacenamiento masivo para cargar ROMs, DSKs y otros archivos desde Nextor (por ejemplo, mediante SofaRun).

## Visión general

Si se ejecuta sin opciones, la herramienta `multirom.exe` escanea el directorio de trabajo actual en busca de archivos ROM de MSX (`.ROM` o `.rom`), analiza cada ROM para adivinar el tipo de mapper, construye una tabla de configuración que describe cada ROM (nombre, byte de mapper, tamaño, offset) e inserta esta tabla en una porción de la ROM de menú de MSX. La herramienta luego concatena el blob del firmware del Pico, la porción del menú, el área de configuración y las cargas útiles de las ROMs y serializa toda la imagen en un archivo UF2 llamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

El archivo UF2 (normalmente multirom.uf2) puede copiarse a un dispositivo de almacenamiento masivo USB del Pico para flashear la imagen combinada. Debes conectar el Pico mientras mantienes presionado el botón BOOTSEL para entrar en el modo de flasheo UF2. Entonces aparece una nueva unidad USB llamada `RPI-RP2`, y puedes copiar `multirom.uf2` en ella. Después de completar la copia, desconecta el Pico e inserta el cartucho en tu MSX para arrancar el menú MultiROM.

Los nombres de los archivos ROM se usan para nombrar las entradas en el menú de MSX. Hay un límite de 50 caracteres por nombre. Se usa un efecto de desplazamiento para mostrar nombres más largos en el menú de MSX, pero si el nombre excede los 50 caracteres se truncará.

![alt text](/images/multirom_2040_menu.png)


> **Nota:** Para usar una memoria USB puede que necesites un adaptador o cable OTG. Eso puede usarse para convertir el puerto USB‑C a un conector USB‑A hembra estándar.

> **Nota:** La opción `-n` incluye una ROM Nextor incrustada experimental que funciona en modelos MSX2 específicos. Esta versión usa un método de comunicación diferente (basado en puertos IO) y puede no funcionar en todos los sistemas.

## Uso por línea de comandos

Solo se proporcionan ejecutables para Microsoft Windows por ahora (`multirom.exe`).

### Uso básico:

```
multirom.exe [options]
```

### Opciones:
- `-n`, `--nextor` : Incluye la ROM NEXTOR beta incrustada desde la configuración y en la salida. Esta opción sigue siendo experimental y en este momento solo funciona en modelos MSX2 específicos.
- `-h`, `--help`   : Muestra la ayuda de uso y sale.
- `-o <filename>`, `--output <filename>` : Establece el nombre de archivo de salida UF2 (por defecto es `multirom.uf2`).
- Si necesitas forzar un tipo de mapper específico para un archivo ROM, puedes añadir una etiqueta de mapper antes de la extensión `.ROM` en el nombre del archivo. La etiqueta no distingue mayúsculas de minúsculas. Por ejemplo, nombrar un archivo `Knight Mare.PL-32.ROM` fuerza el uso del mapper PL-32 para esa ROM. Etiquetas como `SYSTEM` son ignoradas. La lista de etiquetas posibles es: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

### Ejemplos
- Produce el archivo `multirom.uf2` con el menú MultiROM y todos los archivos `.ROM` en el directorio actual. Puedes ejecutar la herramienta usando el símbolo del sistema o simplemente haciendo doble clic en el ejecutable:
  ```
  multirom.exe
  ```
  
## Cómo funciona (alto nivel)

1. La herramienta escanea el directorio de trabajo actual en busca de archivos que terminen en `.ROM` o `.rom`. Para cada archivo:
   - Extrae un nombre para mostrar (nombre de archivo sin extensión, truncado a 50 caracteres).
   - Obtiene el tamaño del archivo y valida que esté entre `MIN_ROM_SIZE` y `MAX_ROM_SIZE`.
   - Llama a `detect_rom_type()` para determinar heurísticamente el byte de mapper a usar en la entrada de configuración. Si hay una etiqueta de mapper en el nombre del archivo, ésta anula la detección.
   - Si la detección del mapper falla, el archivo se omite.
   - Serializa el registro de configuración por-ROM (nombre de 50 bytes, 1 byte de mapper, 4 bytes de tamaño en orden LE, 4 bytes de offset en flash en orden LE) en el área de configuración.
2. Después del escaneo, la herramienta concatena (en orden): binario del firmware del Pico incrustado, una porción inicial de la ROM del menú de MSX (`MENU_COPY_SIZE` bytes), el área de configuración completa (`CONFIG_AREA_SIZE` bytes), opcionalmente la ROM NEXTOR, y luego las cargas útiles de las ROMs descubiertas en el orden de descubrimiento.
3. La carga útil combinada se escribe como un archivo UF2 llamado `multirom.uf2` usando `create_uf2_file()` que produce bloques UF2 de 256 bytes destinados a la dirección de flash del Pico `0x10000000`.

## Heurísticas de detección de mapper
- `detect_rom_type()` implementa una combinación de comprobaciones de firma (cabecera "AB", etiquetas `ROM_NEO8` / `ROM_NE16`) y un escaneo heurístico de opcodes y direcciones para elegir mappers comunes de MSX, incluyendo (pero no limitado a):
  - Plain 16KB (byte de mapper 1) — comprobación de cabecera AB de 16KB
  - Plain 32KB (byte de mapper 2) — comprobación de cabecera AB de 32KB
  - Mapper Linear0 (byte de mapper 4) — comprobación especial de diseño AB
  - NEO8 (byte de mapper 8) y NEO16 (byte de mapper 9)
  - Konami, Konami SCC, ASCII8, ASCII16 y otros mediante puntuación ponderada
- Si ningún mapper puede detectarse de forma fiable, la herramienta omite la ROM y reporta "unsupported mapper". Recuerda que puedes forzar un mapper mediante la etiqueta en el nombre de archivo. Las etiquetas no distinguen mayúsculas y minúsculas y se listan más arriba.
- Solo los siguientes mappers son compatibles en el área de configuración y el menú: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

## Uso del menú selector de ROMs de MSX

Cuando enciendes el MSX con el cartucho PicoVerse 2040 insertado, aparece el menú MultiROM mostrando la lista de ROMs disponibles. Puedes navegar por el menú usando las teclas de flecha del teclado.

![alt text](/images/multirom_2040_menu.png)

Usa las teclas de flecha Arriba y Abajo para mover el cursor de selección por la lista de ROMs. Si tienes más de 19 ROMs, usa las teclas laterales (Izquierda y Derecha) para desplazarte por páginas de entradas.

Pulsa Enter o la barra espaciadora para lanzar la ROM seleccionada. El MSX intentará arrancar la ROM usando los ajustes de mapper apropiados.

En cualquier momento mientras estás en el menú, puedes pulsar la tecla H para leer la pantalla de ayuda con instrucciones básicas. Pulsa cualquier tecla para volver al menú principal.

## Uso de Nextor con el cartucho PicoVerse 2040

La opción `-n` de la herramienta multirom incluye una ROM Nextor incrustada experimental que puede usarse en modelos MSX2 específicos. Sin embargo, esta opción aún está en desarrollo y puede no funcionar en todos los sistemas. Esta versión utiliza un método diferente (basado en puertos IO) para comunicarse con el MSX, lo que le permite funcionar en sistemas donde la ranura del cartucho no es primaria, por ejemplo con expansores de ranura.

Más detalles sobre el protocolo usado para comunicarse con el ordenador MSX se pueden encontrar en el documento Nextor-Pico-Bridge-Protocol.md. Los detalles de la comunicación con el firmware Sunrise IDE Nextor se encuentran en el documento Sunrise-Nextor.md.

### Cómo preparar una memoria USB para Nextor

1. Conecta la memoria USB a tu PC.
2. Crea una partición FAT16 de máximo 4GB en la memoria USB. Puedes usar la herramienta incorporada de Nextor (CALL FDISK mientras estás en MSX Basic) o software de particionado de terceros.
3. Copia los archivos de sistema Nextor al directorio raíz de la partición FAT16. Puedes obtener los archivos del sistema Nextor desde la distribución oficial o el repositorio. También necesitas el archivo COMMAND2 para el intérprete de comandos:
   1.  [Página de descargas de Nextor](https://github.com/Konamiman/Nextor/releases) 
   2.  [Página de descarga de Command2](http://www.tni.nl/products/command2.html)
4. Copia las ROMs de MSX (`.ROM`) o imágenes de disco (`.DSK`) que quieras usar con Nextor al directorio raíz de la memoria USB.
5. Instala SofaRun u otro lanzador compatible con Nextor en la memoria USB si planeas usarla para lanzar ROMs y DSKs. Puedes descargar SofaRun desde su fuente oficial aquí: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Expulsa la memoria USB de forma segura desde tu PC.
7. Conecta la memoria USB al cartucho PicoVerse 2040 usando un adaptador o cable OTG si es necesario.

> **Nota:** No todas las memorias USB son compatibles con el cartucho PicoVerse 2040. Si encuentras problemas, prueba con otra marca o modelo.
>
> **Nota:** Recuerda que Nextor necesita un mínimo de 128 KB de RAM para funcionar. 

## Ideas de mejoras
- Mejorar las heurísticas de detección de tipo de ROM para cubrir más mappers y casos extremos.
- Implementar una pantalla de configuración para cada entrada de ROM (nombre, anulación de mapper, etc).
- Añadir soporte para más mappers de ROM.
- Implementar un menú gráfico con mejor navegación y pantalla de información de ROM.
- Añadir soporte para guardar/cargar la configuración del menú para preservar preferencias del usuario.
- Soportar archivos DSK también, con entradas de configuración adecuadas.
- Añadir soporte para temas personalizados para el menú.
- Permitir la descarga de ROMs desde URLs de Internet e incrustarlas directamente.
- Permitir el uso del joystick para navegar por el menú.

## Problemas conocidos

- La inclusión de la ROM Nextor incrustada sigue siendo experimental y puede no funcionar en todos los modelos MSX2.
- Algunas ROMs con mappers poco comunes pueden no detectarse correctamente y serán omitidas a menos que se use una etiqueta válida para forzar la detección.
- La herramienta MultiROM actualmente solo soporta Windows. No hay versiones para Linux o macOS disponibles aún.
- La herramienta no valida actualmente la integridad de las ROMs más allá del tamaño y comprobaciones básicas de cabecera. Las ROMs corruptas pueden causar comportamiento inesperado.
- Debido a la naturaleza de la herramienta MultiROM (incrustar múltiples archivos en un solo UF2), algunos antivirus pueden marcar el ejecutable como sospechoso. Esto es un falso positivo; asegúrate de descargar la herramienta desde una fuente de confianza.
- El menú MultiROM no soporta archivos DSK; sólo se listan y lanzan archivos ROM.
- La herramienta no soporta subdirectorios actualmente; sólo se procesan ROMs en el directorio de trabajo actual.
- La memoria flash del pico puede desgastarse tras muchos ciclos de escritura. Evita re-flashear el cartucho en exceso.

## Modelos MSX probados

El cartucho PicoVerse 2040 con firmware MultiROM ha sido probado en los siguientes modelos MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Operación verificada |
| Gradiente Expert | MSX1 | OK | Operación verificada |
| JFF MSX | MSX1 | OK | Operación verificada |
| MSX Book | MSX2+ (clon FPGA) | OK | Operación verificada |
| MSX One | MSX1 | Not OK | Cartucho no reconocido |
| National FS-4500 | MSX1 | OK | Operación verificada |
| Omega MSX | MSX2+ | OK | Operación verificada |
| Panasonic FS-A1GT | TurboR | OK | Operación verificada |
| Panasonic FS-A1ST | TurboR | OK | Operación verificada |
| Panasonic FS-A1WX | MSX2+ | OK | Operación verificada |
| Panasonic FS-A1WSX | MSX2+ | OK | Operación verificada |
| Sharp HotBit HB8000 | MSX1 | OK | Operación verificada |
| SMX-HB | MSX2+ (clon FPGA) | OK | Operación verificada |
| Sony HB-F1XD | MSX2 | OK | Operación verificada |
| Sony HB-F1XDJ | MSX2 | OK | Operación verificada |
| Sony HB-F1XV | MSX2+ | OK | Operación verificada |
| TRHMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| uMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| Yamaha YIS604 | MSX1 | OK | Operación verificada |

La ROM Nextor incrustada experimental (opción `-n`) ha sido probada en los siguientes modelos MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB no detectado |
| Sony HB-F1XD | MSX2 | Not OK | Error leyendo sector de disco |
| TRHMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| uMSX | MSX2+ (clon FPGA) | OK | Operación verificada |

Autor: Cristiano Almeida Goncalves
Última actualización: 25/12/2025
# Proyecto MSX PicoVerse 2040

|PicoVerse Frontal|PicoVerse Trasera|
|---|---|
|![PicoVerse Front](/images/2025-12-02_20-05.png)|![PicoVerse Back](/images/2025-12-02_20-06.png)|

El PicoVerse 2040 es un cartucho para MSX basado en Raspberry Pi Pico que utiliza firmware reemplazable para ampliar las capacidades del ordenador. Al cargar diferentes imágenes de firmware, el cartucho puede ejecutar juegos y aplicaciones MSX y emular dispositivos de hardware adicionales (como mappers de ROM, RAM extra o interfaces de almacenamiento), añadiendo efectivamente periféricos virtuales al MSX. Uno de estos firmwares es el sistema MultiROM, que proporciona un menú en pantalla para explorar y lanzar múltiples títulos ROM almacenados en el cartucho.

El cartucho también puede exponer el puerto USB‑C del Pico como un dispositivo de almacenamiento masivo, permitiendo copiar ROMs, DSKs y otros archivos directamente desde un PC con Windows o Linux al cartucho.

Además, se puede usar una variante del firmware Nextor con soporte de mapeador de memoria +240 KB para aprovechar al máximo el hardware PicoVerse 2040 en sistemas MSX con memoria limitada.

Estas son las características disponibles en la versión actual del cartucho PicoVerse 2040:

* Sistema de menú MultiROM para seleccionar y lanzar ROMs MSX.
* Soporte de Nextor OS con mapeador de memoria opcional +240 KB.
* Soporte de dispositivo de almacenamiento masivo USB para cargar ROMs y DSKs.
* Soporte para varios mapeadores de ROM MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidad con sistemas MSX, MSX2 y MSX2+.

## Manual del creador de UF2 para MultiROM

Esta sección documenta la herramienta de consola `multirom` usada para generar imágenes UF2 (`multirom.uf2`) que programan el cartucho PicoVerse 2040.

En su comportamiento por defecto, la herramienta `multirom` escanea archivos ROM MSX en un directorio y los empaqueta en una única imagen UF2 que puede ser grabada en el Raspberry Pi Pico. La imagen resultante típicamente contiene el firmware del Pico, la ROM del menú MultiROM, un área de configuración describiendo cada entrada ROM y las propias cargas útiles ROM.

> **Nota:** Se pueden incluir un máximo de 128 ROMs en una sola imagen, con un límite total de aproximadamente 16 MB.

Dependiendo de las opciones, `multirom` también puede producir imágenes UF2 que arranquen directamente en un firmware personalizado en lugar del menú MultiROM. Por ejemplo, hay opciones para producir UF2 con un firmware que implementa Nextor. En este modo, el puerto USB‑C del Pico puede ser usado como dispositivo de almacenamiento masivo para cargar ROMs, DSKs y otros archivos desde Nextor (por ejemplo, mediante SofaRun).

## Resumen

Si se ejecuta sin opciones, la herramienta `multirom.exe` escanea el directorio de trabajo actual en busca de archivos ROM MSX (`.ROM` o `.rom`), analiza cada ROM para adivinar el tipo de mapeador, construye una tabla de configuración describiendo cada ROM (nombre, byte de mapeador, tamaño, offset) e incrusta esta tabla en una porción de la ROM del menú MSX. La herramienta luego concatena el blob del firmware del Pico, la porción del menú, el área de configuración y las cargas ROM y serializa toda la imagen en un archivo UF2 llamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

El archivo UF2 (normalmente multirom.uf2) puede copiarse al dispositivo de almacenamiento masivo USB del Pico para grabar la imagen combinada. Debes conectar el Pico mientras presionas el botón BOOTSEL para entrar en el modo de grabado UF2. Aparecerá una nueva unidad USB llamada `RPI-RP2`, y podrás copiar `multirom.uf2` en ella. Tras completar la copia, desconecta el Pico e inserta el cartucho en tu MSX para arrancar el menú MultiROM.

Los nombres de archivo ROM se usan para nombrar las entradas en el menú MSX. Hay un límite de 50 caracteres por nombre. Se usa un efecto de desplazamiento para mostrar nombres más largos en el menú MSX, pero si el nombre excede los 50 caracteres será truncado.

![alt text](/images/multirom_2040_menu.png)

> **Nota:** Para usar una memoria USB puede que necesites un adaptador o cable OTG. Esto puede convertir el puerto USB‑C a un puerto USB‑A hembra estándar.

> **Nota:** La opción `-n` incluye una ROM Nextor incrustada experimental que funciona en modelos MSX2 específicos. Esta versión usa un método de comunicación diferente (basado en puertos IO) y puede no funcionar en todos los sistemas.

## Uso por línea de comandos

Por ahora solo se proporcionan ejecutables para Microsoft Windows (`multirom.exe`).

### Uso básico:

```
multirom.exe [options]
```

### Opciones:
- `-n`, `--nextor` : Incluye la ROM NEXTOR incrustada en beta desde la configuración y la salida. Esta opción aún es experimental y en este momento solo funciona en modelos MSX2 específicos.
- `-h`, `--help`   : Muestra la ayuda y sale.
- `-o <filename>`, `--output <filename>` : Establece el nombre del archivo UF2 de salida (por defecto `multirom.uf2`).
- Si necesitas forzar un tipo de mapeador específico para un archivo ROM, puedes añadir una etiqueta de mapeador antes de la extensión `.ROM` en el nombre del archivo. La etiqueta no distingue mayúsculas de minúsculas. Por ejemplo, nombrar un archivo `Knight Mare.PL-32.ROM` fuerza el uso del mapeador PL-32 para esa ROM. Etiquetas como `SYSTEM` se ignoran. La lista de etiquetas posibles es: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

### Ejemplos
- Produce el archivo multirom.uf2 con el menú MultiROM y todos los archivos `.ROM` en el directorio actual. Puedes ejecutar la herramienta desde el símbolo del sistema o simplemente haciendo doble clic en el ejecutable:
  ```
  multirom.exe
  ```

## Cómo funciona (nivel alto)

1. La herramienta escanea el directorio de trabajo actual en busca de archivos que terminen en `.ROM` o `.rom`. Para cada archivo:
   - Extrae un nombre de visualización (nombre de archivo sin extensión, truncado a 50 caracteres).
   - Obtiene el tamaño del archivo y valida que esté entre `MIN_ROM_SIZE` y `MAX_ROM_SIZE`.
   - Llama a `detect_rom_type()` para determinar heurísticamente el byte de mapeador a usar en la entrada de configuración. Si hay una etiqueta de mapeador presente en el nombre de archivo, anula la detección.
   - Si la detección del mapeador falla, el archivo se omite.
   - Serializa el registro de configuración por-ROM (nombre de 50 bytes, 1 byte de mapeador, 4 bytes tamaño LE, 4 bytes offset de flash LE) en el área de configuración.
2. Después del escaneo, la herramienta concatena (en orden): binario del firmware incrustado del Pico, una porción inicial de la ROM del menú MSX (`MENU_COPY_SIZE` bytes), el área de configuración completa (`CONFIG_AREA_SIZE` bytes), la ROM NEXTOR opcional, y luego las cargas ROM descubiertas en orden de descubrimiento.
3. La carga combinada se escribe como un archivo UF2 llamado `multirom.uf2` usando `create_uf2_file()` que produce bloques UF2 de carga de 256 bytes dirigidos a la dirección de flash del Pico `0x10000000`.

## Heurísticas de detección de mapeadores
- `detect_rom_type()` implementa una combinación de comprobaciones de firmas (cabecera "AB", etiquetas `ROM_NEO8` / `ROM_NE16`) y escaneo heurístico de opcodes y direcciones para elegir mapeadores MSX comunes, incluyendo (pero no limitado a):
  - Plain 16KB (byte de mapeador 1) — comprobación de cabecera AB de 16KB
  - Plain 32KB (byte de mapeador 2) — comprobación de cabecera AB de 32KB
  - Mapeador Linear0 (byte de mapeador 4) — comprobación de diseño AB especial
  - NEO8 (byte de mapeador 8) y NEO16 (byte de mapeador 9)
  - Konami, Konami SCC, ASCII8, ASCII16 y otros mediante puntuación ponderada
- Si no se puede detectar un mapeador de forma fiable, la herramienta omite la ROM y reporta "unsupported mapper". Recuerda que puedes forzar un mapeador mediante la etiqueta en el nombre del archivo. Las etiquetas no distinguen mayúsculas y minúsculas y se listan abajo.
- Solo los siguientes mapeadores son compatibles en el área de configuración y el menú: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

## Uso del menú selector de ROMs MSX

Cuando enciendes el MSX con el cartucho PicoVerse 2040 insertado, aparece el menú MultiROM mostrando la lista de ROMs disponibles. Puedes navegar el menú usando las teclas de flecha del teclado.

![alt text](/images/multirom_2040_menu.png)

Usa las flechas Arriba y Abajo para mover el cursor de selección por la lista de ROMs. Si tienes más de 19 ROMs, usa las flechas Laterales (Izquierda y Derecha) para desplazarte por las páginas de entradas.

Pulsa Enter o Espacio para lanzar la ROM seleccionada. El MSX intentará arrancar la ROM usando la configuración de mapeador apropiada.

En cualquier momento mientras estés en el menú, puedes pulsar la tecla H para leer la pantalla de ayuda con instrucciones básicas. Pulsa cualquier tecla para volver al menú principal.

## Uso de Nextor con el cartucho PicoVerse 2040

La opción -n de la herramienta multirom incluye una ROM Nextor incrustada experimental que puede usarse en modelos MSX2 específicos. Sin embargo, esta opción aún está en desarrollo y puede no funcionar en todos los sistemas. Esta versión usa un método diferente (basado en puertos IO) para comunicarse con el MSX, permitiendo que funcione en sistemas donde la ranura del cartucho no es primaria, por ejemplo con expansores de ranuras.

Más detalles sobre el protocolo usado para comunicarse con el ordenador MSX se pueden encontrar en el documento Nextor-Pico-Bridge-Protocol.md.

### Cómo preparar una unidad USB para Nextor

Para preparar una memoria USB para usar con Nextor en el cartucho PicoVerse 2040, sigue estos pasos:

1. Conecta la memoria USB a tu PC.
2. Crea una partición FAT16 de máximo 4GB en la memoria USB. Puedes usar la herramienta incorporada de Nextor (CALL FDISK desde MSX Basic) o software de particionado de terceros.
3. Copia los archivos del sistema Nextor al directorio raíz de la partición FAT16. Puedes obtener los archivos del sistema Nextor desde la distribución oficial de Nextor o su repositorio. También necesitas el archivo COMMAND2 para el intérprete de comandos:
   1.  [Página de descargas de Nextor](https://github.com/Konamiman/Nextor/releases)
   2.  [Página de descargas de Command2](http://www.tni.nl/products/command2.html)
4. Copia cualquier ROM MSX (`.ROM`) o imagen de disco (`.DSK`) que quieras usar con Nextor al directorio raíz de la memoria USB.
5. Instala SofaRun u otro lanzador compatible con Nextor en la memoria USB si planeas usarlo para lanzar ROMs y DSKs. Puedes descargar SofaRun desde: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Expulsa la memoria USB de forma segura desde tu PC.
7. Conecta la memoria USB al cartucho PicoVerse 2040 usando un adaptador o cable OTG si es necesario.

> **Nota:** No todas las memorias USB son compatibles con el cartucho PicoVerse 2040. Si encuentras problemas, prueba con otra marca o modelo.
>
> **Nota:** Recuerda que Nextor necesita un mínimo de 128 KB de RAM para operar.

## Ideas de mejoras
- Mejorar las heurísticas de detección de tipo de ROM para cubrir más mapeadores y casos límite.
- Implementar pantalla de configuración para cada entrada ROM (nombre, anulación de mapeador, etc).
- Añadir soporte para más mapeadores ROM.
- Implementar un menú gráfico con mejor navegación e información de ROM.
- Añadir soporte para guardar/cargar la configuración del menú para preservar preferencias del usuario.
- Soportar archivos DSK también, con entradas de configuración apropiadas.
- Añadir soporte para temas personalizados del menú.
- Permitir descargar ROMs desde URLs de Internet e incrustarlas directamente.
- Permitir el uso del joystick para navegar el menú.

## Problemas conocidos

- La inclusión de la ROM Nextor incrustada sigue siendo experimental y puede no funcionar en todos los modelos MSX2.
- Algunas ROMs con mapeadores poco comunes pueden no detectarse correctamente y serán omitidas a menos que se use una etiqueta válida para forzar el mapeador.
- La herramienta MultiROM actualmente solo soporta Windows. No hay versiones para Linux o macOS todavía.
- La herramienta no valida actualmente la integridad de las ROMs más allá del tamaño y comprobaciones básicas de cabecera. ROMs corruptas pueden producir comportamientos inesperados.
- Debido a la naturaleza de la herramienta MultiROM (incrustar múltiples archivos en un solo UF2), algunos antivirus pueden marcar el ejecutable como sospechoso. Esto suele ser un falso positivo; asegúrate de descargar la herramienta desde una fuente de confianza.
- El menú MultiROM no soporta archivos DSK; solo se listan y lanzan archivos ROM.
- La herramienta no soporta actualmente subdirectorios; solo se procesan las ROMs en el directorio de trabajo actual.
- La memoria flash del Pico puede desgastarse tras muchos ciclos de escritura. Evita regrabar el cartucho en exceso.

## Modelos MSX probados

El cartucho PicoVerse 2040 con firmware MultiROM ha sido probado en los siguientes modelos MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Operación verificada |
| Gradiente Expert | MSX1 | OK | Operación verificada |
| JFF MSX | MSX1 | OK | Operación verificada |
| MSX Book | MSX2+ (clon FPGA) | OK | Operación verificada |
| MSX One | MSX1 | Not OK | Cartucho no reconocido |
| National FS-4500 | MSX1 | OK | Operación verificada |
| Omega MSX | MSX2+ | OK | Operación verificada |
| Panasonic FS-A1GT | TurboR | OK | Operación verificada |
| Panasonic FS-A1ST | TurboR | OK | Operación verificada |
| Panasonic FS-A1WX | MSX2+ | OK | Operación verificada |
| Panasonic FS-A1WSX | MSX2+ | OK | Operación verificada |
| Sharp HotBit HB8000 | MSX1 | OK | Operación verificada |
| SMX-HB | MSX2+ (clon FPGA) | OK | Operación verificada |
| Sony HB-F1XD | MSX2 | OK | Operación verificada |
| Sony HB-F1XDJ | MSX2 | OK | Operación verificada |
| Sony HB-F1XV | MSX2+ | OK | Operación verificada |
| TRHMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| uMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| Yamaha YIS604 | MSX1 | OK | Operación verificada |

La ROM Nextor incrustada experimental (opción `-n`) ha sido probada en los siguientes modelos MSX:

| Modelo | Tipo | Estado | Comentarios |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB no detectado |
| Sony HB-F1XD | MSX2 | Not OK | Error leyendo sector de disco |
| TRHMSX | MSX2+ (clon FPGA) | OK | Operación verificada |
| uMSX | MSX2+ (clon FPGA) | OK | Operación verificada |

Autor: Cristiano Almeida Goncalves
Última actualización: 12/25/2025

