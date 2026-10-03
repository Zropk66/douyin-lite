# douyin_lite

抖音 PC 客户端低内存运行器。通过进程挂起注入与 API Hook，将刷视频时 1.5GB+ 的内存占用稳定控制在 450MB 以内，不修改抖音安装目录的任何文件。

## 原理

`douyin_lite.exe` 以挂起状态启动 `douyin.exe`，注入 `douyin_lite.dll` 后恢复执行。DLL 在目标进程内完成三件事：

1. **参数改写**：Hook `GetCommandLineW` / `CreateProcessW`，将抖音硬编码的 V8 堆上限（16GB）改写为配置值，并注入磁盘/媒体缓存限制与零拷贝渲染参数；子进程（GPU、渲染器）创建时自动链式注入。
2. **冗余进程阻断**：拦截崩溃上报、诊断组件与游戏桌面小组件的创建。
3. **内存水位熔断**：后台线程持续聚合整棵抖音进程树的物理内存，超过阈值立即对所有关联进程执行工作集修剪，带防抖冷却期避免 CPU 缺页风暴。

## 使用

1. 下载 [Releases](../../releases) 中的压缩包并解压（或自行用 `build.bat` 编译）。
2. 将 `douyin_lite.dll`、`douyin_lite.exe`（可选 `douyin_lite.ini`）放到与 `douyin.exe` 同一目录。
3. 双击 `douyin_lite.exe`。

同目录没有 `douyin.exe` 时通过参数指定：

```cmd
douyin_lite.exe --exe "C:\Program Files (x86)\ByteDance\douyin\douyin.exe"
```

- `--exe <路径>`：目标程序路径，缺省为当前目录下的 `douyin.exe`
- `--dll <路径>`：注入模块路径，缺省为当前目录下的 `douyin_lite.dll`
- `--help`：显示用法

抖音已在运行时重复启动 `douyin_lite.exe`，会将现有主窗口唤起到前台，不会重复注入。

## 配置

`douyin_lite.ini` 置于 `douyin_lite.dll` 同目录，可省略（省略时使用内置默认值）：

```ini
[mem]
v8_heap_mb=512          ; V8 JS 堆上限（MB）
total_threshold_mb=450  ; 进程树总物理内存熔断阈值（MB）
single_threshold_mb=250 ; 单进程物理内存熔断阈值（MB）
cooldown_sec=6          ; 两次修剪的最小冷却间隔（秒），防止缺页风暴抬高 CPU
full_trim_sec=12        ; 保底修剪周期（秒）
```

## 构建

本地构建需要 MinGW-w64（g++）：

```cmd
build.bat
```

产物输出至 `bin\`。

## 效果与代价

| 指标 | 原版 | 使用本工具后 |
| --- | --- | --- |
| 物理内存 | 1.5GB ~ 2GB 持续攀升 | 150MB ~ 450MB |
| CPU（划切瞬间） | 18% ~ 22% | 相当（解码为固有开销） |
| 功能完整性 | - | 无变化 |

代价：向回翻看历史视频时缓存已被释放，需重新加载；窗口失焦一段时间后切回可能出现一次极短的软缺页顿挫。

## 目录结构

```text
douyin_lite/
├── bin/                  编译产物
├── src/douyin_lite.cpp   核心 Hook 模块
├── src/launcher.cpp      启动注入器
├── minhook/              MinHook 依赖（仅编译所需源码）
├── douyin_lite.ini       可选配置
└── build.bat             本地构建脚本
```

## 支持项目

如果这个项目对你有帮助，欢迎点个 Star，这是对作者最好的鼓励。

## 协议

[MIT](LICENSE)

MinHook 依赖遵循其原始 BSD-style 协议（见 `minhook/LICENSE.txt`，随源码分发）。

## 免责声明

仅用于个人设备上的内存占用优化，不涉及任何业务数据篡改。请自行承担使用风险，与本仓库作者无关。
