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

//*--------- UART MQTT ͨ��˵�� ---------*/
//
// Ver 0.1
// 1. ����ESP8266 RTOS SDK ���ڵײ��ԭ���޷������ַ����Ľ�����('\0'=>0x00),
//    ��uart_mqtt����������Ҫ�ַ������н�����,�����ʱ�涨:��������Ϣ��esp8266ʱ,
//    ����Ҫ�����ַ�����������uart_mqtt���ص���������������ַ�����������������ƻ�
//    ����һ�汾�޸���
//  
// 2. Ŀǰֻ��ʵ��һЩ�������ܣ���������QOS����SDK�е�MQTT���ԭ��Ŀǰֻ֧��QOS0,��֧
//    ��TLS���ܵ����⣬����������ơ�
// 
// 3. ��û�㶮������̾���������̣�����Ӧ�ý��������������ڲ�ͬ�ļ��У�Ŀǰ��д��һ
//    ��main.c�ļ��С�
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

/* Mqtt ���������� */
typedef struct MqttServerParam
{
    char server_ip[32];
    int server_port;
} mqtt_server_param_t;

/* wifi ���� */
typedef struct WifiParam
{
    char ssid[32];
    char password[64];
} wifi_param_t;

/*-- WIFI �¼��� --*/

static EventGroupHandle_t wifi_event_group;
#define CONNECTED_BIT 1 /* ��1λ������־wifi����״̬ */

/*---------------------------------*/

/* mqtt �ͻ��������� */
static TaskHandle_t mqtt_client_task_handle;

/* UART��Ϣ���� */
static QueueHandle_t uart_queue;

/* uart_mqtt ����������� */
static QueueHandle_t input_parse_queue;

 /* uart_mqtt ������л����� */
static QueueHandle_t output_serialize_queue;

/* wifi ��ʼ������ */
static QueueHandle_t wifi_queue;

/* mqtt ����/���Ķ��� */
static QueueHandle_t mqtt_publish_subscribe_queue;

/*---------- uart_mqtt �������л��뷴���л����� ----------*/

/* ��������uart_mqtt������󳤶� */
#define MSG_TOPIC_MAX_LEN            50  // ���ⳤ�ȼ��� < 256��Ĭ��50���˳��Ⱦ�����������uart_mqtt�������󳤶�
#define MSG_CONTENT_MAX_LEN         500  // ��Ϣ���ݳ��ȼ��� < 1000��Ĭ��500���˳��Ⱦ�����������uart_mqtt��Ϣ���ݵ���󳤶�

#define MSG_TYPE_SUCCESS              1  // �����ɹ���Ϣ: topic->"success" content->������Ϣ
#define MSG_TYPE_FAIL                 2  // ����ʧ����Ϣ: topic->������� content->������Ϣ
#define MSG_TYPE_READY                3  // ׼�������Ϣ��topic->NULL content->NULL
#define MSG_TYPE_CONNECT_WIFI        10  // ����WIFI: topic->ssid content->password
#define MSG_TYPE_CONNECT_MQTT_SERVER 11  // ����MQTT������: topic->ip cotent->port
#define MSG_TYPE_PUBLISH             20  // ��������: topic->���������� content->������������
#define MSG_TYPE_SUBSCRIBE           21  // ���Ļ���: topic->���Ļ����� content->(NULL ���� ���Ļ���ش�����)
#define MSG_TYPE_UNSUBSCRIBE         22  // ȡ�����Ļ���: topic->���Ļ����� content->NULL

/* ������� */
#define ERR_TYPE_SUCCESS   "success"
#define ERR_TYPE_UART      "uart_err"
#define ERR_TYPE_WIFI      "wifi_err"
#define ERR_TYPE_MQTT      "mqtt_err"

/* ������Ϣ�ַ��� */
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


//*----------- uart_mqtt ����ָ�����ݽṹ -------------*/
//
// ��1���ֽڣ�StartFrame(��ʼ֡) = 0xFA
// ��2���ֽڣ�MessageType(��Ϣ����)
// ��3���ֽڣ�TopicLength(�����ַ�������)������֮��TopicLength���ֽ�Ϊ�����ַ��������Ϊ50
// ��4~(4+TopicLength-1)���ֽڣ������ַ���
// ��(4+TopicLength)���ֽڣ������ַ���У��
// ��(5+TopicLength)~(6+TopicLength)���ֽڣ�ContentLength(��Ϣ�����ַ�������)������֮��ContentLength���ֽ�Ϊ�����ַ��������Ϊ1000(MSB��ǰ)
// ��(7+TopicLength)~(7+TopicLength+ContentLength-1)���ֽڣ���Ϣ�����ַ���
// ��(7+TopicLength+ContentLength)���ֽڣ���Ϣ�����ַ���У��
// ��(8+TopicLength+ContentLength)���ֽڣ�EndFrame(��β֡) = 0xFE
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

/* ���л� */
int uart_mqtt_serialize_data(unsigned char* buf,uart_mqtt_msg_t msg)
{
    int i;
    unsigned char checksum;

    /* �ַ�ָ�� */
    unsigned char *p = buf;

    /* ��ʼ֡ */
    *(p++) = 0xFA;

    /* ��Ϣ���� */
    *(p++) = msg.msg_type;

    /* ���ⳤ�� */
    if(msg.msg_topic_len>MSG_TOPIC_MAX_LEN)return -1;
    *(p++) = msg.msg_topic_len;

    checksum = 0;

    /* �������� */
    for(i=0;i<msg.msg_topic_len;i++)
    {
        *(p++) = msg.msg_topic[i];
        checksum += msg.msg_topic[i];
    }

    /* ���㻰���ַ���У��� */
    checksum = ~ checksum;
    *(p++) = checksum;

    /* ��Ϣ���ݳ��� */
    if(msg.msg_content_len>MSG_CONTENT_MAX_LEN)return -1;
    *(p++) = (unsigned char)(((msg.msg_content_len) & 0xFF00) >> 8);
    *(p++) = (unsigned char)((msg.msg_content_len) & 0x00FF);

    checksum = 0;

    /* ��Ϣ���� */
    for(i=0;i<msg.msg_content_len;i++)
    {
        *(p++) = msg.msg_content[i];
        checksum += msg.msg_content[i];
    }

    /* ���㻰���ַ���У��� */
    checksum = ~ checksum;
    *(p++) = checksum;

    /* ��β֡ */
    *p = 0xFE;

    return (8+msg.msg_topic_len+msg.msg_content_len);
}

/* �����л� */
uart_mqtt_msg_t uart_mqtt_deserialize_data(unsigned char* buf)
{
    unsigned char topic_len;
    unsigned int content_len;
    int i;
    unsigned char checksum;

    /* ��Ϣ���� */
    uart_mqtt_msg_t msg;
    memset(&msg,0,sizeof(uart_mqtt_msg_t));

    /* �ַ�ָ�� */
    unsigned char* p = buf;

    if(*(p++) != 0xFA)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    /* ��ȡ��Ϣ���� */
    msg.msg_type = *(p++);

    /* ��ȡ���ⳤ�� */
    topic_len = *(p++);
    msg.msg_topic_len = topic_len;
    if(topic_len > MSG_TOPIC_MAX_LEN)
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    checksum = 0;

    /* ��ȡ���� */
    for(i=0;i<topic_len;i++)
    {
        msg.msg_topic[i] = *(p++);
        checksum += msg.msg_topic[i];
    }

    /* ����У��� */
    checksum = ~checksum;
    if(checksum != *(p++))
    {
        msg.data_integrity_flag = 0;
        return msg;
    }

    /* ��ȡ��Ϣ���ݳ��� */
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

    /* ��ȡ��Ϣ���� */
    for(i=0;i<content_len;i++)
    {
        msg.msg_content[i] = *(p++);
        checksum += msg.msg_content[i];
    }

    /* ����У��� */
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
 *  MQTT ���Ļ����б�
 *  
 *  ��Ϊԭ��mqtt client�⺯�����������Ѱ�һص�������topicΪָ�룬���������ڴ���
 *  topicʱʹ�õı���Ϊ��ʱ��������ᵼ�¶��Ĳ�ͬ������߷�����Ϣ��ʱ�򸲸Ǹ�ָ��
 *  ָ���topic���ݣ���ɳ��������˽���һ�������б��������滰�����ơ�
 * 
 *  - MQTT_SUBSCRIBE_TOPIC_MAX Ϊ����ĵĻ��������ɰ����޸ģ�Ĭ��ֵΪ16
 */
static char* mqtt_sub_topic_list[MQTT_SUBSCRIBE_TOPIC_MAX];

/* �ڻ����б�����Ӷ�Ӧ�Ļ��⣬�����ض�Ӧ��ָ�� */
static int mqtt_sub_topic_add(int len, const char* topic)
{
    int i;
    
    /* �Ѿ��иû��⣬���ش��� */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            return -1;
    }

    /* Ѱ�ҿ�λ */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] == NULL)
            break;
    }

    /* �����б����������ش��� */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return -2;

    /* ���滰�� */
    mqtt_sub_topic_list[i] = malloc(len);
    os_memcpy(mqtt_sub_topic_list[i], topic, len);
    
    /* ���سɹ� */
    return 0;
}

/* �ڻ����б���ɾ����Ӧ�Ļ��� */
static int mqtt_sub_topic_delete(int len, const char* topic)
{
    int i;

    /* Ѱ�Ҷ�Ӧ�Ļ��� */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            break;
    }

    /* û�иû��⣬���ش��� */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return -1;

    /* ɾ������ */
    free(mqtt_sub_topic_list[i]);
    mqtt_sub_topic_list[i] = NULL;
    
    /* ���سɹ� */
    return 0;
}

/* �ڻ����б��л�ȡ��Ӧ�Ļ���ָ�� */
static char* mqtt_sub_topic_get(int len, const char* topic)
{
    int i;

    /* Ѱ�Ҷ�Ӧ�Ļ��� */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strncmp(mqtt_sub_topic_list[i],topic,len) == 0)
            break;
    }

    /* û�иû��⣬���ؿ�ָ�� */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return NULL;
    else return mqtt_sub_topic_list[i];

}


/* uart_mqtt ������Ϣ */
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

/* uart_mqtt ���л����� */
static void uart_mqtt_serialize_task(void *pvParameters)
{
    uart_mqtt_msg_t msg;
    unsigned char buf[1024];

    for (;;) 
    {
        /* �ȴ����� */
        if (xQueueReceive(output_serialize_queue, (void *)&msg, (portTickType)portMAX_DELAY)) 
        {
            gpio_set_level(STATUS_LED_GPIO,0); /* ����ָʾ�� */

            ESP_LOGI(DEBUG_TAG,"Output Message:\r\ntopic=%s,content=%s",msg.msg_topic,msg.msg_content);

            /* ���л������� */
            int len = uart_mqtt_serialize_data(buf,msg);
            if(len > 0)
            {
                uart_write_bytes(UART_NUM, (char *)buf, len);
            }
            else
            {
                ESP_LOGI(DEBUG_TAG,"internal error: output data maxium reached");
            }
            
            gpio_set_level(STATUS_LED_GPIO,1); /* Ϩ��ָʾ�� */
        }
    }

    ESP_LOGW(DEBUG_TAG, "uart_mqtt_serialize_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* mqtt ������Ϣ�ص� */
static void on_mqtt_sub_msg_received(MessageData *data)
{
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    ESP_LOGI(DEBUG_TAG, "receive message:\r\ntopic:%.*s\r\ncontent:%.*s",data->topicName->lenstring.len, \
        (char *)data->topicName->lenstring.data, \
        data->message->payloadlen, (char *)data->message->payload);
    
    /*----------------- uart_mqtt ������Ϣ -----------------*/

    msg.data_integrity_flag = 1;
    msg.msg_type = MSG_TYPE_SUBSCRIBE;
    msg.msg_topic_len = data->topicName->lenstring.len; 
    os_memcpy(msg.msg_topic, (char *)data->topicName->lenstring.data, msg.msg_topic_len);
    msg.msg_content_len = data->message->payloadlen;
    os_memcpy(msg.msg_content, (char *)data->message->payload, msg.msg_content_len);

    xQueueSendToBack(output_serialize_queue,(void *)&msg,(portTickType)portMAX_DELAY);

    /*-----------------------------------------------------*/
    
}

/* mqtt �ͻ��˽��� */
static void mqtt_client_task(void *pvParameters)
{
    char *payload = NULL;
    MQTTClient client;
    Network network;
    int rc = 0;
    char clientID[32] = {0};
    mqtt_server_param_t* mqtt_server_param = (mqtt_server_param_t*)pvParameters;
    uart_mqtt_msg_t msg;

    /* ��ʼ�� mqtt �ͻ���������Ϣ */
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

    /* ������������ײ㺯�� */
    NetworkInit(&network);

    /* ��ʼ�� mqtt �ͻ��� */
    if (MQTTClientInit(&client, &network, 0, NULL, 0, NULL, 0) == false) {
        ESP_LOGE(DEBUG_TAG, "mqtt init err");
        uart_mqtt_return(0, ERR_TYPE_MQTT, ERR_MSG_INIT);
        vTaskDelete(NULL);
    }

    /* ���仺�����ռ� */
    payload = malloc(MQTT_PAYLOAD_BUFFER);

    /* ��ʼ���������ռ� */
    if (!payload) {
        ESP_LOGE(DEBUG_TAG, "mqtt malloc err");
        uart_mqtt_return(0, ERR_TYPE_MQTT, ERR_MSG_MEM);
    } else {
        memset(payload, 0x0, MQTT_PAYLOAD_BUFFER);
    }

    for (;;) {
        ESP_LOGI(DEBUG_TAG, "wait wifi connection...");
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, (portTickType)portMAX_DELAY);   /* �ȴ� wifi ���ӽ��� */

        /* ����������������� */
        if ((rc = NetworkConnect(&network, mqtt_server_param->server_ip, mqtt_server_param->server_port)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from network connect is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_SERVER_CONN);
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT Connecting");

        /*-------------------------- ����������Ϣ ------------------------*/
        connectData.MQTTVersion = MQTT_VERSION;

        sprintf(clientID, "%s_%u", MQTT_CLIENT_ID, esp_random());    /* �Զ�����ID */

        connectData.clientID.cstring = clientID;
        connectData.keepAliveInterval = MQTT_KEEP_ALIVE;

        connectData.cleansession = MQTT_SESSION;
        /*---------------------------------------------------------------*/

        /* �ͻ������ӷ����� */
        if ((rc = MQTTConnect(&client, &connectData)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from MQTT connect is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_SERVER_CONN);
            network.disconnect(&network);
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT server connected");
        
    #if defined(MQTT_TASK)
        /* ���� MQTT ��̨���� */
        if ((rc = MQTTStartTask(&client)) != pdPASS) {
            ESP_LOGE(DEBUG_TAG, "Return code from start tasks is %d", rc);
            uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_TASK);
        } else {
            ESP_LOGI(DEBUG_TAG, "Use MQTTStartTask");
        }
    #endif

        uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_SERVER_CONN);
        
        /* ��շ���ֵ */
        rc = 0;

        for (;;) {

            /* ���µķ���/�����¼� */
            if(xQueueReceive(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY))
            {
                switch(msg.msg_type)
                {
                    case MSG_TYPE_PUBLISH:  /* �����¼� */
                    {
                        /* ������Ϣ */
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

                    case MSG_TYPE_SUBSCRIBE:    /* �����¼� */
                    {
                        char *tmp;

                        /* ���滰�����Ƶ������б� */
                        rc = mqtt_sub_topic_add(msg.msg_topic_len,(char *)msg.msg_topic);
                        if(rc != 0)
                        {
                            if(rc == -1)    /* �Ѿ����ĸû��� */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic already exist");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_SUBSCRIBE_EXIST);
                            }
                            else if(rc == -2)   /* ���Ļ������ﵽ���� */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic number reached max");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_SUBSCRIBE_MAX);
                            }
                            /* ����������������ô����־ */
                            rc = 0;
                        }
                        else
                        {
                            /* ��ȡ����Ļ������� */
                            tmp = mqtt_sub_topic_get(msg.msg_topic_len,(char *)msg.msg_topic);

                            if(tmp != NULL)
                            {            
                                /* ���Ļ��� */
                                if ((rc = MQTTSubscribe(&client, tmp, 0, on_mqtt_sub_msg_received)) != 0) 
                                {
                                    mqtt_sub_topic_delete(msg.msg_topic_len,(char *)msg.msg_topic); /* ɾ�������б��ж�Ӧ�Ļ��� */
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

                    case MSG_TYPE_UNSUBSCRIBE:  /* ȡ�������¼� */
                    {
                        /* ɾ�������б��ж�Ӧ�Ļ��� */
                        rc = mqtt_sub_topic_delete(msg.msg_topic_len,(char *)msg.msg_topic);
                        if(rc != 0)
                        {
                            if(rc == -1)    /* �����ڸû��� */
                            {
                                ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic doesn't exist");
                                uart_mqtt_return(0,ERR_TYPE_MQTT,ERR_MSG_MQTT_UNSUBSCRIBE_NOT_EXIST);
                            }
                            /* ����������������ô����־ */
                            rc = 0;
                        }
                        else
                        {
                            /* ȡ�����Ļ��� */
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

                /* ����ֵС��0Ϊ��������,�ж�ѭ�� */
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

/* ESP8266 ϵͳ�¼��ص����� */
static esp_err_t event_handler(void *ctx, system_event_t *event)
{    
    switch(event->event_id)
    {
        case SYSTEM_EVENT_STA_START:    /* �����¼� */
            esp_wifi_connect(); /* ����WIFI */
            break;

        case SYSTEM_EVENT_STA_GOT_IP:   /* stationģʽ���IP�¼� */
            xEventGroupSetBits(wifi_event_group,CONNECTED_BIT); /* ��־������ */

            ESP_LOGI(DEBUG_TAG, "wifi got ip");
            uart_mqtt_return(1,ERR_TYPE_SUCCESS,SUC_MSG_WIFI);

            break;

        case SYSTEM_EVENT_STA_DISCONNECTED: /* stationģʽʧȥ�����¼� */
            esp_wifi_connect(); /* �������� */
            xEventGroupClearBits(wifi_event_group,CONNECTED_BIT); /* ��־ʧȥ���� */

            ESP_LOGE(DEBUG_TAG, "wifi lost connection");
            uart_mqtt_return(0,ERR_TYPE_WIFI,ERR_MSG_WIFI);

            break;

        default:
            break;
    }
    return ESP_OK;
}

/* ESP8266 �����¼����� */
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *) malloc(UART_RD_BUF_SIZE);

    for (;;) 
    {
        /* �ȴ� UART �¼� */
        if (xQueueReceive(uart_queue, (void *)&event, (portTickType)portMAX_DELAY)) 
        {
            gpio_set_level(STATUS_LED_GPIO,0); /* ����ָʾ�� */

            bzero(dtmp, UART_RD_BUF_SIZE);   /* ������� */

            switch (event.type) 
            {
                /* UART�����¼� */
                case UART_DATA:
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    uart_flush(UART_NUM);
                    uart_mqtt_msg_t msg = uart_mqtt_deserialize_data(dtmp);   /* �����л� */
                    xQueueSendToBack(input_parse_queue, (void *)&msg, (portTickType)portMAX_DELAY); /* ������� */
                    break;

                default:
                    break;
            }
            gpio_set_level(STATUS_LED_GPIO,1); /* Ϩ��ָʾ�� */
        }
    }

    /* �ͷ��ڴ� */
    free(dtmp);
    dtmp = NULL;

    ESP_LOGW(DEBUG_TAG, "uart_event_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* uart_mqtt �������� */
static void uart_mqtt_parse_task(void *pvParameters)
{
    uart_mqtt_msg_t msg;
    mqtt_server_param_t server;
    wifi_param_t wifi;

    for (;;) 
    {
        /* �ȴ����� */
        if (xQueueReceive(input_parse_queue, (void *)&msg, (portTickType)portMAX_DELAY)) 
        {
            /* ������������Ա�־ */
            if(msg.data_integrity_flag==0)
            {
                ESP_LOGW(DEBUG_TAG,"bad data");
                uart_mqtt_return(0,ERR_TYPE_UART,ERR_MSG_UART_BAD_DATA);
                continue;
            }

            switch(msg.msg_type)
            {
                case MSG_TYPE_CONNECT_WIFI: /* ����WIFI */
                    os_memcpy(wifi.ssid,msg.msg_topic,msg.msg_topic_len);
                    os_memcpy(wifi.password,msg.msg_content,msg.msg_content_len);

                    ESP_LOGI(DEBUG_TAG,"Start Connect Wifi,ssid=%s,password=%s",wifi.ssid,wifi.password);

                    xQueueSendToBack(wifi_queue,(void *)&wifi,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_CONNECT_MQTT_SERVER:  /* ����MQTT������ */
                    os_memcpy(server.server_ip,msg.msg_topic,msg.msg_topic_len);
                    server.server_port = atoi((char*)msg.msg_content);

                    ESP_LOGI(DEBUG_TAG,"Start Connect Server,ip=%s,port=%d",server.server_ip,server.server_port);

                    if(mqtt_client_task_handle == NULL)
                    {
                        if(xTaskCreate(mqtt_client_task,    /* ����mqtt�ͻ������� */
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

                case MSG_TYPE_PUBLISH:  /* ������Ϣ */
                    ESP_LOGI(DEBUG_TAG,"Will publish message,topic=%s,content=%s",msg.msg_topic,msg.msg_content);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_SUBSCRIBE:    /* ������Ϣ */
                    ESP_LOGI(DEBUG_TAG,"Will subscribe topic,topic=%s",msg.msg_topic);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_UNSUBSCRIBE:  /* ȡ��������Ϣ */
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

/* ��ʼ�� wifi */
static esp_err_t init_wifi(void)
{
    esp_err_t err;

    /* ��ʼ�� TCP/IP Э��ջ */
    tcpip_adapter_init();  
    
    /* ��ʼ���¼��� */
    wifi_event_group = xEventGroupCreate(); 

    /* �����¼��ص����� */
    if((err = esp_event_loop_init(event_handler, NULL)) != ESP_OK) return err;
    
    /* ��ʼ��Ӳ������ΪĬ��ֵ */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();    
    
    /* ��ʼ��Ӳ�� */
    if((err = esp_wifi_init(&cfg)) != ESP_OK) return err;   
    
    /* WIFI��Ϣ������RAM */
    if((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;   
    
    /* ����Ϊ station ģʽ */
    if((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    
    /* ���� WIFI */
    if((err = esp_wifi_start())!= ESP_OK) return err;     

    return ESP_OK;
}

/* wifi ���ӽ��� */
static void wifi_task(void *pvParameters)
{
    wifi_param_t wifi;
    int connect_flag = 0;

    for (;;) 
    {
        /* �ȴ� wifi �¼� */
        if (xQueueReceive(wifi_queue, (void *)&wifi, (portTickType)portMAX_DELAY)) 
        {
            if(connect_flag == 1)
            {
                /* �Ͽ�WIFI���� */
                ESP_ERROR_CHECK(esp_wifi_disconnect());

                /* WIFI �������� */
                wifi_config_t wifi_config; 
                os_memcpy(&wifi_config.sta.ssid,wifi.ssid,32);   /* WIFI ssid */
                os_memcpy(&wifi_config.sta.password,wifi.password,64);    /* WIFI ���� */
                
                /* ���� WIFI */    
                ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));  
                ESP_ERROR_CHECK(esp_wifi_connect());   
            }
            else
            {
                /* WIFI �������� */
                wifi_config_t wifi_config; 
                os_memcpy(&wifi_config.sta.ssid,wifi.ssid,32);   /* WIFI ssid */
                os_memcpy(&wifi_config.sta.password,wifi.password,64);    /* WIFI ���� */

                /* ���� WIFI */    
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

/* Ӧ����� */
void app_main()
{
    /*---------- ��ʼ��״̬LED ----------*/
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<STATUS_LED_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;

    gpio_config(&io_conf);
    gpio_set_level(STATUS_LED_GPIO,1);
    /*----------------------------------*/

    /*--------- ���� uart0 ���� ---------*/
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

    /*--- ��ʼ�� wifi ��Ҫ�� nvs flash --*/
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    /*----------------------------------*/

    /* ���� wifi */
    esp_err_t err = init_wifi();

    if(err != ESP_OK){
        ESP_LOGE(DEBUG_TAG, "init wifi failed");

        /* ������˸STATUS LED���� */ 
        int cnt = 0;
        while(1)
        {
            gpio_set_level(STATUS_LED_GPIO,(cnt++)%2);
            vTaskDelay(200 / portTICK_RATE_MS);
        }
    }
    
    /* ���� uart ���������Ϣ���� */
    input_parse_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* ���� uart ������л���Ϣ���� */
    output_serialize_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* ���� wifi ������Ϣ���� */
    wifi_queue = xQueueCreate(1,sizeof(wifi_param_t));

    /* ����������Ϣ���� */
    mqtt_publish_subscribe_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    
    /* ���� wifi ���ӽ��� */
    xTaskCreate(wifi_task,"wifi_task", 2048, NULL, 8, NULL);

    /* ���� uart �¼����� */
    xTaskCreate(uart_event_task, "uart_task", 4096, NULL, 9, NULL);
    
    /* ���� uart_mqtt �������� */
    xTaskCreate(uart_mqtt_parse_task, "parse_task", 4096, NULL, 8, NULL);

    /* ���� uart_mqtt ���л����� */
    xTaskCreate(uart_mqtt_serialize_task, "serialize_task", 4096, NULL, 8, NULL);

    /* ������ʱ 500ms */
    vTaskDelay(500 / portTICK_RATE_MS);

    /* ����׼�������Ϣ */
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    msg.data_integrity_flag = 1;
    msg.msg_type = MSG_TYPE_READY;

    xQueueSendToBack(output_serialize_queue,(void *)&msg,(portTickType)portMAX_DELAY);
}
