# 全地图未获取石头定位 (Full-map uncollected Oracle Stone locator)

纯插件实现：在大地图和小地图上定位当前存档尚未获取的全部石头。

## 功能

- 扫描并定位当前存档尚未获取的全部石头
- 在大地图和小地图上显示未获取石头的标记
- 插件窗口内显示石头 ID、所属地图、楼层与坐标
- 可在插件窗口内随时开关地图标记

## 安装

1. 进入游戏后按 `Insert` 打开 Anomaly。
2. 打开 **Plugins > 可用**。
3. 找到 **全地图未获取石头定位**，点击 **安装**。
4. 切到 **已安装**，选中插件并启用。

插件源地址：

```
https://raw.githubusercontent.com/PhysicalWorldDo/FullMapOracleStoneLocator/master/pluginmaster.json
```

## 从源码构建

需要已安装的 Anomaly SDK（提供 `AnomalySDKConfig.cmake`）：

```powershell
cmake -S . -B build -DAnomalySDK_DIR=<path-to-anomaly-sdk>
cmake --build build --config RelWithDebInfo
```

产物位于 `build/package/FullMapOracleStoneLocator/`。

## 许可

AGPL-3.0-only
