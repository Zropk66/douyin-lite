# douyin_lite

抖音 PC 客户端低内存运行器。通过进程挂起注入与 API Hook，将刷视频时 1.5GB~2GB 的内存占用稳定控制在 700MB 以内，不修改抖音安装目录的任何文件。

## 原理

`douyin_lite.exe` 以挂起状态启动 `douyin.exe`，注入 `douyin_lite.dll` 后恢复执行。DLL 在目标进程内完成三件事：

1. **参数改写**：Hook `GetCommandLineW` / `CreateProcessW`，注入 V8 堆上限、渲染进程数限制、磁盘/媒体缓存限制；子进程（GPU、渲染器）创建时自动链式注入。
2. **冗余进程阻断**：拦截崩溃上报、诊断组件、游戏桌面小组件与 guard 保活进程的创建。
3. **内存水位熔断**：后台线程启动后等待预热完成再介入，持续聚合整棵抖音进程树的物理内存，超过阈值时对关联进程设置工作集硬上限（`SetProcessWorkingSetSizeEx` + `QUOTA_LIMITS_HARDWS_MAX_ENABLE`，内核按 LRU 持续挤冷页），带防抖冷却期避免缺页风暴。

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
v8_heap_mb=512
renderer_limit=4
total_threshold_mb=900
single_threshold_mb=400
trim_target_mb=300
cooldown_sec=6
```

各字段含义与调整方法：

| 字段 | 含义 | 调整效果 |
| --- | --- | --- |
| `v8_heap_mb` | 单个渲染进程 V8 JS 堆上限 | 调低可压 JS 对象内存，过低会导致复杂页面卡顿或崩溃，建议不低于 256 |
| `renderer_limit` | 渲染进程数量上限 | 调低减少进程冗余内存，但单进程承担更多页面，内存集中且峰值更明显 |
| `total_threshold_mb` | 整棵进程树物理内存熔断阈值 | 树内存超过此值立即触发全树修剪，是实际内存上限的主开关；调得越高常驻内存越高 |
| `single_threshold_mb` | 单进程物理内存熔断阈值 | 任一进程超过此值触发修剪；调低可更早压制内存大户 |
| `trim_target_mb` | 触发修剪后单进程工作集压到的目标值 | **内存与 CPU 平衡的核心旋钮**；越大内存越高、CPU 越平稳，越小内存越低、CPU 波动越明显 |
| `cooldown_sec` | 两次修剪的最小间隔 | 过小会频繁修剪造成缺页风暴抬高 CPU，过大则内存峰值持续更久 |

修剪触发后的内存走向：各进程工作集被压向 `trim_target_mb`，随后随使用回升，触及 `total_threshold_mb` 或 `single_threshold_mb` 时再次修剪，循环往复，实际内存稳定在这组阈值划定的区间内。

内存与 CPU 占用的平衡规律：**给内存的预算越多，CPU 波动越小**。压缩内存的代价是冷页被挤掉后再次使用需重新从磁盘读入（软缺页），CPU 波动即来源于此。波动为瞬时性质，正常观看单个视频不受影响：

- 波动发生在**下滑加载新视频的瞬间**——新视频的解码缓冲、JS 对象都是刚被挤掉的冷页，需大量软缺页回填
- 想要 CPU 更平稳：调大 `trim_target_mb`（如 300 → 500），并同步上调 `total_threshold_mb` 与 `single_threshold_mb`
- 想要内存更低：调小 `trim_target_mb`（如 300 → 200），并同步下调 `total_threshold_mb`（如 900 → 600）与 `single_threshold_mb`（如 400 → 250）
- 不想手动调参时保持默认值即可，默认配置在内存与 CPU 间取了平衡

修改后重启抖音生效，数值越界或不合法时自动回落内置默认值。

## 构建

本地构建需要 MinGW-w64（g++）：

```cmd
build.bat
```

产物输出至 `bin\`。

## 效果与代价

| 指标 | 原版 | 使用本工具后 |
| --- | --- | --- |
| 物理内存（工作集口径） | 1.5GB ~ 2GB 持续攀升 | 默认配置稳定在 700MB 以内 |
| CPU | 翻页时 20% ~ 30% | 相当，下滑新视频瞬间有概率短时波动增大，正常观看不受影响 |
| 功能完整性 | - | 无变化 |

内存水位由 `douyin_lite.ini` 控制，可按需调高或调低各阈值，改后重启抖音生效。

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
