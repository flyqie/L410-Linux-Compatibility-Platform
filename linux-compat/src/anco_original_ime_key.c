#include "anco_original_ime_key.h"
#include <errno.h>
#include <string.h>

static int32_t ohos_code(int32_t code)
{
    if(code>=7&&code<=16)return code-7+2000;
    if(code>=144&&code<=153)return code-144+2103;
    if(code>=29&&code<=54)return code-29+2017;
    if(code>=55&&code<=77)return code-55+2043;
    if(code>=111&&code<=142)return code-111+2070;
    if(code==143)return 2102;
    switch(code){
    case 3:return 1;case 4:return 2;case 19:return 2012;case 20:return 2013;
    case 21:return 2014;case 22:return 2015;case 24:return 16;case 25:return 17;
    case 26:return 18;case 28:return 5;case 67:return 2055;case 68:return 2056;
    case 69:return 2057;case 81:return 2066;case 82:return 2067;case 84:return 9;
    case 91:return 23;case 92:return 2068;case 93:return 2069;case 113:return 2072;
    case 114:return 2073;case 122:return 2081;case 123:return 2082;case 124:return 2083;
    case 154:return 2113;case 155:return 2114;case 156:return 2115;case 157:return 2116;
    case 158:return 2117;case 159:return 2118;case 164:return 22;default:return -1;
    }
}

static int64_t unicode_for(int32_t code,bool caps,bool num,bool shift)
{
    if(code>=2017&&code<=2042){
        const int64_t low=code-2017+'a',high=code-2017+'A';
        return caps!=shift?high:low;
    }
    if(code>=2000&&code<=2009){
        static const int64_t shifted[]={')','!','@','#','$','%','^','&','*','('};
        return shift?shifted[code-2000]:code-2000+'0';
    }
    int64_t low=0,high=0;
    switch(code){
    case 2056:low='`';high='~';break;case 2057:low='-';high='_';break;
    case 2058:low='+';high='=';break;case 2059:low='[';high='{';break;
    case 2060:low=']';high='}';break;case 2061:low='\\';high='|';break;
    case 2062:low=';';high=':';break;case 2063:low='\'';high='"';break;
    case 2064:low='/';high='?';break;case 2043:low=',';high='<';break;
    case 2044:low='.';high='>';break;case 2113:low='/';break;
    case 2114:low='8';break;case 2115:low='-';break;case 2116:low='+';break;
    case 2117:low='.';break;default:
        if(code>=2103&&code<=2112){low=code-2103+'0';return num?0:low;}
        return 0;
    }
    return shift?high:low;
}

void anco_original_ime_key_reset(AncoOriginalImeKeyState *state)
{
    if(state)*state=(AncoOriginalImeKeyState){.previous_key=-1};
}

int anco_original_ime_key_convert(AncoOriginalImeKeyState *state,
    const AncoNativeInputKey *raw,AncoImfKeyAttribute *attribute,
    const AncoImfKeyItem **items,size_t *count)
{
    if(!state||!raw||!attribute||!items||!count||raw->action_time_us<0||
       (raw->android_action!=0&&raw->android_action!=1))return -EINVAL;
    if(state->previous_action==3){
        for(size_t n=0;n<state->count;++n)if(state->held[n].key_code==state->previous_key){
            memmove(state->held+n,state->held+n+1,(state->count-n-1)*sizeof(state->held[0]));
            --state->count;break;
        }
    }
    const int32_t action=raw->android_action==0?2:3;
    const int32_t key=ohos_code(raw->android_key);
    const bool num=(raw->android_meta&0x200000u)!=0;
    const bool caps=(raw->android_meta&0x100000u)!=0;
    const bool scroll=(raw->android_meta&0x400000u)!=0;
    bool shift=false;size_t found=state->count;
    for(size_t n=0;n<state->count;++n){
        if(state->held[n].pressed&&(state->held[n].key_code==2047||state->held[n].key_code==2048))shift=true;
        if(state->held[n].key_code==key)found=n;
    }
    int64_t down=raw->action_time_us/1000;
    if(action==2){
        if(found==state->count){
            if(state->count==sizeof(state->held)/sizeof(state->held[0]))return -EOVERFLOW;
            state->held[state->count++]=(AncoImfKeyItem){true,down,raw->device_id,key,
                unicode_for(key,caps,num,shift)};
        }
    }else{
        if(found<state->count){
            down=state->held[found].down_time;
            memmove(state->held+found,state->held+found+1,
                (state->count-found-1)*sizeof(state->held[0]));
            --state->count;
        }
        if(state->count==sizeof(state->held)/sizeof(state->held[0]))return -EOVERFLOW;
        state->held[state->count++]=(AncoImfKeyItem){false,down,raw->device_id,key,
            unicode_for(key,caps,num,shift)};
    }
    state->previous_action=action;state->previous_key=key;
    *attribute=(AncoImfKeyAttribute){raw->event_id,raw->action_time_us/1000000,
        action,raw->device_id,key,0,num,caps,scroll};
    *items=state->held;*count=state->count;return 0;
}
