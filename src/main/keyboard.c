#include "keyboard.h"
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"

// Original Cardputer V1.1: 74HC138 decoder + 7 active-low GPIO inputs.
// Cross-checked against the M5Cardputer IOMatrix keyboard reader.
static const gpio_num_t select_pins[3]={GPIO_NUM_8,GPIO_NUM_9,GPIO_NUM_11};
static const gpio_num_t sense_pins[7]={
    GPIO_NUM_13,GPIO_NUM_15,GPIO_NUM_3,GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_6,GPIO_NUM_7
};
#define KEY(r,c) ((r)*14+(c))
#define FN KEY(2,0)
#define SHIFT KEY(2,1)
#define ENTER KEY(2,13)
#define BACK KEY(0,13)
#define UP_FN KEY(2,11)
#define LEFT_FN KEY(3,10)
#define DOWN_FN KEY(3,11)
#define RIGHT_FN KEY(3,12)
#define CANCEL_FN KEY(0,0)
static const char chars[4][14]={
    {'`','1','2','3','4','5','6','7','8','9','0','-','=','\0'},
    {'\t','q','w','e','r','t','y','u','i','o','p','[',']','\\'},
    {'\0','\0','a','s','d','f','g','h','j','k','l',';','\'','\0'},
    {'\0','\0','\0','z','x','c','v','b','n','m',',','.','/',' '}
};
static const char shifted[4][14]={
    {'~','!','@','#','$','%','^','&','*','(',')','_','+','\0'},
    {'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','|'},
    {'\0','\0','A','S','D','F','G','H','J','K','L',':','"','\0'},
    {'\0','\0','\0','Z','X','C','V','B','N','M','<','>','?',' '}
};
static uint64_t stable=0,candidate=0;
static unsigned repeated=0;
static bool pressed(uint64_t mask,int key){return (mask&(UINT64_C(1)<<key))!=0;}
static cp_key_event_t event(cp_key_type_t type,char ch)
{cp_key_event_t e={.type=type,.character=ch};return e;}
void keyboard_init(void)
{
    for(int i=0;i<3;i++){
        gpio_reset_pin(select_pins[i]);gpio_set_direction(select_pins[i],GPIO_MODE_OUTPUT);
        gpio_set_level(select_pins[i],0);
    }
    for(int i=0;i<7;i++){
        gpio_reset_pin(sense_pins[i]);gpio_set_direction(sense_pins[i],GPIO_MODE_INPUT);
        gpio_set_pull_mode(sense_pins[i],GPIO_PULLUP_ONLY);
    }
}
static uint64_t scan(void)
{
    uint64_t result=0;
    for(int bank=0;bank<8;bank++) {
        for(int bit=0;bit<3;bit++)gpio_set_level(select_pins[bit],(bank>>bit)&1);
        esp_rom_delay_us(3);
        for(int j=0;j<7;j++)if(gpio_get_level(sense_pins[j])==0){
            int col=2*j+(bank>3?0:1);
            int row=3-(bank>3?bank-4:bank);
            result|=UINT64_C(1)<<KEY(row,col);
        }
    }
    return result;
}
cp_key_event_t keyboard_poll(void)
{
    uint64_t now=scan();
    if(now!=candidate){candidate=now;repeated=0;return event(CP_KEY_NONE,0);}
    if(++repeated<2||candidate==stable)return event(CP_KEY_NONE,0);
    uint64_t before=stable;stable=candidate;
    uint64_t newly=stable&~before;
    if(pressed(newly,ENTER))return event(CP_KEY_ENTER,0);
    if(pressed(newly,BACK))return event(CP_KEY_BACKSPACE,0);
    bool fn=pressed(stable,FN);
    if(fn){
        if(pressed(newly,CANCEL_FN))return event(CP_KEY_CANCEL,0);
        if(pressed(newly,UP_FN))return event(CP_KEY_UP,0);
        if(pressed(newly,DOWN_FN))return event(CP_KEY_DOWN,0);
        if(pressed(newly,LEFT_FN))return event(CP_KEY_LEFT,0);
        if(pressed(newly,RIGHT_FN))return event(CP_KEY_RIGHT,0);
        // If Fn is pressed second, recognize any already held directional key.
        if(pressed(newly,FN)){
            if(pressed(stable,UP_FN))return event(CP_KEY_UP,0);
            if(pressed(stable,DOWN_FN))return event(CP_KEY_DOWN,0);
            if(pressed(stable,LEFT_FN))return event(CP_KEY_LEFT,0);
            if(pressed(stable,RIGHT_FN))return event(CP_KEY_RIGHT,0);
            if(pressed(stable,CANCEL_FN))return event(CP_KEY_CANCEL,0);
        }
    }
    bool shift=pressed(stable,SHIFT);
    for(int k=0;k<56;k++){
        if(!pressed(newly,k))continue;
        if(k==FN||k==SHIFT||k==KEY(3,0)||k==KEY(3,1)||k==KEY(3,2))continue;
        char c=chars[k/14][k%14];
        if(!c)continue;
        if(shift)c=shifted[k/14][k%14];
        return event(CP_KEY_CHAR,c);
    }
    return event(CP_KEY_NONE,0);
}
