# ESP8266 uart_mqtt 固件

## 关于 ESP8266
esp8266 是由乐鑫推出的低功耗、高集成度的 Wi-Fi 芯片，内置WIFI前端和高性能的32位MCU，并且成本极低，被广泛用于物联网等需要 Wi-Fi 无线连接的应用场合。详见 [乐鑫官网介绍](https://www.espressif.com/zh-hans/products/hardware/esp8266ex/overview)。

## 关于 ESP8266 uart_mqtt
ESP8266 uart_mqtt基于乐鑫 [ESP8266_RTOS_SDK](https://github.com/espressif/ESP8266_RTOS_SDK) 开发的，适用于ESP8266 WIFI芯片的串口 MQTT 客户端固件。本固件目前在ESP-01S模块上进行开发与测试。首次接触RTOS，编程能力所限，不免存在问题，会在之后不断完善。

## 烧写固件至 ESP8266 模块（目前正在测试改进）
1. 前往[乐鑫官网的ESP8266资源页面](https://www.espressif.com/zh-hans/products/hardware/esp8266ex/resources)，找到“工具”下拉菜单，下载“Flash 下载工具（ESP8266 & ESP32）”即可下载烧写软件。
2. 打开烧写软件，选择ESP8266烧写。
3. 在工具里添加bin目录下的 uart_mqtt_vx.x.x_xxxbps.bin 文件(x.x.x为版本号，xxxbps为串口波特率)，烧写地址设置为0x0（此bin文件已经将bootloader以及分区表和固件程序打包在一起），选择串口并烧写。详细的添加bin文件以及烧写步骤请参照[如何为 ESP 系列模组烧录固件](http://wiki.ai-thinker.com/esp_download)。

## 说明
由于目前处在测试改进阶段，固件的部分功能还未完善，现在可以通过串口实现的操作有：

- 连接 WIFI
- 连接 MQTT 服务器 
- 发布消息到话题，仅支持QOS0
- 订阅话题，仅支持QOS0
- 取消订阅话题

## 使用

### 1. uart_mqtt 串口指令数据结构

- 第1个字节：StartFrame（起始帧），固定为 0xFA
  
- 第2个字节：MessageType(消息类型)，目前有以下几种消息类型：
- | 消息类型名 | 值 | 话题字符串内容 | 消息字符串内容 |
  | ------ | ------ | ------ | ------ |
  | 操作成功 | 1 | "success" | 返回消息 |
  | 操作失败 | 2 | 错误类型 | 返回消息 |
  | 准备完成 | 3 | NULL（空） | NULL（空） |
  | 连接WIFI | 10 | WiFi SSID | WiFi 密码 |
  | 连接MQTT服务器 | 11 | 服务器IP | MQTT 端口 | 
  | 发布消息 | 20 | 话题名 | 话题内容 | 
  | 订阅话题 | 21 | 话题名 | NULL（空）
  | 取消订阅话题 | 22 | 话题名 | NULL（空）

  备注：当接收到订阅的话题发来的消息时，将会使用“订阅话题”消息进行回传，此时“订阅话题”消息的消息字符串内容为接收到的消息内容。

- 第3个字节：TopicLength（话题字符串长度），表明之后TopicLength个字节为话题字符串，最大值为50

- 第4~(4+TopicLength-1)个字节：话题字符串

- 第(4+TopicLength)个字节：话题字符串校验

- 第(5+TopicLength)~(6+TopicLength)个字节：ContentLength（消息内容字符串长度），表明之后ContentLength个字节为话题字符串，最大值为500（MSB在前）

- 第(7+TopicLength)~(7+TopicLength+ContentLength-1)个字节：消息内容字符串

- 第(7+TopicLength+ContentLength)个字节：消息内容字符串校验

- 第(8+TopicLength+ContentLength)个字节：EndFrame(结尾帧)，固定为 0xFE

备注：校验方式为将各字符串的有效载荷（即字符串内容）按字节相加得到一个和字节，再将其取反，得到的结果即为校验字节内容。

### 2. 上电
ESP8266模块上电后或多或少都会有内核的输出，为了标识uart_mqtt的正式启动，uart_mqtt会在正常启动完成之后发送一个“准备完成”消息。在接收到这个消息之后，可以开始对uart_mqtt的操作。

    注意：如果模块启动过程中发生了硬件错误等异常，将无法接收到“准备完成”消息，同时连接的状态LED将以约200ms为间隔快速闪烁（LED默认连接到GPIO2）。此时必须重启模块或采取其他措施。

### 3. UART消息校验
uart_mqtt会对送入的消息进行有效性校验，如果消息不符合规范或者校验失败，将返回操作失败消息。返回值如下：

| 校验错误原因 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 校验失败 | 操作失败 | "uart_err" | "broken data" |
| 消息类型错误 | 操作失败 | "uart_err" | "no such message type" |

### 2. 连接WiFi
通过串口发送一个“连接WIFI”消息，可以让uart_mqtt使用话题字符串里指定的SSID和消息内容字符串里指定的密码连接到指定的WIFI。uart_mqtt内部拥有一个wifi进程，将会自动返回wifi状态，返回值如下：

| wifi状态 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 获得IP地址 | 操作成功 | "success" | "wifi got ip" |
| 连接丢失 | 操作失败 | "wifi_err" | "wifi lost connection" |

### 3. 连接MQTT服务器
通过串口发送一个“连接MQTT服务器”消息，可以让uart_mqtt使用话题字符串里指定的IP和消息内容字符串里指定的端口连接到指定的MQTT服务器。uart_mqtt内部将启动一个mqtt客户端进程，如果客户端进程监测到状态变化，将会自动返回客户端状态，返回值如下：

| 客户端状态 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 连接服务器成功 | 操作成功 | "success" | "mqtt connection success" |
| 客户端初始化失败 | 操作失败 | "mqtt_err" | "init error" |
| 客户端内存分配失败 | 操作失败 | "mqtt_err" | "malloc error" |
| 客户端后台进程启动失败 | 操作失败 | "mqtt_err" | "task start error" |
| 客户端失去网络连接 | 操作失败 | "mqtt_err" | "server connection error" |

### 4. 发布消息
通过串口发送一个“发布消息”消息，可以让uart_mqtt在指定话题上发布指定的消息。返回值如下：

| 结果 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 发布成功 | 操作成功 | "success" | "mqtt publish success" |
| 发布失败 | 操作失败 | "mqtt_err" | "mqtt publish fail" |

### 5. 订阅话题
通过串口发送一个“订阅话题”消息，可以让uart_mqtt订阅指定的话题。由于内存的限制，uart_mqtt最大支持订阅16个话题，基本可以满足大多数应用场景的需求。返回值如下：

| 结果 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 订阅成功 | 操作成功 | "success" | "mqtt subscribe success" |
| 订阅失败 | 操作失败 | "mqtt_err" | "mqtt subscribe fail" |
| 要订阅的话题已经存在 | 操作失败 | "mqtt_err" | "mqtt subscribe topic already exist" |
| 订阅话题数达到最大 | 操作失败 | "mqtt_err" | "mqtt subscribe topic number reached max" |

### 5. 取消订阅话题
通过串口发送一个“取消订阅话题”消息，可以让uart_mqtt取消订阅指定的话题。返回值如下：

| 结果 | 消息类型 | 话题字符串内容 | 消息字符串内容 |
| ----- | ----- | ----- | ----- | 
| 取消订阅成功 | 操作成功 | "success" | "mqtt unsubscribe success" |
| 取消订阅失败 | 操作失败 | "mqtt_err" | "mqtt unsubscribe fail" |
| 要取消订阅的话题不存在 | 操作失败 | "mqtt_err" | "mqtt subscribe topic doesn't exist" |