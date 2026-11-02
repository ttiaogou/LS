# 全局传感器层

本分支的 global 变体使用源码构建的 `liblocation_accel.so`，不需要上游发布包中的
闭源传感器库、许可证或二进制补丁。ShadowHook 来自固定版本的开源源码，来源和许可见
[`third_party-shadowhook/UPSTREAM.md`](third_party-shadowhook/UPSTREAM.md)。

## 两种方案

- `scoped`：在 LSPosed 作用域中的目标应用内 Hook Java 传感器接口。
- `global`：在 system_server 内挂钩 SensorService 的 `SensorEventConnection::sendEvents`，
  按控制器配置的 UID 和实际订阅句柄投递计步事件。无需将目标应用加入 LSPosed 作用域；
  模块仍须启用系统框架作用域。定位和无线环境走各自的 Java 系统服务 Hook。

## 当前实现与限制

- 默认 `nativeSensorBackend=steps`；`probe` 仅诊断定位，`passthrough` 透传诊断，
  两者都不模拟步数。`legacy` 已移除，所有构建仅提供 `arm64-v8a`。
- 使用符号、ELF 范围、接收者来源分析和运行时代码比对校验挂钩点；不是所有 ROM 都能适配。
  不满足校验时拒绝安装原生 Hook，不能把某一设备的结果视为跨设备保证。
- Java 控制器按路径距离增量计算累计步数；native 为订阅的 counter/detector 句柄生成事件。
  counter 仅在整数变化等需要同步时投递；原始传感器累计值不要求从零开始。
- 首次接管会等待物理 counter 基准，当前有两秒等待及零基准兜底；静止、缺少物理首帧、
  真实值与模拟值切换仍需真机验收。存活连接的基线不因其他连接注销而重置。
- 事件遵守 BOOTTIME 时间和有效期限制；规划器另有五秒新鲜度限制，过期不合成。
- JNI 保留加速度/气压参数，但当前 native 投递路径以计步为主，不应理解为已完整实现
  上游闭源库的加速度/气压行为。
- `submitted/unconfirmed` 只表示提交给原始发送路径，不代表目标应用已接收或计入运动成绩。
  定位心跳的 `invokeCompleted=true` 也不证明目标应用接受定位。

## 构建和使用

Gradle 运行环境需满足项目 AGP 要求，`core-geo` 另需 JDK 17 toolchain：

```sh
./gradlew :app:assembleGlobalDebug
./gradlew :app:verifyJniExports \
  -PjniCheckApk=app/build/outputs/apk/global/debug/app-global-arm64-v8a-debug.apk
```

安装后启用模块及所需系统作用域并重启；打开应用手动开始模拟。不同包名的新旧版本可并存，
但不能同时启用两个模块。所有地图 Key 自备，不能上传私钥或本地配置。

## 验证状态

原生安装、定位心跳和计步提交曾在 Android 16 设备上观察到。
最近的连接生命周期、进程恢复和界面重建修复已通过离线测试并构建；新版本仍需真机复测。
目标应用曾出现约 0.14 km 距离停增及运动初值异常，尚不能宣称已解决。

适配方法见 [`docs/offset-adaptation.md`](docs/offset-adaptation.md)，
投递契约见 [`docs/native-step-delivery.md`](docs/native-step-delivery.md)，
scoped 计步说明见 [`STEP_SIMULATION.md`](STEP_SIMULATION.md)。
