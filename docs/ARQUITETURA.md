# Verdant FX — Pesquisa, decisão de arquitetura e estado real

Data da pesquisa: 18/10/2026. Fontes consultadas: página pública do SnowyMoon (snowymoon.io),
tópico do fórum do TruckersMP sobre o SnowyMoon, relatos públicos de ReShade no ETS2.
**Não foram consultados** neste ambiente: a documentação oficial da SCS, as regras oficiais
vigentes do TruckersMP, nem a versão instalada do ETS2 (não há jogo aqui).

## 1. O que a pesquisa confirmou

- O ETS2 precisa rodar em **DirectX 11** para esse tipo de plugin (o SnowyMoon exige isso).
- O SnowyMoon se instala copiando um **`dxgi.dll`** (e, em alguns produtos, `dinput8.dll`) para
  `bin\win_x64\` — ou seja, é um proxy de DLL que entra no pipeline DXGI, não um mod `.scs`.
- O SnowyMoon (Lighting v2) lista suporte por versão exata do jogo (1.53 a 1.61) e seu changelog
  mostra correções recorrentes a cada atualização do ETS2. Isso indica que qualquer coisa que
  dependa de shaders/materiais internos do jogo **quebra a cada versão**. O Verdant FX 0.1.0 evita
  isso de propósito: não toca em shaders do jogo, só no resultado final.
- Ele permite coexistir com ReShade em `bin\win_x64\reshade\`, e avisa que ReShade 6.5+ não é
  suportado com ele. Moral: **dois plugins DXGI não convivem na mesma pasta sem encadeamento.**
- O SnowyMoon também documenta falso positivo do Windows Defender no `dxgi.dll`. Um proxy
  próprio terá o mesmo risco (DLL não assinada que entra em processo de jogo).
- Há relato público de ReShade impedindo o ETS2 de abrir em DX11 em alguns sistemas.
  Portanto "funciona em teoria" não substitui testar na sua máquina.
- No fórum do TruckersMP, a equipe respondeu (cerca de 18 meses antes desta pesquisa) que o
  plugin **não é proibido**, mas que usar é por conta e risco, citando um plugin anterior do mesmo
  desenvolvedor que continha código malicioso. **Isso não é autorização oficial para o Verdant FX**
  e pode estar desatualizado.

## 2. Decisão de arquitetura

**Escolhida (v0.1.0): proxy `dxgi.dll` + hook de `IDXGISwapChain::Present` via vtable + um passe
D3D11 de pós-processamento sobre o backbuffer.**

Por quê:
- É a única abordagem sem engenharia reversa do `eurotrucks2.exe`: nada de offsets, assinaturas ou
  escrita na memória do jogo.
- Independe da versão do jogo (usa só a API DXGI/D3D11 pública), então não quebra a cada patch.
- Nitidez adaptativa não exige informações internas do jogo.

Custo da escolha: **sem máscara de vegetação, sem profundidade, sem vetores de movimento.**

Alternativas descartadas por ora:
- *Hook de shaders/materiais de vegetação* (como o SnowyMoon faz para iluminação): exigiria
  identificar shaders internos por versão do jogo. Isso é engenharia reversa de componente interno
  e fragiliza a compatibilidade com o TruckersMP; ficaria como módulo opcional futuro.
- *Plugin de telemetria da SCS SDK*: o SDK oficial de plugins não expõe o pipeline gráfico.
- *Pacote de assets (.scs)*: fora do escopo do plugin; sujeito às regras do TruckersMP.

## 3. Matriz de técnicas

| Técnica | Resolve | Depende de | Estado |
|---|---|---|---|
| Nitidez adaptativa (Adaptive Sharpening) | Perda de definição por filtragem/TAA | Só o backbuffer | **Escrita (HLSL+C++), não compilada**; algoritmo validado em imagens sintéticas |
| Foco em vegetação (Leaf Definition / Grass Clarity) | Aplicar mais nitidez onde há folhagem | Máscara real de vegetação (**não existe**) | **Parcial**: heurística de cor (dominância do verde). Não é identificação de material |
| Proteção de halo / bordas / ruído / sombras | Artefatos do próprio sharpen | Nada extra | Escrita; validada em sintético |
| Preservação de cor | Verde/saturação artificial | Nada extra | Efeito aditivo de luminância (matiz preservado nos testes sintéticos); sem ajuste de cor |
| Anti-aliasing de folhagem | Serrilhado de folhas/galhos | Máscara + profundidade/movimento | **Não implementado** |
| Estabilidade temporal | Cintilação | Vetores de movimento/histórico | **Não implementado** (sem dados temporais confiáveis; não será simulado) |
| Preservação de detalhe à distância / LOD | Troca de modelos, neblina | Acesso a LOD/assets | **Não implementado**; filtro de imagem não reconstrói detalhe ausente |
| Filtragem anisotrópica | Texturas em ângulo | Acesso legítimo ao criador de samplers do jogo | **Não implementado** |
| FSR / DLSS / XeSS / geração de quadros | — | Integração, licenças | **Fora do escopo** |
| GUI "Control Center" dentro do jogo | Configuração | Overlay ImGui no `Present` | **Não implementado**; usa `config.ini` + atalhos |
| Diagnóstico de GPU (timestamps) | Custo medido | `ID3D11Query` | **Não implementado** (só log de eventos) |

## 4. Riscos conhecidos (a validar numa máquina Windows)

1. **Exports do `dxgi.dll`.** O proxy exporta só as funções públicas. O `d3d11.dll` pode importar
   funções não documentadas do `dxgi.dll`; se faltar alguma, o jogo não abre. Verificação:
   `dumpbin /exports C:\Windows\System32\dxgi.dll` e comparar com `src/dxgi.def`.
2. **Vtable da swapchain "dummy".** O hook assume que a swapchain real do jogo usa a mesma classe
   (mesma vtable) da criada no hook. Em modelo flip ou com camadas intermediárias isso pode falhar.
   Nesse caso o plugin simplesmente não atua (e registra no log).
3. **Outros hooks de `Present`** (Steam overlay, RTSS/Afterburner, NVIDIA, Discord): a ordem de
   encadeamento pode causar travamento ou desfazer o efeito.
4. **Formatos de backbuffer.** Suportados: RGBA8/BGRA8 (UNORM/sRGB) e R10G10B10A2. HDR (FP16) e MSAA
   no backbuffer **não** são suportados: o passe é desativado com mensagem no log.
5. **Antivírus.** DLL não assinada em processo de jogo pode gerar falso positivo.
6. **Estado D3D11 do jogo.** O passe salva/restaura os estados que altera (RT, viewport, RS, blend,
   depth, shaders, SRV 0, CB 0). Predicação e *stream-out* não são tratados.
7. **Custo.** Cópia de backbuffer + 1 passe fullscreen por quadro. **Não medido.** A meta (≤3% de FPS
   médio, ≤5% no 1% low) continua sendo só uma meta.

## 5. Estado de compatibilidade com o TruckersMP (classificação honesta)

| Nível | Estado |
|---|---|
| Compatível em testes locais | **Não testado** |
| Compatível em single-player | **Não testado** |
| Inicializa pelo launcher do TruckersMP | **Não testado** |
| Funciona visualmente no multiplayer | **Não testado** |
| Validado em versão específica do jogo | **Nenhuma** |
| Oficialmente autorizado | **Não** — as regras oficiais vigentes não foram consultadas |

O plugin **não** toca em rede, física, tráfego, posições nem anti-cheat, e não tenta se ocultar.
Antes de usar online, leia as regras atuais do TruckersMP e, se houver dúvida, pergunte à equipe.
Em caso de qualquer dúvida, use só em single-player.

## 6. Metodologia de teste (pendente)

Mesma rota, câmera, hora, clima, resolução, escala (100/125/150/200/300%) e antialiasing, com o
plugin desligado e ligado (`Ctrl+Alt+V`), repetindo ao menos 5 vezes por cenário. Cenários:
árvores perto/médio/horizonte, grama, campos, arbustos, rodovia em alta velocidade, chuva, neblina,
amanhecer/entardecer, noite e áreas cheias do TruckersMP. Medição: FPS médio, 1% low, frametime
(PresentMon/CapFrameX), VRAM e RAM, estabilidade em sessão longa. Resultados: **nenhum ainda** —
a tabela final deve ser preenchida apenas com medições reais.

## 7. Próximos passos recomendados

1. Compilar em Windows (MSVC x64) e rodar em single-player; conferir o log.
2. Verificar exports (risco 1) e testar a vtable (risco 2).
3. Capturas A/B e medição de custo; só então ajustar os valores padrão.
4. Decidir, com base nos resultados, se vale adicionar overlay de configuração e GPU timestamps.
5. Ler as regras vigentes do TruckersMP antes de qualquer uso online.
