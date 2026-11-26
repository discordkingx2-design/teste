# XYZ Optimizer

Otimizador para Windows 10/11 feito em C++ (Win32 + GDI+), com interface escura, cards animados e
foco em desempenho, baixa latência de entrada e Windows enxuto. Inclui o modo **Gráfico de batata** para o Roblox.

## Como compilar pelo GitHub (sem instalar nada)

1. Crie um repositório no GitHub e envie todo o conteúdo desta pasta (o `.github/workflows/build.yml` precisa ir junto).
2. Abra a aba **Actions**. O workflow **Build XYZ Optimizer** roda sozinho a cada push (ou clique em *Run workflow*).
3. Quando terminar, baixe o `XYZOptimizer` em **Artifacts**. Dentro está o `XYZOptimizer.exe`.

## Como compilar localmente

Precisa de Visual Studio 2022 (carga "Desenvolvimento para desktop com C++") e CMake:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

O executável fica em `build/Release/XYZOptimizer.exe`. Ele pede permissão de administrador ao abrir.

## O que ele faz

| Aba | Itens |
|---|---|
| Desempenho | Mouse sem aceleração, plano Alto Desempenho, Game DVR desligado, Modo Jogo, sem pop-up de aderência, efeitos visuais mínimos |
| Bloatware | Apps inúteis da Microsoft, apps de terceiros, Xbox (opcional), OneDrive (opcional), anúncios/apps sugeridos, widgets/notícias/Copilot |
| Privacidade | Telemetria, ID de publicidade, apps em segundo plano, relatório de erros |
| Roblox | Gráfico de batata (nível 1) |
| Limpeza | Temporários, lixeira, dumps de falha |

## Segurança e reversão

- Antes de aplicar, o app cria um **ponto de restauração** (dá para desligar no alternador da barra lateral).
- Todo item marcado como **Reversível** guarda o valor original em `%APPDATA%\XYZOptimizer\backup.txt` e volta ao normal com **Reverter selecionados**.
- Itens **Permanentes** (remoção de apps, OneDrive, limpeza) não têm reversão pelo app. Apps removidos podem ser reinstalados pela Microsoft Store.

## Escolhas de projeto

Só entraram ajustes com mecanismo claro. Ficaram de fora, de propósito, os tweaks de registro sem efeito
medido (TCPNoDelay, SystemResponsiveness, "limpadores de RAM") e o timer resolution, que no Windows moderno
vale só por processo. Não existe "input lag zero": o app reduz o que o Windows controla (aceleração do mouse,
captura em segundo plano, interrupções, estabilidade de clock).

## Roblox: ponto a verificar

O modo batata edita `%LOCALAPPDATA%\Roblox\GlobalBasicSettings_13.xml` (configuração do próprio usuário,
com backup). Não altera arquivos do jogo nem usa FFlags, para evitar risco de bloqueio por anti-trapaça.
Se o Roblox estiver em modo gráfico *Automático*, troque para *Manual* uma vez no menu do jogo.
Esse comportamento ainda precisa ser confirmado numa máquina real.

## Medir o resultado

Use o HWiNFO (PresentMon) ou o CapFrameX antes e depois, com o mesmo cenário e 3 passadas. Diferenças abaixo de 2-3% são ruído.
