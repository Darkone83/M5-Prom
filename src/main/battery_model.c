#include "battery_model.h"
#include <string.h>

/* Rough relaxed-voltage SoC table for a 1S Li-ion/LiPo battery. Cannot
 * compensate for load, cell age, charge state or temperature. Do not use
 * this number for hard safety cutoffs or to determine external power status.
 */
typedef struct {int mv,percent;} soc_point_t;
static const soc_point_t curve[] = {
    {3300,0},{3400,5},{3500,10},{3600,19},
    {3700,33},{3750,43},{3800,53},{3850,63},
    {3900,73},{4000,85},{4100,95},{4200,100}
};

int battery_estimate_percent(int mv)
{
    const unsigned count=(unsigned)(sizeof(curve)/sizeof(curve[0]));
    if(mv<=curve[0].mv)return 0;
    if(mv>=curve[count-1].mv)return 100;
    for(unsigned i=1;i<count;i++) {
        if(mv<=curve[i].mv) {
            const int delta=mv-curve[i-1].mv;
            const int range=curve[i].mv-curve[i-1].mv;
            return curve[i-1].percent +
                   (delta*(curve[i].percent-curve[i-1].percent) + range/2)/range;
        }
    }
    return 100;
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
    model->status.percentage=battery_estimate_percent(model->filtered_mv);
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
