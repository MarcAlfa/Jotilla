/* Dense native grid regressions. Only a disposable archive is touched. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
static UINT forced_dpi;
static UINT grid_dpi(HWND window) { return forced_dpi ? forced_dpi : GetDpiForWindow(window); }
#define GetDpiForWindow grid_dpi
#include "../src/archive.c"
#undef GetDpiForWindow

static wchar_t root[1024], settings[1024];
static HWND owner, window;
static int checks;
static void require(BOOL ok,const char *message) {
    if(!ok) { fprintf(stderr,"FAIL: %s (error %lu)\n",message,GetLastError()); exit(1); }
}
static void pump(void) {
    MSG message;
    while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
static BOOL save(void) { return TRUE; }
static BOOL open_note(const wchar_t *id) { (void)id; return TRUE; }
static BOOL remove_note(const wchar_t *id) { (void)id; return FALSE; }
static void location(HWND parent) { (void)parent; }

static void snapshot(HWND target) {
    RECT rect;
    BITMAPINFO info={0};
    BITMAPFILEHEADER header={0};
    HDC dc, memory;
    HBITMAP bitmap;
    HGDIOBJ old;
    void *pixels;
    HANDLE file;
    DWORD written;
    GetClientRect(target,&rect);
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=rect.right;
    info.bmiHeader.biHeight=-rect.bottom; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    dc=GetDC(target); memory=CreateCompatibleDC(dc);
    bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    require(bitmap && memory,"capture native grid"); old=SelectObject(memory,bitmap);
    PrintWindow(target,memory,PW_CLIENTONLY);
    header.bfType=0x4d42; header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
    header.bfSize=header.bfOffBits+(DWORD)(rect.right*rect.bottom*4);
    file=CreateFileW(L"build\\archive-grid.bmp",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    require(file!=INVALID_HANDLE_VALUE,"save grid snapshot");
    WriteFile(file,&header,sizeof(header),&written,NULL); WriteFile(file,&info.bmiHeader,sizeof(info.bmiHeader),&written,NULL);
    WriteFile(file,pixels,header.bfSize-header.bfOffBits,&written,NULL); CloseHandle(file);
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(target,dc);
}
static void check_grid(const char *phase) {
    Archive *archive=(Archive *)GetWindowLongPtrW(window,GWLP_USERDATA);
    RECT tiles[300], intersection;
    DWORD spacing=ListView_GetItemSpacing(archive->list,FALSE);
    int i,j,count=ListView_GetItemCount(archive->list);
    pump();
    printf("Grid %s: DPI %d; spacing %u x %u; items %d.\n",phase,archive->dpi,LOWORD(spacing),HIWORD(spacing),count);
    require(count>1 && count<=300,"dense grid item count");
    for(i=0;i<count;++i) {
        require(ListView_GetItemRect(archive->list,i,&tiles[i],LVIR_ICON),"native tile bounds");
        /* Custom drawing insets the native cell by four logical pixels. */
        InflateRect(&tiles[i],-archive_scaled(archive,4),-archive_scaled(archive,4));
        for(j=0;j<i;++j) {
            if(IntersectRect(&intersection,&tiles[i],&tiles[j])) {
                fprintf(stderr,"Overlap %d [%ld,%ld,%ld,%ld] / %d [%ld,%ld,%ld,%ld]\n",i,tiles[i].left,tiles[i].top,tiles[i].right,tiles[i].bottom,j,tiles[j].left,tiles[j].top,tiles[j].right,tiles[j].bottom);
                snapshot(window); require(FALSE,"native note cards never overlap");
            }
            if(tiles[i].top==tiles[j].top)
                require(abs(tiles[i].left-tiles[j].left)-(tiles[i].right-tiles[i].left)>=archive_scaled(archive,8),"horizontal card gutter");
            if(tiles[i].left==tiles[j].left)
                require(abs(tiles[i].top-tiles[j].top)-(tiles[i].bottom-tiles[i].top)>=archive_scaled(archive,8),"vertical card gutter");
        }
    }
    ++checks;
}
static BOOL CALLBACK check_monitor(HMONITOR monitor,HDC dc,LPRECT bounds,LPARAM data) {
    static const int widths[]={1100,800,520,1200};
    MONITORINFO info={sizeof(info)};
    Archive *archive;
    int i;
    (void)dc; (void)bounds; (void)data;
    require(GetMonitorInfoW(monitor,&info),"monitor work area");
    SetWindowPos(window,NULL,info.rcWork.left+16,info.rcWork.top+16,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    archive=(Archive *)GetWindowLongPtrW(window,GWLP_USERDATA);
    check_grid("monitor change");
    for(i=0;i<4;++i) {
        SetWindowPos(window,NULL,0,0,archive_scaled(archive,widths[i]),archive_scaled(archive,580),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        check_grid("resize");
    }
    ListView_SetItemState(archive->list,140,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_EnsureVisible(archive->list,299,FALSE); check_grid("scroll");
    calendar_mode(archive,TRUE); calendar_mode(archive,FALSE); check_grid("calendar return");
    require(ListView_GetItemState(archive->list,140,LVIS_SELECTED)==LVIS_SELECTED,"calendar round trip preserves selection");
    ShowWindow(window,SW_MAXIMIZE); check_grid("maximize");
    ShowWindow(window,SW_RESTORE); check_grid("restore");
    return TRUE;
}
int wmain(void) {
    wchar_t temporary[1024], path[1024];
    static const ArchiveActions actions={save,open_note,remove_note,location};
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_WIN95_CLASSES};
    HANDLE file;
    DWORD written;
    int i;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControlsEx(&controls);
    GetTempPathW(1024,temporary);
    swprintf_s(root,1024,L"%sJotilla-grid-%lu-%llu",temporary,GetCurrentProcessId(),GetTickCount64());
    require(CreateDirectoryW(root,NULL),"private grid archive");
    swprintf_s(settings,1024,L"%s\\notes.ini",root);
    for(i=0;i<300;++i) {
        swprintf_s(path,1024,L"%s\\note-2026-%02d-%02d_%03d.txt",root,1+i%12,1+i%28,i);
        file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);
        require(file!=INVALID_HANDLE_VALUE,"private grid note");
        require(WriteFile(file,"A realistic note preview\r\nWith another line.",43,&written,NULL),"write grid note"); CloseHandle(file);
    }
    owner=CreateWindowExW(0,L"STATIC",L"Grid test owner",WS_OVERLAPPED,0,0,100,100,NULL,NULL,GetModuleHandleW(NULL),NULL);
    window=archive_show(owner,root,settings,&actions); require(window!=NULL,"open native archive");
    check_grid("initial");
    {
        /* Exercise the native control's cached geometry even on single-monitor CI. */
        static const UINT scales[]={96,120,144,192,96,120};
        Archive *archive=(Archive *)GetWindowLongPtrW(window,GWLP_USERDATA);
        for(i=0;i<6;++i) {
            forced_dpi=scales[i]; archive_fonts(archive); archive_layout(archive);
            check_grid("100/125/150/200 percent geometry");
        }
        forced_dpi=0; archive_fonts(archive); archive_layout(archive);
    }
    require(EnumDisplayMonitors(NULL,NULL,check_monitor,0),"all monitors");
    require(EnumDisplayMonitors(NULL,NULL,check_monitor,0),"repeat monitor transitions");
    snapshot(window);
    archive_close(); DestroyWindow(owner);
    for(i=0;i<300;++i) {
        swprintf_s(path,1024,L"%s\\note-2026-%02d-%02d_%03d.txt",root,1+i%12,1+i%28,i);
        require(DeleteFileW(path),"remove private grid note");
    }
    DeleteFileW(settings);
    require(RemoveDirectoryW(root),"remove private grid archive");
    printf("PASS: %d dense native grid checks, gutters, scrolling, resize, calendar, selection and monitor transitions.\n",checks);
    return 0;
}
