#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LOCAL_LEVEL ESP_LOG_NONE
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
#include "MQTTClient.h"

/*----- UART MQTT Firmware Ver 0.1 beta -----*/
//
// author: James Huang 
// copyright: James Huang 
// github: github.com/thunderbird1997
//
/*-------------------------------------*/

/*--------- UART MQTT ͨ��˵�� ---------*/
//
// Ver 0.1 beta
// 1. ����ESP8266 RTOS SDK ���ڵײ��ԭ���޷������ַ����Ľ�����('\0'=>0x00),
//    ��uart_mqtt����������Ҫ�ַ������н�����,�����ʱ�涨:��������Ϣ��esp8266ʱ,
//    ����Ҫ�����ַ�����������uart_mqtt���ص���������������ַ�����������������ƻ�
//    ����һ�汾�޸���
//  
// 2. Ŀǰֻ��ʵ��һЩ�������ܣ���������QOS����SDK�е�MQTT���ԭ��Ŀǰֻ֧��QOS0��
//    ���Ҳ���ȡ������topic���ȵ����⣬����������ơ�
// 
// 3. ��û�㶮������̾���������̣�����Ӧ�ý��������������ڲ�ͬ�ļ��У�Ŀǰ��д��һ
//    ��main.c�ļ��С�
//
/*-------------------------------------*/

#define DEBUG_TAG "UART_MQTT"

#define UART_NUM                 UART_NUM_0
#define UART_BAUD_RATE           74880
#define UART_RD_BUF_SIZE         1024

#define MQTT_VERSION             3
#define MQTT_CLIENT_ID           "Client"
#define MQTT_USERNAME            "admin"
#define MQTT_PASSWORD            "123456"
#define MQTT_PAYLOAD_BUFFER      2048
#define MQTT_KEEP_ALIVE          30
#define MQTT_SESSION             1

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
#define MSG_TYPE_SUCCESS              1  // �ɹ���Ϣ: topic->0 content->������Ϣ
#define MSG_TYPE_FAIL                 2  // ʧ����Ϣ: topic->������� content->��������
#define MSG_TYPE_CONNECT_WIFI        10  // ����WIFI: topic->ssid content->password
#define MSG_TYPE_CONNECT_MQTT_SERVER 11  // ����MQTT������: topic->ip cotent->port 
#define MSG_TYPE_PUBLISH             15  // ��������: topic->���������� content->������������
#define MSG_TYPE_SUBSCRIBE           16  // ���Ļ���: topic->���Ļ����� content->(NULL ���� ���Ļ���ش�����)

#define ERR_TYPE_SUCCESS "success"
#define ERR_TYPE_UART    "uart_err"
#define ERR_TYPE_WIFI    "wifi_err"
#define ERR_TYPE_MQTT    "mqtt_err"

typedef struct UartMqttMessage
{
    int data_integrity_flag;
    unsigned char msg_type;
    unsigned char msg_topic_len;
    unsigned char msg_topic[256];
    unsigned char msg_content_len;
    unsigned char msg_content[256];
}uart_mqtt_msg_t;

/* ���л� */
int uart_mqtt_serialize_data(unsigned char* buf,uart_mqtt_msg_t msg)
{
    int i;
    int payload_len = 3 + msg.msg_topic_len + msg.msg_content_len;  /* ������У��λ�������ܳ��� */

    /* �ַ�ָ�� */
    unsigned char* p = buf;

    /* ��Ϣ���� */
    *p = msg.msg_type;
    p++;

    /* ���ⳤ�� */
    *p = msg.msg_topic_len;
    p++;

    /* �������� */
    for(i=0;i<msg.msg_topic_len;i++)
    {
        *p = msg.msg_topic[i];
        p++;
    }

    /* ��Ϣ���ݳ��� */
    *p = msg.msg_content_len;
    p++;

    /* ��Ϣ���� */
    for(i=0;i<msg.msg_content_len;i++)
    {
        *p = msg.msg_content[i];
        p++;
    }

    unsigned char checksum = 0;

    /* ����У��� */
    for(p=buf,i=0;i<payload_len;p++,i++)
    {
        checksum += *p;
    }
    checksum = ~ checksum;

    *p = checksum;

    return (payload_len + 1);
}

/* �����л� */
uart_mqtt_msg_t uart_mqtt_deserialize_data(unsigned char* buf)
{
    unsigned char topic_len=0,content_len=0;
    int i;

    /* ��Ϣ���� */
    uart_mqtt_msg_t msg;
    bzero(&msg,sizeof(uart_mqtt_msg_t));

    /* �ַ�ָ�� */
    unsigned char* p = buf;

    /* ��ȡ��Ϣ���� */
    msg.msg_type = *p;
    p++;

    /* ��ȡ���ⳤ�� */
    topic_len = *p;
    msg.msg_topic_len = topic_len;

    /* ��ȡ���� */
    for(i=0;i<topic_len;i++)
    {
        msg.msg_topic[i] = *(++p);
    }
    p++;

    /* ��ȡ��Ϣ���ݳ��� */
    content_len = *p;
    msg.msg_content_len = content_len;

    /* ��ȡ��Ϣ���� */
    for(i=0;i<content_len;i++)
    {
        msg.msg_content[i] = *(++p);
    }
    p++;

    /* ����У��� */
    int payload_len = 3 + topic_len + content_len;
    unsigned char checksum_send = *p;
    unsigned char checksum = 0;

    for(p=buf,i=0;i<payload_len;p++,i++)
    {
        checksum += *p;
    }
    checksum = ~ checksum;

    if (checksum == checksum_send) msg.data_integrity_flag = 1;
    else msg.data_integrity_flag = 0;

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
static char* mqtt_sub_topic_add(const char* topic)
{
    int i;
    
    /* Ѱ�ҿ�λ */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] == NULL)
            break;
    }

    /* �����б����������ؿ�ָ�� */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return NULL;

    /* ���滰�� */
    int len = strlen(topic);
    mqtt_sub_topic_list[i] = malloc(len);
    os_memcpy(mqtt_sub_topic_list[i], topic, len);
    
    /* ����ָ�� */
    return mqtt_sub_topic_list[i];
}

/* �ڻ����б���ɾ����Ӧ�Ļ��� */
static int mqtt_sub_topic_delete(const char* topic)
{
    int i;

    /* Ѱ�Ҷ�Ӧ�Ļ��� */
    for(i=0; i<MQTT_SUBSCRIBE_TOPIC_MAX; i++)
    {
        if(mqtt_sub_topic_list[i] != NULL && strcmp(mqtt_sub_topic_list[i],topic) == 0)
            break;
    }

    /* û�иû��⣬���� */
    if(i == MQTT_SUBSCRIBE_TOPIC_MAX) return -1;

    /* ɾ������ */
    free(mqtt_sub_topic_list[i]);
    mqtt_sub_topic_list[i] = NULL;
    
    return 0;
}

/* uart_mqtt ������Ϣ */
static void uart_mqtt_return(const char* errType, const char* reason)
{
    uart_mqtt_msg_t msg;

    bzero(&msg,sizeof(uart_mqtt_msg_t));

    msg.data_integrity_flag = 1;
    msg.msg_type = MSG_TYPE_FAIL;
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
            ESP_LOGI(DEBUG_TAG,"Output Message:\r\ntopic=%s,content=%s",msg.msg_topic,msg.msg_content);

            /* ���л������� */
            int len = uart_mqtt_serialize_data(buf,msg);
            uart_write_bytes(UART_NUM, (char *)buf, len);
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
        uart_mqtt_return(ERR_TYPE_MQTT,"init error");
        vTaskDelete(NULL);
    }

    /* ���仺�����ռ� */
    payload = malloc(MQTT_PAYLOAD_BUFFER);

    /* ��ʼ���������ռ� */
    if (!payload) {
        ESP_LOGE(DEBUG_TAG, "mqtt malloc err");
        uart_mqtt_return(ERR_TYPE_MQTT,"malloc error");
    } else {
        memset(payload, 0x0, MQTT_PAYLOAD_BUFFER);
    }

    for (;;) {
        ESP_LOGI(DEBUG_TAG, "wait wifi connect...");
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, (portTickType)portMAX_DELAY);   /* �ȴ� wifi ���ӽ��� */

        /* ����������������� */
        if ((rc = NetworkConnect(&network, mqtt_server_param->server_ip, mqtt_server_param->server_port)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from network connect is %d", rc);
            uart_mqtt_return(ERR_TYPE_MQTT,"server connection error");
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT Connecting");

        /*-------------------------- ����������Ϣ ------------------------*/
        connectData.MQTTVersion = MQTT_VERSION;

        sprintf(clientID, "%s_%u", MQTT_CLIENT_ID, esp_random());    /* �Զ�����ID */

        connectData.clientID.cstring = clientID;
        connectData.keepAliveInterval = MQTT_KEEP_ALIVE;

        connectData.username.cstring = MQTT_USERNAME;
        connectData.password.cstring = MQTT_PASSWORD;

        connectData.cleansession = MQTT_SESSION;
        /*---------------------------------------------------------------*/

        /* �ͻ������ӷ����� */
        if ((rc = MQTTConnect(&client, &connectData)) != 0) {
            ESP_LOGE(DEBUG_TAG, "Return code from MQTT connect is %d", rc);
            uart_mqtt_return(ERR_TYPE_MQTT,"server connection error");
            network.disconnect(&network);
            continue;
        }

        ESP_LOGI(DEBUG_TAG, "MQTT server connected");
        
    #if defined(MQTT_TASK)
        /* ���� MQTT ��̨���� */
        if ((rc = MQTTStartTask(&client)) != pdPASS) {
            ESP_LOGE(DEBUG_TAG, "Return code from start tasks is %d", rc);
            uart_mqtt_return(ERR_TYPE_MQTT,"task start error");
        } else {
            ESP_LOGI(DEBUG_TAG, "Use MQTTStartTask");
        }
    #endif

        uart_mqtt_return(ERR_TYPE_SUCCESS,"mqtt connection success");

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
                            uart_mqtt_return(ERR_TYPE_MQTT,"mqtt publish fail");
                        } else {
                            ESP_LOGI(DEBUG_TAG, "Publish OK!");
                            uart_mqtt_return(ERR_TYPE_SUCCESS,"mqtt publish success");
                        }
                    }
                    break;

                    case MSG_TYPE_SUBSCRIBE:    /* �����¼� */
                    {
                        char *tmp;

                        /* ���滰�����Ƶ������б� */
                        tmp = mqtt_sub_topic_add((char *)msg.msg_topic);
                        ESP_LOGI(DEBUG_TAG, "add to mqtt_sub_topic_list ok!\r\ncopied topic name is %s",tmp);

                        if(tmp == NULL)
                        {
                            ESP_LOGE(DEBUG_TAG, "mqtt subscribe topic number reached MAX");
                            uart_mqtt_return(ERR_TYPE_MQTT,"mqtt subscribe topic number reached MAX");
                        }
                        else
                        {              
                            /* ���Ļ��� */
                            if ((rc = MQTTSubscribe(&client, tmp, 0, on_mqtt_sub_msg_received)) != 0) 
                            {
                                ESP_LOGE(DEBUG_TAG, "Return code from MQTT subscribe is %d", rc);
                                uart_mqtt_return(ERR_TYPE_MQTT,"mqtt subscribe fail");
                            }
                            else
                            {
                                ESP_LOGI(DEBUG_TAG, "Subscribe OK!");
                                uart_mqtt_return(ERR_TYPE_SUCCESS,"mqtt subscribe success");
                            }
                        }
                    }  
                    break;  

                    default:
                        break;
                }

                /* ����ֵ��Ϊ0,�ж�ѭ�� */
                if(rc != 0) 
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
            uart_mqtt_return(ERR_TYPE_SUCCESS,"wifi got ip");

            break;

        case SYSTEM_EVENT_STA_DISCONNECTED: /* stationģʽʧȥ�����¼� */
            esp_wifi_connect(); /* �������� */
            xEventGroupClearBits(wifi_event_group,CONNECTED_BIT); /* ��־ʧȥ���� */

            ESP_LOGE(DEBUG_TAG, "wifi lost connection");
            uart_mqtt_return(ERR_TYPE_WIFI,"wifi lost connection");

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
                ESP_LOGW(DEBUG_TAG,"broken data");
                uart_mqtt_return(ERR_TYPE_UART,"broken data");
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

                    xTaskCreate(mqtt_client_task,"mqtt_client_task", 4096, (void *)&server, 8, NULL);   /* ����mqtt�ͻ������� */

                    break;

                case MSG_TYPE_PUBLISH:  /* ������Ϣ */
                    ESP_LOGI(DEBUG_TAG,"Will publish message,topic=%s,content=%s",msg.msg_topic,msg.msg_content);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                case MSG_TYPE_SUBSCRIBE:    /* ������Ϣ */
                    ESP_LOGI(DEBUG_TAG,"Will subscribe topic,topic=%s",msg.msg_topic);

                    xQueueSendToBack(mqtt_publish_subscribe_queue,(void *)&msg,(portTickType)portMAX_DELAY);

                    break;

                default:
                    ESP_LOGW(DEBUG_TAG,"no such message type");
                    uart_mqtt_return(ERR_TYPE_UART,"no such message type");
                    break;
            }
        }
    }

    ESP_LOGW(DEBUG_TAG, "uart_mqtt_parse_task going to be deleted");
    vTaskDelete(NULL);
    return;
}

/* ��ʼ�� wifi */
static void init_wifi(void)
{
    /* ��ʼ�� TCP/IP Э��ջ */
    tcpip_adapter_init();  
    
    /* ��ʼ���¼��� */
    wifi_event_group = xEventGroupCreate(); 

    /* �����¼��ص����� */
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL)); 
    
    /* ��ʼ��Ӳ������ΪĬ��ֵ */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();    
    
    /* ��ʼ��Ӳ�� */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));   
    
    /* WIFI��Ϣ������RAM */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    
    
    /* ����Ϊ station ģʽ */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); 
    
    /* ���� WIFI */
    ESP_ERROR_CHECK(esp_wifi_start()); 
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
    /*-----------------------------------*/

    /*--- ��ʼ�� wifi ��Ҫ�� nvs flash ---*/
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    /*-----------------------------------*/

    /* ���� wifi */
    init_wifi();

    /* ���� uart ���������Ϣ���� */
    input_parse_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* ���� uart ������л���Ϣ���� */
    output_serialize_queue = xQueueCreate(10,sizeof(uart_mqtt_msg_t));

    /* ���� wifi ������Ϣ���� */
    wifi_queue = xQueueCreate(2,sizeof(wifi_param_t));

    /* ����������Ϣ���� */
    mqtt_publish_subscribe_queue = xQueueCreate(20,sizeof(uart_mqtt_msg_t));

    
    /* ���� wifi ���ӽ��� */
    xTaskCreate(wifi_task,"wifi_task", 4096, NULL, 8, NULL);

    /* ���� uart �¼����� */
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 9, NULL);
    
    /* ���� uart_mqtt �������� */
    xTaskCreate(uart_mqtt_parse_task, "parse_task", 4096, NULL, 8, NULL);

    /* ���� uart_mqtt ���л����� */
    xTaskCreate(uart_mqtt_serialize_task, "serialize_task", 4096, NULL, 8, NULL);
}
