# Dev Log – 2026-08-06(公司机): 在线音乐独立播放页 + 分类二级页 + VIP 标识 + 播放崩溃修复

## Branch

- 当前开发分支:`codex/ncm-cloud-music-company-fix-20260805`
- 基线:上一轮 `2c6e7c5`(8/5 修复批次与上游 502 定位交接)
- 本轮全部改动未合并主分支,交接时以本分支 + push 为准

## 本次目标

1. 修复"点歌后几秒跳回首页、无播放状态"的崩溃/跳转问题(核心 bug)
2. 给在线流媒体做独立播放界面,直接移植本地音乐播放页布局,底层仍是同一个播放内核
3. 在线音乐首页改为分类入口:热门歌单 / 歌曲排行榜 / 新歌速递,每个分类有二级菜单页
4. VIP / 付费曲目右侧显示小标识;歌单与榜单行显示封面缩略图

## 播放崩溃修复(真因 + 处理)

### 现象

- 歌单/搜索能加载,点歌后 resolve 成功、音频其实已在播放,但约几秒后设备重启回首页,没有任何播放状态残留

### 根因(两处叠加)

1. **use-after-free(主要)**:`refreshCloudResolveOverlay()` 在页面 `refresh()` 执行中被调用,其中直接 `show(Page::NowPlaying)` 会删除当前屏幕的全部控件;调用方(`refreshCloudMusicSearch()` / `refreshCloudMusicPlaylist()`)随后还在继续操作这些已删除的控件 → 内存访问已释放对象 → 崩溃复位。这与首页布局切换(`show(Page::Home)+return`)不同:那是 refresh 末尾一次性切换,而这里切页后还有代码在跑
2. **任务栈过小(次要)**:`cloudMusicControllerTask` 栈只有 10KB,却同步执行 TLS resolve HTTP + `audio.connecttohost()`(DNS/TLS/网络栈重活),连接真正开始时是栈溢出强嫌疑

### 修复

- 新增**延迟导航**机制:`HifiUi` 增加 `m_pendingNavigate` / `m_pendingNavigateSet`;`refreshCloudResolveOverlay()` 不再直接 `show()`,而是设置 pending,由 `HifiUi::refresh()` **所有页面分支执行完毕后**统一 `show()`(避免任何页面在切页后继续触碰旧控件);`show()` 开头消费并清空 pending,防止陈旧导航二次触发
- `cloudMusicControllerTask` 栈 10KB → **20KB**(低风险高收益)
- 新增一次性"刚开播"闩锁 `s_cloudPlaybackJustStarted`(读写都在 `s_cloudResultMutex` 保护下,避免清 false 竞态吃掉跳转);resolve 成功后置位,UI 读到后延迟导航到新的 `CloudNowPlaying` 页
- `playerCoreReadSnapshot()` 对 `PlayerSource::CloudMusic` 用 `s_cloudNowPlaying` 填充 title/detail(CDN 直链没有 ICY 元数据),让播放页/首页能显示真实曲名与歌手

## 新 UI:在线音乐独立播放页(CloudNowPlaying)

- 完整移植本地音乐播放页的平铺卡片布局:72x72 封面/占位图、标题跑马灯、歌手行、封面下 elapsed/total、28x11 彩虹频谱、进度条、底部控制栏
- 与控制栏要求一致:播放列表按钮改为打开**在线音乐分类首页**;另加 HOME 槽位返回桌面
- 上一曲/下一曲走"队列上下文"(见下);播放/暂停与电台、本地共用 `togglePause()`,底层同一套内核
- 进度条为只读(CDN 流字节级 seek 不可靠,拖动会被下一帧真实进度拉回)
- 封面:resolve 返回的 `cover_url` 由后台任务下载到 SD(`/cloudimg/np_<trackId>.jpg`,按曲目 id 缓存避免串图),下载完成后页面自动重建换图

## 队列上下文(上一曲/下一曲/自动连播)

- 点歌时记录 `m_cloudQueueSource`(Search / Playlist / NewSongs)+ 索引,上一曲/下一曲在该列表内循环(等同本地 RepeatAll)
- 云端曲目自然播完(EOF)自动播队列下一首(与本地 SD 的 eofCount 边缘检测同一处新增 CloudMusic 分支)
- 播放页从哪个列表进入就切哪个列表;打开新歌单点歌会切换队列上下文

## 在线音乐首页 → 分类入口

- `CloudMusicHome` 从"热门歌单列表"改为三个分类磁贴:热门歌单 / 歌曲排行榜 / 新歌速递,各带图标与一行说明,右上角保留搜索
- 二级页:
  - **热门歌单**(`CloudHotPlaylists`):原热门歌单列表迁入,每行 28x28 封面缩略图(后台 `cloudThumbSyncTask` 下载到 `/cloudimg/p_<i>.jpg` 缓存,未就绪先显示占位图,下载完自动多重建一次换图)
  - **歌曲排行榜**(`CloudRankings`):榜单挑选页(飙升榜 / 新歌榜 / 热歌榜等,来自网关新接口),点击榜单直接复用歌单详情页加载曲目(网易云榜单 id 即歌单 id)
  - **新歌速递**(`CloudNewSongs`):新歌曲目列表,行样式与搜索一致,直接点播

## VIP / 付费标识

- 网关 `buildSearchItem()` 新增 `vip`(fee==1)与 `paid`(fee==4)字段——**仅信息提示,不解锁付费内容**(resolve 对 VIP 曲目仍返回 ACCESS_DENIED,与项目边界一致)
- 固件解析 `vip/paid`,搜索 / 歌单 / 新歌列表行右侧显示紫色 `VIP` 或橙色 `付费` 圆角小标,同时该行文字置灰(沿用 playableHint)
- 错误提示中文化:VIP 权限 / NO_PLAYABLE_URL / 502 等翻译为中文(cloudErrToCn)

## 网关新增(需重新部署才生效)

- `GET /esp/v1/rankings` — 榜单列表(上游 `/toplist`,id/name/cover_url/update_freq)
- `GET /esp/v1/new-songs` — 新歌(上游 `/top/song`,归一化为与搜索一致的单曲结构)
- 两者均走设备密钥鉴权 + rateLimit + TTL 缓存
- **部署要求**:把本分支 `services/ncm-gateway` 部署到 Render(服务 A:esp32-ncm-gateway-hifi 重新 Deploy 即可);部署前固件端"排行榜/新歌速递"会显示加载失败(接口 404),热门歌单/搜索/播放不受影响
- 本地已实测:rankings / new-songs 对线上 api-enhanced 均返回正常数据(含 cover_url、vip/paid)

## 构建数据

- RAM 38.8%(127,076 / 327,680;新增约 13KB:榜单+新歌数组、队列、20KB 控制器栈),Flash 30.9%(5,180,130 / 16,777,216)
- 编译干净(无 error,仅有与本次无关的既有 warning)

## 真机验证清单(烧录后)

1. 搜索 / 热门歌单点歌 → 应自动跳到新的在线播放页,显示曲名/歌手/进度,不再跳回首页
2. 播放页上一曲/下一曲在列表内循环;播放键暂停/恢复
3. 分类页:热门歌单(缩略图)、排行榜(需网关已部署新接口)、新歌速递
4. VIP 曲目右侧显示 VIP 小标,点击提示"该曲目需要VIP或付费"
5. 歌单封面缩略图在首次进入后下载缓存,再次进入秒出

## 遗留 / 已知边界

- 排行榜与新歌速递依赖网关新接口部署,固件端已按 404 优雅显示"加载失败"
- 在线播放页暂无歌词(云端曲目没有本地 SYLT 帧,后续可接网关 /lyrics 接口)
- 封面下载使用 HTTP/HTTPS 直链,CDN 防盗链变化时降级为音乐音符占位图(不影响播放)
