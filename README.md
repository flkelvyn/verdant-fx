# Verdant FX — Advanced Vegetation Graphics Enhancement (v0.1.0, protótipo)

Plugin gráfico original para Euro Truck Simulator 2 (DirectX 11, Windows x64), com foco em
**nitidez adaptativa da vegetação**. Referência de categoria: SnowyMoon (sem uso de código dele).

> **Status honesto:** este é um **protótipo em código-fonte**. Nada aqui foi compilado como DLL
> nem executado no ETS2 — o ambiente de desenvolvimento não tem Windows, compilador MSVC/MinGW,
> compilador HLSL nem o jogo. Leia a tabela abaixo antes de instalar qualquer coisa.

## O que foi verificado e o que não foi

| Item | Estado |
|---|---|
| Parser/validador do `config.ini` (portável) | **Compilado e testado** (g++ no Linux, 8 grupos de testes, `tests/test_config.cpp`) |
| Algoritmo de nitidez (halo, ruído, gradiente, matiz, ganho em verde) | **Validado em imagens sintéticas** (`tests/prototype_sharpen.py`) — não usa capturas do jogo |
| Shader HLSL `vegetation_sharpen.hlsl` | Escrito como porta do protótipo. **Não compilado** |
| DLL proxy `dxgi.dll` (hook de `Present`) | Escrita. **Não compilada, não testada** |
| Funcionamento no ETS2 single-player | **Pendente** |
| Funcionamento no TruckersMP | **Pendente** (e autorização oficial **não** verificada) |
| Desempenho (FPS, 1% low, frametime, VRAM) | **Pendente — nenhum número medido** |
| Interface dentro do jogo (Control Center) | **Não implementada** (só `config.ini` + atalhos) |
| Anti-aliasing de folhagem, estabilidade temporal, distância/LOD | **Não implementados** (ver `docs/ARQUITETURA.md`) |

## O que o plugin faz de fato (quando compilado)

Um único passe no `Present`: copia o backbuffer, aplica *unsharp mask* de luminância com
(1) corte de ruído, (2) atenuação em bordas fortes, (3) proteção de sombras e realces,
(4) limite de overshoot contra halos, e (5) peso maior em pixels de **tom verde**.

Limitações importantes:

- Não existe máscara real de vegetação nesse ponto do pipeline; "vegetação" é uma **heurística de cor**.
  Placas verdes, algum HUD e partes do interior também podem ser afetados.
- Roda depois do TAA/FXAA e da redução de escala do jogo: **não recupera** detalhe perdido por LOD,
  textura ou geometria. Ele só reforça contraste local. No teste sintético o ganho de contraste
  local foi modesto (~7% em verde e ~3,5% em cinza, perfil balanced).
- Não aumenta densidade de grama, não altera distância de renderização, não gera geometria.

## Instalação (somente depois de compilar e validar — não há binário pronto)

1. Compile (seção abaixo) e obtenha `dxgi.dll`.
2. Pasta do jogo: `...\Euro Truck Simulator 2\bin\win_x64\`.
3. Copie `dxgi.dll` e a pasta `VerdantFX\` (com `config.ini` e `shaders\`) para lá.
4. **Não use junto** com outro `dxgi.dll` (SnowyMoon, ReShade instalado como DXGI): só pode existir um.
5. Inicie o jogo em **DirectX 11**. O log fica em `VerdantFX\logs\verdantfx.log`.
6. Atalhos (só com o jogo em foco): `Ctrl+Alt+V` liga/desliga, `Ctrl+Alt+R` recarrega o `config.ini`.

## Desinstalação / restauração

Apague `dxgi.dll` e a pasta `VerdantFX\` de `bin\win_x64\`. Nenhum arquivo original do jogo é
alterado, nada é gravado no registro e nenhum serviço é instalado.

## Se o jogo não abrir

Remova o `dxgi.dll` do plugin. Se o log mostrar "não foi possível carregar o dxgi.dll do sistema" ou
o jogo falhar ao iniciar, veja "Riscos" em `docs/ARQUITETURA.md` (lista de exports).

## Compilar

Windows 10/11, Visual Studio 2022 (MSVC x64), CMake 3.20+:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Teste do parser em qualquer SO com g++:

```
g++ -std=c++17 -I src tests/test_config.cpp src/config.cpp -o test_config && ./test_config
python3 tests/prototype_sharpen.py
```

## Perfis

| Perfil | Taps | Intensidade | Observação |
|---|---|---|---|
| performance | 5 | 0,30 | menos amostras por pixel |
| balanced (padrão) | 9 | 0,45 | |
| quality | 9 | 0,60 | |
| custom | 5 ou 9 | `config.ini` | lê `[Sharpen]` |

Valores iniciais **conservadores e sem ajuste por teste visual**.
