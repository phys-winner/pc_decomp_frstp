/* Native GDI data/ownership oracle. No game bytes execute, no cursor is shown. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(BITMAPV4HEADER)==108 && sizeof(ICONINFO)==32 && sizeof(DIBSECTION)==104,"Win64 GDI layouts");
static unsigned checks,cases;
static void check(int yes,const char *why){if(!yes){fprintf(stderr,"CURSOR_BITMAP_ORACLE_FAIL %s error=%lu\n",why,GetLastError());exit(1);}++checks;}
static BITMAPV4HEADER format(int width,int height){BITMAPV4HEADER f={0};f.bV4Size=108;f.bV4Width=width;f.bV4Height=height;f.bV4Planes=1;f.bV4BitCount=32;f.bV4V4Compression=BI_BITFIELDS;f.bV4RedMask=0xff0000;f.bV4GreenMask=0xff00;f.bV4BlueMask=0xff;f.bV4AlphaMask=0xff000000;return f;}
static void pair(HDC dc,int width,int height){
    BITMAPV4HEADER f=format(width,height),before=f;void *pixels=NULL;
    HBITMAP color=CreateDIBSection(dc,(BITMAPINFO*)&f,DIB_RGB_COLORS,&pixels,NULL,3);
    check(color&&pixels,"real V4 DIB with NULL section and ignored offset");check(!memcmp(&f,&before,sizeof(f)),"input V4 header unchanged");
    unsigned rows=height<0?(unsigned)-height:(unsigned)height;size_t bytes=(size_t)width*rows*4;
    for(size_t i=0;i<bytes;++i)((unsigned char*)pixels)[i]=(unsigned char)(i*37+19);
    DIBSECTION d={0};check(GetObjectW(color,sizeof(d),&d)==sizeof(d),"DIBSECTION query");
    check(d.dsBm.bmWidth==width&&d.dsBm.bmHeight==(int)rows&&d.dsBm.bmWidthBytes==width*4&&d.dsBm.bmPlanes==1&&d.dsBm.bmBitsPixel==32&&d.dsBm.bmBits==pixels,"32bit DWORD-pitch owned pixels");
    size_t pitch=((size_t)width+15)/16*2,mask_bytes=pitch*rows;
    unsigned char *mask=malloc(mask_bytes),*readback=malloc(mask_bytes);check(mask&&readback,"oracle storage");
    for(size_t i=0;i<mask_bytes;++i)mask[i]=(unsigned char)(i*13+0x55);
    HBITMAP mono=CreateBitmap(width,(int)rows,1,1,mask);check(mono!=NULL,"real mono bitmap");
    check(GetBitmapBits(mono,(LONG)mask_bytes,readback)==(LONG)mask_bytes&&!memcmp(mask,readback,mask_bytes),"CreateBitmap WORD-pitch byte copy");
    memset(mask,0,mask_bytes);check(GetBitmapBits(mono,(LONG)mask_bytes,readback)==(LONG)mask_bytes,"mono independent source storage");
    for(size_t i=0;i<mask_bytes;++i)check(readback[i]==(unsigned char)(i*13+0x55),"mono source copied");
    ICONINFO icon={0};icon.xHotspot=(DWORD)width/2;icon.yHotspot=rows/2;icon.hbmColor=color;icon.hbmMask=mono;
    HICON cursor=CreateIconIndirect(&icon);check(cursor!=NULL,"native cursor factory capability exists");
    check(DeleteObject(color)&&DeleteObject(mono),"caller deletes both original bitmaps");
    ICONINFO copy={0};check(GetIconInfo(cursor,&copy)&&copy.hbmMask&&copy.hbmColor,"native cursor owns independent bitmap copies");
    check(DeleteObject(copy.hbmMask)&&DeleteObject(copy.hbmColor)&&DestroyIcon(cursor),"release native oracle copies/cursor");
    free(mask);free(readback);++cases;
}
int main(void){
    HDC dc=GetDC(NULL);check(dc!=NULL,"desktop DC");
    const int widths[]={1,17,32,33};for(unsigned i=0;i<4;++i){pair(dc,widths[i],-9);pair(NULL,widths[i],9);}
    for(unsigned kind=0;kind<6;++kind){BITMAPV4HEADER f=format(32,-32);void *pixels=(void*)(uintptr_t)0x12345678;
        if(kind==0)f.bV4Width=0;else if(kind==1)f.bV4Height=0;else if(kind==2)f.bV4Width=-1;else if(kind==3)f.bV4Height=INT32_MIN;else if(kind==4)f.bV4Planes=2;
        SetLastError(0x5a17);HBITMAP b=CreateDIBSection(dc,(BITMAPINFO*)&f,kind==5?2:0,&pixels,NULL,0);
        check(!b&&!pixels&&GetLastError()==87,"invalid native DIB geometry/planes/usage clears output and returns87");}
    void *pixels=(void*)(uintptr_t)0x12345678;SetLastError(0x5a17);
    check(!CreateDIBSection(dc,NULL,0,&pixels,NULL,0)&&!pixels&&GetLastError()==0x5a17,"NULL bitmap info clears output preserving error");
    BITMAPV4HEADER f=format(32,-32);HBITMAP b=CreateDIBSection(dc,(BITMAPINFO*)&f,0,NULL,NULL,0);check(b&&DeleteObject(b),"optional bits output");
    b=CreateBitmap(0,9,1,1,NULL);BITMAP bm={0};check(b&&GetObjectW(b,sizeof(bm),&bm)==sizeof(bm)&&bm.bmWidth==1&&bm.bmHeight==1&&bm.bmBitsPixel==1&&DeleteObject(b),"zero-extent bitmap becomes1x1mono");
    SetLastError(0x5a17);check(!CreateBitmap(-1,9,1,1,NULL)&&GetLastError()==87,"negative bitmap geometry87");
    check(ReleaseDC(NULL,dc)==1,"matching desktop DC release");
    printf("{\"success\":true,\"checks\":%u,\"image_cases\":%u,\"BITMAPV4HEADER_bytes\":108,\"ICONINFO_bytes\":32,\"DIBSECTION_bytes\":104,\"DIB32_pixels_real\":true,\"mono_WORD_pitch_copy\":true,\"native_cursor_copies_bitmap_ownership\":true,\"native_success_LastError_unspecified\":true,\"cursor_shown\":false,\"original_game_executed\":false}\n",checks,cases);return 0;
}
