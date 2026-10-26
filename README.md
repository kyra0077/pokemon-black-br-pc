# Pokémon - Black (BR): executável nativo para PC

Gera `dist/Pokemon - Black (BR).exe`, um executável Windows autônomo: a ROM vai
embutida como recurso Win32 (`RCDATA`) e o núcleo do melonDS 1.1 (com JIT x86_64,
FreeBIOS e firmware gerado) é compilado estaticamente. Basta dar dois cliques: não
precisa de DLLs, BIOS, firmware nem do arquivo `.nds`.

## Compilar

Requisitos (MSYS2, shell MINGW64):

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python
```

Clone com o submódulo do melonDS (ou deixe o `build.bat` baixá-lo):

```
git clone --recurse-submodules <url-deste-repositório>
```

A ROM **não** faz parte do repositório: coloque a sua cópia como `Pokemon - Black (BR).nds`
na raiz (ou use `-DGAME_ROM=...`). Depois rode `build.bat`
(ou `cmake -S . -B build -G Ninja && cmake --build build --target game`).

## Controles

Padrões (todos remapeáveis, veja abaixo):

| Ação | Teclado | Controle |
|---|---|---|
| A / B / X / Y | X / Z / S / A | botões na posição do DS (B/leste = A, A/sul = B, Y/norte = X, X/oeste = Y) |
| L / R | Q / W | LB / RB |
| Direcional | setas | D-pad (o analógico esquerdo também funciona) |
| START / SELECT do DS | Enter / Shift direito ou Backspace | clique do analógico direito / esquerdo |
| Mostrar/ocultar a tela de toque | TAB (ESC sempre funciona) | START, BACK/SELECT, LT ou RT |
| Tocar na tela | clique esquerdo no painel | analógico move o cursor, o botão ligado ao "A" toca |
| Avanço rápido | segurar Espaço | — |
| Tela cheia / janela | F11 (Alt+Enter sempre funciona) | — |
| Configurar controles | **F1** | botão Guide/Home |
| Mostrar/ocultar a minitela | M | — |

### Minitela

Com o painel fechado, uma prévia ao vivo da tela de baixo fica num canto: na faixa preta
lateral em telas largas (16:9) ou no canto superior direito do jogo. A borda pisca quando a
tela de baixo muda bastante (um menu novo, uma pergunta…), e clicar nela abre o painel de
toque. **M** liga/desliga a minitela; a escolha fica salva em `controles.ini` (`[Opcoes]`).

### Remapear controles

Aperte **F1** no jogo (o jogo pausa). Escolha a ação com ↑/↓, mouse ou D-pad e:

- **Enter / clique / Ⓐ**: adiciona uma tecla, botão ou gatilho à ação (aperte a entrada desejada; Esc cancela).
  Cada ação aceita várias entradas; se a entrada já fazia outra coisa, ela é movida.
- **Delete / botão direito do mouse / Ⓧ**: limpa a ação.
- **Restaurar padrões** volta ao mapeamento original; **F1/Esc** fecha e salva.

O mapeamento fica em `controles.ini`, ao lado do `.exe` (criado na primeira execução,
pode ser editado à mão). F1, Esc e Alt+Enter são fixos para ninguém ficar sem saída.

O jogo abre em tela cheia; `--windowed` abre em janela. Clicar fora do painel de
toque o fecha. O save fica em `Pokemon - Black (BR).sav`, ao lado do `.exe`.

## Estrutura

- `tools/pack_rom.py pack`: valida o cabeçalho, apara o preenchimento da ROM (256 → 201 MB),
  extrai o ícone e o título do banner e gera `game.rc`/`rom_info.h` para o build.
- `tools/pack_rom.py extract` (alvo `cmake --build build --target extract_assets`): extrai
  para `build/extracted/` o ARM9 (descomprimido de BLZ e separado das seções de autoload:
  ITCM, DTCM…), o ARM7, os 237 overlays (descomprimidos), os arquivos do NitroFS e um
  `layout.json` com o mapa de memória. É a entrada da recompilação estática (abaixo).
- `src/main.cpp`: frontend SDL2 (loop principal, overlay da tela inferior, toque, áudio, saves).
- `src/Platform.cpp`: camada de plataforma que o núcleo do melonDS exige.
- `third_party/melonDS`: núcleo do melonDS 1.1 (GPLv3).

## Recompilação estática do ARM9 (em andamento)

O `.exe` acima ainda roda o jogo pelo núcleo do melonDS. Em paralelo, o código ARM9 do
jogo está sendo traduzido para C, função por função. Ainda **não existe um runtime que rode o
jogo** com esse código (faltam o hardware do DS, as threads do SDK e as interrupções). O que
existe e é validado é a tradução em si.

- `tools/recomp/armdecode.py`: decodificador ARMv5TE (ARM + Thumb numa só representação).
- `tools/recomp/recomp.py`: descobre as funções (chamadas, tail calls, tabelas de `switch`,
  veneers, ponteiros em dados) do módulo estático e dos 237 overlays e gera C em `build/recomp/`,
  mais a tabela de despacho para chamadas indiretas. Overlays que dividem o mesmo endereço são
  distinguidos por um hash dos bytes de cada função.
- `tools/recomp/seeds_IRBO.txt` / `update_seeds.py`: pontos de entrada vistos em tempo de execução
  que a análise estática não acha (ponteiros calculados). Entram como entradas "fracas", que não
  partem o fluxo de outras funções.
- `src/recomp/recomp_runtime.h`: interface entre o código gerado e o runtime.
- `src/recomp/difftest.cpp`: teste diferencial. Inicializa o jogo no melonDS, tira um snapshot
  da memória e roda cada função recompilada e a mesma função no interpretador do melonDS a partir
  do mesmo estado (4 variações de argumentos), comparando registradores, flags e cada byte de
  RAM/ITCM/DTCM. Overlays são carregados um a um, junto com os overlays de que dependem.

Para gerar e testar (alguns minutos; ~150 MB de C):

```
cmake -S . -B build -G Ninja -DENABLE_RECOMP=ON
cmake --build build --target recomp_difftest
build\recomp_difftest.exe "Pokemon - Black (BR).nds" --frames 60 --overlays build\recomp\overlays.txt
```

`-DRECOMP_STATIC_ONLY=ON` recompila só o módulo estático (bem mais rápido). Para alimentar as
seeds, rode o teste com `--unknown build\misses.txt` e depois
`python -I tools\recomp\update_seeds.py --layout build\extracted\layout.json --misses build\misses.txt --seeds tools\recomp\seeds_IRBO.txt --table build\recomp\recomp_table.c`.

Resultado atual: 32.174 funções (1,15 milhão de instruções, 75,9% dos bytes de código);
**26.337 casos idênticos ao melonDS, 0 diferenças, 0 travamentos do lado de referência**, cobrindo
13.205 funções. Os ~99 mil casos pulados não podem ser comparados isoladamente, cada um com o
motivo listado: acessos a hardware (IO/VRAM), páginas que o próprio MPU do jogo proíbe (entradas
inválidas), chamadas para overlays que não estão carregados, escrita por cima de código, SWI/CP15.

## Licença e distribuição

O núcleo do melonDS é GPLv3, então o executável também é. A ROM embutida é
conteúdo protegido por direitos autorais: o `.exe` é para uso pessoal com a sua
própria cópia do jogo e não deve ser distribuído.
