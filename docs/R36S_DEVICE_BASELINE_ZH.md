# R36S 真机部署与基线记录（初步）

日期：2026-11-07。设备：Anbernic R36S（RK3326，4×Cortex-A35，Mali-G31，约 895 MB 内存），dArkOS，内核 4.4.189。
游戏：NFSU2 PC 版，3DM 的 `SPEED2.EXE` v1.2（4,800,512 字节，sha1 `c92e5c56…`，即本项目支持的 `v12`）。
本文记录第一次把这个 fork 从源码构建、部署到真机、跑通并取得性能基线的过程，以及中途遇到的问题和结论。
所有帧率数据来自真机，不是 VM 估算。

## 1. 结论摘要

- 从源码构建（Docker）并部署成功，真机可以进菜单、建存档、进比赛、开车。
- 默认画质（`performance`，30 fps 封顶，640×480）下，比赛起跑段约 **27.8 fps**。整局（约 5000 帧）中位数 29.2 fps，最差窗口 26.9 fps。
- 瓶颈在 **game 线程的 CPU**（约 26 到 32 ms/帧）。呈现线程（presenter）已经够用：render 线程等空闲 buffer 的时间是 0.0 ms/帧，所以**现在加第三个 buffer 没有收益**。
- 遇到并修复了两个会让游戏整体退出的问题：缺少 `SDATA/sdat.viv` 导致的 `strlen` 崩溃，以及非 UTF-8 存档目录名导致的 `std::filesystem` 异常。
- 加了片头跳过开关和一套真机自动测试脚本，从启动到取得基线约 60 秒。
- 偶发加载卡死一次，根因未知（见 §9）。
- Xbox 存档能否兼容：没有验证，见 §7。

## 2. 构建

构建命令：

```
python3 build.py <SPEED2.EXE> [--check]
```

产物 `build/r36s/nfsu2-v12.aarch64`（约 88 MB，最高 glibc 符号版本 2.18，依赖 `libSDL2-2.0.so.0`、`libGLESv2.so.2`）。

首次完整构建在 16 核、16 GB 内存的主机上约 **20 分钟**（lift 27,742 个函数，生成约 723 MB C 代码，135 个文件，6 路并行编译）。Docker volume `nfsu2-r36s-work` 里缓存了 lift 结果和已编译的 guest 对象，之后只改 `main.cpp`、`d3d9_gles.cpp`、`guest_*.inc` 一类宿主代码时，增量构建不到 2 分钟。

在中国大陆网络下遇到的两个问题和绕法：

1. Docker Hub 不通：先用镜像拉取 `docker.m.daocloud.io/library/ubuntu:22.04`，再 `docker tag … ubuntu:22.04`，`build.py` 的 Dockerfile 不用改。
2. 容器内 `git clone https://github.com/sp00nznet/pcrecomp` 失败：在主机上取全历史，检出 `35548b8`，用 `git am` 打上 `patches/pcrecomp/*.patch`，把 `.nfsu2-patched` 标记写成这些补丁 `tr -d '\r' | sha1sum` 的值，然后复制进 volume 的 `/work/pcrecomp`（注意 `chown root:root`，否则 git 报 dubious ownership）。脚本检测到标记就不再 clone。

## 3. 部署布局（SD 卡 `ports/` 下）

```
NFSU2.sh                       启动脚本（来自 ports/r36s/package/）
nfsu2/
  nfsu2.aarch64.v12            二进制（启动脚本按 SPEED2.EXE 大小选 .v12）
  nfsu2.cfg  nfsu2.gptk  port.json  README.md  widescreen/
  gamedata/                    SPEED2.EXE + CARS GLOBAL TRACKS SOUND LANGUAGES MOVIES SDATA …
```

游戏数据来源：3DM 版光盘 1、光盘 2 的 `compressed.zip` 合并，加上光盘 2 的 `MOVIES/` 和 `SDATA/`。**`SDATA/sdat.viv`（259 MB）必须拷**，见 §4。

## 4. 崩溃：缺少 `SDATA/sdat.viv`

现象：启动后约 5 秒 SIGSEGV（退出码 139），日志停在 `Guest file nisaudio.idx`，没有任何错误信息。

定位：在启动脚本里加了 gdb 运行模式（`NFSU2_GDB=1`），拿到崩溃点 `sub_0043DBB0`，这是游戏里的 `strlen`。调用链 `sub_0057CA10 ← sub_0057CAC0 ← sub_00484250`。

根因：`sub_00484250` 在 `0x484391` 有一个循环，遍历 `0x7ecfa8` 处一张 16 字节一项的语音文件名表（`speech/UG2_Speech.big`、`…_FR.big`、`…_GR.big` …），逐个调用 `sub_0057CAC0` 查找，直到找到一个存在的文件才停。**循环没有边界检查**。语音文件在光盘 2 的 `SDATA/sdat.viv` 里，缺失时一个都找不到，循环读过表尾，把垃圾值 `0x48881a4b` 当字符串指针传给 `strlen`，崩溃。

这是原版代码的缺陷，只在缺文件时触发。修复：拷入 `SDATA/sdat.viv`（md5 `79af04f75a4a4ca1cb19e9a3a8b61f45`）。

## 5. 存档、语言与路径编码

### 5.1 首次建档与屏幕键盘

启动脚本的注册表默认值里有 `NotFirstTime=1`，所以游戏**不会**弹出首次建档提示，主菜单左下角显示"沒有設定檔"。流程是：标题画面 → 儲存/載入 → "你想要建立新設定檔嗎？" → 輸入名稱 → "你確定要儲存名為…的設定檔？"。

R36S 没有键盘。端口自带屏幕键盘，**按 R3（右摇杆按下）弹出**，方向键选字、B 输入、Y 删除、X 切换大小写、Start 回车、A/L3/R3 关闭。README 之前没写这一点。

### 5.2 存档目录和文件名编码

存档在 `gamedata/AppData/Local/NFS Underground 2/<名字>/<名字>`（约 54 KB）。

手动建档时游戏退出了，日志是 `filesystem error: status: Operation not permitted [.../NFS Underground 2/<乱码>V]`。原因：游戏通过 ANSI 代码页（繁体中文版是 Big5）传文件名，字节 `D6 8B 89 56` 不是合法 UTF-8，exFAT 拒绝这样的名字，`std::filesystem::exists()` 抛异常，异常被当作 guest 停止，整个游戏退出。

修复（`android/app/src/main/cpp/guest_files.inc`）：

- `gamePath()` 入口把 ≥0x80 的字节编码成 `%XX`（大写十六进制），列目录（`FindFirstFile`/`FindNextFile`）时还原。编码是幂等的，结果里不含 ≥0x80 的字节。
- `createFile` 的 `exists()` 和 `weakly_canonical()` 改用 `error_code` 重载，不再抛异常。

现在存档目录名是 `%D6%8B%89V`。

**待查疑点：** 界面显示的名字只有 "V"，但目录名前面多了 3 个字节 `D6 8B 89`。可能是屏幕键盘输入或名字缓冲区未清零，没有验证。

### 5.3 语言参数

`nfsu2.cfg` 里 `NFSU2_LANG="Chinese (Traditional)"` 经启动脚本里 `$LANG_ARG` 不加引号展开，被拆成 `--lang=Chinese` 和 `(Traditional)`，实际回退成西班牙语。已改成 `"$LANG_ARG"`。设备上的 `NFSU2.sh` 同步改了（原文件备份为 `NFSU2.sh.orig`）。仓库里的 `ports/r36s/package/NFSU2.sh` 也已改成带引号。

## 6. 片头跳过

开场顺序：`blank.vp6 → ealogo.vp6 → THX_logo.vp6 → PSA.vp6 → FMVOpening.vp6`。游戏对每个视频先"开一次再关"探测存在两次，再打开第三次真正播放。

实测（用按键连发）：EA logo 约 40 帧，THX 约 110 帧，PSA 约 476 帧（约 16 秒，**按键无法跳过**），FMVOpening 开始后 2 帧就被跳过。结论：logo 和 PSA 不能靠按键跳。

采用的办法：让这些 `.vp6` 的读打开失败（`ERROR_FILE_NOT_FOUND`），游戏认为视频不存在，直接进入下一个画面。已验证不会卡死，帧数正常推进。

- 环境变量 `NFSU2_SKIP_MOVIES=ealogo,thx_logo,psa,fmvopening`（文件名主干，不区分大小写，只拒绝只读打开）。
- 启动脚本开关 `NFSU2_SKIP_INTRO=1`（已写进 `nfsu2.cfg`，默认开启），内部设置上面的变量。
- `main.cpp` 里还有一个 `NFSU2_SKIP_INTRO` 的按键连发实现（视频打开期间每 10 帧敲 Enter/Space/Esc 和手柄 A/B）。启动脚本开关打开时视频根本不会打开，所以这段连发不会触发；保留作为只想缩短、不想全拒绝的备用方案。

另外在 `guest_files.inc` 里加了 `.vp6` 句柄计数（`guestMoviesOpen()`），日志里会出现 `Guest movie … opened/closed at frame N`。

## 7. Xbox 存档兼容性（未验证）

Xbox 版存档在 `UDATA/4541005a/000000000056/`，主文件 `V` 大小 53,868 字节，文件头 `20CM`（即 "MC02" 的小端写法），之后是看起来经过压缩或加密的数据块。PC 版存档（54,966 字节）还没有逐字节对比过。两边都是 x86、同一套引擎，内部结构可能相近，但外层容器、校验和和字段版本很可能不同。结论：**是否可转换要先对比两份存档，现在不能保证**。

## 8. 自动测试与基线

### 8.1 脚本

`ports/r36s/tools/pc_run.py`，拷到设备上以 `python3` 运行（需要设备上的 `evdev`）。它通过 `/dev/input/event2` 注入手柄按键，在 EmulationStation 里按 A 启动游戏（光标需要事先停在 NFSU2 上），然后：

1. 启动前检查，**设备上已有游戏在运行就拒绝**，不碰设备。
2. 以 0.5 秒一次的频率点 B（R36S 底部键 = Cross = 菜单确认）。已存在存档时，一路按 B 就能走到比赛。
3. 日志里出现 `InGameRace.bun` 表示比赛加载开始，此后**停止点按**，改为按住 B（油门）。
4. 日志里出现 `STREAML4RA.BUN` 表示加载完成（加载最后才打开赛道流文件）。
5. 等一个 `[NFSU2 PERF]` 窗口（每 120 帧）回到 ≥22 fps（加载后的纹理解码已结束），再取接下来的两个窗口（约 8 秒）求平均。
6. 用 Select+Start 正常退出，并把 `nfsu2.cfg` 恢复原样。
7. 加载超时（默认 120 秒没出现赛道流）判定卡死，用 gdb 导出所有线程回溯到 `/tmp/pcrun.hang.txt`。

用法示例：

```
python3 /tmp/pc_run.py --secs 300 --drive --nwin 2 \
    --cfg "export NFSU2_SKIP_MOVIES=ealogo,thx_logo,psa,fmvopening NFSU2_SHOT_EVERY=300"
```

输出：`/tmp/pcrun.out`（事件和结果）、`/tmp/pcrun.tsv`（每 2 秒一行：RSS、swap、温度、频率、帧率、各线程 CPU%）。

注意：**加载时间必须单独处理**。冷缓存时比赛加载可能要一两分钟，热缓存只要 3 到 5 秒；如果按固定时间截取，会取到加载阶段（10 fps 左右，texture 解码线程占满）。

### 8.2 基线（默认画质，30 fps 封顶，起跑格）

| 项目 | 数值 |
|---|---|
| 帧率 | 27.8 fps（36.1 ms/帧） |
| game 线程 | 26.2 ms CPU/帧（线程表 32.2 ms/帧） |
| render 线程 | 26.7 ms CPU/帧 |
| GL 驱动 CPU | 16.9 ms/帧 |
| draw 数 | 678/帧 |
| GPU 负载 | 约 52%，520 MHz |
| RSS | 约 605 MB |
| 温度 | 60 到 66 ℃，CPU 频率 1.296 GHz |

整局（Quick Race 约 5000 帧，39 个窗口，帧 1080 起）：

| 指标 | 最小 | p10 | 中位 | 均值 | 最大 |
|---|---|---|---|---|---|
| fps | 26.9 | 28.2 | 29.2 | 29.0 | 29.9 |
| 帧时间 ms | 33.4 | 33.5 | 34.3 | 34.5 | 37.2 |
| game 线程 ms | 14.3 | 15.4 | 20.9 | 20.5 | 25.7 |
| draw 数 | 351 | 369 | 499 | 486 | 574 |
| GL 驱动 ms | 8.6 | 9.4 | 13.2 | 12.9 | 15.8 |

另一次约 12 分钟的真实游玩（菜单加比赛，帧 3000 之后 140 个窗口）：中位 29.4 fps，p10 25.8，低于 25 fps 的窗口占 6%，最低 10.5 fps。该日志里进程 RSS 570 到 600 MB，另有 90 到 110 MB 换到 swap，可用内存只剩约 130 MB。

### 8.3 三缓冲

端口已有 presenter 线程加 2 个离屏 buffer（`NFSU2_PRESENT_SLOTS`，最大 3）。比赛中的 `[NFSU2 PRESENT]` 统计：render 线程等空闲 buffer 0.0 ms/帧，presenter 等 GPU 12.9 到 19.2 ms，blit+swap 1.8 ms；`[NFSU2 WAIT]` 里 render 线程每帧空闲 9 到 10 ms，game 线程的帧率封顶等待只有 0.7 到 1.2 ms。

第三个 buffer 没有东西可填，因此预计没有收益，瓶颈是 game 线程的 CPU。`NFSU2_PRESENT_SLOTS=3` 没有做过 A/B，只是推断。

## 9. 已知问题

- **加载卡死（一次）：** 狂按 B（0.5 秒一次）穿过加载界面时，一次加载停在"載入中"，game 线程仍在跑（约 30 fps，`inputDispatch` 在前端循环里），但两个加载线程（`sub_006F57C4` 一带）停在 `pthread_cond_clockwait`，没有新文件打开。当次对手车是 `GEOMETRY_VOLK/OZ`，与正常的那次不同。gdb 回溯在 `tmp/pcrun/hang_bt.txt`（不在仓库内）。后来改成赛道加载开始后停止点按，之后连续几轮正常。**根因未知**，不排除是加载期间的输入或对手车组合触发的竞态，需要复现。
- 存档目录名前多出 3 个字节（§5.2）。
- 启动脚本的 `.swapfile` 建在 exFAT 上多半无法 `swapon`；这台设备本身已有 767 MB swap，实测不受影响。
- 看门狗 `r36s-guard.sh` 只认 `nfsu2_recomp*` 进程名，PC 版没有卡死保护。设备上它的路径已改成 `/roms/ports/nfs8-xbox`（Xbox 版目录改名后）。

## 10. 本次代码改动

| 文件 | 内容 |
|---|---|
| `android/app/src/main/cpp/guest_files.inc` | 文件名 `%XX` 编码；去掉会抛异常的 `std::filesystem` 调用；`NFSU2_SKIP_MOVIES`；`.vp6` 句柄计数 |
| `android/app/src/main/cpp/guest_runtime.{cpp,h}` | `guestMoviesOpen()` |
| `ports/r36s/main.cpp` | `NFSU2_PROFILE_NAME`（`--auto-advance` 建档时输入的名字，默认 `R36s`）；`NFSU2_SKIP_INTRO` 按键连发备用方案 |
| `ports/r36s/d3d9_gles/d3d9_gles.cpp` | `NFSU2_SHOT_EVERY=N`：每 N 帧保存一张 `diag_frame_<帧>.ppm` |
| `ports/r36s/package/NFSU2.sh`、`nfsu2.cfg` | `NFSU2_SKIP_INTRO` 开关 |
| `ports/r36s/tools/pc_run.py` | 真机自动测试脚本 |

环境变量一览：

| 变量 | 作用 |
|---|---|
| `NFSU2_SKIP_INTRO=1` | 启动脚本：跳过片头（默认 1） |
| `NFSU2_SKIP_MOVIES=a,b` | 指定要"消失"的 `.vp6` 文件名主干 |
| `NFSU2_SHOT_EVERY=N` | 每 N 帧截图 |
| `NFSU2_PROFILE_NAME=V` | `--auto-advance` 建档用的名字，最长 7 个字符 |

调试用启动脚本分支（只在设备上改过，没有进仓库）：`NFSU2_GDB=1` 在 gdb 下运行，`NFSU2_EXTRA_ARGS` 追加命令行参数，日志末尾写 `game exit code N`。

## 11. 下一步

1. 对 game 线程做 `perf record`（设备上有 perf，构建会产出带符号的 `.debug`），看 32 ms 花在 lifted 代码、D3D 调用记录还是 guest 同步上。
2. 把 Xbox 重编译器上验证有效的优化搬过来。本 fork 的 `RECOMP_CALL` 每次调用同步全部 8 个寄存器，没有调用点同步消除；Xbox 上这一项让 main 线程少约 2.2 ms，加上 x87 静态栈、memory-base 局部化，合计约 10 ms。
3. 复现并定位加载卡死。
4. 验证存档名前缀字节的来源，对比 Xbox 与 PC 存档。
5. 观察长时间游玩的温度与降频（现在已到 66 ℃，节流点 70 ℃）。
