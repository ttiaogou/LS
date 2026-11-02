# 偏移适配建议：未适配 ROM 的自主重推

本文只解决一件事：**遇到没有现成适配的 ROM（推导失败/形状不认识）时，自己把偏移重新推出来，
并让它在这套实现里生效。**

- profile（已知构建的复用表）只负责"适配过的可重复使用"，与本文无关，本文不设计它。
- **只管 native 传感器层**（`libsensorservice.so`）。Java 侧系统接口（`LocationManagerService`、
  Wi-Fi、电话、蓝牙…）的定位方法见 `xposed/ADAPTATION_GUIDE.md`，两者手段与坑不同，不要混用。
- 上游仓库那份 `ADAPTATION_PROGRESS.md`（本仓库不维护、已移除）只登记 Java 侧各组件，native 那一行写的是
  "本评审分支不包含；独立分支评审与验证"，所以 native 的适配状态没有公开清单，只能按本文现场判定与重推。
- 只谈 arm64。32 位不在本文范围。
- 本文是建议，不是流程规范；每一步都给出**要记录什么**和**通过判据**。

## 1. 输入与交付物

**输入**：该设备的 `/system/lib64/libsensorservice.so`（原始字节 + sha256）。

**交付物**：下面五个量，以及一行全部通过的 compatibility：

| # | 量 | 本机实测（Redmi / HyperOS 3 / Android 16） |
|---|---|---|
| 1 | `sendEvents` 的符号与地址 | `_ZN7android13SensorService21SensorEventConnection10sendEventsE…` |
| 2 | 函数入口序言（16 字节 + 条数） | 已记录；`fragments=2` |
| 3 | 接收者对象里 UID 的偏移 | `receiverOffset=56`（0x38） |
| 4 | `sendEvents` 的**直接分支闭包**（函数内/相邻的直接跳转目标，按 FDE 绑定） | `closure=code-direct-closure-bounded`，`fragments=2` |
| 5 | `sensors_event_t` 布局 | 104 字节（`step_wire.h` 已固化） |

## 2. 先确认确实需要重推

读安装日志那一行：

```
[GlobalSensor] native install=<bool> compatibility=<status>;
  receiver=…; receiverOffset=<N>; closure=…; fragments=<N>;
  runtimeVerified=…; memoryAccess=…; libsensorservice.sha256=<前16位>…
```

先排除这几类"看起来不行、其实不用重推"的情况：

| 现象 | 先做什么 |
|---|---|
| 刚替换过模块 APK | 重启一次再看（热重载后 native 层不会重新加载） |
| 后端选错（跑的是 `legacy`） | 用默认 `steps` 后端重装 |
| `runtimeVerified=false`、`memoryAccess` 异常 | 磁盘镜像与已加载镜像不一致：确认改的是正在运行的那份，必要时重启后再取库 |
| `sha256` 与本机基线不同但其余全绿 | 只更新基线记录，不必重推 |

必须重推的典型：符号找到了但序言/闭包/UID 读取模式证不出来（`closure`、`fragments`、
`receiverOffset` 任一为空或拒绝）。

## 3. 取库与基线

```sh
adb shell su -c 'cat /system/lib64/libsensorservice.so' > libsensorservice.so.device
sha256sum libsensorservice.so.device
```

注意事项：

- **取"正在被加载的那份"**。实现里会用 `dl_iterate_phdr` 比对磁盘与已加载字节；如果设备上的
  文件是旧版本（例如 ROM 热更新后未重启），推出来的偏移与实际运行的代码不一致。
- 记录 sha256（前 8 位足够日常对比，完整值写进记录模板）。同一 ROM 的小版本升级也会变。

## 4. 逐项重推

### 4.1 找到目标函数

- 符号：`android::SensorService::SensorEventConnection::sendEvents(sensors_event_t const*, unsigned long, sensors_event_t*, SensorService::wp<SensorEventConnection const> const*)`。
- 找不到符号时（改名/被内联/被 LTO 收掉）改用调用关系锚定：`SensorService::threadLoop()`，
  以及部分 ROM 上多一层 `sendEventsToAllClients(...)`。
- 记录：符号名（或替代锚点）、虚拟地址、**文件偏移**（后续都用文件偏移记录，便于跨构建对比）。

### 4.2 函数边界与序言

- 记录入口前 16 字节，以及序言覆盖的前若干条指令（本机与已知构建的记录方式都是"字节 + 条数"）。
- 通过判据：序言字节与预期模板一致，且函数体的调用/返回闭包可被证明有界（不许出现
  无法解释的间接跳转出口）。
- 失败时：先确认不是 Thumb/别名/跳板（`b` 到真实体）造成的入口错位；跳板要跟着跳到真实体
  再记录。

### 4.3 UID 偏移

- 目标：接收者对象里那个 UID 字段的偏移，以及 `sendEvents` 里**读它**的那条指令模式。
- 找法：从 `SensorEventConnection` 的构造器（`C1`/`C2`）写入 UID 的地方入手，再回到
  `sendEvents` 中比较 UID 的位置；本机结果是偏移 `0x38`，且只在"活体接收者"上读一次。
- 通过判据：能给出"偏移 + 读取指令模式"，且实现里 `memoryAccess` 只报这一种访问方式。
- 失败时：不要放宽成"扫一段内存找像 UID 的值"——那会引入不可控的内存访问。

### 4.4 直接分支闭包（不是调用者集合）

- 含义：`code_closure.cpp` 逐字扫描根函数，只看 `arm64_flow` 判定为
  **直接跳转且非调用**（`flow.branch && !flow.call`）的指令，把目标按其 FDE 片段并入闭包，
  直到有界（`code-direct-closure-bounded`）、遇到没有 FDE 的跳转目标（`code-branch-without-fde`）、
  离开可执行段（`code-fragment-outside-rx`）或触到上限（`code-closure-limit`）。
  间接跳转/调用目标**不解析**。
- 记录：`closure` 状态 + `fragments` 条数（本机 2）+ 每条片段的入口与字节。
- 通过判据：状态为 `code-direct-closure-bounded` 且片段数稳定；它证明的是跳板能覆盖目标函数
  的完整直接控制流，不是"谁调用了它"。
- **调用者关系不在运行时校验**：`threadLoop()` → `sendEvents(...)`（部分 ROM 经
  `sendEventsToAllClients(...)`）这条调用图只在逆向阶段核对过；本实现不做运行期调用者扫描。
  可以暂时不校验的理由：我们只对"已订阅句柄"的事件做改写/追加，其他调用者拿到的是逐字节
  原样转发的事件，多一个调用者不改变其语义。要补的话，做法是扫 `.text` 里的直接 `bl` 目标
  （`elf_function` + `arm64_flow` 已具备能力），把命中集合与该 ROM 记录的两条拓扑比对。

### 4.5 事件布局

- 记录 `sensors_event_t` 的大小与各字段偏移，与 `xposed/src/main/cpp/step_wire.h` 对齐
  （本机 104 字节：`sensor`、`type`、`timestamp`、payload、flags）。
- 通过判据：用 **passthrough 诊断构建的原始事件转储**核对（`probe` 构建只做结构检查、不挂钩，
  采不到事件；`passthrough` 挂钩但不改写，正好用来取样）：

  ```sh
  ./gradlew :app:assembleGlobalDebug -PnativeSensorBackend=passthrough
  adb install -r app/build/outputs/apk/global/debug/app-global-arm64-v8a-debug.apk
  adb reboot                      # 换包必须重启，见 docs/native-step-delivery.md
  # 让任意应用采一次计步（打开计步类应用/微信运动，或点一次系统步数）
  adb logcat -d | grep "GlobalSensorProbe.*raw step event"
  ```

  期望形如 `size=104 sensor=0x010000bf type=19 version=104 ts=… payloadU64=… payloadF32=… flags=0x…`：
  `size=104` 对应 `step_wire.h` 的布局断言；`type=19` 的载荷按 `payloadU64` 读、`type=18` 按
  `payloadF32` 读；`flags` 的 bit31 由原函数按句柄补，因此 wakeup 句柄出现 `0x80000000` 属正常。
  每个进程最多 8 条 step 事件（`maxDumpedStepEvents`），只读不改写（宿主 `event_dump` 测试断言）。

## 5. 让它生效

当前实现没有表，靠**运行时推导**：把上面五个量对应的形态固化进推导器即可，
每一处对应一个量：

| 量 | 对应实现位置 |
|---|---|
| 目标符号/地址 | `target_locator.cpp`（`findElfFunction` 的符号名与锚点选择） |
| 序言与函数边界 | `elf_function.cpp` / `cf. arm64_flow.cpp` / `code_closure.cpp` |
| UID 偏移与读取模式 | `uid_pattern.cpp` / `uid_receiver.cpp` |
| 直接分支闭包（含调用者关系的说明） | `code_closure.cpp` + `arm64_flow.cpp`（调用者集合不校验，见 4.4） |
| 事件布局 | `step_wire.h`（仅在布局真的不同时改，改动要同步宿主机测试） |

固化的是**模式**（指令形态、读取方式）而不是**地址**：地址随构建变化，模式相对稳定；
一旦固化成地址，ROM 小版本升级就会失效。

改完必须重跑：

1. 宿主机测试 `ctest`（当前 20 项，含 ABI 与事件构成相关的回归用例）；
2. 真机用默认 `steps` 后端构建、安装、**重启**，确认 compatibility 行全绿，并看到
   `步数传感器目录: N 组（wakeup=… non-wake=…）`；
3. 更新本文第 1 节表格里的实测值与设备/ROM 标识。

## 6. 记录模板

```
日期：
设备 / ROM / Android 版本：
libsensorservice.so sha256：
后端与构建：           nativeSensorBackend=steps，versionName=…
sendEvents 符号或锚点：
  - 虚拟地址 / 文件偏移：
  - 入口 16 字节：
  - 序言条数：
UID 偏移（receiverOffset）：
  - 读取指令模式：
调用闭包：             threadLoop → sendEvents（是否经 sendEventsToAllClients）
事件布局：             大小 / 字段偏移
compatibility 行（粘贴原文）：
备注（异常、替代锚点、未解释的分支）：
```
