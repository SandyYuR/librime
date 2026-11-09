# 中州鹅 Android 引擎 SDK

Android 引擎的源码、依赖构建和版本发布集中在本仓库。输入法固定引用本仓库 Release 的 `rime-android-sdk.zip` 及 SHA-256，不再检出 prebuilder 或 prebuilt。

## 构建

在 Linux 使用 Python 3.12 或以上、Git、Ninja、C++ 编译器、NDK `28.0.13004108` 和 CMake `3.31.6`：

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

## Windows 本地开发

Windows 同样使用固定 NDK 和 CMake。配置 `RIME_HOST_CC` / `RIME_HOST_CXX` 为宿主 MSVC，`RIME_HOST_RC` / `RIME_HOST_MT` 为 Windows SDK 工具，并设置相应 INCLUDE/LIB；宿主只负责 OpenCC 数据工具，Android 库由 NDK Clang 交叉编译。

中州鹅工作区的 `tools-env.ps1` 从 `D:\GitHub\tools` 读取全部相关工具，`build-engine-local.ps1` 封装开发构建；`-BuildApp` 还会接入主应用 Debug 并运行 APK/链接门禁。

```powershell
python downstream/android/build.py --abi arm64-v8a --work D:/GitHub/tools/rime-build/windows --cache D:/GitHub/tools/rime-android-sources --jobs 4 --development
python downstream/android/package.py D:/GitHub/tools/rime-build/windows/product --commit <构建记录的基准提交> --development-output D:/GitHub/tools/rime-sdk-local --abis arm64-v8a
```

开发模式以临时 Git 索引导出当前源树，不改变真实暂存区或提交历史；未改的 C++ 输入保留时间戳，以便增量编译。Windows 安全复制内部链接内容，无需符号链接权限。锁和补丁按 Git 的 LF 字节校验，其他改动仍必须匹配摘要。

开发 SDK 明确记录 `development`、源树 SHA、ABI 和逐文件摘要。主应用需显式传 `-PlocalRimeSdkDir=...`，只允许 Debug；去掉参数恢复原发行锁。正式打包仍要求四 ABI、已提交源码，并拒绝开发输入。

## 迁移来源与署名

构建参数和插件/依赖补丁迁自 `SandyYuR/prebuilder@6ffb3f230260c53851d1e9ebc60d4edc7a2f868f`；其来源为 Fcitx5 for Android prebuilder。保留原补丁的作者署名，`librime-lua.patch` 的原作者为 Rocka。旧仓库及历史标签保留，支持重建此前发布版本。

## 依赖更新

用户词典管理通过 `GetUserDictList` 精确排除恢复流程保留的 `.temp` 临时库（默认落盘为 `.temp.userdb`），避免残留目录被列为正式词典；官方 PR #1247 截至本次同步仍未合入。枚举不删除目录，也不忽略其他以点开头的有效词典。`test/user_dict_manager_test.cc` 验证枚举、真实词典同步及仅有临时库残留时的处理。该源码补丁只有在重新构建并消费对应 SDK 后生效；应用侧捕获同步异常不能替代引擎更新。

临时库由 `UserDictManager::Restore` 在同步快照合并、词库恢复或旧格式升级时创建。整批同步先通过数据库自身的删除操作回收旧 `.temp`；恢复在打开临时库前安装清理守卫，拒绝复用未清空的旧内容，并把打开、占用及最终清理失败返回调用方。数据库实现不认识的文件只记录并保留，不递归删除。回归使用真实 LevelDB 验证普通词典备份、其他设备快照合并、坏库、占用、删除失败、重复恢复及正常库内容不丢失。

依赖不会自动跟随上游分支；`sources.lock.json` 的 URL 和 SHA-256 决定实际使用的源码。升级时同时更新版本或提交、源码 URL、源码摘要，检查本地补丁并更新其摘要。补丁或安装布局改变时一并调整构建配方。每次依赖升级都发布新的 SDK 标签，再由输入法更新 SDK 锁；已发布版本不覆盖。

2026-11-05 更新 Lua 5.4.7 → 5.4.9、Boost 1.90.0 → 1.92.0、OpenCC → 1.4.2，并同步 librime-lua、octagram、LevelDB 的固定上游提交。Lua 使用 lua.org 官方 5.4.9 源码包及最小 CMake 安装配方，保留 32 位 Android API 23 的文件定位兼容补丁；不跨越到 Lua 5.5。OpenCC 的 Android 库与主机生成工具使用同一版本源码。predict 无后续提交；glog 0.7.1、yaml-cpp 0.9.0、Marisa 0.3.1 已是最新正式版，保持原版本。

复用构建目录时，Boost 头文件缓存按源码摘要重新生成，避免依赖锁已更新而实际仍编译旧头文件。Android SDK CI 的主机回归使用 Ubuntu 系统依赖，不能替代升级组件在 Android 上的运行验证。

依赖配方的配置、编译和安装均限制 Git 向父目录查找，避免 OpenCC 在 CI 的嵌套源码目录中误用 librime 标签生成自身版本号。Android 库与主机工具都校验生成的 OpenCC 版本必须与依赖锁完全一致。
