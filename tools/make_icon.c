/* Jotilla: a lowercase Greek iota on the app's framed yellow note. Deterministic
   supersampled C geometry; Windows WIC encodes compact PNG icon frames. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Blob { BYTE *bytes; DWORD size; } Blob;
typedef struct Point { double x,y; } Point;
static Point stroke[19];
static void u16(FILE *file,uint16_t n) { fwrite(&n,2,1,file); }
static void u32(FILE *file,uint32_t n) { fwrite(&n,4,1,file); }

static double distance2(double x,double y,Point a,Point b) {
    double dx=b.x-a.x,dy=b.y-a.y,t=((x-a.x)*dx+(y-a.y)*dy)/(dx*dx+dy*dy);
    t=t<0?0:t>1?1:t; dx=x-a.x-t*dx; dy=y-a.y-t*dy;
    return dx*dx+dy*dy;
}

static DWORD sample(double x,double y,DWORD bar_color,DWORD body_color) {
    int i;
    if (x<.07 || x>=.93 || y<.07 || y>=.93) return 0;
    if (x<.097 || x>=.903 || y<.097 || y>=.903 || (y>=.235 && y<.262)) return 0xff20201b;
    if (y<.235 && (distance2(x,y,(Point){.632,.168},(Point){.708,.168})<.009*.009 ||
                  distance2(x,y,(Point){.782,.131},(Point){.850,.199})<.008*.008 ||
                  distance2(x,y,(Point){.782,.199},(Point){.850,.131})<.008*.008)) return 0xff20201b;
    for (i=0;i<18;++i)
        if (distance2(x,y,stroke[i],stroke[i+1])<.034*.034) return 0xff20201b;
    return y<.235 ? bar_color : body_color;
}

static BYTE *raster(int size,DWORD bar_color,DWORD body_color) {
    BYTE *pixels=(BYTE *)malloc((size_t)size*size*4);
    int x,y,sx,sy;
    if (!pixels) return NULL;
    for (y=0;y<size;++y) for (x=0;x<size;++x) {
        unsigned r=0,g=0,b=0,a=0;
        BYTE *pixel=pixels+((size_t)y*size+x)*4;
        for (sy=0;sy<8;++sy) for (sx=0;sx<8;++sx) {
            DWORD value=sample((x+(sx+.5)/8)/size,(y+(sy+.5)/8)/size,bar_color,body_color);
            if (value) { ++a; r+=(value>>16)&255; g+=(value>>8)&255; b+=value&255; }
        }
        pixel[0]=(BYTE)(a?b/a:0); pixel[1]=(BYTE)(a?g/a:0); pixel[2]=(BYTE)(a?r/a:0);
        pixel[3]=(BYTE)((a*255+32)/64);
    }
    return pixels;
}

static BOOL encode(IWICImagingFactory *factory,int size,Blob *blob,DWORD bar_color,DWORD body_color) {
    IStream *stream=NULL;
    IWICBitmapEncoder *encoder=NULL;
    IWICBitmapFrameEncode *frame=NULL;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    STATSTG stat={0};
    HGLOBAL memory=NULL;
    BYTE *pixels=raster(size,bar_color,body_color);
    HRESULT hr=pixels?CreateStreamOnHGlobal(NULL,TRUE,&stream):E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr=IWICImagingFactory_CreateEncoder(factory,&GUID_ContainerFormatPng,NULL,&encoder);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,NULL);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Initialize(frame,NULL);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetSize(frame,(UINT)size,(UINT)size);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat32bppBGRA)) hr=E_FAIL;
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_WritePixels(frame,(UINT)size,(UINT)size*4,(UINT)size*size*4,pixels);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Commit(encoder);
    if (SUCCEEDED(hr)) hr=IStream_Stat(stream,&stat,STATFLAG_NONAME);
    if (SUCCEEDED(hr)) hr=GetHGlobalFromStream(stream,&memory);
    if (SUCCEEDED(hr)) {
        void *bytes=GlobalLock(memory);
        blob->size=stat.cbSize.LowPart; blob->bytes=(BYTE *)malloc(blob->size);
        if (!bytes || !blob->bytes || stat.cbSize.HighPart) hr=E_OUTOFMEMORY;
        else memcpy(blob->bytes,bytes,blob->size);
        if (bytes) GlobalUnlock(memory);
    }
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IStream_Release(stream);
    free(pixels);
    return SUCCEEDED(hr);
}

static BOOL build_icon(IWICImagingFactory *factory,const char *ico_path,const char *png_path,
                       DWORD bar_color,DWORD body_color) {
    const int sizes[]={16,20,24,32,40,48,64,128,256};
    Blob frames[9]={{0}};
    FILE *file=NULL;
    DWORD offset=6+9*16;
    int i;
    BOOL ok=FALSE;
    for (i=0;i<9;++i) if (!encode(factory,sizes[i],&frames[i],bar_color,body_color)) goto done;
    if (fopen_s(&file,ico_path,"wb")) goto done;
    u16(file,0); u16(file,1); u16(file,9);
    for (i=0;i<9;++i) {
        fputc(sizes[i]&255,file); fputc(sizes[i]&255,file); u16(file,0); u16(file,1); u16(file,32);
        u32(file,frames[i].size); u32(file,offset); offset+=frames[i].size;
    }
    for (i=0;i<9;++i) fwrite(frames[i].bytes,1,frames[i].size,file);
    if (ferror(file)) goto done;
    if (fclose(file)) { file=NULL; goto done; } file=NULL;
    if (png_path) {
        if (fopen_s(&file,png_path,"wb")) goto done;
        if (fwrite(frames[8].bytes,1,frames[8].size,file)!=frames[8].size) goto done;
        if (fclose(file)) { file=NULL; goto done; } file=NULL;
    }
    ok=TRUE;
done:
    if (file) fclose(file);
    for (i=0;i<9;++i) free(frames[i].bytes);
    return ok;
}

int main(void) {
    IWICImagingFactory *factory=NULL;
    int i,result=1;
    if (FAILED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&factory))) goto done;
    stroke[0]=(Point){.426,.384}; stroke[1]=(Point){.497,.365}; stroke[2]=(Point){.486,.655};
    for (i=1;i<=16;++i) {
        double t=i/16.0,s=1-t;
        stroke[i+2]=(Point){s*s*s*.486+3*s*s*t*.473+3*s*t*t*.622+t*t*t*.653,
                           s*s*s*.655+3*s*s*t*.795+3*s*t*t*.802+t*t*t*.717};
    }
    if (build_icon(factory,"assets/jotilla.ico","assets/jotilla.png",0xffefdf8d,0xffffee97) &&
        build_icon(factory,"assets/jotilla_hidden.ico",NULL,0xff7088a1,0xff8ea7c2))
        result=0;
done:
    if (factory) IWICImagingFactory_Release(factory);
    CoUninitialize();
    return result;
}
