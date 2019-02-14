# ESP8266 uart_mqtt 固件

## 关于 ESP8266
esp8266 是由乐鑫推出的低功耗、高集成度的 Wi-Fi 芯片，内置WIFI前端和高性能的32位MCU，并且成本极低，被广泛用于物联网等需要 Wi-Fi 无线连接的应用场合。详见 [乐鑫官网介绍](https://www.espressif.com/zh-hans/products/hardware/esp8266ex/overview)。

## 关于 ESP8266 uart_mqtt
ESP8266 uart_mqtt基于乐鑫 [ESP8266_RTOS_SDK](https://github.com/espressif/ESP8266_RTOS_SDK) 开发的，适用于ESP8266 WIFI芯片的串口 MQTT 客户端固件。本固件目前在ESP-01S模块上进行开发与测试。首次接触RTOS，编程能力所限，不免存在问题，会在之后不断完善。

## 烧写固件至 ESP8266 模块（目前正在测试改进，无使用文档）
1. 前往[乐鑫官网的ESP8266资源页面](https://www.espressif.com/zh-hans/products/hardware/esp8266ex/resources)，找到“工具”下拉菜单，下载“Flash 下载工具（ESP8266 & ESP32）”即可下载烧写软件。
2. 打开烧写软件，选择ESP8266烧写。
3. 在工具里添加bin目录下的 uart_mqtt_vx.x.x.bin 文件(x.x.x为版本号)，烧写地址设置为0x0（此bin文件已经将bootloader以及分区表和固件程序打包在一起），选择串口并烧写。详细的添加bin文件以及烧写步骤请参照[如何为 ESP 系列模组烧录固件](http://wiki.ai-thinker.com/esp_download)。

## 说明
 由于目前处在测试改进阶段，固件的部分功能还未完善，文档编写暂时还无法进行。现在可以通过串口实现的操作指令有：

- 连接 WIFI
- 连接 MQTT 服务器 
- 发布消息到话题
- 订阅话题

