# 防休眠助手 PreventSleepMode

一个 Windows 小工具：定时模拟鼠标移动，防止系统在长时间无操作后自动休眠/熄屏。
纯 Win32 API 实现，单文件 exe，无任何运行库依赖。

## 功能

1. **定时移动鼠标**：每隔 N 秒（默认 120 秒 = 2 分钟，可调，范围 5–7200 秒）将鼠标轻移 1 像素（方向交替，肉眼几乎不可见），同时通过 `SetThreadExecutionState` 阻止系统休眠和熄屏。
2. **定时关闭**：设置运行时长（分钟），到时自动停止；填 0 表示不限制。运行时界面会显示"距自动停止"倒计时。
3. **暂停快捷键**：全局热键默认 **Ctrl+Alt+P**，随时暂停/继续（暂停期间允许系统正常休眠）。热键可在界面中自定义后点"应用"。

## 使用方法

- 双击 `PreventSleepMode.exe` → 点 **启动** 即可；状态栏显示倒计时。
- 修改间隔/时长后，重新点一次 **启动** 生效。
- 停止（或定时停止）后系统恢复正常的休眠策略。

## 构建方法

需要 Visual Studio（含 C++ 工作负载）：

```bat
build.bat
```

或手动：

```bat
rc /nologo /fo app.res resources\app.rc
cl /nologo /utf-8 /O2 /EHsc /DUNICODE /D_UNICODE src\main.cpp app.res /Fe:PreventSleepMode.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib
```

## 应用图标

`resources/app.ico` 由 [gen_icon.py](gen_icon.py) 生成（需要 Python + Pillow）：蓝色渐变底上的白色鼠标与移动线条，右上角"禁止月亮"角标表示拒绝休眠。包含 16/24/32/48/64/128/256 七个尺寸。修改设计后重新运行 `python gen_icon.py` 并再次构建即可。

## 备注

- 程序**不会**修改任何系统电源设置，退出后一切如常。
- 在锁屏界面或 UAC 安全桌面上，模拟输入不生效，但 `SetThreadExecutionState` 仍会保持系统唤醒（程序未暂停时）。
- 默认热键被占用时，界面底部会提示，换一个组合后点"应用"即可。
