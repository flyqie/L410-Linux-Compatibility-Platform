#ifndef ANCO_TEXT_UTF8_H
#define ANCO_TEXT_UTF8_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Validate scalar UTF-8 and cursor/anchor byte boundaries without locale. */
static inline bool anco_text_utf8_valid(const char *s,size_t n,int32_t cursor,int32_t anchor)
{
    if(cursor<0 || anchor<0 || (size_t)cursor>n || (size_t)anchor>n)return false;
    for(size_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i];size_t k;uint32_t value,min;
        if(c<128){k=1;value=c;min=0;}
        else if(c>=0xc2 && c<=0xdf){k=2;value=c&31;min=128;}
        else if(c>=0xe0 && c<=0xef){k=3;value=c&15;min=2048;}
        else if(c>=0xf0 && c<=0xf4){k=4;value=c&7;min=65536;}
        else return false;
        if(k>n-i)return false;
        for(size_t j=1;j<k;++j) {
            unsigned b=(unsigned char)s[i+j];
            if((b&0xc0)!=0x80 || (size_t)cursor==i+j || (size_t)anchor==i+j)return false;
            value=(value<<6)|(b&63);
        }
        if(value<min || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
        i+=k;
    }
    return true;
}
#endif
