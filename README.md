# ClipNest

[![Build](https://github.com/xxzzzzy/ClipNest/actions/workflows/build.yml/badge.svg)](https://github.com/xxzzzzy/ClipNest/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/xxzzzzy/ClipNest)](https://github.com/xxzzzzy/ClipNest/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

ClipNest 是一个面向 Windows 10/11 的轻量原生文本剪切板管理器。它使用 Win32、Direct2D 和 DirectWrite，不包含浏览器内核、账号、联网同步或统计功能。

## 下载

普通用户请从 [GitHub Releases](https://github.com/xxzzzzy/ClipNest/releases/latest) 下载 `ClipNest-portable-x64.zip`，解压后直接运行 `ClipNest.exe`。

- 支持 Windows 10/11 x64
- 不需要管理员权限
- 便携运行，不需要安装程序
- 当前发布文件未进行商业代码签名，Windows SmartScreen 可能显示“未知发布者”；请从本仓库 Release 下载并核对 SHA-256

GitHub 自动生成的“Source code”压缩包只包含源码，不包含可直接运行的 EXE。

## 主要功能

- 默认使用 `Ctrl + Alt + V` 呼出快速选择窗口；面板优先出现在鼠标右侧，空间不足时自动切换到左侧
- 快速窗口采用“收藏 + 最近记录”的高密度布局，记录超过单列容量时自动扩展第三列
- `1-0`、`A-Z` 直接选择前 36 条活动记录
- 方向键选择，`Enter` 粘贴，`Ctrl + Enter` 只复制，`Esc` 关闭
- 管理器采用“收藏夹 + 活动列表 + 文本预览”的纵向卡片结构
- 收藏区和活动列表支持右键管理；收藏支持置顶、上移、下移和置底
- 管理器右上角的调节按钮会让设置面从右侧快速滑入，不另占一个顶层窗口
- 可修改呼出热键、自动粘贴、界面透明度、记录上限、剪贴板监听和启动行为
- 托盘右键仅保留“打开主界面”和“退出”
- 浅色、深色、高 DPI 和多显示器支持
- 相同文本自动去重并移动到活动列表顶部

## 隐私与本地数据

ClipNest 只监听并保存纯文本剪贴板内容，不记录图片和文件，不进行网络请求。

数据保存在：

```text
%LOCALAPPDATA%\ClipboardTool\clips.dat
```

该文件使用 Windows DPAPI 加密，通常只有创建数据的同一 Windows 用户能够解密。目录沿用早期内部名称 `ClipboardTool`，以兼容已有数据。

默认活动记录上限为 50 条、收藏上限为 100 条，可在设置面板调整为 10-500 条。退出程序不会自动删除历史记录。

## 更新与卸载

更新便携版：

1. 从托盘菜单退出 ClipNest。
2. 下载新版本 ZIP。
3. 用新的 `ClipNest.exe` 替换旧文件。

卸载：

1. 在设置面板关闭“开机自启”。
2. 从托盘菜单退出 ClipNest。
3. 删除 `ClipNest.exe` 和所在文件夹。
4. 如需同时删除历史与收藏，再删除 `%LOCALAPPDATA%\ClipboardTool`。
5. 如需彻底清除偏好设置，可删除注册表项 `HKCU\Software\ClipboardTool`。

## 从源码构建

需要 Visual Studio 2022 Build Tools，并安装“使用 C++ 的桌面开发”组件。项目使用 C++20 和静态运行库。

```powershell
.\build.ps1
```

Release 输出：

```text
x64\Release\ClipNest.exe
```

GitHub Actions 会在每次推送和拉取请求时执行同一 Release 构建。

## 校验下载文件

Release 同时提供 `SHA256SUMS.txt`。在 PowerShell 中执行：

```powershell
Get-FileHash .\ClipNest-portable-x64.zip -Algorithm SHA256
```

结果应与 `SHA256SUMS.txt` 中的值完全一致。

## 许可证

[MIT License](LICENSE)
