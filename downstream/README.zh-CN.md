# 中州鹅 librime 定制源码

本仓库复刻自 rime/librime。默认 master 用于跟踪官方源码；中州鹅使用 zhongzhou 分支的固定提交。

引擎功能以本仓库中的源码提交为唯一维护来源。prebuilder 只固定源码、准备插件和执行编译/测试，不能再重复应用已迁入的核心补丁；prebuilt 保存验证后的四 ABI 产物，输入法再固定 prebuilt gitlink。

## 已迁入的功能

- 音节/编码选择器及扩展 API；按运行条件启用的翻译器回归。
- 音节查询缓存、词典并行部署、用户词典缓存及删除/重新学习的失效处理。
- 映射文件修复、忘词后统一刷新，避免多个监听器误删其他候选。
- rime/librime PR #1232 的 rewriter；该 PR 尚未进入官方 master，仍由此分支维护。
- 冷启动配置指纹。

9 项原补丁分别迁成 9 个提交，源补丁、摘要与提交映射保存在 [manifest.json](manifest.json)。插件仍由 prebuilder 按固定 gitlink 和各自插件补丁准备。本次迁移的核心代码和之前已验证版本逐文件一致。

## 维护流程

1. 从 zhongzhou 新开功能/升级分支。按功能提交源码和必要测试，勿在官方跟踪分支上开发。
2. 更新上游时配置 upstream=https://github.com/rime/librime，获取官方固定提交，在工作分支合并、解决冲突，并更新 manifest.json 的 upstream_commit。migration 字段是初次迁移的历史，不随日常功能改动重写。
3. 运行引擎测试，审查音节选择、忘词、重新学习、rewriter、部署与启动指纹；提交 PR 到 zhongzhou。必须等 PR 最新提交的 Commit CI 全部通过，确认格式检查及 Linux、macOS、Windows、Docker 任务没有因前置失败而跳过，才可合并。合并后的 zhongzhou CI 也须通过后再推进下游。已发布提交保留不可变标签，不强推替换已发布历史。
4. 在 prebuilder 工作分支把 librime gitlink 更新到上述已通过 CI 的确切提交，执行 librime-only；四 ABI、原生测试和源码契约检查全部通过后，才可合并并推进 prebuilt。prebuilder 的成功不能代替源码仓库自身的 CI。
5. 核对插件、依赖与逐 ABI 产物摘要，提交到 prebuilt 的工作分支；输入法再更新其 native/prebuilt gitlink，完成 APK 与设备回归。

不会通过追踪移动分支自动改变已发布 APK。回退时恢复固定的源码/产物提交，保留用户数据。

## 2026-11-04 上游同步

- 官方基线更新到 `e930f57b5cc2eeac91aed3b6c7fe62e96f1fc8bc`，包含 #1203 的有界编译图、共享输出串行化、内存预算及串行回退。
- 合入 #1196 的 `a559ef5949481b5157b478401d78b70b3c17dab1`，补齐跨实例共享缓存、事务内连续更新和回滚一致性，以及禁用缓存时的回退修复。
- #1232 的 `8530499b19f2944776ba1d6988fc70712ddeacf2` 已与现有 12 个源码文件逐字节一致；保留其注册及部署钩子，并将 workspace 串行准备与发布/回滚接入新的编译图。
- 音节/编码选择 API、忘词刷新和启动配置指纹继续保留；初次迁移记录不重写。三个 PR 的固定来源见 `manifest.json` 的 `upstream_prs`。
