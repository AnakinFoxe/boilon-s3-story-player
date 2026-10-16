# P5-1 设计图：产品当前形态

<!-- 由 `node docs/boilon/p5_1_prototype/reference.js --gallery docs/boilon/p5_1_gallery` 生成，不要手改。 -->

设备每个画面的样子：240×296，按 2 倍保存，四角按估计的 40 px 圆角裁掉，示例数据是假的。图由原型画出；
`test_p5_1_ui` 证明固件画出的每一张都与原型逐像素一致。屏幕上的实际颜色以真机为准。默认组合是暖纸配色、
A 故事卡、字幕关；设计说明见 [P5_1_PIXEL_UI.md](../P5_1_PIXEL_UI.md)。

## 播放页 · A 故事卡（默认）

标题优先。状态栏、25 段进度条和时间行是 A、C 共用的；AI 就绪时右上角有喵喵头像；不是顺序播放时状态栏有播放模式图标。

<table>
<tr>
<td align="center"><img src="play-paper-card.png" width="240" alt="正在播放"><br>正在播放</td>
<td align="center"><img src="pause-paper-card.png" width="240" alt="已暂停"><br>已暂停</td>
<td align="center"><img src="locate-paper-card.png" width="240" alt="开机续播，正在定位"><br>开机续播，正在定位</td>
</tr>
<tr>
<td align="center"><img src="long-paper-card.png" width="240" alt="长目录名和长标题"><br>长目录名和长标题</td>
<td align="center"><img src="end-paper-card.png" width="240" alt="目录播完"><br>目录播完</td>
<td align="center"><img src="offline-paper-card.png" width="240" alt="AI 不在线、电量低"><br>AI 不在线、电量低</td>
</tr>
<tr>
<td align="center"><img src="tofu-paper-card.png" width="240" alt="字库外的字显示方框"><br>字库外的字显示方框</td>
<td align="center"><img src="battery-unknown-paper-card.png" width="240" alt="电量未知"><br>电量未知</td>
<td align="center"><img src="mode-loop-paper-card.png" width="240" alt="目录循环（状态栏有图标）"><br>目录循环（状态栏有图标）</td>
</tr>
<tr>
<td align="center"><img src="mode-single-paper-card.png" width="240" alt="单曲循环"><br>单曲循环</td>
<td align="center"><img src="mode-shuffle-paper-card.png" width="240" alt="随机播放"><br>随机播放</td>
</tr>
</table>

## 播放页 · C 喵喵陪听

喵喵戴着耳机陪着听，暂停、定位和播完各有样子。

<table>
<tr>
<td align="center"><img src="play-paper-cat.png" width="240" alt="正在播放"><br>正在播放</td>
<td align="center"><img src="pause-paper-cat.png" width="240" alt="已暂停"><br>已暂停</td>
<td align="center"><img src="locate-paper-cat.png" width="240" alt="正在定位"><br>正在定位</td>
</tr>
<tr>
<td align="center"><img src="end-paper-cat.png" width="240" alt="目录播完"><br>目录播完</td>
<td align="center"><img src="offline-paper-cat.png" width="240" alt="AI 不在线、电量低"><br>AI 不在线、电量低</td>
<td align="center"><img src="mode-shuffle-paper-cat.png" width="240" alt="随机播放"><br>随机播放</td>
</tr>
</table>

## 覆盖提示与选择目录

调音量、跳转和短提示叠在播放页上，过一会儿自动消失。

<table>
<tr>
<td align="center"><img src="volume-paper-card.png" width="240" alt="调音量"><br>调音量</td>
<td align="center"><img src="seek-paper-card.png" width="240" alt="后退（连点累计）"><br>后退（连点累计）</td>
<td align="center"><img src="toast-offline-paper-card.png" width="240" alt="喵喵暂时不在线"><br>喵喵暂时不在线</td>
</tr>
<tr>
<td align="center"><img src="toast-activated-paper-card.png" width="240" alt="已激活"><br>已激活</td>
<td align="center"><img src="toast-refused-paper-card.png" width="240" alt="现在不能配网"><br>现在不能配网</td>
<td align="center"><img src="toast-lowbat-paper-card.png" width="240" alt="电量低（只提示一次）"><br>电量低（只提示一次）</td>
</tr>
<tr>
<td align="center"><img src="directory-paper.png" width="240" alt="选择目录"><br>选择目录</td>
</tr>
</table>

## 系统页

只有本地故事和 AI 都用不了时才用整页。

<table>
<tr>
<td align="center"><img src="checking-paper.png" width="240" alt="开机检查 SD 卡"><br>开机检查 SD 卡</td>
<td align="center"><img src="nocard-paper.png" width="240" alt="没有 SD 卡（AI 就绪）"><br>没有 SD 卡（AI 就绪）</td>
<td align="center"><img src="nocard-offline-paper.png" width="240" alt="没有 SD 卡（AI 不在线）"><br>没有 SD 卡（AI 不在线）</td>
</tr>
<tr>
<td align="center"><img src="empty-paper.png" width="240" alt="卡里没有故事"><br>卡里没有故事</td>
<td align="center"><img src="error-paper.png" width="240" alt="读卡出错"><br>读卡出错</td>
<td align="center"><img src="goodbye-paper.png" width="240" alt="关机"><br>关机</td>
</tr>
<tr>
<td align="center"><img src="lowbattery-paper.png" width="240" alt="电量低，关机"><br>电量低，关机</td>
<td align="center"><img src="updating-paper.png" width="240" alt="从 SD 卡升级"><br>从 SD 卡升级</td>
<td align="center"><img src="updatefail-paper.png" width="240" alt="升级没有成功"><br>升级没有成功</td>
</tr>
<tr>
<td align="center"><img src="updated-paper.png" width="240" alt="升级完成"><br>升级完成</td>
<td align="center"><img src="hotspot-paper.png" width="240" alt="配网热点"><br>配网热点</td>
<td align="center"><img src="activation-paper.png" width="240" alt="激活码"><br>激活码</td>
</tr>
</table>

## AI 对话

紫蓝底，喵喵 3 倍。只有真的打断了正在播的故事，底部才写「故事已暂停」。

<table>
<tr>
<td align="center"><img src="ai-connecting-paper.png" width="240" alt="连接中"><br>连接中</td>
<td align="center"><img src="ai-listening-paper.png" width="240" alt="我在听（打断了故事）"><br>我在听（打断了故事）</td>
<td align="center"><img src="ai-listening-paused-paper.png" width="240" alt="我在听（故事原本暂停）"><br>我在听（故事原本暂停）</td>
</tr>
<tr>
<td align="center"><img src="ai-thinking-paper.png" width="240" alt="想一想"><br>想一想</td>
<td align="center"><img src="ai-speaking-paper.png" width="240" alt="我在说"><br>我在说</td>
<td align="center"><img src="ai-returning-paper.png" width="240" alt="回到故事"><br>回到故事</td>
</tr>
<tr>
<td align="center"><img src="ai-returning-failed-paper.png" width="240" alt="出错后回到故事"><br>出错后回到故事</td>
<td align="center"><img src="ai-listening-nostory-paper.png" width="240" alt="没卡时对话"><br>没卡时对话</td>
<td align="center"><img src="ai-returning-nostory-paper.png" width="240" alt="没卡时对话结束"><br>没卡时对话结束</td>
</tr>
<tr>
<td align="center"><img src="ai-returning-nostory-failed-paper.png" width="240" alt="没卡时出错结束"><br>没卡时出错结束</td>
</tr>
</table>

## AI 动画帧

同一画面的其他帧：眨眼、右耳抖、思考泡依次亮起、嘴开合。

<table>
<tr>
<td align="center"><img src="ai-connecting-blink-paper.png" width="240" alt="连接中：眨眼"><br>连接中：眨眼</td>
<td align="center"><img src="ai-listening-twitch-paper.png" width="240" alt="我在听：右耳抖"><br>我在听：右耳抖</td>
<td align="center"><img src="ai-listening-blink-paper.png" width="240" alt="我在听：眨眼"><br>我在听：眨眼</td>
</tr>
<tr>
<td align="center"><img src="ai-thinking-one-paper.png" width="240" alt="想一想：第一个思考泡"><br>想一想：第一个思考泡</td>
<td align="center"><img src="ai-speaking-closed-paper.png" width="240" alt="我在说：闭嘴"><br>我在说：闭嘴</td>
</tr>
</table>

## 说话时的表情

服务器发来的情绪词归到这几种脸，只在喵喵说话时用；嘴照常开合。

<table>
<tr>
<td align="center"><img src="ai-face-happy-paper.png" width="240" alt="开心"><br>开心</td>
<td align="center"><img src="ai-face-happy-open-paper.png" width="240" alt="开心（张嘴）"><br>开心（张嘴）</td>
<td align="center"><img src="ai-face-surprised-paper.png" width="240" alt="惊讶"><br>惊讶</td>
</tr>
<tr>
<td align="center"><img src="ai-face-surprised-open-paper.png" width="240" alt="惊讶（张嘴）"><br>惊讶（张嘴）</td>
<td align="center"><img src="ai-face-sad-paper.png" width="240" alt="难过"><br>难过</td>
<td align="center"><img src="ai-face-sad-open-paper.png" width="240" alt="难过（张嘴）"><br>难过（张嘴）</td>
</tr>
<tr>
<td align="center"><img src="ai-face-sleepy-paper.png" width="240" alt="困了"><br>困了</td>
<td align="center"><img src="ai-face-sleepy-open-paper.png" width="240" alt="困了（张嘴）"><br>困了（张嘴）</td>
<td align="center"><img src="ai-face-thinking-paper.png" width="240" alt="思考"><br>思考</td>
</tr>
<tr>
<td align="center"><img src="ai-face-thinking-open-paper.png" width="240" alt="思考（张嘴）"><br>思考（张嘴）</td>
</tr>
</table>

## 字幕（打开时）

对喵喵说「打开字幕」：想一想时显示孩子说的话，说话时显示正在说的那一句。

<table>
<tr>
<td align="center"><img src="ai-subs-thinking-paper.png" width="240" alt="想一想：孩子说的话"><br>想一想：孩子说的话</td>
<td align="center"><img src="ai-subs-speaking-paper.png" width="240" alt="我在说：正在说的那一句"><br>我在说：正在说的那一句</td>
<td align="center"><img src="ai-subs-speaking-heard-paper.png" width="240" alt="我在说：第一句还没到"><br>我在说：第一句还没到</td>
</tr>
<tr>
<td align="center"><img src="ai-subs-speaking-long-paper.png" width="240" alt="太长时末尾省略"><br>太长时末尾省略</td>
<td align="center"><img src="ai-subs-speaking-happy-paper.png" width="240" alt="字幕和表情"><br>字幕和表情</td>
<td align="center"><img src="ai-subs-speaking-nostory-paper.png" width="240" alt="没卡时"><br>没卡时</td>
</tr>
<tr>
<td align="center"><img src="ai-subs-listening-paper.png" width="240" alt="我在听时不显示气泡"><br>我在听时不显示气泡</td>
</tr>
</table>

## 夜灯配色

对喵喵说「换成夜灯」，所有页面换成深蓝底。

<table>
<tr>
<td align="center"><img src="play-night-card.png" width="240" alt="播放页 A"><br>播放页 A</td>
<td align="center"><img src="volume-night-card.png" width="240" alt="调音量"><br>调音量</td>
<td align="center"><img src="play-night-cat.png" width="240" alt="播放页 C"><br>播放页 C</td>
</tr>
<tr>
<td align="center"><img src="directory-night.png" width="240" alt="选择目录"><br>选择目录</td>
<td align="center"><img src="nocard-night.png" width="240" alt="没有 SD 卡"><br>没有 SD 卡</td>
<td align="center"><img src="ai-listening-night.png" width="240" alt="我在听"><br>我在听</td>
</tr>
<tr>
<td align="center"><img src="ai-returning-failed-night.png" width="240" alt="出错后回到故事"><br>出错后回到故事</td>
<td align="center"><img src="ai-subs-speaking-night.png" width="240" alt="字幕"><br>字幕</td>
</tr>
</table>
