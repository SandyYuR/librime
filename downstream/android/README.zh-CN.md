# 中州鹅 Android 引擎 SDK

Android 引擎的源码、依赖构建和版本发布集中在本仓库。输入法固定引用本仓库 Release 的 `rime-android-sdk.zip` 及 SHA-256，不再检出 prebuilder 或 prebuilt。

## 构建

在 Linux 使用 Python 3.12、Git、Ninja、C++ 编译器、NDK `28.0.13004108` 和 CMake `3.31.6`：

```sh
export ANDROID_NDK_ROOT=/path/to/android-sdk/ndk/28.0.13004108
export CMAKE_BIN=/path/to/android-sdk/cmake/3.31.6/bin/cmake
export PATH="$(dirname "$CMAKE_BIN"):$PATH"
python3 downstream/android/build.py --abi arm64-v8a --work build/android
```

其他 ABI 为 `armeabi-v7a`、`x86`、`x86_64`。本地可以复用同一个 work 目录依次编译四 ABI，CI 使用独立矩阵。下载缓存位于 `~/.cache/rime-android-sources`，内容逐次按摘要校验；清空缓存后仍能从源码完整构建。

`sources.lock.json` 固定六个原生依赖、Boost 和三个插件的来源与摘要。构建不读取 prebuilt 归档，不追踪插件移动分支，不修改调用者的源码。核心源码从当前提交导出；插件只在隔离的源码副本内应用固定补丁。OpenCC 数据由本机工具从相同来源生成。

## 验证与发布

1. 在 `zhongzhou` 更新源码、必要的依赖锁及插件补丁，运行源码 CI 和 Android SDK CI。后者执行原生回归、四 ABI 完整编译与最终链接验证。
2. 核验 SDK CI 的 `rime-android-sdk` 产物。打包前验证每个静态库的全部 ELF 对象架构、库/接口摘要、工具链、依赖、OpenCC 数据完整性和必需的定制 API。
3. 在已验证提交上创建一个新的 `android-sdk-<日期>-<版本>` 标签；Android SDK CI 完成后自动发布该标签对应的 SDK。已发布标签和产物不覆盖；发现问题发布新版本。
4. 在输入法修改 `native/rime-sdk.lock.json` 的固定标签、源码提交与包摘要，运行下载校验、JNI 链接审计和 APK 回归。输入法更新只涉及源码仓库和输入法仓库。

SDK 只包含 Direct Rime 实际使用的七个静态库、头文件、OpenCC 数据、许可证和来源清单。每个 ABI 的 `source-manifest.json` 记录本仓库源码/官方基线、工具链、依赖及插件和补丁摘要。SDK 顶层清单再记录全部文件摘要。

## 迁移来源与署名

构建参数和插件/依赖补丁迁自 `SandyYuR/prebuilder@6ffb3f230260c53851d1e9ebc60d4edc7a2f868f`；其来源为 Fcitx5 for Android prebuilder。保留原补丁内容和其中的作者署名，`librime-lua.patch` 的原作者为 Rocka。依赖和插件保持迁移前固定版本，本次不改变引擎功能或方案配置。旧仓库及历史标签保留，支持重建此前发布版本。
