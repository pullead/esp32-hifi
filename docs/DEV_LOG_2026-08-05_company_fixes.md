# Dev Log — 2026-08-05(公司机):在线音乐 UI/配置修复 + 历史网关配置 + 上游部署问题定位

## Branch

- Handoff branch (new): `codex/ncm-cloud-music-company-fix-20260805`
- Base: `origin/codex/ncm-cloud-music` at `4fac4f0`(家里 Claude Code 的阶段 1-4 交接)
- 本机真机接手后发现问题并修复,今天这批改动 + 本日志都在新分支上,交接回家里电脑。

## 背景

家里已完成阶段 1-4(网关 + 浏览 + 播放闭环),但真机上一测就暴露了几个问题:

1. 配置页中文乱码、键盘只有小写字母、数字/符号切换疯狂抖动、保存报"地址无效"
2. 网关/密钥验证:https 有效,http 会 301 跳转
3. 打开"在线音乐"持续 `upstream/top/playlist returned HTTP 502`

## 修复清单(全部已编译并烧录真机)

### UI / 输入

- **乱码**:8 处中文标签(网关地址/设备密钥行、编辑按钮、歌单/搜索/错误文案)误用纯拉丁 `montserrat_12` → 全部改 `cjk_13`。
- **三模式键盘**:新增共享 `buildSharedKb()`——26 键小写 / 大写(ABC 键)/ 数字+符号("1#" 键),云配置、搜索、WiFi 密码三个输入页统一。
- **模式切换抖动根治**:根因是 LVGL 普通键在"按下"时就发 `VALUE_CHANGED`,切换地图后手指位置对应的按钮索引变了,PRESSING 事件又把它切回来,来回狂闪。给 "1#"/"abc" 模式键加 `CLICK_TRIG | NO_REPEAT`(ctrl=0x221),改为松手触发、长按不重复。
- **保存失败提示精确化**:原来"地址无效"是误导(实际是另一个字段为空)。现在逐字段报:"网关地址不能为空"/"设备密钥不能为空"/"输入过长"。
- **布局**:配置行加高 40px,标签贴顶、值贴底,不再重叠;按钮改为三连:测试连接 / 扫码配置 / 历史配置。

### 网络 / 配置

- **URL 规范化**:保存与读取时统一把 `http://` 转成 `https://`(Render 只提供 https,http 是 301 桩;此前 http 配置会让 TLS 客户端连 80 端口直接失败)。
- **请求统一显式 TLS**:`cloudMusicHttpGet` 改回 `WiFiClientSecure + setInsecure`(与天气/歌词同一模式);health 超时 3s→10s。
- **配置持久化 bug 修复**:`cloudMusicLoadConfig()` 原来在 `pref.begin()` **之前**调用,Preferences 命名空间未打开,开机必然读空——这就是"每次烧录后配置全没"的根因。已移到 `pref.begin()` 之后,重启/烧录(仅 app 分区)配置自动恢复。

### 新功能

- **手机扫码配置**:设置 → 在线音乐 → "扫码配置"显示二维码(`http://<板子IP>/cloud_config`);手机打开网页输入网关地址+密钥 → 保存 → 板子 NVS 同步并触发健康检查。新增 web 命令 `cloud_config` / `cloud_config_json` / `cloud_config_set`。
- **网关历史配置**:最多 5 组,每次保存/使用自动 upsert(按 URL 去重、刷新密钥+时间戳、最新在前),NVS 持久化(`cm_h<i>u/k/t`),只有手动"删除"才移除;UI"历史配置"子页:点行切换为当前配置、"当前"标记、每行删除按钮;时间显示依赖 NTP(未同步显示"时间未同步")。
- **上游唤醒重试**:搜索/热门歌单/歌单详情/解析播放最多重试 8 次、间隔 7s(≈56s 窗口),适配 Render 免费层休眠唤醒;被新命令取代时立即退出(按 generation 判断)。

## 上游 502 定位(重要交接)

- 现象:`/top/playlist`、`/search` 持续 502,重试 2 分钟无效。
- 本机复现:同一份 `pullead/api-enhanced` 代码在本地跑通,`/top/playlist` 与 `/search` 都返回真实网易云数据 → **代码没问题,是 Render 部署问题**。
- 根因:Render 上的 `esp32-ncm-gateway.onrender.com` 这个服务被部署成了**网关代码**(`services/ncm-gateway`)而不是上游 api-enhanced——日志反复出现
  `FATAL: NCM_UPSTREAM_URL must be set in production`(出自 `services/ncm-gateway/src/server.js`),启动即退出 → 网关调它必 502。
- **待家机在 Render 控制台处置**(我无法登录用户的 Render 账号):
  1. 把 `esp32-ncm-gateway` 服务改为部署 `https://github.com/pullead/api-enhanced`(main, Root=/);
     Environment = **Docker**(仓库自带 Dockerfile;若失败退回 Node:构建 `pnpm install --frozen-lockfile --prod`,启动 `node app.js`)。
  2. 设环境变量 `ENABLE_GENERAL_UNBLOCK=false`、`ENABLE_RANDOM_CN_IP=false`、`ENABLE_PROXY=false`。
  3. Deploy。因为 URL 不变(`esp32-ncm-gateway.onrender.com`),网关的 `NCM_UPSTREAM_URL` **不用改**。
  - 恢复后验证:`/top/playlist`、`/search` 返回真实数据;固件无需改动(重试机制会自动接上)。

## 待家机真机验证

- 上游恢复后:热门歌单、搜索、点歌播放闭环。
- 历史配置:保存/切换/删除/重启后保留、按时间排序。
- 扫码配置:手机输入 → 板子同步 → 概览显示已配置。
- 烧录后配置保留(不再"每次烧录就没了")。

## 构建 / 烧录

- 增量构建多次通过;RAM 34.7%(113,812 / 327,680),Flash 30.8%(5,161,414 / 16,777,216)。
- 已烧录真机多轮验证。
