# Projeto MSX PicoVerse 2040

|PicoVerse Frente|PicoVerse Verso|
|---|---|
|![PicoVerse Front](/images/2025-12-02_20-05.png)|![PicoVerse Back](/images/2025-12-02_20-06.png)|

O PicoVerse 2040 é um cartucho para MSX baseado no Raspberry Pi Pico que usa firmware substituível para ampliar as capacidades do computador. Ao carregar diferentes imagens de firmware, o cartucho pode executar jogos e aplicações MSX e emular dispositivos de hardware adicionais (como mappers de ROM, RAM extra ou interfaces de armazenamento), adicionando efetivamente periféricos virtuais ao MSX. Um desses firmwares é o sistema MultiROM, que fornece um menu na tela para navegar e iniciar múltiplos títulos ROM armazenados no cartucho.

O cartucho também pode expor a porta USB‑C do Pico como um dispositivo de armazenamento em massa, permitindo que você copie ROMs, DSKs e outros arquivos diretamente de um PC com Windows ou Linux para o cartucho.

Estas são as funcionalidades disponíveis na versão atual do cartucho PicoVerse 2040:

* Sistema de menu MultiROM para selecionar e iniciar ROMs MSX.
* Opção de Menu Inline com suporte Nextor.
* Suporte a dispositivo de armazenamento USB para carregar ROMs e DSKs.
* Suporte a vários mappers de ROM MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidade com sistemas MSX, MSX2 e MSX2+.

## Manual do Criador de UF2 do MultiROM

Esta seção documenta a ferramenta de console `multirom` usada para gerar imagens UF2 (`multirom.uf2`) que gravam o cartucho PicoVerse 2040.

Por padrão, a ferramenta `multirom` varre arquivos ROM MSX em um diretório e os empacota em uma única imagem UF2 que pode ser gravada no Raspberry Pi Pico. A imagem resultante tipicamente contém o binário do firmware do Pico, a ROM do menu MultiROM, uma área de configuração que descreve cada entrada de ROM e as cargas úteis (payloads) das ROMs.

> **Nota:** É possível incluir no máximo 128 ROMs em uma única imagem, com limite de tamanho total de aproximadamente 16 MB.

Dependendo das opções fornecidas, o `multirom` também pode produzir imagens UF2 que inicializam diretamente em um firmware personalizado em vez do menu MultiROM. Por exemplo, há opções para produzir arquivos UF2 com um firmware que implementa o Nextor. Nesse modo, a porta USB‑C do Pico pode ser usada como dispositivo de armazenamento em massa para carregar ROMs, DSKs e outros arquivos a partir do Nextor (por exemplo, via SofaRun).

## Visão Geral

Se executado sem opções, a ferramenta `multirom.exe` varre o diretório de trabalho atual em busca de arquivos ROM MSX (`.ROM` ou `.rom`), analisa cada ROM para adivinhar o tipo de mapper, constrói uma tabela de configuração descrevendo cada ROM (nome, byte de mapper, tamanho, offset) e incorpora essa tabela em um pedaço da ROM de menu MSX. A ferramenta então concatena o blob do firmware do Pico, o trecho do menu, a área de configuração e os payloads das ROMs e serializa toda a imagem em um arquivo UF2 chamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

O arquivo UF2 (normalmente multirom.uf2) pode ser copiado para o dispositivo de armazenamento USB do Pico para gravar a imagem combinada. Você precisa conectar o Pico enquanto pressiona o botão BOOTSEL para entrar no modo de gravação UF2. Então uma nova unidade USB chamada `RPI-RP2` aparece, e você pode copiar `multirom.uf2` para ela. Após a cópia completar, você pode desconectar o Pico e inserir o cartucho no seu MSX para iniciar o menu MultiROM.

Os nomes dos arquivos ROM são usados para nomear as entradas no menu MSX. Há um limite de 50 caracteres por nome. Um efeito de rolagem é usado para mostrar nomes mais longos no menu MSX, mas se o nome exceder 50 caracteres ele será truncado.

![alt text](/images/multirom_2040_menu.png)

> **Nota:** Para usar um pendrive USB pode ser necessário um adaptador ou cabo OTG. Ele pode converter a porta USB‑C para uma porta USB‑A fêmea padrão.

> **Nota:** A opção `-n` inclui uma ROM Nextor embutida experimental que funciona em modelos MSX2 específicos. Esta versão usa um método diferente de comunicação (baseado em porta IO) e pode não funcionar em todos os sistemas.

## Uso por Linha de Comando

Apenas executáveis para Microsoft Windows são fornecidos por enquanto (`multirom.exe`).

### Uso básico:

```
multirom.exe [options]
```

### Opções:
- `-n`, `--nextor` : Inclui a ROM NEXTOR beta embutida da configuração na saída. Esta opção ainda é experimental e neste momento funciona apenas em modelos MSX2 específicos.
- `-h`, `--help`   : Mostra a ajuda de uso e sai.
- `-o <filename>`, `--output <filename>` : Define o nome do arquivo UF2 de saída (padrão é `multirom.uf2`).
- Se você precisar forçar um tipo de mapper específico para um arquivo ROM, pode adicionar uma tag de mapper antes da extensão `.ROM` no nome do arquivo. A tag não diferencia maiúsculas de minúsculas. Por exemplo, nomear um arquivo `Knight Mare.PL-32.ROM` força o uso do mapper PL-32 para essa ROM. Tags como `SYSTEM` são ignoradas. A lista de tags possíveis é: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

### Exemplos
- Gera o arquivo `multirom.uf2` com o menu MultiROM e todos os arquivos `.ROM` no diretório atual. Você pode executar a ferramenta usando o prompt de comando ou clicando duas vezes no executável:
  ```
  multirom.exe
  ```
  
## Como funciona (visão geral)

1. A ferramenta varre o diretório de trabalho atual em busca de arquivos terminados em `.ROM` ou `.rom`. Para cada arquivo:
   - Extrai um nome de exibição (nome do arquivo sem extensão, truncado a 50 caracteres).
   - Obtém o tamanho do arquivo e valida que esteja entre `MIN_ROM_SIZE` e `MAX_ROM_SIZE`.
   - Chama `detect_rom_type()` para determinar heurísticamente o byte de mapper a usar na entrada de configuração. Se houver uma tag de mapper no nome do arquivo, ela sobrescreve a detecção.
   - Se a detecção do mapper falhar, o arquivo é ignorado.
   - Serializa o registro de configuração por-ROM (nome de 50 bytes, 1 byte de mapper, 4 bytes de tamanho LE, 4 bytes de offset em flash LE) na área de configuração.
2. Após a varredura, a ferramenta concatena (na ordem): binário do firmware do Pico embutido, um slice inicial da ROM do menu MSX (`MENU_COPY_SIZE` bytes), a área de configuração completa (`CONFIG_AREA_SIZE` bytes), opcionalmente a ROM NEXTOR, e então os payloads das ROMs descobertas na ordem de descoberta.
3. O payload combinado é escrito como um arquivo UF2 chamado `multirom.uf2` usando `create_uf2_file()` que produz blocos UF2 de 256 bytes direcionados ao endereço de flash do Pico `0x10000000`.

## Heurísticas de detecção de mapper
- `detect_rom_type()` implementa uma combinação de verificações de assinatura (cabeçalho "AB", tags `ROM_NEO8` / `ROM_NE16`) e uma varredura heurística de opcodes e endereços para escolher mappers comuns do MSX, incluindo (mas não limitado a):
  - Plain 16KB (byte de mapper 1) — checagem de cabeçalho AB de 16KB
  - Plain 32KB (byte de mapper 2) — checagem de cabeçalho AB de 32KB
  - Mapper Linear0 (byte de mapper 4) — checagem especial de layout AB
  - NEO8 (byte de mapper 8) e NEO16 (byte de mapper 9)
  - Konami, Konami SCC, ASCII8, ASCII16 e outros via pontuação ponderada
- Se nenhum mapper puder ser detectado com confiança, a ferramenta ignora a ROM e reporta "unsupported mapper". Lembre-se que você pode forçar um mapper via tag no nome do arquivo. As tags são case-insensitive e listadas acima.
- Apenas os seguintes mappers são suportados na área de configuração e no menu: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

## Uso do menu seletor de ROMs do MSX

Quando você liga o MSX com o cartucho PicoVerse 2040 inserido, o menu MultiROM aparece, mostrando a lista de ROMs disponíveis. Você pode navegar pelo menu usando as teclas de seta do teclado.

![alt text](/images/multirom_2040_menu.png)

Use as teclas de seta para Cima e Para Baixo para mover o cursor de seleção pela lista de ROMs. Se você tiver mais de 19 ROMs, use as teclas laterais (Esquerda e Direita) para rolar pelas páginas de entradas.

Pressione Enter ou Espaço para iniciar a ROM selecionada. O MSX tentará inicializar a ROM usando as configurações de mapper apropriadas.

A qualquer momento enquanto estiver no menu, você pode pressionar a tecla H para ler a tela de ajuda com instruções básicas. Pressione qualquer tecla para voltar ao menu principal.

## Usando Nextor com o cartucho PicoVerse 2040

A opção `-n` da ferramenta multirom inclui uma ROM Nextor embutida experimental que pode ser usada em modelos MSX2 específicos. Contudo, esta opção ainda está em desenvolvimento e pode não funcionar em todos os sistemas. Esta versão usa um método diferente (baseado em portas IO) para se comunicar com o MSX, permitindo que funcione em sistemas onde o slot do cartucho não é primário, por exemplo com expansores de slot.

Mais detalhes sobre o protocolo usado para se comunicar com o computador MSX estão no documento Nextor-Pico-Bridge-Protocol.md. Os detalhes da comunicação com o firmware Sunrise IDE Nextor estão no documento Sunrise-Nextor.md.

### Como preparar um pendrive para Nextor

1. Conecte o pendrive ao seu PC.
2. Crie uma partição FAT16 de no máximo 4GB no pendrive. Você pode usar a ferramenta interna do Nextor (CALL FDISK enquanto estiver no MSX Basic) ou software de particionamento de terceiros.
3. Copie os arquivos do sistema Nextor para o diretório raiz da partição FAT16. Você pode obter os arquivos do sistema Nextor na distribuição oficial ou repositório. Também é necessário o arquivo COMMAND2 para o shell de comandos:
   1.  [Página de Download do Nextor](https://github.com/Konamiman/Nextor/releases) 
   2.  [Página de Download do Command2](http://www.tni.nl/products/command2.html)
4. Copie quaisquer ROMs MSX (`.ROM`) ou imagens de disco (`.DSK`) que você queira usar com o Nextor para o diretório raiz do pendrive.
5. Instale o SofaRun ou qualquer outro lançador compatível com Nextor no pendrive se planeja usá-lo para iniciar ROMs e DSKs. Você pode baixar o SofaRun na sua fonte oficial aqui: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Ejetar o pendrive com segurança do seu PC.
7. Conecte o pendrive ao cartucho PicoVerse 2040 usando um adaptador ou cabo OTG se necessário.

> **Nota:** Nem todos os pendrives são compatíveis com o cartucho PicoVerse 2040. Se você encontrar problemas, tente outro modelo ou marca.
>
> **Nota:** Lembre-se que o Nextor precisa de no mínimo 128 KB de RAM para operar.

## Ideias de melhorias
- Melhorar as heurísticas de detecção de tipo de ROM para cobrir mais mappers e casos de borda.
- Implementar uma tela de configuração para cada entrada de ROM (nome, override de mapper, etc).
- Adicionar suporte a mais mappers de ROM.
- Implementar um menu gráfico com melhor navegação e exibição de informações da ROM.
- Adicionar suporte para salvar/carregar a configuração do menu para preservar preferências do usuário.
- Suportar arquivos DSK também, com entradas de configuração apropriadas.
- Adicionar suporte a temas personalizados para o menu.
- Permitir o download de ROMs de URLs da Internet e incorporá-las diretamente.
- Permitir o uso do joystick para navegar pelo menu.

## Problemas conhecidos

- A inclusão da ROM Nextor embutida ainda é experimental e pode não funcionar em todos os modelos MSX2.
- Algumas ROMs com mappers incomuns podem não ser detectadas corretamente e serão ignoradas, a menos que uma tag válida seja usada para forçar a detecção.
- A ferramenta MultiROM atualmente só suporta Windows. Não há versões para Linux ou macOS ainda.
- A ferramenta não valida atualmente a integridade das ROMs além do tamanho e verificações básicas de cabeçalho. ROMs corrompidas podem causar comportamento inesperado.
- Devido à natureza da MultiROM (incorporar múltiplos arquivos em um único UF2), alguns antivírus podem marcar o executável como suspeito. Isto é um falso positivo; certifique-se de baixar a ferramenta de uma fonte confiável.
- O menu MultiROM não suporta arquivos DSK; apenas arquivos ROM são listados e iniciados.
- A ferramenta não suporta subdiretórios atualmente; apenas ROMs no diretório de trabalho atual são processadas.
- A memória flash do Pico pode se desgastar após muitos ciclos de escrita. Evite regravar o cartucho em excesso.

## Modelos MSX testados

O cartucho PicoVerse 2040 com firmware MultiROM foi testado nos seguintes modelos MSX:

| Modelo | Tipo | Estado | Comentários |
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

A ROM Nextor embutida experimental (opção `-n`) foi testada nos seguintes modelos MSX:

| Modelo | Tipo | Estado | Comentários |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB não detectado |
| Sony HB-F1XD | MSX2 | Not OK | Erro ao ler setor do disco |
| TRHMSX | MSX2+ (clone FPGA) | OK | Operação verificada |
| uMSX | MSX2+ (clone FPGA) | OK | Operação verificada |

Autor: Cristiano Almeida Goncalves
Última atualização: 25/12/2025
# Projeto MSX PicoVerse 2040

|PicoVerse Frente|PicoVerse Verso|
|---|---|
|![PicoVerse Front](/images/2025-12-02_20-05.png)|![PicoVerse Back](/images/2025-12-02_20-06.png)|

O PicoVerse 2040 é um cartucho para MSX baseado em Raspberry Pi Pico que utiliza firmware substituível para estender as capacidades do computador. Ao carregar diferentes imagens de firmware, o cartucho pode executar jogos e aplicativos MSX e emular dispositivos de hardware adicionais (como mappers de ROM, RAM extra ou interfaces de armazenamento), adicionando efetivamente periféricos virtuais ao MSX. Um desses firmwares é o sistema MultiROM, que fornece um menu na tela para navegar e iniciar múltiplos títulos ROM armazenados no cartucho.

O cartucho também pode expor a porta USB‑C do Pico como um dispositivo de armazenamento em massa, permitindo copiar ROMs, DSKs e outros arquivos diretamente de um PC com Windows ou Linux para o cartucho.

Além disso, você pode usar uma variante do firmware Nextor com suporte ao mapeador de memória +240 KB para aproveitar ao máximo o hardware PicoVerse 2040 em sistemas MSX com memória limitada.

Estas são as funcionalidades disponíveis na versão atual do cartucho PicoVerse 2040:

* Sistema de menu MultiROM para selecionar e iniciar ROMs MSX.
* Suporte ao Nextor OS com mapeador de memória opcional +240 KB.
* Suporte a dispositivo de armazenamento em massa USB para carregar ROMs e DSKs.
* Suporte a vários mappers de ROM MSX (PL-16, PL-32, KonSCC, Linear, ASC-08, ASC-16, Konami, NEO-8, NEO-16).
* Compatibilidade com sistemas MSX, MSX2 e MSX2+.

## Manual do criador de UF2 do MultiROM

Esta seção documenta a ferramenta de console `multirom` usada para gerar imagens UF2 (`multirom.uf2`) que programam o cartucho PicoVerse 2040.

Por padrão, a ferramenta `multirom` varre arquivos ROM MSX em um diretório e os empacota em uma única imagem UF2 que pode ser gravada no Raspberry Pi Pico. A imagem resultante normalmente contém o firmware do Pico, a ROM do menu MultiROM, uma área de configuração descrevendo cada entrada ROM e os próprios payloads ROM.

> **Nota:** Um máximo de 128 ROMs pode ser incluído em uma única imagem, com um limite total de aproximadamente 16 MB.

Dependendo das opções fornecidas, o `multirom` também pode produzir imagens UF2 que inicializam diretamente em um firmware personalizado em vez do menu MultiROM. Por exemplo, opções podem ser usadas para produzir arquivos UF2 com um firmware que implementa o Nextor. Neste modo, a porta USB‑C do Pico pode ser usada como dispositivo de armazenamento em massa para carregar ROMs, DSKs e outros arquivos do Nextor (por exemplo, via SofaRun).

## Visão geral

Se executado sem opções, a ferramenta `multirom.exe` varre o diretório de trabalho atual em busca de arquivos ROM MSX (`.ROM` ou `.rom`), analisa cada ROM para adivinhar o tipo de mapper, constrói uma tabela de configuração descrevendo cada ROM (nome, byte de mapper, tamanho, offset) e incorpora essa tabela em um pedaço da ROM do menu MSX. A ferramenta então concatena o blob do firmware do Pico, o pedaço do menu, a área de configuração e os payloads ROM e serializa toda a imagem em um arquivo UF2 chamado `multirom.uf2`.

![alt text](/images/2025-11-29_20-49.png)

O arquivo UF2 (normalmente multirom.uf2) pode então ser copiado para o dispositivo de armazenamento em massa USB do Pico para gravar a imagem combinada. Você precisa conectar o Pico enquanto pressiona o botão BOOTSEL para entrar no modo de gravação UF2. Então uma nova unidade USB chamada `RPI-RP2` aparece, e você pode copiar `multirom.uf2` para ela. Após a cópia completar, desconecte o Pico e insira o cartucho no seu MSX para inicializar o menu MultiROM.

Os nomes dos arquivos ROM são usados para nomear as entradas no menu MSX. Há um limite de 50 caracteres por nome. Um efeito de rolagem é usado para mostrar nomes mais longos no menu MSX, mas se o nome exceder 50 caracteres ele será truncado.

![alt text](/images/multirom_2040_menu.png)

> **Nota:** Para usar um pen drive USB você pode precisar de um adaptador ou cabo OTG. Isso pode ser usado para converter a porta USB‑C para uma porta USB‑A fêmea padrão.

> **Nota:** A opção `-n` inclui uma ROM NEXTOR incorporada experimental que funciona em modelos MSX2 específicos. Esta versão usa um método de comunicação diferente (baseado em porta IO) e pode não funcionar em todos os sistemas.

## Uso por linha de comando

Por enquanto apenas executáveis do Microsoft Windows são fornecidos (`multirom.exe`).

### Uso básico:

```
multirom.exe [options]
```

### Opções:
- `-n`, `--nextor` : Inclui a ROM NEXTOR incorporada em beta a partir da configuração e saída. Esta opção ainda é experimental e neste momento funciona apenas em modelos MSX2 específicos.
- `-h`, `--help`   : Mostra a ajuda e sai.
- `-o <filename>`, `--output <filename>` : Define o nome do arquivo UF2 de saída (padrão é `multirom.uf2`).
- Se você precisar forçar um tipo específico de mapper para um arquivo ROM, você pode anexar uma tag de mapper antes da extensão `.ROM` no nome do arquivo. A tag não diferencia maiúsculas de minúsculas. Por exemplo, nomear um arquivo `Knight Mare.PL-32.ROM` força o uso do mapper PL-32 para essa ROM. Tags como `SYSTEM` são ignoradas. A lista de possíveis tags que podem ser usadas é: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

### Exemplos
- Produz o arquivo multirom.uf2 com o menu MultiROM e todos os arquivos `.ROM` no diretório atual. Você pode executar a ferramenta usando o prompt de comando ou apenas clicando duas vezes no executável:
  ```
  multirom.exe
  ```

## Como funciona (alto nível)

1. A ferramenta varre o diretório de trabalho atual em busca de arquivos que terminem com `.ROM` ou `.rom`. Para cada arquivo:
   - Extrai um nome de exibição (nome do arquivo sem extensão, truncado para 50 caracteres).
   - Obtém o tamanho do arquivo e valida que esteja entre `MIN_ROM_SIZE` e `MAX_ROM_SIZE`.
   - Chama `detect_rom_type()` para determinar heurísticamente o byte do mapper a ser usado na entrada de configuração. Se uma tag de mapper estiver presente no nome do arquivo, ela substitui a detecção.
   - Se a detecção do mapper falhar, o arquivo é ignorado.
   - Serializa o registro de configuração por-ROM (nome de 50 bytes, 1 byte de mapper, 4 bytes tamanho LE, 4 bytes offset de flash LE) na área de configuração.
2. Após a varredura, a ferramenta concatena (em ordem): binário do firmware incorporado do Pico, uma fatia inicial da ROM do menu MSX (`MENU_COPY_SIZE` bytes), a área de configuração completa (`CONFIG_AREA_SIZE` bytes), a ROM NEXTOR opcional, e então os payloads ROM descobertos na ordem de descoberta.
3. O payload combinado é escrito como um arquivo UF2 chamado `multirom.uf2` usando `create_uf2_file()` que produz blocos UF2 de payload de 256 bytes direcionados ao endereço de flash do Pico `0x10000000`.

## Heurísticas de detecção de mappers
- `detect_rom_type()` implementa uma combinação de verificações de assinatura (cabeçalho "AB", tags `ROM_NEO8` / `ROM_NE16`) e varredura heurística de opcodes e endereços para escolher mappers MSX comuns, incluindo (mas não limitado a):
  - Plain 16KB (byte do mapper 1) — verificação de cabeçalho AB de 16KB
  - Plain 32KB (byte do mapper 2) — verificação de cabeçalho AB de 32KB
  - Mapper Linear0 (byte do mapper 4) — verificação de layout AB especial
  - NEO8 (byte do mapper 8) e NEO16 (byte do mapper 9)
  - Konami, Konami SCC, ASCII8, ASCII16 e outros via pontuação ponderada
- Se nenhum mapper puder ser detectado com confiabilidade, a ferramenta ignora a ROM e reporta "unsupported mapper". Lembre-se de que você pode forçar um mapper via tag no nome do arquivo. As tags não diferenciam maiúsculas de minúsculas e estão listadas abaixo.
- Apenas os seguintes mappers são suportados na área de configuração e no menu: `PL-16,  PL-32,  KonSCC,  Linear,  ASC-08,  ASC-16,  Konami,  NEO-8,  NEO-16`

## Uso do menu seletor de ROMs MSX

Ao ligar o MSX com o cartucho PicoVerse 2040 inserido, o menu MultiROM aparece, mostrando a lista de ROMs disponíveis. Você pode navegar pelo menu usando as teclas de seta do teclado.

![alt text](/images/multirom_2040_menu.png)

Use as teclas de seta para Cima e Para Baixo para mover o cursor de seleção pela lista de ROMs. Se você tiver mais de 19 ROMs, use as teclas laterais (Esquerda e Direita) para rolar pelas páginas de entradas.

Pressione Enter ou Espaço para iniciar a ROM selecionada. O MSX tentará inicializar a ROM usando as configurações de mapper apropriadas.

A qualquer momento enquanto estiver no menu, você pode pressionar a tecla H para ler a tela de ajuda com instruções básicas. Pressione qualquer tecla para voltar ao menu principal.

## Usando Nextor com o cartucho PicoVerse 2040

A opção -n da ferramenta multirom inclui uma ROM Nextor incorporada experimental que pode ser usada em modelos MSX2 específicos. No entanto, essa opção ainda está em desenvolvimento e pode não funcionar em todos os sistemas. Esta versão usa um método diferente (baseado em porta IO) para se comunicar com o MSX, permitindo que funcione em sistemas onde o slot do cartucho não é primário, por exemplo com expansores de slot.

Mais detalhes sobre o protocolo usado para se comunicar com o computador MSX podem ser encontrados no documento Nextor-Pico-Bridge-Protocol.md.

### Como preparar um pen drive para Nextor

Para preparar um pen drive USB para uso com Nextor no cartucho PicoVerse 2040, siga estes passos:

1. Conecte o pen drive USB ao seu PC.
2. Crie uma partição FAT16 de no máximo 4GB no pen drive. Você pode usar a ferramenta incorporada do Nextor (CALL FDISK enquanto estiver no MSX Basic) ou software de particionamento de terceiros.
3. Copie os arquivos do sistema Nextor para o diretório raiz da partição FAT16. Você pode obter os arquivos do sistema Nextor na distribuição oficial do Nextor ou repositório. Você também precisa do arquivo COMMAND2 para o shell de comandos:
   1.  [Página de Download do Nextor](https://github.com/Konamiman/Nextor/releases)
   2.  [Página de Download do Command2](http://www.tni.nl/products/command2.html)
4. Copie quaisquer ROMs MSX (`.ROM`) ou imagens de disco (`.DSK`) que você deseja usar com Nextor para o diretório raiz do pen drive.
5. Instale o SofaRun ou qualquer outro lançador compatível com Nextor no pen drive se você planeja usá-lo para iniciar ROMs e DSKs. Você pode baixar o SofaRun em: [SofaRun](https://www.louthrax.net/mgr/sofarun.html)
6. Ejete o pen drive com segurança do seu PC.
7. Conecte o pen drive ao cartucho PicoVerse 2040 usando um adaptador ou cabo OTG, se necessário.

> **Nota:** Nem todos os pen drives USB são compatíveis com o cartucho PicoVerse 2040. Se você encontrar problemas, tente usar uma marca ou modelo diferente.
>
> **Nota:** Lembre-se de que o Nextor precisa de um mínimo de 128 KB de RAM para operar.

## Ideias de melhorias
- Melhorar as heurísticas de detecção de tipo de ROM para cobrir mais mappers e casos extremos.
- Implementar tela de configuração para cada entrada ROM (nome, substituição de mapper, etc).
- Adicionar suporte para mais mappers ROM.
- Implementar um menu gráfico com melhor navegação e exibição de informações da ROM.
- Adicionar suporte para salvar/carregar configuração do menu para preservar preferências do usuário.
- Suportar arquivos DSK também, com entradas de configuração adequadas.
- Adicionar suporte para temas personalizados do menu.
- Permitir o download de ROMs a partir de URLs da Internet e incorporá-las diretamente.
- Permitir o uso do joystick para navegar no menu.

## Problemas conhecidos

- A inclusão da ROM Nextor incorporada ainda é experimental e pode não funcionar em todos os modelos MSX2.
- Algumas ROMs com mappers incomuns podem não ser detectadas corretamente e serão ignoradas, a menos que uma tag válida seja usada para forçar o mapper.
- A ferramenta MultiROM atualmente suporta apenas Windows. Versões para Linux e macOS não estão disponíveis ainda.
- A ferramenta não valida atualmente a integridade dos arquivos ROM além do tamanho e verificações básicas de cabeçalho. ROMs corrompidas podem levar a comportamentos inesperados.
- Devido à natureza da ferramenta MultiROM (incorporar múltiplos arquivos em um único UF2), alguns antivírus podem sinalizar o executável como suspeito. Isso é normalmente um falso positivo; certifique-se de baixar a ferramenta de uma fonte confiável.
- O menu MultiROM não suporta arquivos DSK; apenas arquivos ROM são listados e iniciados.
- A ferramenta não suporta atualmente subdiretórios; apenas ROMs no diretório de trabalho atual são processadas.
- A memória flash do Pico pode desgastar-se após muitos ciclos de gravação. Evite regravar o cartucho em excesso.

## Modelos MSX testados

O cartucho PicoVerse 2040 com firmware MultiROM foi testado nos seguintes modelos MSX:

| Modelo | Tipo | Estado | Comentários |
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

A ROM Nextor incorporada experimental (opção `-n`) foi testada nos seguintes modelos MSX:

| Modelo | Tipo | Estado | Comentários |
| --- | --- | --- | --- |
| Omega MSX | MSX2+ | Not OK | USB não detectado |
| Sony HB-F1XD | MSX2 | Not OK | Erro ao ler setor do disco |
| TRHMSX | MSX2+ (clone FPGA) | OK | Operação verificada |
| uMSX | MSX2+ (clone FPGA) | OK | Operação verificada |

Autor: Cristiano Almeida Goncalves
Última atualização: 12/25/2025