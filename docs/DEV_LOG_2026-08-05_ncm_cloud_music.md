# Dev Log — 2026-08-05: 网易云在线音乐功能（阶段1-4）+ 首次真机验证

Board: Waveshare ESP32-S3-Touch-LCD-1.9. Branch: `codex/ncm-cloud-music`
(based on `codex/usb-msc-reboot-mode` at `d2ca52c`, the company machine's
Phase B USB MSC reboot-mode handoff — that branch's own on-device
verification is still separately pending, untouched by this entry).

## 0. 起点：读方案文档，明确边界

用户提供了一份完整的《ESP32-S3 网易云音乐流媒体功能开发方案》（Render
Free Web Service + NeteaseCloudMusicApiEnhanced + 音乐 CDN 直连），核心
架构原则：Render 网关只解析元数据和临时播放地址，从不转发/缓存音频正
文，ESP32 直接连音乐 CDN 播放，复用现有 AudioEngine（不新建解码器）。

**明确拒绝的请求**：用户一度要求"绕过登录和地区限制"，这被拒绝——绕过
网易云的访问控制、获取账号/地区本没有权限播放的内容，不在协助范围内。
最终方案严格限定为：只播放账号（或匿名访问）本身有权限播放的内容，
`ENABLE_GENERAL_UNBLOCK`/`ENABLE_RANDOM_CN_IP`/`ENABLE_PROXY` 三个开关
在网关和上游两端都强制为 `false`（网关启动时甚至会检查这三个值，任一
为 `true` 直接拒绝启动）。

## 1. 阶段0：只读分析

按方案文档要求，先做只读分析再动代码。关键发现：

- `PlayerService`/`PlayerSnapshot` 架构已经很干净：UI 只认
  `PlayerSource`/`PlayerSnapshot`，真正的连接靠 `main.cpp` 里的
  `playerCoreXxx()` 桥接函数。`playerCorePlayRadioUrl()` 本质就是
  `connecttohost(url)`——通用的"连 HTTP(S) 音频流"调用，不是电台专属逻
  辑，意味着在线音乐播放可以直接复用这条路径，不需要新解码器。
- 项目没有引入 JSON 库，所有 JSON 解析都是手写的字符串查找
  （`jsonNumber`/`jsonStringField`），风格上要延续这个约定，不能引入
  ArduinoJson。
- 已有 HTTPS+异步任务+缓存的成熟先例可以照抄：`fetchWeatherOnce`（
  `WiFiClientSecure`+`HTTPClient`）、`radioIconSyncTask`（下载图片到 SD
  缓存+JPEG解码）、`lyricsFetchTask`（队列+常驻任务+"当前索引仍匹配才
  应用结果"的世代校验雏形）。
- 项目目前没有真正的"请求取消/generation丢弃过期响应"机制，只有
  `lyricsFetchTask` 那种简化版先例——网易云的搜索/resolve 需要自己实现。

## 2. 阶段1：Render Music Gateway（`services/ncm-gateway/`）

新增一个独立的 Node/Express 服务，只做元数据/URL解析转发：

- `config.js` 集中管理所有环境变量，`ENABLE_GENERAL_UNBLOCK`/
  `ENABLE_RANDOM_CN_IP`/`ENABLE_PROXY` 任一为 true 时进程直接拒绝启动。
- `auth.js`：`X-Device-Key` 鉴权 + 按 bucket 分级限流（元数据
  60/min，resolve 20/min 更严格）。
- `cache.js`：纯内存 TTL+LRU（无 DB、无持久化），`/tracks/:id/resolve`
  永不缓存（缓存住一个已过期的临时 CDN URL 比没缓存更危险）。
- `ncmProvider.js`：所有上游接口的字段映射集中在这一个文件；
  `resolveTrack()` 会校验返回的 `stream.url` 协议/host/格式，拒绝
  m3u8/mpd/HTML 和回指网关自己的地址。
- 6 个 `/esp/v1/*` 端点，10 个单元测试（`node:test`+`supertest`）全过。

**架构决定**：网关本身**不**内置 NeteaseCloudMusicApiEnhanced，而是通过
`NCM_UPSTREAM_URL` 指向一个单独部署的上游实例——把这个高频变动的第三
方逆向工程 API 隔离在一个可替换的配置项后面，不牵连网关自身的稳定性。

## 3. 阶段2-4：固件端配置、浏览、播放

- **阶段2**：`CloudMusicConfig`（网关地址+设备密钥，NVS 持久化）+
  `CloudServiceState`（Unknown/Waking/Ready/Offline）冷启动状态机——立
  即尝试一次，之后每 7 秒重试，70 秒后放弃标记 Offline。
- **阶段3**：热门歌单/搜索/歌单详情三个浏览页，纯 UI，不接播放。新写
  了 `jsonArrayItems()`——按大括号深度+引号转义手动扫描，从 JSON 数组
  里切出每个对象的原始文本片段，再用已有的
  `jsonStringField`/`jsonNumber` 逐条解析，是这个项目"不用 JSON 库"
  这个既有约定的自然延伸。
- **阶段4**：resolve + 播放闭环。核心难点：电台和在线音乐都通过同一个
  `connecttohost()` 连播放流，`playerCoreReadSnapshot()` 原来只能靠
  `s_f_isWebConnected` 推断来源，没法区分两者——加了个
  `s_cloudMusicPlaying` 标志位消歧。resolve 成功后**直接在后台任务
  （`cloudMusicControllerTask`）里调用 `connecttohost()`**，不经过
  LVGL 线程转发（这个项目里后台任务直接摸音频对象已有先例，
  `usbStorageMountTask` 的 `stopSong()` 就是这么做的）。

每个阶段都是独立 commit，clean build 通过（RAM 从 29.0% 涨到 34.4%，
主要是搜索/歌单/播放队列几个固定容量数组，符合方案 10.2 节的内存预算）。

## 4. 真实部署 Render，端到端跑通

这部分全程用户在自己的浏览器里操作 Render 网页，我只给指令、看日志截
图排查，从未接触过用户的登录凭证或 Cookie 明文。

**踩的坑**：
- 上游 NeteaseCloudMusicApiEnhanced（fork 到 `pullead/api-enhanced`）
  首次 Docker 构建失败：`husky install` 因为 `NODE_ENV=production` 跳
  过了 devDependencies 安装而找不到命令，整个构建中断。修复：
  `package.json` 的 `prepare` 脚本改成 `"husky install || true"`（这
  是上游仓库自己的问题，不在我们仓库里）。
- 网关的 `NCM_UPSTREAM_URL` 环境变量一度被误填成网关自己的地址（自己
  指自己），以及和另一行重复导致 Render 拒绝保存；`DEVICE_API_KEY` 也
  有一次没匹配上返回 401——都是 Render 网页表单操作上的笔误，非代码
  问题。
- Render 免费层服务闲置一段时间会休眠，第一次请求要经历约 50 秒冷启
  动，中间会看到 502/超时，属预期行为。

**验证结果**：
- 网关 `/esp/v1/health`、`/esp/v1/search` 返回真实网易云搜索结果，字
  段格式和 `ncmProvider.js` 的假设完全吻合。
- 未登录状态下 `/esp/v1/tracks/:id/resolve` 对大多数曲目返回
  `NO_PLAYABLE_URL`（符合预期——网易云很多曲目本来就需要登录/会员）。
  用户通过上游自带的 `/qrlogin.html` 扫码登录自己的网易云账号，把拿到
  的 Cookie 配置进网关的 `NCM_COOKIE`（全程没有把 Cookie 明文发给我）
  后，resolve 对能听的曲目**返回了真实可播放的 CDN 直链
  （`http://m801.music.126.net/.../xxx.mp3?...`）**。

后端到此为止彻底端到端验证通过，是这次真正的关键里程碑。

## 5. 首次真机烧录，发现两个真实 UI bug

网关跑通后，编译烧录 `codex/ncm-cloud-music` 分支固件到真机，第一次
实机测试"设置 > 在线音乐"页面就发现了两个此前只在桌面上看代码没能预
判到的问题：

1. **密码遮罩乱码**：LVGL 文本框密码模式默认用 `LV_SYMBOL_BULLET`
   （真的是 "•" 这个符号，属于 montserrat 符号字体范围）做遮罩字符，
   项目的 CJK 字体只烘焙了中文+ASCII 0x20-0x7F（见
   `scripts/gen_fonts_mac.sh`），不含这个符号，每输入一个字符就是一
   个方框。用 `lv_textarea_set_password_bullet(field, "*")` 换成 ASCII
   字符修复——**这个 bug 同样存在于 WiFi 密码输入页，是这个项目里所
   有密码输入框的通病，一并修了**。
2. **键盘符号模式显示不全**：自定义的 3 行紧凑键盘只给
   `LV_KEYBOARD_MODE_TEXT_LOWER`（字母）注册了 map，点击 "1#" 切到
   `LV_KEYBOARD_MODE_SPECIAL`（LVGL 认出这个字符串会自动切换模式）
   时，因为没注册对应 map，回退成 LVGL 内置的默认符号键盘（行数更
   多），超出键盘固定高度被裁到屏幕外，输入网址所需的数字和 `.`/`:`/
   `/`/`-` 根本按不到。给 CloudMusicSettings、CloudMusicSearch、WiFi
   密码输入这三处键盘都补上了 SPECIAL 模式的自定义 map。

修完这两个后，用户反馈布局本身也局促（两个字段+压缩键盘挤在一屏，导
致文字换行溢出、键盘被进一步压扁）。顺势把 `CloudMusicSettings` 整页
重新设计成跟 WiFi 密码页同款的"一次只编辑一个字段"模式（新增
`CloudMusicConfigStage` 状态机：Overview 展示网关地址/设备密钥两行 +
编辑按钮 + 测试连接按钮；点编辑进全屏字段 + 完整 150px 键盘）。

## 当前状态 / 待验证

- 后端（Render 网关+上游）：**已在真实环境验证通过**，搜索、歌单、
  resolve 全部返回真实数据。
- 固件：4 个阶段 + UI bug 修复全部 clean build 通过，已烧录到真机。
  **最新一版（含设置页重排版）尚未做完整的真机回归**——上一次真机验
  证是重排版之前的版本，只确认到"方框乱码+键盘裁切"这两个具体 bug，
  重排版后的效果（能否正常输入、保存、连接状态是否正确显示）需要下
  一次真机测试确认。
- 首页导航栏"在线"入口（`Page::CloudMusicHome`）、搜索、歌单详情三
  个浏览页尚未做过真机测试——目前只验证了网关本身，UI 层面的浏览/播
  放闭环还没有在真机上走过一遍完整流程（搜索→选歌单→点歌→出声音）。

## 交接给公司电脑

- 分支：`codex/ncm-cloud-music`，已推送到 GitHub。
- Render 网关已部署且工作正常：`https://esp32-ncm-gateway-hifi.onrender.com`
  （`DEVICE_API_KEY`/`NCM_COOKIE` 已配置在 Render 环境变量里，不在这
  份日志或任何提交里出现）。
- 下一步建议：先完成真机端到端测试（设置页重排版效果确认 → 首页"在
  线"入口 → 搜索/热门歌单 → 点歌播放），走通后再考虑方案文档的阶段5-7
  （封面/歌词/首页联动、播放队列/预取/收藏、稳定性测试）。
- 另外 `codex/usb-msc-reboot-mode` 分支（USB MSC 重启模式）的真机验
  证也还没做完，跟这条线是并行独立的两件事，不要混在一起测。
