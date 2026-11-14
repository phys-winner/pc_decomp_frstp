/* Native SDK ABI and normal display API behavior; no visible windows/drawing. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(DISPLAY_DEVICEW)==840 && offsetof(DISPLAY_DEVICEW,DeviceName)==4 && offsetof(DISPLAY_DEVICEW,StateFlags)==324 && offsetof(DISPLAY_DEVICEW,DeviceID)==328,"DISPLAY_DEVICEW");
_Static_assert(sizeof(DEVMODEW)==220 && offsetof(DEVMODEW,dmSize)==68 && offsetof(DEVMODEW,dmDriverExtra)==70 && offsetof(DEVMODEW,dmFields)==72 && offsetof(DEVMODEW,dmPosition)==76 && offsetof(DEVMODEW,dmDisplayOrientation)==84 && offsetof(DEVMODEW,dmDisplayFixedOutput)==88 && offsetof(DEVMODEW,dmBitsPerPel)==168 && offsetof(DEVMODEW,dmPelsWidth)==172 && offsetof(DEVMODEW,dmPelsHeight)==176 && offsetof(DEVMODEW,dmDisplayFrequency)==184,"DEVMODEW");
_Static_assert(sizeof(MONITORINFO)==40 && offsetof(MONITORINFO,rcMonitor)==4 && offsetof(MONITORINFO,rcWork)==20 && offsetof(MONITORINFO,dwFlags)==36 && sizeof(MONITORINFOEXW)==104 && offsetof(MONITORINFOEXW,szDevice)==40,"MONITORINFO");
_Static_assert(sizeof(BITMAPINFOHEADER)==40 && offsetof(BITMAPINFOHEADER,biBitCount)==14 && offsetof(BITMAPINFOHEADER,biCompression)==16 && offsetof(BITMAPINFOHEADER,biSizeImage)==20,"BITMAPINFOHEADER");
static unsigned checks,callbacks;static RECT seen;
static void require(int valid,const char *why){if(!valid){fprintf(stderr,"DISPLAY_ORACLE_FAIL %s error=%lu\n",why,GetLastError());exit(1);}++checks;}
static BOOL CALLBACK monitor(HMONITOR object,HDC dc,LPRECT r,LPARAM data){(void)dc;require(object!=NULL && data==0x1234,"native monitor callback args");seen=*r;++callbacks;return TRUE;}
int main(void){
    struct {DISPLAY_DEVICEW device;unsigned char canary[16];} device;memset(&device,0xa5,sizeof(device));device.device.cb=sizeof(device.device);SetLastError(0x5a17);
    require(!EnumDisplayDevicesW(NULL,0x7fffffff,&device.device,0) && device.device.cb==840 && GetLastError()==0x5a17,"native device end result/error/cb");
    for(unsigned i=4;i<840;++i)require(!((unsigned char*)&device)[i],"native device end clears body");for(unsigned i=0;i<16;++i)require(device.canary[i]==0xa5,"native device canary");
    DEVMODEW mode;memset(&mode,0xa5,sizeof(mode));mode.dmSize=sizeof(mode);mode.dmDriverExtra=0;DEVMODEW before=mode;SetLastError(0x5a17);
    require(!EnumDisplaySettingsW(NULL,0x7fffffff,&mode) && GetLastError()==0x5a17,"native mode end result/error");before.dmSize=mode.dmSize;require(!memcmp(&before,&mode,sizeof(mode)),"native mode end only normalizes dmSize");
    RECT clip={0,0,1,1};require(EnumDisplayMonitors(NULL,&clip,monitor,0x1234) && callbacks==1 && EqualRect(&clip,&seen),"native monitor callback clip intersection");
    struct {BITMAPINFOHEADER header;DWORD masks[3];unsigned char canary[16];} format={0},query;memset(&query,0xa5,sizeof(query));format.header.biSize=40;format.header.biWidth=format.header.biHeight=1;format.header.biPlanes=1;format.header.biBitCount=32;format.header.biCompression=BI_BITFIELDS;format.masks[0]=0xff0000;format.masks[1]=0xff00;format.masks[2]=0xff;
    void *pixels=NULL;HDC dc=CreateDCW(L"DISPLAY",NULL,NULL,NULL);require(dc!=NULL,"native display metrics DC");HBITMAP bitmap=CreateDIBSection(dc,(BITMAPINFO*)&format,DIB_RGB_COLORS,&pixels,NULL,0);require(bitmap && pixels,"native CPU-backed1x1 DIB");*(uint32_t*)pixels=0;
    query.header.biSize=40;query.header.biBitCount=0;require(GetDIBits(dc,bitmap,0,1,NULL,(BITMAPINFO*)&query,DIB_RGB_COLORS)!=0 && query.header.biWidth==1 && query.header.biHeight==1 && query.header.biPlanes==1 && query.header.biBitCount==32,"native bitmap metadata query");
    unsigned first_masks_touched=0;for(unsigned i=40;i<52;++i)if(((unsigned char*)&query)[i]!=0xa5)++first_masks_touched;
    printf("NATIVE_BITMAP_QUERY first_compression=%lu first_mask_bytes_touched=%u\n",query.header.biCompression,first_masks_touched);
    require(GetDIBits(dc,bitmap,0,1,NULL,(BITMAPINFO*)&query,DIB_RGB_COLORS)!=0,"native second format query");
    printf("NATIVE_BITMAP_QUERY second_compression=%lu masks=%lx,%lx,%lx\n",query.header.biCompression,query.masks[0],query.masks[1],query.masks[2]);
    for(unsigned i=0;i<16;++i)require(query.canary[i]==0xa5,"native bitmap query52 canary");
    require(DeleteObject(bitmap) && DeleteDC(dc),"native object release");
    printf("DISPLAY_ORACLE_PASS checks=%u ABI840_220_40_104=1 native_dmSize=%u clip_intersection=1 CPU_bitmap_metadata=1 physical_modes_not_assumed=1 original_game_exec=0\n",checks,mode.dmSize);return 0;
}
