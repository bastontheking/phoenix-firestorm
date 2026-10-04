# Firestorm "Perf + AI" — um fork experimental feito no vibe coding

> **Isto não é o Firestorm oficial.** É um fork pessoal e experimental do
> [Firestorm Viewer](https://github.com/FirestormViewer/phoenix-firestorm)
> (o viewer open source para Second Life). Não tem relação com a equipe do
> Firestorm nem com a Linden Lab. Use por sua conta e risco.

## Por que este fork existe

O código base dos viewers de Second Life é **antigo**: boa parte do frame roda
em uma única thread, e muito trabalho pesado acontece na thread principal no
meio do frame. Em CPUs modernas, com muitos núcleos, isso deixa a maior parte
do processador parada enquanto um núcleo só segura todo o resto, e o resultado
são travadinhas (stutter), principalmente em lugares cheios.

Eu fiz este fork no **vibe coding**: as mudanças foram escritas conversando
com uma IA (Claude, da Anthropic), que leu o código, propôs, implementou,
compilou e corrigiu, enquanto eu testava no Second Life e dizia o que estava
bom ou ruim. Os objetivos eram:

1. **Desempenho e estabilidade de frame**: menos travadas, mais FPS, sem
   baixar a qualidade gráfica global.
2. **Recursos novos usando IA local**: uma LLM rodando na minha própria
   máquina (llama.cpp, LM Studio, Ollama...). Nada vai para serviços de IA na
   nuvem.

Nos meus testes ficou melhor que o original, mas **não é um benchmark formal**.
O viewer tem um overlay e um log de frame pacing (descritos abaixo) para quem
quiser medir antes e depois na própria máquina.

Todas as mudanças no código estão marcadas com `<FS:Perf>` e ficam no branch
`perf-overhaul`. O primeiro commit do fork vem logo depois do commit upstream
`48d525fca3`.

---

## Parte 1 — Desempenho

Os detalhes técnicos (arquitetura, gargalos encontrados, como medir) estão em
[`doc/performance_overhaul.md`](doc/performance_overhaul.md). Resumo do que foi
feito:

### 1.1 Job system (paralelismo dentro do frame)
- Novo `LL::JobSystem` (`indra/llcommon/lljobsystem.*`): um `parallelFor` do
  tipo fork-join que usa os núcleos da CPU para trabalho que o frame precisa
  **agora**.
- Sem locks no caminho principal; a thread que chama também trabalha, e o
  número de workers é calculado automaticamente a partir dos núcleos.
- Num teste isolado de estresse, todos os casos passaram: cobertura,
  chamadas aninhadas e várias threads chamando ao mesmo tempo. Em 16 threads
  ficou 10,5× mais rápido que a versão serial.
- Setting: `FSJobSystemThreads` (0 = automático).

### 1.2 Trabalho tirado da thread principal (parado em série → paralelo)
| O quê | Antes | Agora | Setting |
|---|---|---|---|
| Prioridade das texturas (área na tela de cada face) | serial na main thread | paralelo no job system | `FSParallelTextureStats` |
| Repriorização de emergência quando a memória acaba | todas as texturas em um único frame | paralelo | — |
| Análise de transparência (alpha) e máscara de clique das texturas | varredura pixel a pixel na main thread | paralela, com resultado idêntico | — |
| Matrizes de skinning dos avatares | montadas uma a uma dentro do desenho | todos os avatares visíveis em paralelo antes do culling | `FSParallelSkinningPalettes` |

### 1.3 Travadas (stutter) eliminadas
- **Reconstrução de geometria:** o Firestorm recebia um orçamento de tempo
  para isso e **ignorava** esse orçamento, processando a fila inteira num
  único frame. Agora o orçamento é respeitado. Objetos próximos, avatares,
  attachments, HUDs e meshes que acabaram de carregar continuam sendo
  reconstruídos na hora; o resto é distribuído entre os frames seguintes.
  Settings: `FSBudgetGeometryUpdates`, `FSGeomUpdateMinBudgetMs`,
  `FSGeomUpdateNearDistance`.
- **Meshes carregadas:** antes eram copiadas inteiras na main thread; agora
  os dados são movidos, sem cópia. A fila de meshes prontas também ganhou um
  limite por frame (`FSMeshLoadedBudgetMs`).
- **Impostors** (avatares desenhados como "foto"): antes todos os
  desatualizados eram regenerados no mesmo frame. Agora só os N mais antigos
  por frame (`FSMaxImpostorUpdatesPerFrame`).
- **Workers dos thread pools:** antes esperavam com `Sleep(1)` em loop, o que
  somava 1–2 ms de atraso a cada tarefa de decodificação, download ou mesh.
  Agora acordam na hora, por notificação.
- **Limitador de FPS:** antes truncava a espera para milissegundos inteiros.
  Agora agenda os frames por prazo, com precisão abaixo de 1 ms.

### 1.4 Qualidade adaptativa (orçamento de frame)
- Novo `LLFrameBudget`: compara o tempo de frame com a meta
  (`FSFrameBudgetTargetFPS`, 60 por padrão) e calcula uma "pressão" de 0 a 1.
- Quando falta tempo, reduz **primeiro** o que não dá para perceber:
  - a frequência de animação dos avatares pequenos na tela;
  - o LOD de objetos pequenos na tela;
  - quantos impostors são atualizados por frame;
  - o tempo gasto em reconstruções que podem esperar.
- **Nunca** degrada o que está grande ou perto da câmera, nem attachments ou
  HUDs. A qualidade volta aos poucos quando sobra tempo.
- Ignora frames com a janela fora de foco ou minimizada e respeita o
  limitador de FPS.
- Settings: `FSAdaptiveQuality`, `FSAvatarAnimLOD`.

### 1.5 LOD de animação
- Avatares que aparecem pequenos na tela animam a 1/2, 1/3 ou 1/4 da taxa
  de frames. A pose fica parada entre as atualizações, mas a velocidade da
  animação continua correta.

### 1.6 Medição
- **Overlay na tela:** *Advanced > Show Info > Show Performance Stats*
  (`FSShowPerfStats`). Mostra:
  - FPS médio, mediana, p99 e p99,9 do tempo de frame;
  - 1% low e 0,1% low;
  - spikes (frames mais lentos que o dobro da mediana);
  - pressão do orçamento de frame;
  - uso dos workers;
  - reconstruções adiadas;
  - avatares com animação reduzida.
- **Log:** `FSLogFramePacing` grava uma linha `FramePacing` no log a cada 5 s.
- Quase todas as otimizações têm uma chave em *Debug Settings*. Desligando
  todas, você tem o comportamento antigo **no mesmo executável**, para
  comparar.

---

## Parte 2 — Recursos de IA (com a sua própria LLM)

Todos os recursos de IA usam um servidor **compatível com a API da OpenAI**
que você mesmo roda: [llama.cpp](https://github.com/ggml-org/llama.cpp)
(`llama-server`), LM Studio, Ollama, vLLM etc. O texto vai **apenas** para o
endereço que você configurar. O único serviço externo é a busca na web do
ChatBot, que é opcional e vem desligada: quando ligada, só o texto da sua
pergunta vai para o DuckDuckGo.

Para modelos que "pensam" antes de responder (Gemma 4, Qwen3...), o viewer
pede ao servidor para pular o raciocínio (`FSAIWriterDisableThinking`). Sem
isso, o modelo gasta todos os tokens pensando e a resposta volta vazia.

### 2.1 Janela de configuração (botão ⚙)
O botão de engrenagem na barra do chat ou do ChatBot abre esta janela:
- **Server:** endereço do servidor, por exemplo
  `http://IP:8080/v1/chat/completions` (`FSAIWriterEndpoint`).
- **Model:** o modelo a usar, com o botão **Load models**, que pega a lista
  em `/v1/models` (`FSAIWriterModel`).
- **Skip model reasoning:** liga ou desliga o raciocínio do modelo.
- **Tradução:** tradução automática, item no menu de clique direito e idioma
  de destino.
- **ChatBot:** acesso à internet, mostrar as fontes, contexto do Second Life
  e **Clear history**.
- **Test connection:** faz uma tradução de teste para conferir o servidor.

### 2.2 Sugestões de escrita na barra do chat (nearby e IM)
```
[ sugestões — aparecem sozinhas enquanto você digita        ]
[campo de texto] [PT-BR/EN] [Estilo ▾] [⚙] [emoji] [send]
```
- Você digita, e **1 segundo** depois de parar (`FSAIWriterAutoSuggestDelay`)
  aparecem **3 sugestões** para a sua frase.
- **Idioma** (PT-BR ou EN): o idioma em que as sugestões são escritas. Se
  precisar, a frase é traduzida.
- **Estilo:** Nicer, Formal, Casual, Romantic, Funny ou Fix only (só corrige
  erros).
- Clicar numa sugestão coloca o texto no campo de digitação. **Nada é
  enviado sozinho**; é você quem aperta Enter.
- A faixa de sugestões some quando o campo está vazio e quando a mensagem é
  enviada.
- Ignora comandos que começam com `/` e janelas que não estão visíveis.
- Settings: `FSAIWriterAutoSuggest`, `FSAIWriterLanguage`, `FSAIWriterStyle`,
  `FSAIWriterShowButton`.

### 2.3 Tradução de mensagens
- **Pelo clique direito** em qualquer mensagem do chat ou de um IM, em
  *Translate message (AI)*. A tradução aparece **logo abaixo** da mensagem
  original, em itálico, com cor própria, no mesmo tamanho de fonte:
  ```
  [20:14] Pessoa: Hi!
  [Tradutor] Pessoa: Oi!
  ```
- **Automática** (`FSAIWriterAutoTranslate`): cada mensagem que chega em outro
  idioma ganha a tradução embaixo. Ficam de fora:
  - mensagens que já estão no idioma de destino;
  - as suas próprias mensagens;
  - avisos do sistema;
  - mensagens de objetos;
  - o histórico carregado do log.
- Cada linha é traduzida **uma vez só**, e uma tradução nunca é traduzida de
  novo. A linha de tradução existe **só na sua tela**; nada é enviado ao SL.
- Settings: `FSAIWriterTranslateTo` (Brazilian Portuguese por padrão),
  `FSAIWriterTranslatorTag` ("Tradutor"), `FSAIWriterTranslatorColor`,
  `FSAIWriterTranslateMenu`.

### 2.4 ChatBot (aba na janela Conversations)
Uma aba fixa logo abaixo de **Nearby Chat**, com um ícone de robô:
```
[ conversa ................................................... ]
[ mensagem para o ChatBot ...... ] [⚙] [Clear] [Send ▴]
```
- É uma conversa privada com a sua LLM, **com histórico**. O histórico fica
  salvo por conta (`ai_chatbot_history.xml`), e as últimas
  `FSAIChatbotHistoryLength` mensagens vão como contexto.
- **Send ▴:** a setinha troca entre **Send** e **Send w/o history**. O
  segundo modo é uma pergunta avulsa: não lê nem altera a conversa e aparece
  em itálico.
- **Clear** (ou digitar `/clear`): apaga a conversa.
- **Acesso à internet** (opcional, `FSAIChatbotWebSearch`): pesquisa a
  pergunta na web (DuckDuckGo Lite por padrão, ou o seu próprio SearXNG via
  `FSAIChatbotSearchURL`) e entrega os resultados à IA. As fontes podem
  aparecer embaixo da resposta, com links clicáveis
  (`FSAIChatbotShowSources`).
- **Conhece o Second Life** (`FSAIChatbotSLContext`):
  - Cada pergunta leva junto a região, o seu avatar, os avatares a até 256 m
    (com distância) e os amigos online.
  - Se a pergunta citar um avatar **próximo ou amigo** (por display name,
    username ou primeiro nome), o viewer busca o **perfil** dele: bio, data de
    criação da conta, parceiro, grupos, picks e link do perfil web.
  - Exemplo: *"fale sobre <nome do avatar>"*.
  - O texto dos perfis é tratado apenas como autodescrição da pessoa, nunca
    como instrução para a IA.
  - Respeita as restrições do RLV (@shownames e @showloc).
- Para esconder a aba: `FSAIChatbotEnabled`.

---

## Parte 3 — Ajustes menores
- **Corretor ortográfico:** já existia no Firestorm (Hunspell, com pt-BR
  incluído). Só foi preciso disponibilizar os dicionários no build de
  desenvolvimento.
- **Fast Timers:** removido o atalho Ctrl+Shift+9 que abria essa janela. No
  teclado ABNT2, Shift+9 é `(`, então ela abria sem querer. A janela continua
  em *Advanced > Consoles*.
- **Build em caminhos com espaço:** corrigido o include do compilador de
  recursos (`rc.exe`) no CMake.

---

## Como compilar (Windows)

Os passos são os mesmos de [`doc/building_windows.md`](doc/building_windows.md).
Eu usei o Visual Studio 2026 (toolset 14.4x+) e o `AUTOBUILD_VSVER=180`:

```bash
set AUTOBUILD_VSVER=180
set AUTOBUILD_VARIABLES_FILE=<caminho>\fs-build-variables\variables
autobuild configure -A 64 -c ReleaseFS_open -- --chan Perf -DLL_TESTS:BOOL=FALSE
autobuild build -A 64 -c ReleaseFS_open --no-configure
```

Sem `--package`, o executável é de desenvolvimento e precisa ser iniciado com
a **pasta de trabalho em `indra\newview`**, por exemplo:
```bat
cd /d phoenix-firestorm\indra\newview
start "" ..\..\build-vc180-64\newview\Release\firestorm-bin.exe
```

---

## Avisos importantes
- **Não é o Firestorm oficial.** Não peça suporte deste fork à equipe do
  Firestorm.
- Se for **distribuir executáveis** para outras pessoas:
  - use outro nome, sem os logos do Firestorm;
  - siga a política de marcas da Linden Lab e a
    [Third Party Viewer Policy](https://secondlife.com/corporate/third-party-viewers);
  - lembre que os recursos de IA podem enviar texto de outras pessoas
    (mensagens recebidas, perfis) ao servidor configurado. Deixe isso claro
    para os usuários e mantenha esses recursos desligados por padrão.
- Código **vibe coded**: foi revisado e testado, mas pode ter bugs.

## Licença
O código continua sob a **GNU LGPL 2.1**, como o projeto original (veja
[`doc/LICENSE-source.txt`](doc/LICENSE-source.txt)). Os arquivos novos deste
fork também são LGPL 2.1. A arte e as marcas seguem as licenças e políticas
originais da Linden Lab e do Phoenix Firestorm Project.
