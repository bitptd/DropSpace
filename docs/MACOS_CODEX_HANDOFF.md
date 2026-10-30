# DropSpace macOS 客户端 Codex 开发交接

版本：1.1  
整理日期：2026-10-30（Asia/Shanghai）  
目标读者：负责在 macOS/Xcode 环境继续开发 DropSpace 第一阶段的 Codex

## 1. 先读与任务边界

开始编码前，必须完整阅读同仓库以下文件：

1. `DROP_SPACE_CODEX_GUIDE.md`：产品约束和 P0–P6 验收标准的权威来源。
2. `docs/protocol.md`：当前 VPS API、ZIP 和目录树摘要格式。
3. `docs/architecture.md`：已经固定的跨平台架构和容量限制。
4. `docs/acceptance.md`：Windows/服务器已经实际验证和仍未验证的内容。
5. 本文：macOS 实现顺序、UI 约定和交接状态。

本轮只开发 macOS 客户端。不要重写已经运行的 Go/SQLite 服务端，也不要改变现有协议，除非发现明确的跨平台协议缺陷并先记录兼容影响。Windows 客户端由另一工作区维护。

只支持 Windows 和 macOS 客户端；不要增加 Electron、浏览器运行时、Redis、对象存储 SDK、文件系统驱动、Finder 扩展、系统级占位文件或目录同步框架。

## 2. 当前可连接的服务

- API：`https://43.108.80.211:8443`
- 协议前缀：`/v1`
- 健康检查：`GET /healthz`
- 生产请求必须正常验证 HTTPS 证书，禁止关闭 TLS 校验。
- 服务端是 Linux 上的 Go + SQLite 单实例，应用仅监听 VPS 的 `127.0.0.1:8787`，Caddy 对外提供 8443。
- 当前服务已验证配对、原始文件上传、文件夹 ZIP 上传、SHA-256、服务端 ZIP 校验、快照、SSE、下载、manifest 和逻辑删除。
- 不要把 SSH 密码、配对令牌或设备 token 写入源码、Markdown、SQLite或日志。

配对时由 VPS 管理者在服务器生成十分钟一次性令牌：

```bash
sudo -u dropspace /usr/local/bin/dropspace-server pair \
  -config /etc/dropspace/config.json -minutes 10
```

macOS 客户端使用令牌调用 `POST /v1/pair`，`platform` 填写 `macos`。返回的长期设备 token 只能保存到 macOS Keychain；本地 SQLite 只保存非秘密的 `device_id`、`space_id`、`server_url` 和同步状态。

## 3. 不可改变的产品规则

### 3.1 内容对象

- 普通文件直接作为 `raw` 不可变对象上传。
- 文件夹先扫描并生成一个标准 ZIP，再将 ZIP 作为单一 `zip` 不可变对象上传。
- 普通文件和文件夹 ZIP 共用上传、下载、重试、SHA-256、按需获取、自动预缓存和租约逻辑。
- ZIP 只是传输格式。用户从 macOS 拖出文件夹时必须收到真正的文件夹，不能收到需要手动解压的 ZIP。
- 对象只有在 VPS 完整落盘并通过长度、SHA-256，以及文件夹 ZIP 安全验证后才进入 `ready` 并出现在其他设备。

### 3.2 自动预缓存

每个设备的自动预缓存硬上限固定为：

```text
52,428,800 bytes
```

候选必须满足 ready、未删除、未过期、大小合法，并且：

```text
upload_device_id != current_device_id
```

按 `(object_size_bytes, created_at, id)` 升序排列，依次贪心选入；放不下的对象跳过，继续检查后面的对象。不得改成 LRU、最新优先或背包最优算法。文件夹按 ZIP 的实际对象字节数计入，不按展开大小计入。

本机上传的内容不建立长期自动缓存。原路径有效时可以验证后复用；路径丢失、权限失效或内容变化时自动回退 VPS。

### 3.3 普通拖放限制

普通拖放不能假定源应用能取得目标目录。不得通过 Finder 窗口探测、鼠标坐标、辅助功能脚本、HTTP URL、不存在的路径或零字节占位文件伪造交付。

macOS 必须使用 `NSFilePromiseProvider`。只有 Promise 的 `writePromiseTo` 回调提供的 URL 才是系统明确授权的准确写入目标，该 URL 不能推广为普通拖放通用能力。

拖入和拖出始终为复制语义。成功拖出不删除云端条目、缓存或用户原文件。

## 4. 推荐工程结构与复用策略

继续采用 Qt 6 Widgets + C++20 + CMake，共享业务层；仅平台桥使用 Objective-C++：

```text
client/src/core/
  ApiClient.*          # REST、SSE、上传、下载、预缓存和内容获取
  CachePlanner.*       # 52,428,800-byte 纯函数计划
  FolderPacker.*       # 文件夹扫描、ZIP 和 tree_sha256
  SafeExtractor.*      # ZIP 安全解压及原子发布
client/src/storage/
  ClientDatabase.*     # 本地 SQLite
client/src/ui/
  MainWindow.*         # 跨平台 UI，隔离 Windows 条件分支
client/src/platform/macos/
  CredentialStore.h
  CredentialStore.mm   # Security.framework / Keychain
  FilePromiseDrag.h
  FilePromiseDrag.mm   # AppKit NSFilePromiseProvider
  MacStatusItem.h
  MacStatusItem.mm     # 如 Qt 托盘行为不足，再使用 NSStatusItem
```

优先复用当前 `CachePlanner`、`ApiClient`、`ClientDatabase`、`FolderPacker` 和 `SafeExtractor` 的行为与测试。不要复制一套 macOS 专用网络/缓存算法。平台桥只消费冻结后的条目描述、内容获取回调、进度、取消令牌和租约，不直接修改缓存数据库。

macOS CMake 需要启用 `OBJCXX`，并链接 AppKit、Foundation、Security 和 UniformTypeIdentifiers。Qt 私有 `QZipWriter` 与 Qt 版本绑定，macOS 上必须锁定并验证对应 Qt 版本；升级 Qt 时重新运行文件夹打包兼容测试。

## 5. macOS 原生 File Promise 实现

### 5.1 拖动开始

1. 从 Qt 图标/列表的当前多选集合冻结条目顺序、类型、导出名称、对象大小和 `file_id`。
2. 在本次多选内部解决同名、大小写和 Unicode 规范化冲突，稳定分配如 `report (2).pdf` 的名称。
3. 每个顶层条目创建一个 `NSFilePromiseProvider` 和对应 `NSDraggingItem`。
4. 普通文件使用可解析的 `UTType`；无法判断时使用通用数据类型。
5. 文件夹声明目录类型，不要把 `.zip` 扩展名交给 Finder。
6. 原生拖动操作只允许 Copy。

### 5.2 Promise delegate

实现 `NSFilePromiseProviderDelegate`：

- `filePromiseProvider(_:fileNameForType:)` 返回已经净化并冻结的顶层导出名。
- `operationQueue(for:)` 返回专用后台队列，网络下载、SHA-256 和解压都不能阻塞 Qt/AppKit 主线程。
- `filePromiseProvider(_:writePromiseTo:completionHandler:)` 才开始或等待内容准备，并写入系统给出的准确 URL。
- completion handler 成功或失败必须且只能调用一次；不能先报成功再下载。
- 普通文件：获取带租约的已验证对象，将内容写入目标同目录随机临时文件，校验并完成后原子发布。
- 文件夹：获取已验证 ZIP，调用统一 `SafeExtractor` 在随机临时目录完整展开，全部成功后再发布为 Promise 要求的真实顶层目录。
- 下载、复制、解压、协调或取消失败时不得留下被当作成功的空文件、半文件夹或裸 ZIP。

### 5.3 生命周期

- 鼠标释放或拖动动画结束不等于 Promise 已交付。
- `NSFilePromiseProvider`、delegate、任务上下文、下载句柄和内容租约必须至少存活到 completion handler 完成。
- 活动 Promise 存在时，退出应用必须提示等待或取消，不能直接释放临时内容。
- 同一内容被多个 Promise 消费时可以共享下载，但每个消费者单独持有租约；取消其中一个不能破坏其他消费者。
- 第一阶段先实现完整下载并校验后写入目标，不需要 HTTP Range 或网络直通可 seek 流。

### 5.4 Finder 必须实测

最先完成 P1 原型，不要等完整 UI 做完后才验证：

- 3 秒延迟的未缓存普通文件直接拖到 Finder。
- 15 秒延迟的未缓存文件夹，最终交付含嵌套文件、Unicode 名称和空目录的真实文件夹。
- Finder 在鼠标释放后等待 Promise 写入时，UI 保持响应。
- 单文件、多个文件、文件夹、文件与文件夹混合多选。
- 同名、取消、失败、断网，以及拖动结束后目标继续读取。

记录 macOS 版本、CPU 架构、Qt 版本、Finder 结果、文件 SHA-256 和文件夹 `tree_sha256`，更新 `docs/native-drag-spike.md` 和 `docs/acceptance.md`。仅编译通过或 mock delegate 不算 Finder 验收。

## 6. 文件夹打包与安全解压

文件夹拖入后立即扫描和打包上传。扫描只接受普通文件和目录，并保留空目录；遇到符号链接、alias、package/bundle 特殊语义、跨根挂载或其他特殊文件时拒绝整个文件夹并显示首个相对路径，不跟随、不静默跳过。

ZIP 路径使用 NFC 规范化的 UTF-8 相对路径和 `/`。拒绝绝对路径、盘符、UNC、NUL、`.`、`..`、重复分隔符、Windows 保留名、尾点尾空格、非法字符、大小写冲突和 Unicode 规范化冲突，以确保 Windows/macOS 双端都能恢复。

目录树摘要格式必须与服务端完全一致：

```text
目录：D NUL path NUL
文件：F NUL path NUL decimal_size NUL lowercase_content_sha256 NUL
```

记录按规范相对路径稳定排序后计算 SHA-256。ZIP 内只放用户根文件夹的子树，根文件夹名保存在元数据中，避免交付时出现双层根目录。

`SafeExtractor` 必须统一供 Promise、另存为和兼容准备使用：

- 防止路径穿越、绝对路径、链接和特殊文件。
- 防止压缩炸弹：同时限制真实累计展开字节、条目数、深度和压缩比。
- 防止大小写及 Unicode 规范化冲突。
- 拒绝符号链接；发布前检查目标路径中不存在符号链接替换竞态。
- 先写随机同级临时根，逐项排他创建；全部成功后再原子发布。
- 失败或取消时清理未发布临时根，不暴露半成品目录。
- 不调用系统 `unzip` 命令。

MVP 默认限制：ZIP 对象 1 GiB、展开 4 GiB、10,000 项、深度 64；客户端与服务端保持一致。

## 7. 原路径复用和 VPS 回退

本机上传成功后，仅在本机数据库保存 `file_id → original_path`、文件身份、快速快照、mtime、对象 SHA-256；文件夹另存 `tree_sha256`。绝对路径绝不能同步到服务器。

内容获取顺序：

1. 本机上传条目尝试原路径。
2. 已验证的预缓存或传输区对象。
3. VPS 完整下载到独立传输区。

原路径不能只凭 mtime 判断。普通文件必须通过已打开句柄创建一次性快照并边复制边计算 SHA-256；文件夹必须重新冻结清单并验证目录树，必要时重新打包。原路径丢失、移动、修改、权限失效或验证失败时自动回退 VPS，不能把当前同名但已变化的内容交给用户。

如果未来启用 App Sandbox，必须另外实现 security-scoped URL/bookmark 的保存、访问和失效处理，并单独验收。第一阶段默认直接分发桌面应用，不声称已经支持 Mac App Store sandbox。

## 8. 本地存储、传输区和 Keychain

- 使用 Qt 标准应用目录 API，不硬编码用户名。
- SQLite 和持久状态放 `AppLocalDataLocation`。
- 自动预缓存放可重建的 `CacheLocation`。
- 独立传输区和上传 ZIP 暂存放应用私有目录，默认总预算 4 GiB。
- `.part`、完整对象和任务预留必须统一入账；不能靠改名绕过预算。
- 下载写随机 `.part`，边读边计算 SHA-256，长度与哈希都正确后原子发布。
- 启动清理只处理应用拥有、无活动租约并超过宽限期的资源；绝不清理用户原文件和 Finder 目标副本。
- “清理本地缓存”与“删除云端条目”必须是两个不同动作。
- 设备 token 使用 Keychain，以服务器 URL/空间/设备 ID 组成稳定 service/account；日志和错误提示不得泄漏 token。

## 9. UI 和交互需要与 Windows 版本保持一致

主界面以一个虚线框文件区域为核心。拖入文件或文件夹后立即进入上传队列，上传条目和同步到的 ready 条目都显示在虚线框内，不放在框外的独立列表。

### 9.1 文件区域

- 默认采用 Finder 风格图标网格：系统文件/文件夹图标、名称位于图标下方。
- 支持列表视图。
- 支持空白处拖框多选，以及 Command/Shift 多选；拖动任一已选图标时保持整个选择集合。
- 选择和拖动时不显示复选框。
- 鼠标悬停在文件或文件夹上时显示明确的预选反馈（浅色背景和边框），但不能改变实际选择；已选中且悬停时使用更强的高亮。完整窗口、菜单栏虚线弹窗、图标视图和列表视图必须保持一致。
- 用户直接拖动条目图标开始 File Promise，不使用单独的“拖出”按钮。
- 双击或右键提供“另存为”；右键条目提供云端删除。
- 同一次多选操作只显示一次批量删除确认。

空白处右键菜单：

- 刷新
- 排序方式：名称、大小、项目类型、上传时间；支持升序/降序
- 查看：图标、列表

底部只保留紧凑操作：

- 固定在最上层/取消固定
- 删除
- 设备设置/配对

macOS 置顶用正常窗口级别实现并保留窗口位置；不要让 File Promise 或系统授权对话框被错误遮挡。

### 9.2 状态栏/菜单栏驻留

- 点击窗口关闭按钮时隐藏窗口并继续在菜单栏运行，不直接终止进程。
- 左键点击菜单栏图标时，只在图标附近弹出一个独立、无边框的虚线文件区域，不重新打开整个主窗口。
- 虚线弹窗内必须保留完整右键能力：右击条目显示“另存为/删除”，右击空白处显示“刷新/排序方式/查看”。
- 点击弹窗外部、桌面或其他应用时自动隐藏；点击弹窗内部空白处不应被误判为外部点击。
- 打开右键菜单、另存为对话框、删除确认框或其他属于本应用的模态窗口时，不得因为失焦提前隐藏弹窗或关闭菜单；这些子窗口结束后继续按正常的外部点击规则处理。
- macOS 实现应根据本进程的活动窗口/Popover/Menu 状态判断，而不是照搬 Windows 的前台进程轮询。若使用 `NSPopover`，需要验证 transient/semitransient 行为与右键菜单兼容；若使用无边框 `NSPanel`，需自行实现可靠的 resign-key/outside-click 处理。
- 菜单栏图标右键菜单至少包含“打开完整窗口”和“彻底退出”。
- 存在上传、下载或 File Promise 时，“彻底退出”必须提示等待或取消。
- 使用仓库当前 `icon.jpg` 生成 macOS App Icon/`.icns`，不要换回旧图。菜单栏的极小尺寸可生成系统需要的缩放资源，但不能替换图案或使用旧缓存资源。

Windows Explorer 的 `IconCache.db` 清理仅用于 Windows 开发机排查旧 EXE 图标，macOS 不应照搬。macOS 若显示旧 App 图标，应检查 App bundle 的 `CFBundleIconName`/Asset Catalog、构建产物、LaunchServices/Dock 缓存和签名，而不是修改业务逻辑。

## 10. API 要点

所有业务请求使用：

```http
Authorization: Bearer <device_token>
```

主要端点：

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/v1/pair` | 兑换一次性配对令牌 |
| GET | `/v1/me` | 当前设备、空间和限制 |
| GET | `/v1/devices` | 设备列表 |
| DELETE | `/v1/devices/{id}` | 撤销设备 |
| POST | `/v1/files` | 创建幂等上传意图 |
| PUT | `/v1/files/{id}/content` | 上传原始文件或 ZIP 字节流 |
| GET | `/v1/snapshot` | ready 条目与一致的 `snapshot_seq` |
| GET | `/v1/events?after=N` | 持久 SSE 事件 |
| GET | `/v1/files/{id}/content` | 下载完整不可变对象 |
| GET | `/v1/files/{id}/manifest` | 服务端验证过的文件夹清单 |
| DELETE | `/v1/files/{id}` | 幂等逻辑删除 |

SSE 断线后携带最后已经与本地事务一起提交的游标退避重连。重复 seq 幂等忽略；本空间 seq 可以跳号。收到 `resync_required` 时重新获取快照。不要把文件正文放入 JSON/SSE。

错误结构：

```json
{"code":"machine_code","message":"safe explanation","request_id":"trace id"}
```

400/403/413/422 不盲目重试；网络错误和 5xx 有界退避；401 停止后台任务并要求重新配对。

## 11. 建议实施顺序

### P0：macOS 构建基线

- 记录 macOS、CPU 架构、Xcode、Clang、CMake、Qt 版本。
- 让共享 C++/Qt 客户端在 macOS 构建启动。
- 把 Windows 平台源文件改成正确的条件编译，新增 Objective-C++ 桥。
- 更新 `docs/acceptance.md`，不得提前标记 Finder 通过。

### P1：先做 File Promise 技术验证

- 完成 3 秒文件和 15 秒真实文件夹 Promise。
- 验证慢准备发生在放下之后，UI 不阻塞。
- 验证 provider/delegate/租约生命周期和失败回调。
- 把实机证据写入 `docs/native-drag-spike.md`。

### P2/P3：接现有 VPS、Keychain、上传和同步

- 不重写服务端。
- 完成配对、Keychain、本地 SQLite、snapshot/SSE。
- Finder 拖入普通文件直接上传；文件夹扫描、ZIP、上传。
- 用独立 macOS 设备 ID 与 Windows 设备验证互相可见。

### P4：混合缓存、原路径和 SafeExtractor

- 运行并扩展 `CachePlanner` 边界/随机测试。
- 完成预缓存预留、`.part`、哈希、原子发布和动态重新规划。
- 完成原路径验证、VPS 回退、传输区租约和启动清理。
- 完成恶意 ZIP 测试。

### P5：真实条目接入 File Promise

- 缓存、本机原路径和 VPS 三种来源全部走统一内容获取接口。
- 验证未缓存大文件、未缓存文件夹、混合多选和同名。
- 增加进度、取消、明确错误和“另存为”降级路径。

### P6：打包和跨平台验收

- 构建 `.app`；如未签名、公证或制作 DMG，必须明确标为未完成。
- Windows 上传 → macOS Finder 拖出；macOS 上传 → Windows Explorer 拖出，各至少一次。
- 普通文件比较 SHA-256；文件夹比较目录树摘要、Unicode、嵌套层级和空目录。
- 记录包体、空闲 CPU/内存、缓存和传输区峰值。

## 12. macOS 最低验收矩阵

| 场景 | 必须结果 |
| --- | --- |
| Finder 普通文件拖入 | 放下后直接上传并最终 ready |
| Finder 文件夹拖入 | 自动 ZIP 上传，含嵌套、Unicode 和空目录 |
| 已缓存文件拖出 | Finder 获得字节一致的真实文件 |
| 60 MiB 以上未缓存文件拖出 | 放下后按需下载并正确交付 |
| 未缓存文件夹拖出 | Finder 获得自动安全解压后的真实文件夹，不出现 ZIP |
| 文件/文件夹混合多选 | 一次拖动全部正确交付 |
| 本机上传原路径有效 | 校验后复用，不重复下载 |
| 本机原路径失效或变化 | 不交付错误内容，自动回退 VPS |
| 预缓存 | 永远排除本设备，合计不超过 52,428,800 bytes |
| ZIP 攻击样本 | 路径穿越、炸弹、链接、大小写/Unicode 冲突全部拒绝 |
| Promise 取消/断网/磁盘满 | 明确失败，不残留假成功或半目录 |
| Promise 期间触发清理/退出 | 活跃内容不被提前删除 |
| 完整窗口与菜单栏弹窗悬停 | 文件图标有预选高亮，已选中项悬停更明显，不改变真实选择 |
| 菜单栏弹窗右键 | 条目菜单与空白菜单都可操作，菜单/对话框打开期间弹窗不误关 |
| 关闭窗口 | 隐藏到菜单栏；点击图标只弹出虚线文件区；点击外部可靠隐藏 |

## 13. 当前已知未完成项

这些 Windows 端仍未完全验证的项目，也应在 macOS 端主动补测：

- 持久重试队列和完整取消链路。
- 4 GiB 传输区严格预留账本、持久租约及动态压力测试。
- 多选混合拖出、同名冲突和第三方目标兼容矩阵。
- Promise 完成前退出、清理和目标持续读取的竞态。
- 网络断开、VPS 到期、磁盘满和权限失效的真实 UI 行为。
- 正式签名、公证、DMG/安装包和 Apple Silicon/Intel 架构覆盖。

不要把 Windows 已通过的测试直接标成 macOS 已通过。共享纯函数测试可以复用，但 AppKit、Finder、Keychain、`.app` 打包、签名和公证都必须在真实 macOS 环境单独验证。

## 14. 可直接交给 macOS Codex 的启动指令

```text
请完整阅读 DROP_SPACE_CODEX_GUIDE.md、docs/protocol.md、
docs/acceptance.md 和 docs/MACOS_CODEX_HANDOFF.md，然后实际开发
DropSpace 第一阶段 macOS 客户端，不要只输出计划。

保留现有 Qt 6 Widgets + C++ 核心、Go/SQLite VPS 协议和不可变对象模型。
复用 CachePlanner、ApiClient、ClientDatabase、FolderPacker、SafeExtractor，
新增 Objective-C++ AppKit 桥、NSFilePromiseProvider 和 macOS Keychain。
不要修改服务端协议来绕过 Finder 原生延迟交付。

从 P0 开始构建，优先完成 P1：在真实 Finder 验证 3 秒未缓存文件和
15 秒未缓存 ZIP 文件夹的延迟拖出。文件夹最终必须成为真实文件夹，
不能把 ZIP 交给用户。普通拖放不能假定能取得目标目录。

预缓存固定为 52,428,800 bytes，排除 upload_device_id 等于当前设备，
其余 ready 对象按对象大小升序贪心选入，放不下就跳过。本机上传不做
长期自动缓存，原路径验证失败时回退 VPS。

随后完成配对/Keychain、上传、snapshot/SSE、预缓存、按需下载、
SafeExtractor、原路径复用、混合多选拖出、菜单栏弹窗和 macOS 打包。
每完成一个阶段都运行测试并更新 docs/acceptance.md，明确区分已实现、
已在哪个 macOS/CPU 上验证以及仍未验证的内容。
```

