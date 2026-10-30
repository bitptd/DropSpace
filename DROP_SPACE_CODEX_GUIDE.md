# DropSpace 第一阶段 Codex 开发指南

版本：1.1 · 2026-10-29

本文可以直接交给 Codex 作为项目实施说明。必须落实下列产品约束；未指定的工程参数采用本文标出的 MVP 默认值，并写入配置，不把这些默认值声称为此前对话的决定。

## 1. 产品目标与不可变约束

DropSpace 是 Windows/macOS 之间的轻量文件传输工具：把文件或文件夹拖进窗口上传到 VPS，其他已配对设备同步看到条目，再从窗口拖到资源管理器或 Finder，交付真实文件或还原后的真实文件夹。

1. 第一阶段只支持 Windows 和 macOS，不开发 iOS、iPadOS、Android、Linux 客户端。
2. VPS 中转是权威路径：所有上传必须在 VPS 完整落盘、校验通过后，才成为跨设备可用文件。不是 P2P，不要求两台设备同时在线。
3. 采用混合缓存：先同步完整元数据；每设备自动预缓存上限 **50 MiB = 52,428,800 bytes**；未预下载文件按需下载。
4. 自动预缓存必须排除 `upload_device_id == current_device_id` 的条目。不因为原文件丢失而自动给本机上传条目做预缓存。
5. 对其他设备上传的可用文件，按字节大小升序排序，逐个贪心选入；放不下就跳过。不得改成最新优先、LRU 优先或“大文件永远不显示”。
6. 本机上传条目可仅记录原始本地路径。普通文件直接上传；文件夹先压缩成 ZIP，再把 ZIP 作为单一不可变对象上传。交付时校验后复用原路径；路径不可用、内容改变或权限失效时从 VPS 按需下载对象。
7. 独立临时传输区用于按需下载、导出快照和拖放兼容交付；它与 50 MiB 自动预缓存分开管理，并有独立容量与清理策略。
8. 拖出采用原生虚拟文件/延迟文件提供机制。**不能假设普通拖放能返回目标目录，不能把不存在的本地路径、HTTP URL 或零字节占位文件当作交付实现。**
9. 拖入、拖出均为复制语义。拖出成功不自动删除云端条目、缓存或用户原文件。
10. 不引入虚拟磁盘驱动、文件系统同步守护服务、Shell 扩展、Finder 扩展、系统级占位文件框架或浏览器运行时来绕过拖放问题。
11. ZIP 是文件夹的传输格式，不是用户最终收到的形式。上传、下载、校验和预缓存统一复用单文件内容流；拖出文件夹时由客户端安全解压并交付真实文件夹。

“轻量化”首先指依赖少、无浏览器运行时、无全量自动下载、低空闲活动量。安装体积、内存、CPU 必须实测记录；不得仅凭技术选型承诺数值。

## 2. 第一阶段范围

### 2.1 必须交付

- 可安装/启动的桌面客户端：文件/文件夹列表、拖入区、拖出、手动另存为、上传/下载进度、失败重试、缓存状态、删除条目、设备设置。
- 普通文件和文件夹上传、多选拖出；文件夹拖入后自动压缩，拖出后自动还原；同名不同内容条目可并存。
- 一台 Linux VPS 上的单实例服务：认证、流式上传/下载、元数据、事件同步、过期与清理。
- 服务端和客户端完整性校验、重启恢复、自动预缓存调度。
- Windows Explorer 与 macOS Finder 的实际原生拖放验收。

### 2.2 不做

- 符号链接、Windows junction/reparse point、macOS alias，以及 macOS package/bundle 的透明语义保留。文件夹扫描遇到这些对象时拒绝该文件夹并指出相对路径，不跟随链接，也不静默漏传。
- 通用云盘、双向目录同步、版本历史、在线编辑、公开分享链接、团队权限系统。
- 自动发现局域网、P2P、分布式存储、Redis、消息队列、对象存储 SDK。
- 断点续传与分块上传；第一阶段中断后重传单个文件，不能声称已支持续传。
- 端到端加密。HTTPS 和磁盘权限不等于 E2EE，VPS 管理者仍可接触内容。
- 保留 ACL、扩展属性、resource fork 或可执行权限；MVP 承诺普通文件的数据字节，以及文件夹内普通文件、空目录、相对层级、展示名称和可选修改时间。

### 2.3 建议默认配置（可改，非既有对话决定）

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| 自动预缓存 | 固定 50 MiB/设备 | 核心约束，不跟随临时区容量调整 |
| 单对象大小上限 | 1 GiB | 普通文件为原始大小，文件夹为 ZIP 大小；仍允许大于 50 MiB 的对象 |
| 文件夹展开上限 | 4 GiB、10,000 项、深度 64 | 防止压缩炸弹和失控的临时占用；前后端一致检查 |
| 空间总容量 | 10 GiB | 包括未完成上传的预留容量 |
| ready 条目数上限 | 10,000 | 避免零字节文件与元数据无限增长 |
| 云端保留期 | 上传成功后 24 小时 | 展示到期时间；删除/过期不会删除用户交付副本 |
| 临时传输区上限 | 4 GiB/设备 | 活跃任务也计入，另检查真实剩余磁盘空间 |
| 传输文件闲置清理期 | 1 小时 | 路径兼容导出见第 7 节的较长保护期 |
| 未完成上传超时 | 无活动 1 小时 | 活跃流持续更新时间；另设 24 小时总时限 |
| 并发上传/前台下载/预缓存 | 2 / 2 / 1 | 前台优先，可暂停预缓存 |
| 事件保留期 | 7 天 | 游标失效后重新获取快照 |

## 3. 最小技术栈与工程结构

若工作区为空，直接采用本节默认栈，不先给用户一长串框架选择。若已有项目，先读 `AGENTS.md`、构建配置和现有代码，保留可用架构，只替换无法满足原生交付的部分。

### 3.1 推荐默认栈

| 层 | 选择 | 原因 |
| --- | --- | --- |
| 客户端 | C++20、Qt 6 Widgets、CMake | 一套 UI/业务代码，不携带 Chromium |
| 客户端网络/状态 | Qt Network、Qt SQL + SQLite、QCryptographicHash | 复用 Qt 模块，不再引入 HTTP/JSON/哈希库 |
| Windows 原生桥 | C++、Win32 OLE/COM | `IDataObject`、`IDropSource`、`IStream` |
| macOS 原生桥 | Objective-C++ `.mm`、AppKit/Foundation | `NSFilePromiseProvider` 与原生拖放会话 |
| VPS | Go、标准库 `net/http`、`database/sql` | 一个进程、HTTP API、SSE、流式 I/O |
| 服务数据库 | SQLite，单一 Go 驱动 `modernc.org/sqlite` | 一个业务外部依赖；开发开始时核实 Go 兼容版本并锁定 |
| HTTPS | Caddy 反向代理 | 证书与 TLS；服务只监听本机地址 |
| 部署 | systemd + 目录存储 | 不强制 Docker，不增加编排层 |

固定经验证的 Go、Qt、SQLite 驱动版本；提交 `go.mod`、`go.sum`、CMake 配置和构建说明。选择的 Qt 版本必须覆盖所声明支持的系统，禁止写“支持所有 Windows/macOS”。建议先实测 Windows 11 x64 与一台实际可用的 macOS 设备；Intel/Apple Silicon 哪些架构未测试就明确标记。

Qt 的 `QMimeData::setUrls()` 适合已有本地文件路径，不能独自完成未下载云端文件的原生延迟交付。Qt 官方也说明 Windows 特有格式与 MIME 的映射；本项目直接写窄原生桥，避免依赖 Qt 私有接口。[Qt QMimeData 文档](https://doc.qt.io/qt-6/qmimedata.html)

发布前完成 Qt 许可与第三方通知检查。构建 macOS 桥必须有 macOS/Xcode 环境；在 Windows 仅生成 `.mm` 文件不算验证成功。

### 3.2 目录建议

```text
DropSpace/
  README.md
  CMakeLists.txt
  CMakePresets.json
  client/
    CMakeLists.txt
    src/ui/
    src/core/              # CachePlanner、TransferManager、SyncEngine
    src/storage/           # SQLite、本地原路径记录、迁移
    src/platform/windows/ # OLE/COM
    src/platform/macos/   # Objective-C++ AppKit
    tests/
  server/
    go.mod
    go.sum
    cmd/dropspace-server/
    internal/{api,auth,store,events,gc}/
    migrations/
  deploy/
    Caddyfile.example
    dropspace.service
    config.example.json
  docs/
    architecture.md
    protocol.md
    native-drag-spike.md
    acceptance.md
```

核心只暴露 `planPrecache()`、`acquireReadableObject()`、`releaseLease()`、`startExport()` 等小接口。`acquireReadableObject()` 对普通文件返回原始文件内容，对文件夹返回 ZIP 内容；网络、缓存和校验层不需要两套实现。平台桥不直接修改缓存数据库；网络对象留在所属线程，原生回调通过线程安全队列与传输服务通信。

## 4. 总体流程与状态

```text
设备 A 普通文件 ─────────────────────────┐
设备 A 文件夹 ──扫描并压缩成 ZIP─────────┼──上传──> VPS 不可变对象
                                         │           │
                                         │   元数据快照 + 事件
                                         │           ▼
设备 B 条目 ──预缓存计划──> 最多 50 MiB 对象缓存（文件原文或文件夹 ZIP）
       │
       └──拖出/另存为──> acquireReadableObject() ──> 条目交付器
                          │                           ├─普通文件：直接提供
                          ├─有效的本设备原路径       └─文件夹：安全解压并提供目录树
                          ├─已校验的预缓存/临时对象
                          └─VPS 按需下载到传输区
```

上传状态：普通文件为 `queued → hashing → uploading → ready`；文件夹为 `queued → scanning → compressing → hashing → uploading → server_validating → ready`。出错进入 `failed`，重试回到合适阶段。其他设备只看到 `ready`，不看到半文件或未验证压缩包。

缓存状态：`metadata_only / queued / downloading / cached / failed`。本设备上传条目额外显示“本机上传”，原路径是否仍有效属于本地状态，不能同步成全局事实。

交付状态：`preparing → downloading_or_validating → extracting_if_folder → providing → provider_completed / failed / cancelled`。UI 文案区分“内容已提供给目标”和“已确认目标操作结束”；不能把 `DoDragDrop` 返回或拖动动画结束当作文件或目录树已经完整落地。

## 5. 50 MiB 自动预缓存：精确定义

### 5.1 候选集合和算法

候选：当前空间全部 `ready`、未到期、未删除、对象大小合法，且 `upload_device_id != current_device_id` 的条目。未登录、设备身份异常时停止调度。普通文件的对象是原始内容；文件夹的对象是 ZIP。

```python
LIMIT = 50 * 1024 * 1024  # 52,428,800 bytes

def plan_precache(files, current_device_id, now):
    candidates = [f for f in files
                  if f.state == "ready"
                  and f.expires_at > now
                  and not f.deleted
                  and f.upload_device_id != current_device_id
                  and f.object_size_bytes >= 0]
    # 平局规则仅用于稳定结果，不改变大小优先原则。
    candidates.sort(key=lambda f: (f.object_size_bytes, f.created_at, f.id))
    selected, used = [], 0
    for f in candidates:
        if used + f.object_size_bytes <= LIMIT:
            selected.append(f.id)
            used += f.object_size_bytes
        else:
            continue  # 明确跳过，不能超配，也不能改成全量下载
    return selected
```

算法按 VPS 不可变对象的实际字节数 `object_size_bytes` 计算，不用磁盘稀疏占用、文件夹展开大小或字符串“MB”。普通文件按原始字节数；文件夹按 ZIP 字节数。50 MiB 是网络对象预缓存上限，不包含解压交付产生的临时目录。上限不保证恰好填满；不拆分对象来补齐剩余容量。这是指定的贪心策略，不求最大容量利用率或背包最优解。

示例，文件均来自其他设备，单位 MiB：

- `[10, 15, 30, 60]` → 选 `10 + 15 = 25`；30 放不下，60 也跳过。
- `[20, 30, 60]` → 选 `20 + 30 = 50`。
- `[51, 70]` → 全部仅元数据，仍可拖出按需下载。
- 本机上传的 1 MiB 与远端的 49 MiB → 只预缓存远端 49 MiB。
- 零字节文件允许选入，仍受条目数限制。

### 5.2 执行与硬上限

1. `CachePlanner` 是无 I/O 的纯函数，只生成目标集合。事件批次、登录、启动、恢复网络、到期或删除后重新计算；短暂合并密集事件。
2. 先取消不再选中的预缓存任务，清理未被使用的旧条目和 `.part`，再预留新文件的完整大小。
3. 完整对象、预缓存 `.part` 和尚未写满的预留统一入账。对同一任务只计一次：`完整对象大小之和 + 各在途任务预留完整对象大小之和 <= LIMIT`。文件夹只计 ZIP 一次；展开目录计入独立传输区。实际 `.part` 占用不得超出该预留。
4. 被导出租约占用的旧缓存不能立即删；仍计入预算，等待释放后再安排新任务。不能为了腾出账面容量，把旧缓存随意改名为传输文件长期保留。
5. 下载到随机 `.part`，流式计算 SHA-256；长度和哈希匹配后原子改名，才标记 `cached`。失败不留下可用标记。
6. 下载上限按声明长度执行；响应超长、中途断流、哈希错误均失败。缓存文件缺失或损坏退回仅元数据并重试。
7. 失败重试采用有上限的退避；不因一次失败改变候选排序，也不无限高频重试。
8. 同一文件的前台与后台请求共享任务/已校验内容，前台优先；取消一个消费者不能取消其他消费者仍需的任务。
9. 清理不再选中的闲置预缓存。本机上传条目即便在临时区有按需副本，也不能晋升到自动预缓存区。
10. 设备重新注册得到新 ID 时清空原身份关联状态再重新规划；不要复制设备身份到另一台机器来规避规则。

界面至少分别显示“自动缓存 x / 50 MiB”和“临时传输占用 y”；不得把 50 MiB 描述成客户端所有磁盘占用上限。

## 6. 原路径复用与按需获取

### 6.1 本设备上传

上传时只在本机数据库记录 `file_id → original_path`、条目类型、文件身份标识（能获取则记录）、大小/目录快速快照、mtime 与成功上传的对象 SHA-256；文件夹另存目录树摘要。**原始绝对路径禁止发送到 VPS 或其他设备。**不为了上传后复用再保存一份完整本地缓存。

普通文件采用流式读取。可先顺序计算 SHA-256 再上传；上传请求体自身也必须由服务端校验。上传期间若源文件变化，最终摘要不匹配则失败，不发布 `ready`。不把完整文件读入内存。

### 6.2 文件夹打包格式

文件夹拖入后先扫描，再在独立上传暂存区生成一个 ZIP。ZIP 是统一内容对象，因此上传、VPS 存储、下载、SHA-256 校验、预缓存、重试和租约全部复用普通文件的单对象内容流。上传成功或失败任务结束后及时清除本机 ZIP 暂存；它不是本设备上传内容的长期缓存。

- 只收录普通文件和目录，包含空目录；不跟随符号链接、junction、reparse point、alias 或跨越根目录的挂载。
- ZIP 条目必须使用根目录内的规范相对路径和 `/` 分隔符；禁止绝对路径、盘符、UNC、NUL、`..`、重复分隔符和根目录逃逸。
- 扫描时冻结文件清单。压缩过程中逐项检查身份、大小和 mtime；流式读取并计算每个文件摘要。任何文件被加入、删除或改变都使本次打包失败并提示重试，不能发布不一致快照。
- 生成目录树摘要 `tree_sha256`：按规范相对路径稳定排序，依次纳入条目类型、路径、普通文件大小和内容 SHA-256。不要用 ZIP 的时间戳、压缩级别或容器字节作为目录树身份。
- ZIP 使用标准 Deflate/Store 和 ZIP64（需要时），不加密，不嵌套自定义容器。保留用户根文件夹名作为条目展示名，ZIP 内部只包含其子树，避免交付时出现双重根目录。
- 客户端在创建上传意图时提交 `kind=folder`、ZIP 的 `object_size_bytes/object_sha256`、`expanded_size_bytes`、`entry_count` 和 `tree_sha256`。
- 服务端在 ready 前用标准 ZIP 解析器验证中央目录和实际解压流：拒绝加密、链接/特殊文件、路径穿越、重复或大小写/Unicode 规范化后冲突的路径、超深层级、条目数超限、展开总量超限、单项声明与实际输出不符。服务端重算目录树摘要；只信任验证结果，不信任客户端声明。
- 为确保 Windows 与默认 macOS 文件系统都能还原，MVP 在上传前拒绝任一路径组件中的 Windows 保留名/尾点尾空格/非法字符，以及规范化或不区分大小写后冲突的兄弟路径，并明确显示首个冲突相对路径。
- 防压缩炸弹必须同时限制 ZIP 对象大小、实际累计展开字节数、条目数、目录深度和可接受的压缩比；解压过程中边读边计数，不能仅信中央目录中的未压缩大小。

### 6.3 `acquireReadableObject(file_id, reason)`

按下列顺序获取与云端不可变内容完全一致的可读资源，返回带租约的句柄，不仅返回裸路径：

1. 若是本设备上传，尝试原路径。普通文件检查可读、文件类型和大小后验证内容；文件夹重新计算目录树摘要，匹配时按需重新生成 ZIP 对象。mtime/快速目录快照只能快速否定，不能替代内容或目录树摘要。
2. 使用已验证的预缓存或临时传输副本。首次打开或发现外部改动时重新校验；损坏就失效，不交付。
3. 否则从 VPS 下载到独立临时传输区，长度和哈希都匹配后交付。

为避免“校验原路径后又按路径打开”产生竞态，普通文件通过已打开句柄复制到一次性导出 `.part`，边复制边哈希，通过后成为只读导出快照。文件夹则在独立暂存区按同一冻结清单重新生成 ZIP，只有 `object_sha256` 与云端对象一致时才复用；仅有相同 `tree_sha256` 但 ZIP 容器字节不同，可以确认内容等价并直接生成目标目录，但不能冒充云端 ZIP 缓存。临时资源结束后清理，不属于自动预缓存。

原路径丢失、移动、改写、目录树改变、权限拒绝或快照校验失败 → 丢弃失败快照 → 自动从 VPS 下载。VPS 已删除/到期且没有符合要求的本地内容时，明确失败；不能交付当前原路径中已经变化的同名文件或文件夹。

首次未缓存的文件建议先完整下载并校验，再交给目标读取；第一阶段无需实现可寻址的网络直通流或 HTTP Range。用户仍可直接拖起云端条目，由原生提供器在目标请求内容时等待准备。

## 7. 独立传输区、租约与清理

客户端路径使用 Qt 标准应用目录 API，不硬编码用户名。元数据放 AppLocalDataLocation 等持久应用目录，自动预缓存放可重建的 CacheLocation，传输区放应用私有且可控生命周期的应用目录。设置应用标记，禁止拿整个系统临时目录当清理根。

```text
<app-data>/metadata.sqlite
<cache-root>/precache/<file_id>.bin
<cache-root>/precache/<file_id>.<task_id>.part
<app-data>/transfers/<session_id>/<file_id>.part
<app-data>/transfers/<session_id>/<safe_export_name>
<app-data>/transfers/<session_id>/<safe_folder_name>/
<app-data>/uploads/<task_id>/<folder_object>.zip.part
```

- 传输区默认 4 GiB；任务开始按最坏完整大小预留，包含快照、下载、文件夹展开、兼容导出和上传 ZIP 暂存，任何额外复制也须计入。文件夹同时存在 ZIP 与展开树时两者分别计入；不能靠改名绕过额度。
- 重用现有缓存时尽量用只读句柄，不再复制。确需兼容路径时另外生成受保护副本；不要把可变的用户原文件直接暴露为稳定导出。
- 每次内容消费持有 `lease/ref_count`；活动下载、原生数据对象、流对象、Promise 回调、兼容拖出都会阻止删除其资源。
- Windows 异步操作中，拖动结束仍可能在读取；数据对象/流还活着就保持租约。macOS Promise 在拖动结束后才可能开始写入，必须保持 provider/delegate 与上下文存活。
- 有明确终止回调且租约归零后进入闲置期，默认 1 小时清理；失败/取消的 `.part` 在最后消费者释放后尽快清理。
- 使用真实路径的兼容拖出没有通用“目标读完”保证：至少保留 24 小时闲置保护期。这个期限只是兼容策略，不能保证所有第三方程序；超过边界提示用户使用另存为。
- 启动时清除进程已死且超过宽限期的孤儿 `.part`，重建预算。新进程不信任上次留下的 `active=true`；通过单实例锁/进程所有权识别真实活动资源。
- 磁盘不足或临时额度不足，先清理允许删除的闲置项；仍不足就排队或提示，绝不删活跃文件、用户原文件或用户目标副本。
- “清理本地缓存”只清预缓存和可清理传输项，不删除云端；“删除云端条目”是独立动作。
- 云端删除/过期后移除闲置自动缓存。已经开始的本地交付可按租约完成，不主动撤销目标副本；新云端下载返回不可用。
- 删除前验证规范化后的路径确在应用自有根目录；拒绝符号链接/reparse point 越界。清理按本地记录和对象 ID 工作，不拼接不可信文件名。

文件夹解压统一走一个经过测试的 `SafeExtractor`，供原生拖出、另存为和兼容准备复用。它必须先创建随机同级临时目录，以排他方式创建每一层；逐组件校验规范路径仍在根内；拒绝链接、设备文件、替代数据流和重解析点；逐流执行展开字节/条目/深度/压缩比限制。全部成功后再将临时根原子发布为交付目录；失败或取消时删除未发布根，不能把半个目录标成成功。不要调用系统 `unzip` 命令，也不要把 ZIP 文件名直接拼成磁盘路径。

临时区占满时，支持显式另存为直接写到用户授权目录中的临时文件，校验后再发布，避免额外全文件副本；用户输出不计入应用缓存额度，但失败产物仍需安全清理。

## 8. 原生拖放：必须先做技术验证

### 8.1 共同限制与交付接口

源应用通常不知道用户放到了哪个目录；目标还可能是邮件、聊天或其他应用，而不是文件夹。鼠标坐标、前台窗口标题、辅助功能窥探、脚本查询 Explorer/Finder 不能成为交付路径。

目标不支持虚拟文件/Promise 时不能保证拖出。核心验收目标是 Explorer、Finder；第三方应用逐个记录兼容性。不支持时提供“另存为…”或“准备本地文件后重新拖出”，不悄悄退化成网址/文本。

平台桥统一输入：冻结的条目列表、条目类型、净化后的导出名、对象大小、展开信息、内容获取回调、取消令牌、进度回调、租约。拖起时冻结索引与名称，不因后台列表更新而错配内容。

ZIP 单对象流可以并且必须复用，但它只解决“如何取得文件夹内容”，不自动解决“目标应得到 ZIP 还是目录树”。普通文件的导出器把对象流直接提供给目标；文件夹导出器读取同一种对象流、验证 ZIP 后再把内容映射为目录树。不得把 ZIP 流直接命名成文件夹，也不得依赖 Explorer/Finder 自动解压 ZIP。

### 8.2 Windows：OLE 虚拟文件

以 `IDataObject` 提供 `CFSTR_FILEDESCRIPTORW` 的文件组信息，用 `CFSTR_FILECONTENTS` 的 `TYMED_ISTREAM` 提供内容；`FORMATETC.lindex` 对应组内文件索引。真实本地文件才使用 `CF_HDROP`。Microsoft 的格式定义明确区分两者。[Shell Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)

实现任务：

1. STA 线程正确初始化 OLE，提供 `IDropSource`、`IDataObject`，调用原生拖放入口；只允许 Copy。
2. 正确实现格式枚举/查询、描述符大小/Unicode 文件名、索引校验、错误 HRESULT 和 COM 引用计数。
3. 格式查询与描述符生成不做网络 I/O。收到内容请求时才调用 `acquireReadableObject()`；有内容后，普通文件直接返回只读 `IStream`，文件夹按清单提供目录描述符及成员流。
4. 流应提供实际需要的 `Read/Seek/Stat/CopyTo/Clone` 等行为；每个消费者独立位置，支持大于 32 位的文件长度表示。首版用已完整落地的文件后端，避免网络流 seek 问题。[IStream 文档](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nn-objidl-istream)
5. 实现并测试 `IDataObjectAsyncCapability`，协商后台提取。该能力需要目标合作，不是“实现接口就必然异步”；`StartOperation/EndOperation` 与对象引用共同管理生命周期。[异步接口文档](https://learn.microsoft.com/en-us/windows/win32/api/shldisp/nn-shldisp-idataobjectasynccapability)
6. 原型必须验证 Explorer 首次拖出未缓存大文件时的真实行为。目标同步提取时可能等待；不能在 UI 线程阻塞网络，也不能让网络完成反过来等待被阻塞 UI 造成死锁。
7. 若无法可靠协商，显示准备进度，准备完成后允许用户重新拖出；这只算降级路径。Explorer 的未缓存直接拖出仍是完成 MVP 的必要验收项，不能用降级按钮代替。
8. 拖动返回后保留仍在操作的数据对象、流和租约；超时、取消和失败回调必须贯通。

文件夹的首选 Windows 实现：从已验证 ZIP 清单生成完整的 `FILEGROUPDESCRIPTORW` 层级，目录描述符设置目录属性且不提供内容流，普通文件描述符通过对应 `lindex` 提供只读 `IStream`。每个文件流可由 `SafeExtractor` 解到受租约保护的临时树后打开；不要为每个成员重新下载 ZIP。P1 必须在目标 Windows/Explorer 版本实测嵌套目录、空目录、多文件和长路径，因为格式存在不等于具体 Explorer 版本会按预期还原整个层级。

若实测发现目标不可靠支持虚拟目录树，则使用明确降级：先把已验证 ZIP 安全解压到独立传输区，再以真实顶层目录的 `CF_HDROP` 发起第二次拖动。UI 显示“正在准备文件夹”，准备完成后让用户重新拖出；不能声称第一次放下已经完成，也不能猜测第一次拖放的目标目录。

Windows 通常由目标消费流并在目标位置创建文件，源应用无需获取目标目录。官方传输场景文档建议以文件描述与内容流提供类文件数据。[Handling Shell Data Transfer Scenarios](https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios)

接收拖入时支持 Explorer 的真实普通文件与目录路径。目录进入第 6.2 节扫描/压缩流程；拒绝链接、重解析点和非本地 URL。来自 Outlook 等应用的虚拟附件或虚拟目录拖入可以留后续，首版明确不支持，不能当作空文件上传。

### 8.3 macOS：AppKit File Promise

为每个条目创建 `NSFilePromiseProvider`，以 `NSDraggingItem` 开始原生会话，并实现 `NSFilePromiseProviderDelegate`。采用可解析的文件 UTI，未知类型回退通用数据类型；不靠扩展名执行内容。[Apple File Promise 示例](https://developer.apple.com/documentation/appkit/supporting-table-view-drag-and-drop-through-file-promises)

实现 `filePromiseProvider(_:fileNameForType:)` 提供安全名称；`operationQueue(for:)` 使用后台队列；在 `filePromiseProvider(_:writePromiseTo:completionHandler:)` 中获取经校验内容，并写入系统给出的准确目标 URL。普通文件复制对象内容；文件夹声明目录类型并由 `SafeExtractor` 在该 URL 创建完整目录树。这个 URL 是 Promise 专有回调提供的，不能推广为普通拖放都能获知目录。[Apple 写入回调文档](https://developer.apple.com/documentation/appkit/nsfilepromiseproviderdelegate/filepromiseprovider(_:writepromiseto:completionhandler:))

写入遵循 `NSFileCoordinator` 和目标权限要求；在回调内处理冲突与失败，成功/失败均恰好调用一次 completion handler，不能先报成功再开始下载。保持 provider/delegate 生命周期；用户结束拖动不等于 Promise 已交付。准备内容失败时不要留下假成功的空目标。

输入先支持 Finder 本地普通文件与目录 URL，目录进入第 6.2 节扫描/压缩流程。第一阶段默认按直接分发桌面应用实现，不承诺 App Store sandbox；若开启 sandbox，须实现用户授权 URL、security-scoped access/bookmark 的保存、访问及失效回退，并单独验收。权限失效时回退 VPS，不能通过绝对路径绕过授权。

### 8.4 同名、安全文件名与兼容模式

- 在本次多选导出内预先为同名/大小写冲突名分配稳定后缀，如 `report (2).pdf`，并在描述符/Promise 中使用相同名字。
- 文件名只取 basename，过滤 `/`、`\`、NUL、控制字符、`.`、`..`，处理 Windows 保留名称、尾点尾空格、路径长度与 Unicode 规范化。保留原名作展示，实际导出名独立存储。
- 与目标已有文件的冲突优先由原生目标处理；源应用需要写目标 URL 时不得无提示覆盖，使用排他创建/安全协调，返回冲突或通过明确用户操作处理。
- 不用名称作为对象主键，不用名称定位 VPS 物理文件。
- “准备后重新拖出”只能提供真实存在且有租约保护的路径，并明确额外临时占用。取消拖动后不凭鼠标释放就删除文件。

## 9. 数据模型

所有大小用 64 位整数 bytes；时间由服务端生成，API 用 UTC RFC 3339；客户端按用户时区显示。UUID/随机 ID 不承载路径信息。

### 9.1 服务端 SQLite

```text
spaces(
  id PK, name, quota_bytes, max_file_bytes, max_items, retention_seconds
)
devices(
  id PK, space_id FK, display_name, platform,
  token_hash UNIQUE, created_at, last_seen_at, revoked_at
)
files(
  id PK, space_id FK, upload_device_id FK,
  kind,                  # file / folder
  display_name, object_format,
  object_size_bytes, object_sha256,
  expanded_size_bytes, entry_count, tree_sha256,
  state,                 # uploading / ready / deleted / expired
  created_at, ready_at, expires_at, deleted_at,
  object_key UNIQUE,     # 服务端生成；不暴露为用户路径
  client_upload_id, UNIQUE(upload_device_id, client_upload_id)
)
uploads(
  file_id PK FK, reserved_bytes, bytes_received,
  started_at, last_activity_at, temp_key, attempt_id
)
events(
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  space_id FK, type, file_id, payload_json, created_at
)
event_streams(
  space_id PK FK, last_seq, minimum_valid_after
)                         # 与事件同事务更新；剪裁后仍保留游标头/有效下界
schema_migrations(version PK, applied_at)
```

普通文件固定 `object_format=raw`；文件夹固定 `object_format=zip`。`object_sha256` 对 ready 内容不可变；文件夹的 `tree_sha256` 标识还原后的目录内容。更新内容必须新建 `file_id`。不做内容去重，减少跨权限关联与 GC 复杂度。索引至少覆盖 `(space_id,state)`、`(space_id,seq)` 和到期扫描。

### 9.2 客户端 SQLite

```text
device_identity(device_id, space_id, server_url)  # token 不明文存在这里
remote_files(file_id PK, upload_device_id, kind, name, object_format,
             object_size_bytes, object_sha256, expanded_size_bytes,
             entry_count, tree_sha256, state, created_at, expires_at)
sync_state(space_id PK, last_applied_seq)
local_sources(file_id PK, kind, original_path, file_identity,
              quick_snapshot, mtime_ns, uploaded_object_sha256, tree_sha256)
cache_entries(file_id PK, path, state, expected_bytes,
              verified_sha256, last_verified_at)
transfer_entries(task_id PK, file_id, session_id, path, state,
                 reserved_bytes, updated_at, purpose)
upload_jobs(client_upload_id PK, original_path, file_id, state, last_error)
```

活动引用与锁在进程内管理，持久记录用于崩溃回收；不把 SQLite 里的旧引用计数当成跨重启有效租约。`original_path`、缓存路径、设备 token 都不参与元数据同步。

## 10. API 合约

统一前缀 `/v1`，JSON 元数据与原始字节流分离。除配对兑换和受限健康检查外均需 `Authorization: Bearer <device_token>`；服务端从认证上下文决定空间和 `upload_device_id`，忽略或拒绝请求伪造的这两个字段。

| 方法与路径 | 功能 |
| --- | --- |
| `POST /v1/pair` | 一次性短期配对令牌兑换设备身份与设备 token |
| `GET /v1/me` | 当前设备、空间、额度与保留策略 |
| `GET /v1/devices` | 设备列表 |
| `DELETE /v1/devices/{id}` | 撤销设备凭据；空间内设备相互信任的 MVP 管理动作 |
| `POST /v1/files` | 创建上传意图与配额预留，返回 `file_id` |
| `PUT /v1/files/{id}/content` | 上传原始 body；仅上传者设备可写、仅 uploading 可写 |
| `GET /v1/snapshot` | 一致性 ready 列表与同一快照的 `snapshot_seq` |
| `GET /v1/events?after={seq}` | SSE 增量元数据事件 |
| `GET /v1/files/{id}/content` | 下载 ready 内容；认证、同空间校验 |
| `DELETE /v1/files/{id}` | 幂等逻辑删除并产生事件 |

创建示例：

```json
{
  "client_upload_id": "client-generated-uuid",
  "kind": "folder",
  "name": "Project",
  "object_format": "zip",
  "object_size_bytes": 123456,
  "object_sha256": "64-lowercase-hex-characters-in-real-request",
  "expanded_size_bytes": 456789,
  "entry_count": 42,
  "tree_sha256": "64-lowercase-hex-characters-in-real-request"
}
```

普通文件请求使用 `kind=file`、`object_format=raw`，展开大小等于对象大小、`entry_count=1`，不需要 `tree_sha256`。实际 API 严格校验 SHA-256 的 64 位十六进制格式，上述字符串仅解释占位。成功返回文件 ID、`state=uploading`、上传超时与服务端限制；重复幂等键且参数一致返回原结果，参数不同返回 409。不要通过重试反复创建同一上传。

### 10.1 上传事务和持久化

1. 创建意图时在事务中检查空间剩余额度、条目数与预留量，避免并发超配。
2. PUT 采用 `application/octet-stream`，限制长度与读超时；普通文件 body 是原始内容，文件夹 body 是 ZIP。逐块写临时对象并计算 SHA-256。每条目同一时间只允许一个活动 PUT。
3. 大小/哈希不符返回错误，清理失败字节，保留或可重新申请上传意图。重试从第 0 字节覆盖新的 attempt 临时文件，不与旧 attempt 混写。
4. 文件夹对象在发布前按第 6.2 节验证 ZIP、展开限制和目录树摘要；验证产生的临时展开数据必须清除，VPS 长期只保存 ZIP 对象。完整且通过验证的对象 flush/fsync 后同文件系统原子改名，再在 SQLite 单事务内设置 `ready`、确定 `expires_at`、插入 `file.ready` 事件；必要时同步父目录确保崩溃持久性。
5. 文件系统与 SQLite 不能组成同一个原子事务。启动恢复必须处理“对象已落盘但数据库未 ready”的孤儿，以及“数据库 ready 但对象不存在”的损坏，不能凭记录返回空内容。恢复可以校验后补交或回收，但须记录决定。
6. PUT 在服务端成功但客户端超时时，以相同幂等键查询 POST 返回状态，不能重复上传一个新的逻辑条目。

### 10.2 下载、删除与错误

- 返回准确 `Content-Length`、稳定 ETag（按内容摘要）、安全编码的附件名；服务端仍按 ID 找对象，不按 URL 中的文件名找路径。
- 第一阶段完整 GET 即可，Range 可明确不实现；客户端不能依赖断点续传。
- 删除或到期先提交墓碑和 `file.deleted` / `file.expired` 事件，再禁止新下载。已取得对象句柄的活动下载可以完成；物理 GC 等活动引用归零后执行，重启再扫描补清。
- 服务端删除不会请求用户电脑删除其原文件或已交付文件。
- 错误统一 `{ "code": "...", "message": "...", "request_id": "..." }`；至少覆盖 400、401、403、404/410、409、413、422（长度/摘要错误）、429 和 5xx。
- 400/403/413/422 不盲目重试；网络/5xx 有界退避；401 停止后台任务并提示重新配对。任何错误响应不泄漏路径、凭据或堆栈。

## 11. 元数据与事件同步

默认采用 HTTPS REST + SSE，不同时实现 WebSocket。事件只携带元数据变化，文件内容单独 GET；不把内容编码成 JSON/Base64 事件。

1. 客户端获取 `/snapshot`。服务端在同一个 SQLite 读事务中读取 ready 列表与该空间 `event_streams.last_seq`，返回 `{files, snapshot_seq}`；先建一致性读视图，不能分别读取后产生缺口。事件发布与 last_seq 更新在同一写事务；不得用已被剪裁的事件表 `MAX(seq)` 代替持久游标头。
2. 客户端原子替换远端元数据并存储游标，再连接 `/events?after=snapshot_seq`。
3. SSE 先按序回放持久事件，再进入等待；通知通道仅唤醒查询，数据库事件表才是事实来源，避免回放到订阅间丢事件。
4. 每个事件带全局单调 `seq`、`type`、`file_id`、必要的元数据；只返回认证空间的事件。不同空间导致的序号跳跃合法，不能要求本空间序号连续加一。
5. 客户端应用事件和更新 `last_applied_seq` 在同一个本地事务内完成；重复 seq 幂等忽略。
6. 断网带最后成功提交的游标退避重连；可支持 `Last-Event-ID`，与 query 同时出现时明确优先级，建议 query 为准。
7. 游标已超保留期返回明确 `resync_required`，客户端重新快照。事件剪裁与 `minimum_valid_after` 更新在同一事务；不能只看剩余行猜测是否缺历史。客户端游标大于当前空间持久游标头（例如服务端回滚恢复）也要求重新快照。
8. 定时 heartbeat 与低频完整对账防止长连接静默失效；反向代理关闭 SSE 缓冲并配置足够的读超时。
9. 客户端本地到期定时器仅负责提示与保守停止预缓存；云端状态最终以服务端为准。不要依赖设备时钟准确来决定服务端授权。

## 12. 安全与 VPS 运维

- 这是单用户/少量可信设备共享空间，设备在同空间内相互信任，可下载和删除条目；不是多租户权限产品。仍必须测试跨空间访问隔离。
- 配对由 VPS 本地管理命令创建一次性高熵令牌，限制有效期与尝试次数；不暴露永久管理员 token，不写入 URL。兑换成功生成随机设备 token，服务端仅存摘要，比较采用合适的恒时验证。
- 客户端用 Windows Credential Manager / macOS Keychain 保存凭据；日志与设置导出不得包含 token。设备 ID 不是秘密，也不能单独作为认证。
- 强制有效 HTTPS 证书校验；仅显式开发模式可连 localhost HTTP，发布版不得关闭证书校验。
- 请求体大小、并发、速率、空间配额、上传临时预留和磁盘低水位全部限制。对 chunked 请求同样做真实读取上限；反向代理限制应与 API 匹配。
- 不接受客户端提供 `object_key`、绝对路径或覆盖其他设备上传对象；原始文件名只作显示数据。清理与读写都基于随机 ID。
- 私有存储目录仅服务账户可读写，不把对象目录直接映射成静态站点。反向代理转发 API，由 API 每次授权。
- 不自动打开/执行下载内容，不渲染不可信 HTML 预览；显示净化文本。平台来源标记/隔离元数据按系统机制处理并验证，不主动清除 quarantine 或 Mark-of-the-Web 来规避提示。
- SQLite WAL、短事务、单实例服务；迁移前备份数据库。备份若包含对象须与元数据保持可解释的一致性，否则明确只恢复元数据不能恢复内容。
- 结构化日志记录 request_id、device_id、file_id、结果与耗时；不记本机原路径、token、配对令牌或完整正文。
- systemd 使用独立低权限账户；只给数据目录写权限。健康检查不返回设备列表或文件名。
- 服务端定时 GC：超时上传、过期 ready、墓碑对象、孤儿临时文件、过期事件；删除与引用竞争必须有锁/状态机保护。
- 记录 VPS 带宽、磁盘使用、失败率；不为了指标引入专门监控集群。备份、日志轮转和磁盘满告警写入 README。

## 13. 界面与可观察行为

主窗口保持简单：拖入区域 + 文件/文件夹列表 + 状态栏。条目显示类型、名称、对象大小；文件夹可另显展开大小和项数；同时显示上传设备、到期时间，以及“正在压缩 / 正在验证 / 本机上传 / 已预缓存 / 仅云端 / 正在下载 / 正在解压 / 失败”。

- 文件一经 ready 就在其他设备出现，无需等预缓存完成。
- 50 MiB 以外文件也能选择和拖出；第一次交付有下载进度和取消入口。
- 大文件超过预缓存限额时说明“按需下载”，不能标红为错误。
- 文件夹的预缓存进度按 ZIP 对象大小计算；解压进度和临时空间另行显示，避免把压缩大小误当成最终磁盘占用。
- 本机上传不展示为“自动缓存完成”；原路径复用只是内容来源优化。
- 删除云端需要清楚的动作提示，可做确认；“移除本地缓存”不会触发云端删除。
- 菜单提供“另存为…”作为文件管理器外目标不兼容时的明确出口。
- 关闭窗口默认退出可作为最简行为；若实现托盘驻留，明确说明后台传输状态。存在活动 Promise/异步导出时，退出需提示等待或取消，不突然释放被目标使用的资源。

## 14. Codex 执行顺序与阶段验收

每阶段完成代码、构建、必要测试和短报告再继续。报告必须区分“已实现”“已在何系统验证”“尚未验证”。除真实缺少凭据、macOS 环境或需要用户部署授权外，独立完成可执行部分，不以计划代替开发。

### P0：仓库和环境检查

1. 读取项目指令与现有代码，记录可用编译器、Qt/Go/CMake 版本、操作系统和架构。
2. 在 `docs/architecture.md` 固定本文约束和选定版本；建空项目，确保本机可构建启动。
3. 建 `docs/acceptance.md` 验收矩阵，不把未执行的平台标成通过。

交付：构建骨架、依赖安装说明、真实环境记录。没有 macOS 时仍完成其他阶段，但不能宣称双平台已验收。

### P1：先验证原生延迟拖出（关键风险）

1. 不接完整 UI/服务，做最小文件列表和平台桥。
2. 用测试内容提供器模拟“没有本地文件、延迟 3 秒和 15 秒、可失败/可取消”，通过真实原生机制拖到文件管理器。
3. 同时验证普通文件流和一个 ZIP 单对象流。ZIP 在交付层还原为含嵌套文件、空目录和 Unicode 名称的真实文件夹；验证单文件、多文件、文件夹、多选混合、同名、目标取消、目标应用不支持、会话结束后继续提取。
4. 再用 localhost 测试 HTTP 文件验证真实异步下载与对象生命周期。
5. 记录线程、引用、回调时序和具体系统版本，写 `docs/native-drag-spike.md`。

交付：可运行 Windows/macOS 原型及实测证据。未缓存文件和未缓存 ZIP 文件夹的直接拖出问题必须在这里解决，不能等服务和 UI 做完才发现架构不支持。

### P2：VPS 最小闭环

1. 建 SQLite 迁移、空间配置和一次性配对命令。
2. 实现上传意图、配额预留、流式 PUT、哈希校验、ready 发布、完整 GET、删除和到期。
3. 实现一致性 snapshot 与持久 SSE 事件。
4. 加启动恢复、GC、日志、Caddy/systemd 示例。
5. 本地临时数据目录运行集成测试，验证错误上传不出现 ready。

交付：单实例服务、API 文档、本地运行与 VPS 部署说明。没有真实服务器凭据也应完成本地闭环与部署文件。

### P3：客户端上传和同步

1. 持久设备身份与安全凭据；配对、重连、撤销处理。
2. Explorer/Finder 普通文件与目录拖入；文件夹扫描、ZIP 暂存、安全校验、目录树摘要、上传、进度、重试和原路径记录。
3. SQLite 本地元数据、snapshot/SSE、事件幂等与恢复。
4. 两个设备身份验证“上传后另一端出现”，此阶段可全部仅元数据。

交付：真实跨身份上传与列表同步，不自动复制本机上传内容。

### P4：混合缓存和内容获取

1. 独立实现纯函数 `CachePlanner`，按第 5 节验证边界。
2. 实现预留、`.part`、摘要校验、原子提交、取消与重新规划。
3. 实现 `acquireReadableObject()`、普通文件/文件夹原路径安全复用、VPS 回退、消费者去重和 `SafeExtractor`。
4. 实现独立传输区、租约、容量限制、启动清理与手动清理。

交付：后台预缓存始终不超过 52,428,800 bytes，且本机上传条目绝不进入自动候选。

### P5：集成原生拖出与降级交付

1. P1 平台桥接入真实传输服务；所有内容源走统一租约。
2. 测试普通文件与 ZIP 文件夹在已有缓存、未缓存大对象、本机原路径有效/已变更三种主要来源下的交付。
3. 完成进度、取消、错误提示、多选、安全文件名、另存为和兼容准备流程。
4. 检查 UI 不假报成功，失败不残留可用空文件，目标读取期间不清理内容。

交付：A 拖入文件或文件夹 → VPS ready → B 显示 → B 拖到系统目录。普通文件 SHA-256 与上传一致；文件夹自动解压为真实目录，目录树摘要与上传一致，用户不需要手动处理 ZIP。

### P6：打包和真实双平台验收

1. Windows/macOS 分别构建、打包，记录签名/公证是否完成；未签名包如实说明。
2. 完成第 15 节矩阵；真实网络断线、重启与 VPS 重启后重测关键闭环。
3. 输出 README、部署步骤、原生兼容表、已知限制、测试结果与安装产物。
4. 测量空闲内存/CPU、打包体积、缓存和传输区峰值，给实测值。

## 15. 必须执行的测试与验收

自动化测试集中在规则、竞态、数据正确性和权限边界；原生目标交付必须有实机测试，不能只 mock API 或靠截图证明内容正确。

### 15.1 缓存规则（适合自动化）

| 场景 | 必须结果 |
| --- | --- |
| 空列表、全是本机上传 | 计划为空，预缓存占用为 0 |
| 混合上传设备 | 永远排除当前设备 ID |
| 乱序输入、相同大小 | 大小升序，平局规则稳定 |
| 刚好 52,428,800 bytes | 可选入 |
| 单对象 52,428,801 bytes | 不预缓存，但可按需交付 |
| 文件夹 ZIP 为 40 MiB、展开为 400 MiB | 预缓存计 40 MiB；展开只进入独立传输区并受其上限约束 |
| 10/15/30/60 MiB | 只选 10 和 15，剩余允许空闲 |
| 正在下载时新来更小文件 | 重新规划，取消旧任务，预算不超限 |
| 旧缓存有导出租约 | 不删除；新任务等待真实预算释放 |
| 缺文件、摘要错误、断流 | 不标记 cached，安全重试 |
| 元数据已有到期/删除项 | 不入候选并回收闲置缓存 |
| 重启遗留 .part、身份切换 | 正确回收、重新计账，不继承错误身份 |

增加随机集合性质测试：所有选中项合法、稳定、总和 <= LIMIT；对每个被跳过候选，在它被遍历时剩余预算确实不足。

### 15.2 服务端与同步（自动化集成）

- 未认证、伪造 `upload_device_id`、跨空间 file_id、撤销 token：均不能访问对象。
- 同一幂等键重复、PUT 重入、失败后重传、服务端成功但客户端断线：不产生重复逻辑文件。
- 并发上传到额度边界：包含预留后仍不超配。
- 超长 body、短 body、错误 SHA、零字节文件：结果符合协议。
- ZIP 路径穿越、绝对路径、链接、重复/大小写冲突路径、伪造展开大小、压缩炸弹、过深或过多条目：服务端验证拒绝，不发布 ready。
- 发布 ready 前、对象改名后、数据库提交前后崩溃：恢复后不提供损坏内容。
- snapshot 与新事件同时发生、SSE 重复/断线/游标失效：无漏项、无复活已删条目。
- 事件来自其他空间时序号可跳跃，客户端不误报缺包。
- 删除/过期与活跃 GET 竞争：已接收传输可结束，新请求拒绝；GC 不删使用中的对象。
- 文件名包含路径穿越、保留名、Unicode 与同名：对象存储不越界，导出安全。

### 15.3 原路径和临时区

- 本机上传后没有自动预缓存副本；文件夹上传 ZIP 暂存在任务结束后清除，只记录原路径与上传记录。
- 同一原文件或原文件夹有效时导出正确，不重复从 VPS 下载；通过网络计数验证。文件夹最终目录树摘要必须一致。
- 原文件/文件夹改名、删除、同大小改写、增加或移除子项、mtime 伪装、权限撤销：不交付错误内容，正常回退 VPS。
- 校验期间改写：最终内容仍须匹配云端摘要，否则回退/失败。
- 两次同时交付同一内容：共享下载但各自持有租约，一次取消不影响另一次。
- 传输区满、真实磁盘不足、应用崩溃：不删用户文件、不超配、不暴露半文件为可用。
- 闲置清理、手动清理、启动清理：只清应用拥有且无活动租约的资源。

### 15.4 原生实机矩阵

每格记录系统版本、CPU 架构、Qt 版本、对象大小、文件夹展开大小、网络条件、结果，以及普通文件 SHA-256 或文件夹目录树摘要。

| 来源与动作 | Windows Explorer | macOS Finder |
| --- | --- | --- |
| 普通本地文件拖入上传 | 必须通过 | 必须通过 |
| 含嵌套层级与空目录的文件夹拖入，自动压缩上传 | 必须通过 | 必须通过 |
| 已预缓存文件拖出到目录/桌面 | 必须通过 | 必须通过 |
| 60 MiB 或更大且从未下载文件直接拖出 | 必须通过 | 必须通过 |
| 未缓存、模拟慢网，放下后仍需等待下载 | 必须通过 | 必须通过 |
| 未缓存 ZIP 文件夹拖出，自动还原真实文件夹 | 必须通过 | 必须通过 |
| 文件与文件夹混合多选拖出 | 必须通过 | 必须通过 |
| 本机上传条目，原路径有效与失效 | 两者通过 | 两者通过 |
| 多文件混合缓存状态、同名冲突 | 必须通过 | 必须通过 |
| 取消、断网、到期、磁盘满 | 明确失败，不假成功 | 明确失败，不假成功 |
| 拖动结束后持续读，期间触发清理 | 内容不被提前删除 | Promise 完成前不删 |
| 拖到已有同名文件目录 | 原生冲突处理，无静默覆盖 | 安全冲突处理，无静默覆盖 |
| 第三方应用目标 | 记录兼容结果与降级 | 记录兼容结果与降级 |

同机两个身份只能验证逻辑，不代替 Windows ↔ macOS 的真实互传。至少完成 Windows 上传 → macOS 拖出、macOS 上传 → Windows 拖出各一次。

### 15.5 MVP 完成标准

只有以下条件全部满足才能声明第一阶段完成：

1. 两个平台原生未缓存拖出经过实机验收；普通文件输出字节正确，文件夹由 ZIP 自动还原且目录树摘要正确。
2. 预缓存每设备 <= 50 MiB，大小升序贪心、本机上传排除、动态重新规划都有测试证据。
3. 未预缓存文件和 ZIP 文件夹都能按需交付，本机原路径变化能安全回退 VPS；用户拖出文件夹后不需要手动解压。
4. 元数据同步、事件恢复、过期、删除、临时区清理和崩溃恢复可工作。
5. HTTPS、凭据保护、认证授权、配额与路径防护已落实。
6. 可按 README 构建客户端、运行服务、部署到 VPS，并列出实际未完成的签名/公证或平台兼容事项。

## 16. 可直接发给 Codex 的启动指令

```text
请按 DROP_SPACE_CODEX_GUIDE.md 开发 DropSpace 第一阶段。

先读工作区指令和已有项目。若是空目录，采用指南中的 Qt Widgets +
C++/平台原生桥客户端，以及 Go + SQLite 单实例服务默认栈。
不要引入 Electron、Redis、对象存储 SDK、文件系统驱动或目录同步框架。

先建立环境与构建记录，然后优先完成 Windows OLE 虚拟文件和 macOS
NSFilePromiseProvider 的最小原型，验证未缓存内容在放下后延迟提供。
普通拖放不能假定可取得目标目录；禁止以不存在的路径或下载链接替代。

文件夹拖入后扫描并生成标准 ZIP，把 ZIP 当作一个不可变对象。上传、
下载、哈希校验、预缓存和重试必须复用普通文件的单对象内容流；
差异只存在于交付层：普通文件直接提供，文件夹通过 SafeExtractor
自动还原成真实目录树。不得把 ZIP 直接交给用户要求其手动解压。

随后按 P2–P6 完成服务、同步、文件/文件夹上传、缓存、按需获取、
原路径回退、临时区租约清理、原生拖出与打包。预缓存固定
52,428,800 bytes/设备，文件夹按 ZIP 对象实际字节数计入，
排除 upload_device_id 等于当前设备的条目；其余 ready 条目按对象大小升序
贪心选入，放不下跳过，不要求容量一定填满。

每阶段提交实际代码和必要验证，更新 docs/acceptance.md。
有可用环境就继续执行，不在只给计划后停下。缺失某平台环境时，
完成其余能做的工作，准确标明该平台尚未实测，不能伪称通过。
最终提供构建产物、运行说明、测试证据和真实剩余限制。
```

## 17. 技术依据与变更原则

本指南的产品约束来自本轮请求及附带对话预览；未依赖预览中不可见的助手回答补充产品决定。工程默认值属于本指南建议。

原生实现依据为文中链接的 Microsoft、Apple 与 Qt 官方文档。实际目标行为可能随操作系统和目标应用变化；Codex 必须以 P1 原型和实机矩阵验证，不把文档存在某接口等同于全部目标兼容。

如果需要改变 50 MiB 限额、排除本机上传、大小排序、VPS 中转或系统支持范围，应先明确说明对产品行为的影响并获得用户指示。线程组织、模块划分、错误码补充和安全的实现细节可由 Codex 自行决定，不必反复询问。
