# Projeto MSX PicoVerse 2040

|PicoVerse Frente|PicoVerse Verso|
|---|---|
|![PicoVerse Frente](/images/2025-12-02_20-05.png)|![PicoVerse Verso](/images/2025-12-02_20-06.png)|

O PicoVerse 2040 é um cartucho para MSX baseado no Raspberry Pi Pico que utiliza firmware substituível para estender as capacidades do computador. Ao carregar diferentes imagens de firmware, o cartucho pode executar jogos e aplicações de MSX e emular dispositivos de hardware adicionais (como mappers de ROM, RAM extra ou interfaces de armazenamento), adicionando efetivamente periféricos virtuais ao MSX. Um desses firmwares é o sistema MultiROM, que fornece um menu na tela para navegar e iniciar vários títulos de ROM armazenados no cartucho.

O cartucho também pode expor a porta USB-C do Pico como um dispositivo de armazenamento em massa, permitindo copiar ROMs, DSKs e outros arquivos diretamente de um PC com Windows ou Linux para o cartucho.

Estas são as funcionalidades disponíveis na versão atual do cartucho PicoVerse 2040:

* Sistema de menu MultiROM para selecionar e iniciar ROMs de MSX.
* Opção de Menu Inline com suporte ao Nextor OS.
* Suporte a dispositivo de armazenamento em massa USB para carregar ROMs e DSKs.
* Suporte para vários mappers de ROM de MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidade com sistemas MSX, MSX2 e MSX2+.

## Manual do Criador de UF2 MultiROM

Esta seção documenta a ferramenta de console `multirom` usada para gerar imagens UF2 (`multirom.uf2`) que programam o cartucho PicoVerse 2040.

Em seu comportamento padrão, a ferramenta `multirom` varre arquivos ROM de MSX em um diretório e os empacota em uma única imagem UF2 que pode ser gravada no Raspberry Pi Pico. A imagem resultante normalmente contém o firmware do Pico, a ROM do menu MSX do MultiROM, uma área de configuração descrevendo cada entrada de ROM e os próprios payloads das ROMs.

> **Nota:** Um máximo de 128 ROMs pode ser incluído em uma única imagem, com um limite de tamanho total de aproximadamente 16 MB.

Dependendo das opções fornecidas, o `multirom` também pode produzir imagens UF2 que iniciam diretamente em um firmware personalizado em vez do menu MultiROM. Por exemplo, opções podem ser usadas para produzir arquivos UF2 com um firmware que implementa o Nextor OS. Neste modo, a porta USB-C do Pico pode ser usada como um dispositivo de armazenamento em massa para carregar ROMs, DSKs e outros arquivos a partir do Nextor (por exemplo, via SofaRun).

## Visão Geral

Se executada sem nenhuma opção, a ferramenta `multirom.exe` varre o diretório de trabalho atual em busca de arquivos ROM de MSX (`.ROM` ou `.rom`), analisa cada ROM para adivinar o tipo de mapper, constrói uma tabela de configuração descrevendo cada ROM (nome, byte do mapper, tamanho, offset) e incorpora esta tabela em uma fatia da ROM do menu MSX. A ferramenta então concatena o blob do firmware do Pico, a fatia do menu, a área de configuração e os payloads das ROMs e serializa toda a imagem em um arquivo UF2 chamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

O arquivo UF2 (geralmente multirom.uf2) pode então ser copiado para um dispositivo de armazenamento em massa USB do Pico para gravar a imagem combinada. Você precisa conectar o Pico enquanto pressiona o botão BOOTSEL para entrar no modo de gravação UF2. Então, uma nova unidade USB chamada `RPI-RP2` aparece, e você pode copiar o `multirom.uf2` para ela. Após a conclusão da cópia, você pode desconectar o Pico e inserir o cartucho no seu MSX para iniciar o menu MultiROM.

Os nomes dos arquivos ROM são usados para nomear as entradas no menu MSX. Há um limite de 50 caracteres por nome. Um efeito de rolagem é usado para mostrar nomes mais longos no menu MSX, mas se o nome exceder 50 caracteres, ele será truncado.

![alt text](/images/multirom_2040_menu.png)

> **Nota:** Para usar um pendrive USB, você pode precisar de um adaptador ou cabo OTG. Isso pode ser usado para converter a porta USB-C em uma porta USB-A fêmea padrão.

> **Nota:** A opção `-n` inclui uma ROM Nextor incorporada experimental que funciona em modelos específicos de MSX2. Esta versão usa um método de comunicação diferente (baseado em porta de E/S) e pode não funcionar em todos os sistemas.

## Uso via linha de comando

Apenas executáveis para Microsoft Windows são fornecidos por enquanto (`multirom.exe`).

### Uso básico:

```
multirom.exe [opções]
```

### Opções:
- `-n`, `--nextor` : Inclui a ROM NEXTOR incorporada beta na configuração e saídas. Esta opção ainda é experimental e, neste momento, só funciona em modelos específicos de MSX2.
- `-h`, `--help`   : Mostra a ajuda de uso e sai.
- `-o <nome_do_arquivo>`, `--output <nome_do_arquivo>` : Define o nome do arquivo UF2 de saída (o padrão é `multirom.uf2`).
- Se você precisar forçar um tipo de mapper específico para um arquivo ROM, você pode anexar uma tag de mapper antes da extensão `.ROM` no nome do arquivo. A tag não diferencia maiúsculas de minúsculas. Por exemplo, nomear um arquivo como `Knight Mare.PL-32.ROM` força o uso do mapper PL-32 para essa ROM. Tags como `SYSTEM` são ignoradas. A lista de tags possíveis que podem ser usadas é: `PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16`

### Exemplos
- Produz o arquivo multirom.uf2 com o menu MultiROM e todos os arquivos `.ROM` no diretório atual. Você pode executar a ferramenta usando o prompt de comando ou apenas clicando duas vezes no executável:
  ```
  multirom.exe
  ```

## Como funciona (nível alto)

1. A ferramenta varre o diretório de trabalho atual em busca de arquivos terminados em `.ROM` ou `.rom`. Para cada arquivo:
   - Extrai um nome de exibição (nome do arquivo sem extensão, truncado para 50 caracteres).
   - Obtém o tamanho do arquivo e valida se está entre `MIN_ROM_SIZE` e `MAX_ROM_SIZE`.
   - Chama `detect_rom_type()` para determinar heuristicamente o byte do mapper a ser usado na entrada de configuração. Se uma tag de mapper estiver presente no nome do arquivo, ela substitui a detecção.
   - Se a detecção do mapper falhar, o arquivo é ignorado.
   - Serializa o registro de configuração por ROM (nome de 50 bytes, mapper de 1 byte, tamanho de 4 bytes LE, flash-offset de 4 bytes LE) na área de configuração.
2. Após a varredura, a ferramenta concatena (em ordem): o binário do firmware do Pico incorporado, uma fatia inicial da ROM do menu MSX (bytes `MENU_COPY_SIZE`), a área de configuração completa (bytes `CONFIG_AREA_SIZE`), a ROM NEXTOR opcional e, em seguida, os payloads das ROMs descobertas na ordem de descoberta.
3. O payload combinado é gravado como um arquivo UF2 chamado `multirom.uf2` usando `create_uf2_file()`, que produz blocos UF2 de payload de 256 bytes direcionados ao endereço de flash do Pico `0x10000000`.

## Heurísticas de detecção de mapper
- `detect_rom_type()` implementa uma combinação de verificações de assinatura (cabeçalho "AB", tags `ROM_NEO8` / `ROM_NE16`) e varredura heurística de opcodes e endereços para escolher mappers comuns de MSX, incluindo (mas não se limitando a):
  - Plain 16KB (byte do mapper 1) — verificação de cabeçalho AB de 16KB
  - Plain 32KB (byte do mapper 2) — verificação de cabeçalho AB de 32KB
  - Mapper Linear0 (byte do mapper 4) — verificação de layout AB especial
  - NEO8 (byte do mapper 8) e NEO16 (byte do mapper 9)
  - Konami, Konami SCC, ASCII8, ASCII16 e outros via pontuação ponderada
- Se nenhum mapper puder ser detectado com segurança, a ferramenta ignora a ROM e relata "mapper não suportado". Lembre-se de que você pode forçar um mapper via tag no nome do arquivo. As tags não diferenciam maiúsculas de minúsculas e estão listadas abaixo.
- Apenas os seguintes mappers são suportados na área de configuração e no menu: `PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16`

## Usando o menu Seletor de ROM de MSX

Quando você liga o MSX com o cartucho PicoVerse 2040 inserido, o menu MultiROM aparece, mostrando a lista de ROMs disponíveis. Você pode navegar no menu usando as teclas de seta do teclado.

![alt text](/images/multirom_2040_menu.png)

Use as teclas de seta Para Cima e Para Baixo para mover o cursor de seleção pela lista de ROMs. Se você tiver mais de 19 ROMs, use as teclas de seta laterais (Esquerda e Direita) para rolar pelas páginas de entradas.

Pressione a tecla Enter ou Espaço para iniciar a ROM selecionada. O MSX tentará iniciar a ROM usando as configurações de mapper apropriadas.

A qualquer momento enquanto estiver no menu, você pode pressionar a tecla H para ler a tela de ajuda com instruções básicas. Pressione qualquer tecla para retornar ao menu principal.

## Usando o Nextor com o cartucho PicoVerse 2040

A opção -n da ferramenta multirom inclui uma ROM Nextor incorporada experimental que pode ser usada em modelos específicos de MSX2. No entanto, esta opção ainda está em desenvolvimento e pode não funcionar em todos os sistemas. Esta versão usa um método diferente (baseado em porta de E/S) para se comunicar com o MSX, permitindo que funcione em sistemas onde o slot do cartucho não é primário, por exemplo, com expansores de slot.

Mais detalhes sobre o protocolo usado para se comunicar com o computador MSX podem ser encontrados no documento Nextor-Pico-Bridge-Protocol.md. 

### Como preparar um pendrive para o Nextor

Para preparar um pendrive USB para uso com o Nextor no cartucho PicoVerse 2040, siga estes passos:

1. Conecte o pendrive USB ao seu PC.
2. Crie uma partição FAT16 de no máximo 4 GB no pendrive. Você pode usar a ferramenta integrada do Nextor OS (CALL FDISK enquanto estiver no MSX Basic) ou software de particionamento de terceiros para fazer isso.
3. Copie os arquivos de sistema do Nextor para o diretório raiz da partição FAT16. Você pode obter os arquivos de sistema do Nextor na distribuição ou repositório oficial do Nextor. Você também precisa do arquivo COMMAND2 para o shell de comando:
   1. [Página de Download do Nextor](https://github.com/Konamiman/Nextor/releases)
   2. [Página de Download do Command2](http://www.tni.nl/products/command2.html)
4. Copie quaisquer ROMs de MSX (arquivos `.ROM`) ou imagens de disco (arquivos `.DSK`) que você deseja usar com o Nextor para o diretório raiz do pendrive.
5. Instale o SofaRun ou qualquer outro lançador compatível com o Nextor no pendrive se planeja usá-lo para iniciar ROMs e DSKs. Você pode baixar o SofaRun de sua fonte oficial aqui: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Ejete o pendrive com segurança do seu PC.
7. Conecte o pendrive ao cartucho PicoVerse 2040 usando um adaptador ou cabo USB OTG, se necessário.

> **Nota:** Nem todos os pendrives USB são compatíveis com o cartucho PicoVerse 2040. Se você encontrar problemas, tente usar uma marca ou modelo diferente de pendrive.
>
> **Nota:** Lembre-se de que o Nextor precisa de um mínimo de 128 KB de RAM para operar.

## Ideias de melhorias
- Melhorar as heurísticas de detecção de tipo de ROM para cobrir mais mappers e casos extremos.
- Implementar tela de configuração para cada entrada de ROM (nome, substituição de mapper, etc.).
- Adicionar suporte para mais mappers de ROM.
- Implementar um menu gráfico com melhor navegação e exibição de informações da ROM.
- Adicionar suporte para salvar/carregar a configuração do menu para preservar as preferências do usuario.
- Suportar arquivos DSK também, com entradas de configuração adequadas.
- Adicionar suporte para temas personalizados para o menu.
- Permitir o download de ROMs de URLs da Internet e incorporá-las diretamente.
- Permitir o uso do joystick para navegar no menu.

## Problemas conhecidos

- A inclusão da ROM Nextor incorporada ainda é experimental e pode não funcionar em todos os modelos de MSX2.
- Algumas ROMs com mappers incomuns podem não ser detectadas corretamente e serão ignoradas, a menos que uma tag de mapper válida seja usada para forçar a detecção.
- A ferramenta MultiROM atualmente suporta apenas Windows. Versões para Linux e macOS ainda não estão disponíveis.
- A ferramenta não valida atualmente a integridade dos arquivos ROM além do tamanho e verificações básicas de cabeçalho. ROMs corrompidas podem levar a comportamentos inesperados.
- Devido à natureza da ferramenta MultiROM (incorporando vários arquivos em um único UF2), alguns softwares antivírus podem sinalizar o executável como suspeito. Isso é um falso positivo; certifique-se de baixar a ferramenta de uma fonte confiável.
- O menu MultiROM não suporta arquivos DSK; apenas arquivos ROM são listados e iniciados.
- A ferramenta não suporta subdiretórios no momento; apenas arquivos ROM no diretório de trabalho atual são processados.
- A memória flash do pico pode se desgastar após muitos ciclos de gravação. Evite a regravação excessiva do cartucho.

## Modelos de MSX testados

O cartucho PicoVerse 2040 com firmware MultiROM foi testado nos seguintes modelos de MSX:

| Modelo | Tipo | Status | Comentários |
| --- | --- | --- | --- |
| Adermir Carchano Expert 4 | MSX2+ | OK | Operação verificada |
| Gradiente Expert | MSX1 | OK | Operação verificada |
| JFF MSX | MSX1 | OK | Operação verificada |
| MSX Book | MSX2+ (clone FPGA) | OK | Operação verificada |
| MSX One | MSX1 | Not OK | Cartucho não reconhecido |
| National FS-4500 | MSX1 | OK | Operação verificada |
| Omega MSX | MSX2+ | OK | Operação verificada |
| Panasonic FS-A1GT | TurboR | OK | Operação verificada |
| Panasonic FS-A1ST | TurboR | OK | Operação verificada |
| Panasonic FS-A1WX | MSX2+ | OK | Operação verificada |
| Panasonic FS-A1WSX | MSX2+ | OK | Operação verificada |
| Sharp HotBit HB8000 | MSX1 | OK | Operação verificada |
| SMX-HB | MSX2+ (clone FPGA) | OK | Operação verificada |
| Sony HB-F1XD | MSX2 | OK | Operação verificada |
| Sony HB-F1XDJ | MSX2 | OK | Operação verificada |
| Sony HB-F1XV | MSX2+ | OK | Operação verificada |
| TRHMSX | MSX2+ (clone FPGA) | OK | Operação verificada |
| uMSX | MSX2+ (clone FPGA) | OK | Operação verificada |
| Yamaha YIS604 | MSX1 | OK | Operação verificada |

A ROM Nextor incorporada experimental (opção `-n`) foi testada nos seguintes modelos de MSX:

| Modelo | Tipo | Status | Comentários |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB não detectado |
| Sony HB-F1XD | MSX2 | Not OK | Erro ao ler setor do disco |
| TRHMSX | MSX2+ (clone FPGA) | OK | Operação verificada |
| uMSX | MSX2+ (clone FPGA) | OK | Operação verificada |

Autor: Cristiano Almeida Goncalves
Última atualização: 25/12/2025
