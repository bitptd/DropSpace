# 给 macOS Codex 的启动 Prompt

把下面整段直接发送给 macOS 上的 Codex：

```text
你现在负责 DropSpace 第一阶段的 macOS 客户端开发。Windows 客户端和
Go/SQLite VPS 服务端已经由另一端完成基础实现；你的任务是通过 GitHub
在独立 macos 分支继续开发，不得破坏 Windows 构建和现有服务端协议。

GitHub 仓库：
https://github.com/bitptd/DropSpace.git

先执行：

git clone https://github.com/bitptd/DropSpace.git
cd DropSpace
git fetch origin
git switch macos
git pull --ff-only origin macos

不要在 main 或 windows 分支直接开发。所有 macOS 修改提交到 macos 分支，
每完成一个可验证阶段就提交并执行：

git push origin macos

开始编码前必须完整阅读：

1. DROP_SPACE_CODEX_GUIDE.md
2. docs/MACOS_CODEX_HANDOFF.md
3. docs/protocol.md
4. docs/architecture.md
5. docs/acceptance.md
6. README.md

不要只输出开发计划。先检查 macOS、CPU 架构、Xcode、Clang、CMake、Qt
和 Git 环境，然后从 P0 开始实际修改代码、构建并运行测试。每完成一个
阶段都更新 docs/acceptance.md，严格区分：已经实现、在哪个 macOS/CPU
上真实验证、仍未验证。

技术栈保持 Qt 6 Widgets + C++20 + CMake。复用现有跨平台核心：

- client/src/core/ApiClient.*
- client/src/core/CachePlanner.*
- client/src/core/FolderPacker.*
- client/src/core/SafeExtractor.*
- client/src/storage/ClientDatabase.*

新增 Objective-C++ AppKit/Foundation/Security 平台桥，例如：

- client/src/platform/macos/CredentialStore.mm
- client/src/platform/macos/FilePromiseDrag.mm
- 必要时增加 NSStatusItem/NSPopover 或 NSPanel 桥

Windows 专属源文件必须继续只在 WIN32 下编译。不要删除、重写或假装
验证 Windows OLE 代码。新增 macOS CMake preset/target 后，保证共享核心
测试仍可构建；如果只能在 Mac 上验证 Mac 构建，就在 acceptance 中如实写明。

当前 VPS API：
https://43.108.80.211:8443

必须正常验证 HTTPS 证书，禁止关闭 TLS 校验。设备 token 只能存 macOS
Keychain，不得写入 SQLite、源码、Markdown 或日志。需要一次性配对令牌时
再向用户提出，因为该令牌必须在 VPS 上临时生成。不要索要或记录 SSH 密码。

不可改变的产品规则：

1. 只支持 Windows 和 macOS。
2. 文件拖入后立即上传。
3. 文件夹拖入后自动生成标准 ZIP，把 ZIP 作为单一不可变对象上传。
4. 普通文件与文件夹 ZIP 共用上传、下载、SHA-256、按需获取、预缓存和重试。
5. 文件夹拖出时自动安全解压并交付真实文件夹，不能把 ZIP 交给用户。
6. 每设备自动预缓存硬上限为 52,428,800 bytes。
7. 排除 upload_device_id == current_device_id 的条目。
8. 其余 ready 对象按 (object_size_bytes, created_at, id) 升序贪心选入，
   放不下就跳过；不得改成 LRU、最新优先或背包算法。
9. 本设备上传内容不做长期自动缓存；原路径校验通过后复用，失效时回退 VPS。
10. 普通拖放不能假定能取得目标目录。
11. 不使用 Electron、Redis、Finder 扩展、文件系统驱动、系统级占位文件。

最高优先级是 P1 原生拖出验证。使用 NSFilePromiseProvider、
NSDraggingItem 和 NSFilePromiseProviderDelegate，在真实 Finder 验证：

- 3 秒延迟、没有现成本地路径的普通文件；
- 15 秒延迟、含嵌套文件、Unicode 名称和空目录的文件夹；
- 鼠标放下后 Promise 才在后台准备内容，UI 不阻塞；
- completion handler 只调用一次；
- provider、delegate、任务上下文和内容租约存活到 Promise 完成；
- 普通文件交付字节正确；
- 文件夹由 SafeExtractor 交付为真实文件夹，不出现裸 ZIP；
- 取消、失败、断网和拖动结束后继续读取不会泄漏半文件。

文件夹扫描和解压必须拒绝/防止：路径穿越、绝对路径、符号链接、alias、
特殊文件、压缩炸弹、大小写冲突、Unicode 规范化冲突、深度/条目/展开量
超限及半成品目录泄漏。先写同级随机临时根，全部成功后原子发布。

UI 必须与当前 Windows 产品行为保持一致：

- 上传队列和 ready 条目都显示在同一个虚线文件区域内；
- Finder 风格图标网格和列表视图；
- 框选、Command/Shift 多选；选择与拖动时不显示复选框；
- 直接拖动文件图标开始 File Promise；
- 完整窗口和菜单栏虚线弹窗均有鼠标悬停预选高亮，悬停不改变真实选择；
- 条目右键包含另存为、删除；空白处右键包含刷新、排序方式和查看；
- 排序支持名称、大小、项目类型、上传时间及升序/降序；
- 查看支持图标和列表；
- 底部仅保留置顶、删除、设备设置/配对；
- 关闭完整窗口后驻留菜单栏；
- 左键菜单栏图标只在图标附近弹出独立虚线文件框，不重开完整窗口；
- 弹窗内右键菜单和系统对话框必须可用；点击桌面或其他应用时弹窗隐藏；
- 菜单栏右键提供打开完整窗口和彻底退出；活动 Promise 存在时退出需提示；
- 使用仓库当前 icon.jpg 生成 macOS App Icon，不得换回旧图。

先完成 P0/P1，再继续配对/Keychain、文件与文件夹上传、snapshot/SSE、
混合缓存、原路径回退、按需下载、SafeExtractor、混合多选拖出和打包。
不要修改现有 VPS 协议来绕过 NSFilePromiseProvider。

至少执行这些验收：

- Windows 上传 -> macOS 显示并拖出；
- macOS 上传 -> Windows 显示并拖出；
- 普通文件比较 SHA-256；
- 文件夹比较 tree_sha256、嵌套层级、Unicode 和空目录；
- 60 MiB 以上未预缓存对象直接拖出；
- 本机原路径有效与失效回退；
- 预缓存始终 <= 52,428,800 bytes 且排除本设备；
- 路径穿越、炸弹、符号链接、大小写/Unicode 冲突 ZIP 全部拒绝；
- 取消、断网、到期、磁盘满及 Promise 生命周期。

Git 协作规则：

- 开始每个阶段前运行 git status 和 git pull --ff-only origin macos。
- 不提交 build、Qt SDK、.app 构建目录、DMG、缓存、token 或本机数据库。
- 不使用 git reset --hard，不覆盖 Windows 端尚未合并的改动。
- 共享核心若必须修改，保持跨平台条件编译并补测试，提交说明影响范围。
- 每阶段使用清晰提交信息，例如：
  macOS P1: implement Finder file promises
- 推送后向用户报告 commit SHA、测试结果、macOS/CPU/Qt 版本和剩余项。
- 不自行把 macos 强推到 main。完成稳定阶段后让用户或主 Codex 创建 PR，
  合并后再从 origin/main 同步共享改动。

现在开始执行，不要停在计划阶段。
```

## 分支约定

| 分支 | 用途 |
| --- | --- |
| `main` | 两端已经合并并验证的稳定共享基线 |
| `windows` | Windows 客户端和服务器侧后续开发 |
| `macos` | macOS 客户端开发 |

两端不要互相强推开发分支。共享改动通过 PR 合入 `main`，再分别合并或变基最新 `origin/main`。

