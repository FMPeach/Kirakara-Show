# Kirakara Show / Player — JSON 配置参考

Show 接受 `Kirakara Player Demo` 导出的 KRL / JSON 配置。下表列出当前默认值。

想从现成配置开始，可以复制 `config/kirakara-show-default.json`。KRL 文件自己的 `config`
区块可以覆盖其中的同名设置。

---

## 排版 / 字体

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `fontFamily` | string | `"'MotoyaLMaru W3 Mono', monospace"` | CSS font-family 语法，逗号分隔多个备选 |
| `fontSize` | number | `62` | 主文字号 (px) |
| `fontBold` | bool | `true` | 主字粗体 |
| `letterSpacing` | number | `12` | 主字字间距 (px) |

## 颜色 / 描边

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `colorBefore` | hex | `"#ffffff"` | 走字前的填充色 |
| `colorAfter` | hex | `"#0000a5"` | 走字后的填充色 |
| `strokeColorBefore` | hex | `"#000000"` | 走字前的描边色 |
| `strokeColorAfter` | hex | `"#ffffff"` | 走字后的描边色 |
| `strokeWidth` | number | `4` | 主字描边宽度 (px) |

## 注音 (ruby 上方假名)

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `rubySize` | number | `26` | 注音字号 (px) |
| `rubyBold` | bool | `true` | 注音粗体 |
| `rubyOffset` | number | `1` | 注音距主字上方的偏移 (px) |
| `rubyLetterSpacing` | number | `5` | 注音字间距 (px) |
| `rubyStrokeWidth` | number | `3` | 注音描边宽度 (px) |

## 注音2 (ruby 下方罗马字)

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `ruby2Size` | number | `20` | 注音2 字号 (px) |
| `ruby2Bold` | bool | `false` | 注音2 粗体 |
| `ruby2Offset` | number | `4` | 注音2 距主字下方的偏移 (px) |
| `ruby2LetterSpacing` | number | `4` | 注音2 字间距 (px) |
| `ruby2StrokeWidth` | number | `3` | 注音2 描边宽度 (px) |

## 注音避让

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `rubyIsolateEnabled` | bool | `false` | 注音超出主字宽度时自动撑宽，避免溢出 |

## 背景

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `bgColor` | hex | `"#095500"` | 纯色背景 |
| `bgImageOpacity` | number | `1.0` | 背景图片不透明度 (0-1) |

## 布局 (1280×720 逻辑画布)

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `line1X` | number | `128` | 第一行歌词 X 坐标 |
| `line1Y` | number | `450` | 第一行歌词 Y 坐标 |
| `line2Right` | number | `128` | 第二行歌词距右边界的距离 |
| `line2Y` | number | `566` | 第二行歌词 Y 坐标 |

## 淡入淡出

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `fadeEnabled` | bool | `true` | 启用淡入淡出 |
| `fadeParagraphOnly` | bool | `true` | `true`=仅段首/尾淡入淡出；`false`=每行都淡 |
| `fadeDurationMs` | number | `666` | 淡入/淡出持续时间 (毫秒) |

## 指示灯

| Key | 类型 | 默认值 | 说明 |
|-----|------|--------|------|
| `indicatorEnabled` | bool | `true` | 启用段落首行指示灯 |
| `indicatorDuration` | number | `3` | 指示灯持续秒数 |
| `indicatorFadeRatio` | number | `0` | 指示灯渐隐比例 (0=不平滑, 1=全程平滑) |
| `indicatorSize` | number | `34` | 指示灯圆点直径 (px) |
| `indicatorSpacing` | number | `12` | 圆点间距 (px) |
| `indicatorStrokeWidth` | number | `3` | 圆点描边宽度 (px) |
| `indicatorFillColor` | hex | `"#ffffff"` | 圆点填充色 |
| `indicatorStrokeColor` | hex | `"#000000"` | 圆点描边色 |
| `indicatorOffsetX` | number | `0` | 指示灯水平偏移 (px) |
| `indicatorOffsetY` | number | `8` | 指示灯垂直偏移 (px) |

## 角色配置 (characterProfiles)

每个角色独立配置，放在 `characterProfiles` 对象内。
key 为角色名（与歌词中 `【@角色】` 的角色名一致），value 为以下字段的对象。
**所有字段可选**，缺省时走全局设置 → 引擎调色板。

全局 `showRoleLabels` 默认是 `false`。角色颜色仍可生效；只有开启全局标签后，
各 profile 的 `showLabel` 才决定是否显示该角色的文字/图片标签。

```json
"characterProfiles": {
    "初音ミク": {
        // -- 颜色覆盖 (0 / 不填 = 使用全局色) --
        "colorBefore":        "#ffffff",
        "colorAfter":         "#00ffff",
        "strokeColorBefore":  "#000000",
        "strokeColorAfter":   "#ffffff",
        "strokeWidth":        4,

        // -- 标签显示 --
        "showLabel":          true,
        "imageMode":          true,
        "displayName":        "MIKU",
        "displayColor":       "#00a5c6",
        "labelStrokeColor":   "#000000",
        "labelScale":         100,
        "labelMarginLeft":    0,
        "labelMarginRight":   0,
        "imageOffsetY":       0,
        "image":              "/path/to/miku.png"
    }
}
```

| Key | 类型 | 默认 | 说明 |
|-----|------|------|------|
| `colorBefore` | hex | — | 该角色走字前填充色 |
| `colorAfter` | hex | — | 该角色走字后填充色 |
| `strokeColorBefore` | hex | — | 该角色走字前描边色 |
| `strokeColorAfter` | hex | — | 该角色走字后描边色 |
| `strokeWidth` | number | — | 该角色描边宽度 |
| `showLabel` | bool | `false` | 是否显示角色标签 (需全局 `showRoleLabels: true`) |
| `imageMode` | bool | `true` | `true`=显示图片；`false`=显示文字 |
| `displayName` | string | 角色名 | 标签文字 / 图片的 alt |
| `displayColor` | hex | — | 标签颜色 (text mode) |
| `labelStrokeColor` | hex | — | 标签描边色 |
| `labelScale` | number | `100` | 标签缩放百分比 |
| `labelMarginLeft` | number | `0` | 标签左边距 (px) |
| `labelMarginRight` | number | `0` | 标签右边距 (px) |
| `imageOffsetY` | number | `0` | 图片标签垂直偏移 (px) |
| `image` | string/object | — | 图片路径，或 KRL 内嵌 `{name,mime,base64}` 对象 (imageMode=true 时) |

## 完整示例

```json
{
    "fontFamily": "'MotoyaLMaru W3 Mono', monospace",
    "fontSize": 62,
    "fontBold": true,
    "letterSpacing": 12,
    "colorBefore": "#ffffff",
    "colorAfter": "#0000a5",
    "strokeColorBefore": "#000000",
    "strokeColorAfter": "#ffffff",
    "strokeWidth": 4,
    "rubySize": 26,
    "rubyBold": true,
    "rubyOffset": 1,
    "rubyLetterSpacing": 5,
    "rubyStrokeWidth": 3,
    "ruby2Size": 20,
    "ruby2Bold": false,
    "ruby2Offset": 4,
    "ruby2LetterSpacing": 4,
    "ruby2StrokeWidth": 3,
    "rubyIsolateEnabled": false,
    "bgColor": "#095500",
    "line1X": 128,
    "line1Y": 450,
    "line2Right": 128,
    "line2Y": 566,
    "fadeEnabled": true,
    "fadeParagraphOnly": true,
    "fadeDurationMs": 666,
    "indicatorEnabled": true,
    "indicatorDuration": 3,
    "indicatorFadeRatio": 0,
    "indicatorSize": 34,
    "indicatorSpacing": 12,
    "indicatorStrokeWidth": 3,
    "indicatorFillColor": "#ffffff",
    "indicatorStrokeColor": "#000000",
    "indicatorOffsetX": 0,
    "indicatorOffsetY": 8,
    "characterProfiles": {
        "初音ミク": {
            "colorAfter": "#00ffff",
            "displayName": "MIKU",
            "displayColor": "#00a5c6"
        },
        "鏡音リン": {
            "colorAfter": "#ffcc00",
            "displayName": "RIN",
            "displayColor": "#ffb300"
        }
    }
}
```
