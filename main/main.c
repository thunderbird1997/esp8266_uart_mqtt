#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LOCAL_LEVEL ESP_LOG_NONE
//#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#define MQTT_TASK 1

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "os.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "MQTTClient.h"

//*----- UART MQTT Firmware Ver 0.1 ----*/
//
// author: James Huang 
// copyright: James Huang 
// github: github.com/thunderbird1997
//
//*-------------------------------------*/

//*--------- UART MQTT 通信说明 ---------*/
//
// Ver 0.1
// 1. 由于ESP8266 RTOS SDK 串口底层的原因，无法发送字符串的结束符('\0'=>0x00),
//    而uart_mqtt部分命令需要字符串带有结束符,因此临时规定:当发送消息到esp8266时,
//    均需要发送字符串结束符。uart_mqtt返回的所有命令均不带字符串结束符。此问题计划
//    于下一版本修复。
//  
// 2. 目前只能实现一些基本功能，存在诸如QOS由于SDK中的MQTT库的原因目前只支持QOS0,不支
//    持TLS加密等问题，将会继续完善。
// 
// 3. 还没搞懂这个工程具体编译流程，本来应该将各函数分类置于不同文件中，目前先写在一
//    个main.c文件中。
//
//*-------------------------------------*/

#define DEBUG_TAG "UART_MQTT"

#define UART_NUM                 UART_NUM_0
#define UART_BAUD_RATE           4800
#define UART_RD_BUF_SIZE         2048

#define MQTT_VERSION             4
#define MQTT_CLIENT_ID           "ESP8266"
#define MQTT_PAYLOAD_BUFFER      2048
#define MQTT_KEEP_ALIVE          30
#define MQTT_SESSION             1

#define STATUS_LED_GPIO          2

#define MQTT_SUBSCRIBE_TOPIC_MAX 16

/* Mqtt 服务器参数 */
typedef struct MqttServerParam
{
    char server_ip[32];
    int server_port;
} mqtt_server_param_t;

/* wifi 参数 */
typedef struct WifiParam
{
    char ssid[32];
    char password[64];
} wifi_param_t;

/*-- WIFI 事件组 --*/

static EventGroupHandle_t wifi_event_group;
#define CONNECTED_BIT 1 /* 第1位用来标志wifi连接状态 */

/*---------------------------------*/

/* mqtt 客户端任务句柄 */
static TaskHandle_t mqtt_client_task_handle;

/* UART消息队列 */
static QueueHandle_t uart_queue;

/* uart_mqtt 输入解析队列 */
static QueueHandle_t input_parse_queue;

 /* uart_mqtt 输出序列化队列 */
static QueueHandle_t output_serialize_queue;

/* wifi 初始化队列 */
static QueueHandle_t wifi_queue;

/* mqtt 发布/订阅队列 */
static QueueHandle_t mqtt_publish_subscribe_queue;

/*---------- uart_mqtt 数据序列化与反序列化函数 ----------*/

/* 串口送入uart_mqtt话题最大长度 */
#define MSG_TOPIC_MAX_LEN            50  // 话题长度极限 < 256，默认50，此长度决定串口送入uart_mqtt话题的最大长度
#define MSG_CONTENT_MAX_LEN         500  // 消息内容长度极限 < 1000，默认500，此长度决定串口送入uart_mqtt消息内容的最大长度

#define MSG_TYPE_SUCCESS              1  // 操作成功消息: topic->"success" content->返回消息
#define MSG_TYPE_FAIL                 2  // 操作失败消息: topic->错误代码 content->返回消息
#define MSG_TYPE_READY                3  // 准备完成消息：topic->NULL content->NULL
#define MSG_TYPE_CONNECT_WIFI        10  // 配置WIFI: topic->ssid content->password
#define MSG_TYPE_CONNECT_MQTT_SERVER 11  // 连接MQTT服务器: topic->ip cotent->port
#define MSG_TYPE_PUBLISH             20  // 发布话题: topic->发布话题名 content->发布话题内容
#define MSG_TYPE_SUBSCRIBE           21  // 订阅话题: topic->订阅话题名 content->(NULL 或者 订阅话题回传数据)
#define MSG_TYPE_UNSUBSCRIBE         22  // 取消订阅话题: topic->订阅话题名 content->NULL

/* 错误代码 */
#define ERR_TYPE_SUCCESS   "success"
#define ERR_TYPE_UART      "uart_err"
#define ERR_TYPE_WIFI      "wifi_err"
#define ERR_TYPE_MQTT      "mqtt_err"

/* 返回消息字符串 */
#define ERR_MSG_CLIENT_STARTED             "cli started"
#define ERR_MSG_INIT                       "init err"
#define ERR_MSG_MEM                        "mem err"
#define ERR_MSG_SERVER_CONN                "serv err"
#define ERR_MSG_TASK                       "task err"
#define SUC_MSG_SERVER_CONN                "serv suc"
#define ERR_MSG_MQTT_PUBLISH               "pub err"
#define SUC_MSG_MQTT_PUBLISH               "pub suc"
#define ERR_MSG_MQTT_SUBSCRIBE_EXIST       "sub exist"
#define ERR_MSG_MQTT_SUBSCRIBE_MAX         "sub max"
#define ERR_MSG_MQTT_SUBSCRIBE             "sub err"
#define SUC_MSG_MQTT_SUBSCRIBE             "sub suc"
#define ERR_MSG_MQTT_UNSUBSCRIBE_NOT_EXIST "unsub exist"
#define ERR_MSG_MQTT_UNSUBSCRIBE           "unsub err"
#define SUC_MSG_MQTT_UNSUBSCRIBE           "unsub suc"
#define ERR_MSG_WIFI                       "wifi err"
#define SUC_MSG_WIFI                       "wifi suc"
#define ERR_MSG_UART_BAD_DATA              "bad data"
#define ERR_MSG_UART_NO_MSG_TYPE           "type err"


//*----------- uart_mqtt 串口指令数据结构 -------------*/
//
// 第1个字节：StartFrame(起始帧) = 0xFA
// 第2个字节：MessageType(消息类型)
// 第3个字节：TopicLength(话题字符串长度)，表明之后TopicLength个字节为话题字符串，最大为50
// 第4~(4+TopicLength-1)个字节：话题字符串
// 第(4+TopicLength)个字节：话题字符串校验
// 第(5+TopicLength)~(6+TopicLength)个字节：ContentLength(消息内容字符串长度)，表明之后ContentLength个字节为话题字符串，最大为1000(MSB在前)
// 第(7+TopicLength)~(7+TopicLength+ContentLength-1)个字节：消息内容字符串
// 第(7+TopicLength+ContentLength)个字节：消息内容字符串校验
// 第(8+TopicLength+ContentLength)个字节：EndFrame(结尾帧) = 0xFE
//
//*--------------------------------------------------*/

typedef struct UartMqttMessage
{
    int data_integrity_flag;
    unsigned char msg_type;
    unsigned char msg_topic_len;
    unsigned char msg_topic[MSG_TOPIC_MAX_LEN];
    unsigned int msg_content_len;
    unsigned char msg_content[MSG_CONTENT_MAX_LEN];
}uart_mqtt_msg_t;

/* 序列化 */
int uart_mqtt_serialize_data(unsigned char* buf,uart_mqtt_msg_t msg)
{
    int i;
    unsigned char checksum;

    /* 字符指针 */
    unsigned char *p = buf;

    /* 起始帧 */
    *(p++) = 0xFA;

    /* 消息类型 */
    *(p++) = msg.msg_type;

    /* 话题长度 */
    if(msg.msg_topic_len>MSG_TOPIC_MAX_LEN)return -1;
    *(p++) = msg.msg_topic_len;

    checksum = 0;

    /* 话题内容 */
    for(i=0;i<msg.msg_topic_len;i++)
    {
        *(p++) = msg.msg_topic[i];
        checksum += msg.msg_topic[i];
    }

    /* 计算话题字符串校验和 */
    checksum = ~ checksum;
    *(p++) = checksum;

    /* 消息内容长度 */
    if(msg.msg_content_len>MSG_CONTENT_MAX_LEN)return -1;
    *(p++) = (unsigned char)(((msg.msg_content_len) & 0xFF00) >> 8);
    *(p++) = (unsigned char)((msg.msg_content_len) & 0x00FF);

    checksum = 0;

    /* 消息内容 */
    for(i=0;i<msg.msg_content_len;i++)
    {
        *(p++) = msg.msg_content[i];
        checksum += msg.msg_content[i];
    }

    /* 计算话题字符串校验和 */
    checksum = ~ checksum;
    *(p++) = checksum;

    /* 结尾帧 */
    *p = 0xFE;

    return (8+msg.msg_topic_len+msg.msg_content_len);
}

/* 反序列化 */
uart_mqtt_msg_t uart_mqtt_deserialize_data(unsigned char* buf)
{
    unsigned char topic_len;
    unsigned int content_len;
    int i;
    unsigned char checksum;

    /* 消息对象 */
    uart_mqtt_msg_t msg;
    memset(&msg,0,sizeof(uart_mqtt_msg_t));

    /* 字符指针 */
    unsigned char* p = buf;

    if(*(p++) != 0xFA)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    /* 获取消息类型 */
    msg.msg_type = *(p++);

    /* 获取话题长度 */
    topic_len = *(p++);
    msg.msg_topic_len = topic_len;
    if(topic_len > MSG_TOPIC_MAX_LEN)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    checksum = 0;

    /* 获取话题 */
    for(i=0;i<topic_len;i++)
    {
        msg.msg_topic[i] = *(p++);
        checksum += msg.msg_topic[i];
    }

    /* 计算校验和 */
    checksum = ~checksum;
    if(checksum != *(p++))
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    /* 获取消息内容长度 */
    content_len = *(p++);
    content_len <<= 8 ;
    content_len += *(p++);
    msg.msg_content_len = content_len;
    if(content_len > MSG_CONTENT_MAX_LEN)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    checksum = 0;

    /* 获取消息内容 */
    for(i=0;i<content_len;i++)
    {
        msg.msg_content[i] = *(p++);
        checksum += msg.msg_content[i];
    }

    /* 计算校验和 */
    checksum = ~checksum;
    if(checksum != *(p++))
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    if(*p != 0xFE)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    msg.data_integrity_flag = 1;
    return msg;
}
/*------------------------------------------------------*/

/**
 *  MQTT 订阅话题列表
 *  
 *  因为原生mqtt client库函数保存的用于寻找回调函数的topic为指针，而程序中在传递
 *  topic时使用的变量为临时变量，这会导致订阅不同话题或者发布消息的时候覆盖该指针
 *  指向的topic内容，造成程序错误，因此建立一个话题列表用来保存话题名称。
 * 
 *  - MQTT_SUBSCRIBE_TOPIC_MAX 为最大订阅的话题数，可按需修改，默认值为16
 */
static char* mqtt_sub_topic_list[MQTT_SUBSCRIBE_TOPIC_MAX];

/* 在话题列表中添加对应的话题，并返回对应的指针 */
static int mqtt_sub_topic_add(int len, const char* topic)
{
    int i;
    
    /* 已经有该话题，返回错误 */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            return -1;
    }

    /* 寻找空位 */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] == NULL)
            break;
    }

    /* 话题列表已满，返回错误 */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return -2;

    /* 保存话题 */
    mqtt_sub_topic_list[i] = malloc(len);
    os_memcpy(mqtt_sub_topic_list[i], topic, len);
    
    /* 返回成功 */
    return 0;
}

/* 在话题列表中删除对应的话题 */
static int mqtt_sub_topic_delete(int len, const char* topic)
{
    int i;

    /* 寻找对应的话题 */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            break;
    }

    /* 没有该话题，返回错误 */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return -1;

    /* 删除话题 */
    free(mqtt_sub_topic_list[i]);
    mqtt_sub_topic_list[i] = NULL;
    
    /* 返回成功 */
    return 0;
}

/* 在话题列表中获取对应的话题指针 */
static char* mqtt_sub_topic_get(int len, const char* topic)
{
    int i;

    /* 寻找对应的话题 */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            break;
    }

    /* 没有该话题，返回空指针 */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return NULL;
    else return mqtt_sub_topic_list[i];

}


/* uart_mqtt 返回消息 */
static void uart_mqtt_return(int status, const char* errType, const char* reason)
{
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    msg.data_integrity_flag = 1;

    if(status != 0)
    {
        msg.msg_type = MSG_TYPE_SUCCESS;
    }
    else
    {
        msg.msg_type = MSG_TYPE_FAIL;
    }

    msg.msg_topic_len = strlen(errType);
    os_memcpy(msg.msg_topic, errType, msg.msg_topic_len);
    msg.msg_content_len = strlen(reason);
    os_memcpy(msg.msg_content, reason, msg.msg_content_len);

    xQueueSendToBack(output_serialize_queue,(void *)&msg,(portTickType)portMAX_DELAY);
}

/* uart_mqtt 序列化进程 */
static void uart_mqtt_serialize_task(void *pvParameters)
{
    uart_mqtt_msg_t msg;
    unsigned char buf[1024];

    for (;;) 
    {
        /* 等待数据 */
        if (xQueueReceive(output_serialize_queue, (void *)&msg, (portTickType)portMAX_DELAY)) 
        {
            gpio_set_level(STATUS_LED_GPIO,0); /* 点亮指示灯 */

            ESP_LOGI(DEBUG_TAG,"Output Message:\r\ntopic=%s,content=%s",msg.msg_topic,msg.msg_content);

            /* 序列化并发送 */
            int len = uart_mqtt_serialize_data(buf,msg);
            if(len > 0)
            {
                uart_write_bytes(UART_NUM, (char *)buf, len);
            }
            else
            {
                ESP_LOGI(DEBUG_TAG,"internal error: output data maxium reached");
            }
            
            gpio_set_level(STATUS_LED_GPIO,1); /* 熄灭指示灯 */
        }
    }

    ESP_LOGW(DEBUG_TAG, "uart_mqtt_serialize_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* mqtt 订阅消息回调 */
static void on_mqtt_sub_msg_received(MessageData *data)
{
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    ESP_LOGI(DEBUG_TAG, "receive message:\r\ntopic:%.*s\r\ncontent:%.*s",data->topicName->lenstring.len, \
        (char *)data->topicName->lenstring.data, \
        data->message->payloadlen, (char *)data->message->payload);
    
    /*----------------- uart_mqtt 返回消息 -----------------*/

    msg.data_integrity_flag = 1;
    msg.msg_type = MSG_TYPE_SUBSCRIBE;
    msg.msg_topic_len = data->topicName->lenstring.len; 
    os_memcpy(msg.msg_topic, (char *)data->topicName->lenstring.data, msg.msg_topic_len);
    msg.msg_content_len = data->message->payloadlen;
    os_memcpy(msg.msg_content, (char *)data->message->payload, msg.msg_content_len);

    xQueueSendToBack(output_serialize_queue,(void *)&msg,(portTickType)portMAX_DELAY);

    /*-----------------------------------------------------*/
    
}

/* mqtt 客户端进程 */
static void mqtt_client_task(void *pvParameters)
{
    char *payload = NULL;
    MQTTClient client;
    Network network;
    int rc = 0;
    char clientID[32] = {0};
    mqtt_server_param_t* mqtt_server_param = (mqtt_server_param_t*)pvParameters;
    uart_mqtt_msg_t msg;

    /* 初始化 mqtt 客户端连接信息 */
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

    /* 关联网络操作底层函数 */
    NetworkInit(&network);

    /* 初始化 mqtt 客户端 */
    if (MQTTClientInit(&client, &network, 0, NULL, 0, NULL, 0) == false) {
        ESP_LOGE(DEBUG_TAG, "mqtt init err");
        uart_mqtt_return(0, ERR_TYPE_MQTT, ERR_MSG_INIT);
        vTaskDelete(NULL);
    }

    /* 分配缓冲区空间 */
    payload = malloc(MQTT_PAYLOAD_BUFFER);

    /* 初始化缓冲区空间 */
    if (!payload) {
        ESP_LOGE(DEBUG_TAG, "mqtt malloc err");
        uart_mqtt_return(0, ERR_TYPE_MQTT, ERR_MSG_MEM);
    } else {
        memset(payload, 0x0, MQTT_PAYLOAD_BUFFER);
    }

    for (;;) {
        ESP_LOGI(DEBUG_TAG, "wait wifi connection...");
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, (portTickType)portMAX_DELAY);   /* 等待 wifi 连接建立 */

        /* 建立与服务器的连接 */
        if ((rc = NetworkConnect(&network, mqtt_server_param->server_ip, mqtt_server_param->server_port)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from network connect is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_SERVER_CONN);
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT Connecting");

        /*-------------------------- 配置连接信息 ------------------------*/
        connectData.MQTTVersion = MQTT_VERSION;

        sprintf(clientID, "%s_%u", MQTT_CLIENT_ID, esp_random());    /* 自动产生ID */

        connectData.clientID.cstring = clientID;
        connectData.keepAliveInterval = MQTT_KEEP_ALIVE;

        connectData.cleansession = MQTT_SESSION;
        /*---------------------------------------------------------------*/

        /* 客户端连接服务器 */
        if ((rc = MQTTConnect(&client, &connectData)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from MQTT connect is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_SERVER_CONN);
            network.disconnect(&network);
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT server connected");
        
    #if defined(MQTT_TASK)
        /* 启动 MQTT 后台进程 */
        if ((rc = MQTTStartTask(&client)) != pdPASS) {
            ESP_LOGE(DEBUG_TAG, "Return code from start tasks is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_TASK);
        } else {
            ESP_LOGI(DEBUG_TAG, "Use MQTTStartTask");
        }
    #endif

        uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_SERVER_CONN);
        
        /* 清空返回值 */
        rc = 0;

        for (;;) {

            /* 有新的发布/订阅事件 */
            if(xQueueReceive(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY))
            {
                switch(msg.msg_type)
                {
                    case MSG_TYPE_PUBLISH:  /* 发布事件 */
                    {
                        /* 创建消息 */
                        MQTTMessage message = {
                            .qos = 0,
                            .retained = 0,
                            .payload = msg.msg_content,
                            .payloadlen = msg.msg_content_len,
                        };

                        if ((rc = MQTTPublish(&client, (char *)msg.msg_topic, &message)) != 0) {
                            ESP_LOGE(DEBUG_TAG, "Return code from MQTT publish is %d", rc);
                            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_PUBLISH);
                        } else {
                            ESP_LOGI(DEBUG_TAG, "Publish OK!");
                            uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_MQTT_PUBLISH);
                        }
                    }
                    break;

                    case MSG_TYPE_SUBSCRIBE:    /* 订阅事件 */
                    {
                        char *tmp;

                        /* 保存话题名称到话题列表 */
                        rc = mqtt_sub_topic_add(msg.msg_topic_len,(char *)msg.msg_topic);
                        if(rc != 0)
                        {
                            if(rc == -1)    /* 已经订阅该话题 */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic already exist");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_SUBSCRIBE_EXIST);
                            }
                            else if(rc == -2)   /* 订阅话题数达到上限 */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic number reached max");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_SUBSCRIBE_MAX);
                            }
                            /* 非致命网络错误，重置错误标志 */
                            rc = 0;
                        }
                        else
                        {
                            /* 获取保存的话题名称 */
                            tmp = mqtt_sub_topic_get(msg.msg_topic_len,(char *)msg.msg_topic);

                            if(tmp != NULL)
                            {            
                                /* 订阅话题 */
                                if ((rc = MQTTSubscribe(&client, tmp, 0, on_mqtt_sub_msg_received)) != 0) 
                                {
                                    mqtt_sub_topic_delete(msg.msg_topic_len,(char *)msg.msg_topic); /* 删除话题列表中对应的话题 */
                                    ESP_LOGE(DEBUG_TAG, "Return code from MQTT subscribe is %d", rc);
                                    uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_SUBSCRIBE);
                                }
                                else
                                {
                                    ESP_LOGI(DEBUG_TAG, "Subscribe OK!");
                                    uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_MQTT_SUBSCRIBE);
                                }
                            }
                        }
                    }  
                    break; 

                    case MSG_TYPE_UNSUBSCRIBE:  /* 取消订阅事件 */
                    {
                        /* 删除话题列表中对应的话题 */
                        rc = mqtt_sub_topic_delete(msg.msg_topic_len,(char *)msg.msg_topic);
                        if(rc != 0)
                        {
                            if(rc == -1)    /* 不存在该话题 */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic doesn't exist");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_UNSUBSCRIBE_NOT_EXIST);
                            }
                            /* 非致命网络错误，重置错误标志 */
                            rc = 0;
                        }
                        else
                        {
                            /* 取消订阅话题 */
                            if ((rc = MQTTUnsubscribe(&client, (char *)msg.msg_topic)) != 0) 
                            {
                                ESP_LOGE(DEBUG_TAG, "Return code from MQTT unsubscribe is %d", rc);
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_UNSUBSCRIBE);
                            }
                            else
                            {
                                ESP_LOGI(DEBUG_TAG, "Unsubscribe OK!");
                                uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_MQTT_UNSUBSCRIBE);
                            }
                        }
                    }
                    break;

                    default:
                        break;
                }

                /* 返回值小于0为致命错误,中断循环 */
                if(rc < 0) 
                {

                    break;
                }
            }

        }

        network.disconnect(&network);
    }

    ESP_LOGW(DEBUG_TAG, "mqtt_client_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* ESP8266 系统事件回调函数 */
static esp_err_t event_handler(void *ctx, system_event_t *event)
{    
    switch(event->event_id)
    {
        case SYSTEM_EVENT_STA_START:    /* 启动事件 */
            esp_wifi_connect(); /* 连接WIFI */
            break;

        case SYSTEM_EVENT_STA_GOT_IP:   /* station模式获得IP事件 */
            xEventGroupSetBits(wifi_event_group,CONNECTED_BIT); /* 标志已连接 */

            ESP_LOGI(DEBUG_TAG, "wifi got ip");
            uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_WIFI);

            break;

        case SYSTEM_EVENT_STA_DISCONNECTED: /* station模式失去连接事件 */
            esp_wifi_connect(); /* 掉线重连 */
            xEventGroupClearBits(wifi_event_group,CONNECTED_BIT); /* 标志失去连接 */

            ESP_LOGE(DEBUG_TAG, "wifi lost connection");
            uart_mqtt_return(0,ERR_TYPE_WIFI,ERR_MSG_WIFI);

            break;

        default:
            break;
    }
    return ESP_OK;
}

/* ESP8266 串口事件进程 */
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *) malloc(UART_RD_BUF_SIZE);

    for (;;) 
    {
        /* 等待 UART 事件 */
        if (xQueueReceive(uart_queue, (void *)&event, (portTickType)portMAX_DELAY)) 
        {
            gpio_set_level(STATUS_LED_GPIO,0); /* 点亮指示灯 */

            bzero(dtmp, UART_RD_BUF_SIZE);   /* 缓存清空 */

            switch (event.type) 
            {
                /* UART数据事件 */
                case UART_DATA:
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    uart_flush(UART_NUM);
                    uart_mqtt_msg_t msg = uart_mqtt_deserialize_data(dtmp);   /* 反序列化 */
                    xQueueSendToBack(input_parse_queue, (void *)&msg, (portTickType)portMAX_DELAY); /* 送入队列 */
                    break;

                default:
                    break;
            }
            gpio_set_level(STATUS_LED_GPIO,1); /* 熄灭指示灯 */
        }
    }

    /* 释放内存 */
    free(dtmp);
    dtmp = NULL;

    ESP_LOGW(DEBUG_TAG, "uart_event_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* uart_mqtt 解析进程 */
static void uart_mqtt_parse_task(void *pvParameters)
{
    uart_mqtt_msg_t msg;
    mqtt_server_param_t server;
    wifi_param_t wifi;

    for (;;) 
    {
        /* 等待数据 */
        if (xQueueReceive(input_parse_queue, (void *)&msg, (portTickType)portMAX_DELAY)) 
        {
            /* 检查数据完整性标志 */
            if(msg.data_integrity_flag==0)
            {
                ESP_LOGW(DEBUG_TAG,"bad data");
                uart_mqtt_return(0,ERR_TYPE_UART,ERR_MSG_UART_BAD_DATA);
                continue;
            }

            switch(msg.msg_type)
            {
                case MSG_TYPE_CONNECT_WIFI: /* 连接WIFI */
                    os_memcpy(wifi.ssid,msg.msg_topic,msg.msg_topic_len);
                    os_memcpy(wifi.password,msg.msg_content,msg.msg_content_len);

                    ESP_LOGI(DEBUG_TAG,"Start Connect Wifi,ssid=%s,password=%s",wifi.ssid,wifi.password);

                    xQueueSendToBack(wifi_queue,(void *)&wifi,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_CONNECT_MQTT_SERVER:  /* 连接MQTT服务器 */
                    os_memcpy(server.server_ip,msg.msg_topic,msg.msg_topic_len);
                    server.server_port = atoi((char*)msg.msg_content);

                    ESP_LOGI(DEBUG_TAG,"Start Connect Server,ip=%s,port=%d",server.server_ip,server.server_port);

                    if(mqtt_client_task_handle == NULL)
                    {
                        if(xTaskCreate(mqtt_client_task,    /* 创建mqtt客户端任务 */
                            "mqtt_client_task",
                            4096, 
                            (void *)&server, 
                            8, 
                            &mqtt_client_task_handle
                            ) != pdPASS)
                        {
                            ESP_LOGE(DEBUG_TAG,"FAILED Creating mqtt client task!");
                            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MEM);
                        }   
                    }
                    else
                    {
                        ESP_LOGW(DEBUG_TAG,"mqtt_client_task already started");
                        uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_CLIENT_STARTED);
                    }
                    break;

                case MSG_TYPE_PUBLISH:  /* 发布消息 */
                    ESP_LOGI(DEBUG_TAG,"Will publish message,topic=%s,content=%s",msg.msg_topic,msg.msg_content);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_SUBSCRIBE:    /* 订阅消息 */
                    ESP_LOGI(DEBUG_TAG,"Will subscribe topic,topic=%s",msg.msg_topic);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_UNSUBSCRIBE:  /* 取消订阅消息 */
                    ESP_LOGI(DEBUG_TAG,"Will unsubscribe topic,topic=%s",msg.msg_topic);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                default:
                    ESP_LOGW(DEBUG_TAG,"no such message type");
                    uart_mqtt_return(0,ERR_TYPE_UART,ERR_MSG_UART_NO_MSG_TYPE);
                    break;
            }
        }
    }

    ESP_LOGW(DEBUG_TAG, "uart_mqtt_parse_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* 初始化 wifi */
static esp_err_t init_wifi(void)
{
    esp_err_t err;

    /* 初始化 TCP/IP 协议栈 */
    tcpip_adapter_init();  
    
    /* 初始化事件组 */
    wifi_event_group = xEventGroupCreate(); 

    /* 设置事件回调函数 */
    if((err = esp_event_loop_init(event_handler, NULL)) != ESP_OK) return err;
    
    /* 初始化硬件配置为默认值 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();    
    
    /* 初始化硬件 */
    if((err = esp_wifi_init(&cfg)) != ESP_OK) return err;   
    
    /* WIFI信息保存在RAM */
    if((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;   
    
    /* 配置为 station 模式 */
    if((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    
    /* 启动 WIFI */
    if((err = esp_wifi_start())!= ESP_OK) return err;     

    return ESP_OK;
}

/* wifi 连接进程 */
static void wifi_task(void *pvParameters)
{
    wifi_param_t wifi;
    int connect_flag = 0;

    for (;;) 
    {
        /* 等待 wifi 事件 */
        if (xQueueReceive(wifi_queue, (void *)&wifi, (portTickType)portMAX_DELAY)) 
        {
            if(connect_flag == 1)
            {
                /* 断开WIFI连接 */
                ESP_ERROR_CHECK(esp_wifi_disconnect());

                /* WIFI 连接设置 */
                wifi_config_t wifi_config; 
                os_memcpy(&wifi_config.sta.ssid,wifi.ssid,32);   /* WIFI ssid */
                os_memcpy(&wifi_config.sta.password,wifi.password,64);    /* WIFI 密码 */
                
                /* 连接 WIFI */    
                ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));  
                ESP_ERROR_CHECK(esp_wifi_connect());   
            }
            else
            {
                /* WIFI 连接设置 */
                wifi_config_t wifi_config; 
                os_memcpy(&wifi_config.sta.ssid,wifi.ssid,32);   /* WIFI ssid */
                os_memcpy(&wifi_config.sta.password,wifi.password,64);    /* WIFI 密码 */

                /* 设置 WIFI */    
                ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));     
                ESP_ERROR_CHECK(esp_wifi_connect());   

                connect_flag = 1;
            }
        }
    }

    ESP_LOGW(DEBUG_TAG, "wifi_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* 应用入口 */
void app_main()
{
    /*---------- 初始化状态LED ----------*/
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<STATUS_LED_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;

    gpio_config(&io_conf);
    gpio_set_level(STATUS_LED_GPIO,1);
    /*----------------------------------*/

    /*--------- 配置 uart0 参数 ---------*/
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM, &uart_config);

    uart_driver_install(UART_NUM, 2048, 2048, 100, &uart_queue);
    /*----------------------------------*/

    /*--- 初始化 wifi 需要的 nvs flash --*/
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    /*----------------------------------*/

    /* 启动 wifi */
    esp_err_t err = init_wifi();

    if(err != ESP_OK){
        ESP_LOGE(DEBUG_TAG, "init wifi failed");

        /* 快速闪烁STATUS LED报错 */ 
        int cnt = 0;
        while(1)
        {
            gpio_set_level(STATUS_LED_GPIO,(cnt++)%2);
            vTaskDelay(200 / portTICK_RATE_MS);
        }
    }
    
    /* 创建 uart 输入解析消息队列 */
    input_parse_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* 创建 uart 输出序列化消息队列 */
    output_serialize_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* 创建 wifi 连接消息队列 */
    wifi_queue = xQueueCreate(1,sizeof(wifi_param_t));

    /* 创建发布消息队列 */
    mqtt_publish_subscribe_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    
    /* 启动 wifi 连接进程 */
    xTaskCreate(wifi_task,"wifi_task", 2048, NULL, 8, NULL);

    /* 启动 uart 事件进程 */
    xTaskCreate(uart_event_task, "uart_task", 4096, NULL, 9, NULL);
    
    /* 启动 uart_mqtt 解析进程 */
    xTaskCreate(uart_mqtt_parse_task, "parse_task", 4096, NULL, 8, NULL);

    /* 启动 uart_mqtt 序列化进程 */
    xTaskCreate(uart_mqtt_serialize_task, "serialize_task", 4096, NULL, 8, NULL);

    /* 启动延时 500ms */
    vTaskDelay(500 / portTICK_RATE_MS);

    /* 发送准备完成消息 */
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    msg.data_integrity_flag = 1;
    msg.msg_type = MSG_TYPE_READY;

    xQueueSendToBack(output_serialize_queue,(void *)&msg,(portTickType)portMAX_DELAY);
}
