#include "battery_model.h"
#include <string.h>

/* Match M5Stack's M5Unified Power_Class::getBatteryLevel() conversion for
 * the Cardputer family: 3300mV = 0%, 4100mV = 100%, clamped.
 * This is NOT a fuel gauge; the measured battery voltage under load/USB power
 * is not a reliable measurement of remaining capacity or charging state.
 * We smooth the measured voltage below, rather than inventing a different
 * chemistry curve and disagreeing with M5Stack's own battery example.
 */
int battery_estimate_percent(int mv)
{
    if(mv<=3300)return 0;
    if(mv>=4100)return 100;
    return (mv-3300)*100/800;
}

void battery_model_init(battery_model_t *model)
{
    if(model)memset(model,0,sizeof(*model));
}

static int sorted_median(const int *raw,unsigned count)
{
    int sorted[5];
    for(unsigned i=0;i<count;i++)sorted[i]=raw[i];
    for(unsigned i=1;i<count;i++){
        int t=sorted[i];unsigned j=i;
        while(j>0&&sorted[j-1]>t){sorted[j]=sorted[j-1];j--;}
        sorted[j]=t;
    }
    return sorted[count/2];
}

bool battery_model_sample(battery_model_t *model,int voltage_mv,uint32_t now_ms)
{
    if(!model||voltage_mv<2500||voltage_mv>4500)return false;
    battery_status_t old=model->status;
    model->ring[model->index]=voltage_mv;
    model->index=(model->index+1)%5u;
    if(model->count<5)model->count++;
    const int med=sorted_median(model->ring,model->count);
    if(!model->status.valid){
        model->filtered_mv=med;
        model->trend_baseline_mv=med;
        model->trend_baseline_ms=now_ms;
        model->trend_initialized=true;
    } else {
        // First-order IIR; remove noisy instantaneous percentage jumps.
        model->filtered_mv += (med-model->filtered_mv)/4;
    }
    model->status.valid=true;
    model->status.millivolts=model->filtered_mv;
    int percent=battery_estimate_percent(model->filtered_mv);
    if(!old.valid){
        model->status.percentage=percent;
    }else{
        /* Four-millivolt hysteresis on the nominal 8mV-per-point scale
         * prevents a battery sitting on a threshold from jumping between
         * adjacent percentage values. Large genuine changes still propagate.
         */
        const int previous=old.percentage;
        if(percent>previous){
            const int threshold=3300+(previous+1)*8+4;
            if(model->filtered_mv>=threshold)model->status.percentage=percent;
        }else if(percent<previous){
            const int threshold=3300+previous*8-4;
            if(model->filtered_mv<=threshold)model->status.percentage=percent;
        }
    }
    // A full ~60s observation window and 30mV hysteresis prevent ADC noise
    // from being interpreted as a voltage trend; this is NOT charger detection.
    if(model->trend_initialized &&
       (uint32_t)(now_ms-model->trend_baseline_ms)>=60000u){
        const int delta=model->filtered_mv-model->trend_baseline_mv;
        model->status.trend=delta>=30?BATTERY_TREND_RISING:
                            delta<=-30?BATTERY_TREND_FALLING:
                                       BATTERY_TREND_STABLE;
        model->trend_baseline_mv=model->filtered_mv;
        model->trend_baseline_ms=now_ms;
    }
    return (!old.valid || old.percentage!=model->status.percentage ||
            old.trend!=model->status.trend ||
            old.millivolts/10!=model->status.millivolts/10);
}
