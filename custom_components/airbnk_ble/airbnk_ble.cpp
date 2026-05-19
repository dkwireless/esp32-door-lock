/*
 * airbnk_ble.cpp — Airbnk BLE-MQTT bridge (ESP-IDF, no entity headers)
 *
 * Scans BLE, parses BABA ads, publishes JSON to MQTT, subscribes to
 * MQTT command topic. All entity state is in JSON — no esphome::sensor etc.
 */
#include "airbnk_ble.h"
#include "esphome/core/log.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "airbnk_crypto.h"
#include "airbnk_lock.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <cstring>
#include <cstdio>

namespace esphome { namespace airbnk_ble {

static const char *TAG = "airbnk_ble";
static AirbnkBleComponent *g_inst = nullptr;
static const uint32_t AD_TIMEOUT_MS = 10000;

extern "C" {
static void adv_bridge(const airbnk_advert_data_t *a) {
    if (g_inst && a) g_inst->on_advertisement(*a);
}
}

/* ── Base64 ────────────────────────────────────────────────────────────── */

bool AirbnkBleComponent::b64decode_(const std::string &s, uint8_t *o, size_t n){
    static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0; uint32_t b=0; int bits=0;
    for(char c : s){
        if(c=='=') break;
        const char *p=strchr(t,c); if(!p) return false;
        b=(b<<6)|(uint32_t)(p-t); bits+=6;
        if(bits>=8){ bits-=8; if(j<n) o[j++]=(uint8_t)(b>>bits); }
    }
    return j==n;
}

uint8_t AirbnkBleComponent::batpct_(float v, float a,float b,float c){
    if(v<=a) return 0; if(v>=c) return 100;
    if(v>=b) return (uint8_t)(66.6f+33.3f*(v-b)/(c-b));
    return (uint8_t)(33.3f+33.3f*(v-a)/(b-a));
}

/* ── Setters ───────────────────────────────────────────────────────────── */

void AirbnkBleComponent::set_mac_address(const std::string &s){
    int v[6];
    if(sscanf(s.c_str(),"%02X:%02X:%02X:%02X:%02X:%02X",
              &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6)
        for(int i=0;i<6;i++) mac_[i]=(uint8_t)v[i];
}
void AirbnkBleComponent::set_manufacturer_key(const std::string &b64){
    if(!b64decode_(b64,mfkey_,16)) ESP_LOGE(TAG,"Bad mfkey");
}
void AirbnkBleComponent::set_binding_key(const std::string &b64){
    if(!b64decode_(b64,bdkey_,20)) ESP_LOGE(TAG,"Bad bdkey");
}
void AirbnkBleComponent::set_device_id(const std::string &s){
    strncpy(devid_,s.c_str(),sizeof(devid_)-1);
    devid_[sizeof(devid_)-1]='\0';
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void AirbnkBleComponent::setup(){
    ESP_LOGI(TAG,"Setup dev=%s",devid_);
    g_inst=this; busy_=false;
    memset(&last_,0,sizeof(last_)); last_.state=AIRBNK_STATE_UNKNOWN;

    keys_ok_=false;
    for(int i=0;i<16;i++) if(mfkey_[i]){keys_ok_=true;break;}
    if(!keys_ok_) ESP_LOGW(TAG,"Keys missing");

    if(airbnk_nimble_init()!=0){ESP_LOGE(TAG,"NimBLE fail");return;}
    if(airbnk_nimble_start_scan(adv_bridge)!=0) ESP_LOGE(TAG,"Scan fail");

    auto *mq=mqtt::global_mqtt_client;
    if(mq && mq->is_connected()){
        std::string t = std::string("airbnk/")+devid_+"/availability";
        mq->publish(t,"online",0,true);
    }
    if(mq){
        std::string ct = std::string("airbnk/")+devid_+"/command";
        mq->subscribe(ct,[this](const std::string &,const std::string &p){
            cJSON *r=cJSON_Parse(p.c_str()); if(!r) return;
            cJSON *a=cJSON_GetObjectItem(r,"action");
            if(a && cJSON_IsString(a)){
                if(!strcmp(a->valuestring,"unlock")) trigger_unlock();
                else if(!strcmp(a->valuestring,"lock")) trigger_lock();
            }
            cJSON_Delete(r);
        },0);
    }
    ESP_LOGI(TAG,"Ready");
}

void AirbnkBleComponent::loop(){}
void AirbnkBleComponent::dump_config(){
    ESP_LOGCONFIG(TAG,"Airbnk BLE-MQTT dev=%s",devid_);
}

/* ── Commands ──────────────────────────────────────────────────────────── */

void AirbnkBleComponent::trigger_unlock(){ send_cmd_(1,last_); }
void AirbnkBleComponent::trigger_lock()  { send_cmd_(2,last_); }

/* ── Advert ────────────────────────────────────────────────────────────── */

void AirbnkBleComponent::on_advertisement(const airbnk_advert_data_t &a){
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    memcpy(&last_,&a,sizeof(last_)); last_ms_=now;

    if(busy_ && a.lock_events > cmd_events_){
        ESP_LOGI(TAG,"Cmd verified: %lu→%lu",
                 (unsigned long)cmd_events_,(unsigned long)a.lock_events);
        busy_=false; cmd_done_(true);
    }
    pub_state_(a);
}

/* ── MQTT publish ──────────────────────────────────────────────────────── */

void AirbnkBleComponent::pub_state_(const airbnk_advert_data_t &a){
    auto *mq=mqtt::global_mqtt_client;
    if(!mq || !mq->is_connected()) return;

    uint8_t bp=batpct_(a.voltage,v1_,v2_,v3_);

    cJSON *r=cJSON_CreateObject();
    cJSON_AddStringToObject(r,"state",airbnk_state_to_string(a.state));
    cJSON_AddNumberToObject(r,"voltage",(double)a.voltage);
    cJSON_AddNumberToObject(r,"battery_pct",bp);
    cJSON_AddNumberToObject(r,"lock_events",(double)a.lock_events);
    cJSON_AddNumberToObject(r,"rssi",a.rssi);
    cJSON_AddBoolToObject(r,"low_battery",a.is_low_battery||bp<20);
    cJSON_AddBoolToObject(r,"is_init",a.is_init);
    cJSON_AddBoolToObject(r,"auto_lock",a.is_enable_auto);

    char *js=cJSON_PrintUnformatted(r);
    if(js){
        std::string t=std::string("airbnk/")+devid_+"/state";
        mq->publish(t,std::string(js),0,false);
        free(js);
    }
    cJSON_Delete(r);
}

/* ── Send command ──────────────────────────────────────────────────────── */

bool AirbnkBleComponent::send_cmd_(uint8_t dir,const airbnk_advert_data_t &a){
    if(busy_) return false;
    if(!keys_ok_) return false;

    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    if(last_ms_==0||(now-last_ms_)>AD_TIMEOUT_MS){
        ESP_LOGW(TAG,"No recent advert"); return false;
    }

    uint32_t ts=(uint32_t)(esp_timer_get_time()/1000000);
    uint8_t op[36];
    airbnk_make_package_v3(dir,ts,a.lock_events,mfkey_,bdkey_,op);

    airbnk_cmd_t c; memset(&c,0,sizeof(c));
    memcpy(c.mac_address,mac_,6); memcpy(c.opcode,op,36);
    c.on_done=[](bool ok){ if(g_inst) g_inst->cmd_done_(ok); };

    if(airbnk_lock_send_command(&c)!=0) return false;

    busy_=true; cmd_events_=a.lock_events;
    ESP_LOGI(TAG,"Cmd dir=%d",(int)dir);
    return true;
}

void AirbnkBleComponent::cmd_done_(bool ok){
    busy_=false;
    auto *mq=mqtt::global_mqtt_client;
    if(mq && mq->is_connected()){
        std::string t=std::string("airbnk/")+devid_+"/command_result";
        mq->publish(t,std::string(ok?"{\"result\":\"OK\"}":"{\"result\":\"FAIL\"}"),0,false);
    }
    ESP_LOGI(TAG,"Cmd %s",ok?"OK":"FAIL");
}

}}  // namespaces
