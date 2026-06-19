# FPSeco – Monitor de desempenho (FPS)

Dep: LibreHardwareMonitor (Open Source)

**FPSeco** é um overlay leve para Windows que exibe em tempo real:
- FPS do jogo (DirectX 9/10/11/12, Vulkan e OpenGL)
- Uso e temperatura da CPU
- Uso e temperatura da GPU
- Uso de VRAM e RAM
- Frequência da CPU e GPU (com gráfico sparkline)
- Nome do processo/jogo em execução

## ⬇️ Download

- Windows 10/11: [Download FPSeco](https://github.com/zRafaX/FPSeco/releases/latest)

> Extraia o `.zip` e execute `FPSeco.exe`.

| Layout padrão |
|----------------|
| ![03](https://github.com/zRafaX/StatusOverlay/blob/main/assets/03.png?raw=true) |

| Layout configuração |
|----------------|
|  ![02](https://github.com/zRafaX/StatusOverlay/blob/main/assets/Painel_config.png?raw=true) |

## ⚙️ Configurações (config.ini)

Todas as opções são salvas automaticamente em `config.ini`, no mesmo diretório do executável. Você pode editá‑lo manualmente, se desejar.

Exemplo:

```ini
[Display]
showFPS=1
showCpuUsage=1
showCpuTemp=1
showGpuUsage=1
showGpuTemp=1
showVRAM=1
showRAM=1
showProcessName=1

[Layout]
layoutStyle=0      ; 0=vertical, 1=horizontal, 2=Steam
useFahrenheit=0
position=0         ; 0=SupEsq, 1=SupCentro, 2=SupDir, 3=InfEsq, 4=InfCentro, 5=InfDir
opacity=85
overlayScale=100

[Hotkeys]
toggleKey=45       ; VK_INSERT
exitKey=35         ; VK_END

[GPU]
selectedGpu=0
