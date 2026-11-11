/* Native, virtual archive view. Only visible previews are read and cached. */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
#include "archive.h"
#include "colors.h"
#include "tags.h"
#include "search.h"
#include "calendar.h"
#include "metadata.h"

#define ARCHIVE_CLASS L"Jotilla.Archive"
#define ARCHIVE_PATH_CAP 1024
#define DELETE_NOTES 1
#define ARCHIVE_REFRESH (WM_APP + 1)
#define ARCHIVE_SEARCH_DONE (WM_APP + 2)
#define ARCHIVE_TAGS_REFRESH (WM_APP + 3)
#define SEARCH_TIMER 1
#define TAG_TREE 6
#define TAG_BUTTON 7
#define DATE_BUTTON 8
#define LOCATION_BUTTON 9
#define CALENDAR_BUTTON 10
#define CALENDAR_VIEW 11
#define ARCHIVE_PAPER RGB(244,238,226)
#define ARCHIVE_SIDEBAR RGB(235,226,209)
#define ARCHIVE_INK RGB(58,52,43)
#define ARCHIVE_SELECTION RGB(211,194,162)

static const wchar_t *MONTH_NAMES[] = {
    L"January", L"February", L"March", L"April", L"May", L"June",
    L"July", L"August", L"September", L"October", L"November", L"December"
};

typedef struct ArchiveEntry {
    wchar_t id[80];
    wchar_t *preview, *tags, *excerpt;
    int highlight, highlight_length;
    unsigned date, scheduled;
    UINT state;
    COLORREF paper, ink, bar_ink;
    BOOL loaded, titled;
} ArchiveEntry;

typedef struct Archive {
    HWND window, tree, tag_tree, tag_button, date_button, list, search, clear, location;
    HWND calendar, calendar_button;
    HFONT font, bold;
    HBRUSH background, sidebar;
    HIMAGELIST tiles;
    ArchiveEntry *entries;
    int count, first, range, visible, dpi, recheck;
    unsigned filter;
    const wchar_t *directory, *settings;
    ArchiveActions actions;
    SearchWorker *worker;
    SearchResult *results;
    TagCatalog catalog;
    wchar_t tag_filter[TAG_NAME_CAP];
    int *day_entries, day_capacity, day_offsets[367];
    POINT note_scroll;
    BOOL calendar_view, calendar_dirty;
    BOOL rebuilding, refresh_pending, searching, tags_view, tags_dirty, tags_pending;
    RECT normal_rect;
    BOOL has_rect, maximized;
} Archive;

static HWND archive_window;
static BOOL archive_topmost = TRUE;
static void schedule_search(Archive *archive);
static BOOL refresh_archive(Archive *archive, BOOL reload);
static void open_archive_item(Archive *archive, int index);
static BOOL refresh_tags(Archive *archive);
static void sidebar_mode(Archive *archive, BOOL tags);
static void calendar_update(Archive *archive);
static void calendar_mode(Archive *archive, BOOL enabled);
static void load_preview(Archive *archive, ArchiveEntry *entry);

static int entry_index(const Archive *archive, int row) {
    return archive->results ? archive->results->hits[row].index : archive->first + row;
}

typedef struct MonitorList {
    HMONITOR handles[16];
    MONITORINFO info[16];
    int count;
} MonitorList;

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data) {
    MonitorList *list = (MonitorList *)data;
    MONITORINFO info = {sizeof(info)};
    (void)dc; (void)rect;
    if (list->count >= 16) return FALSE;
    if (GetMonitorInfoW(monitor, &info)) {
        list->handles[list->count] = monitor;
        list->info[list->count] = info;
        ++list->count;
    }
    return TRUE;
}

static int monitor_number(const RECT *rect) {
    MonitorList list = {0};
    HMONITOR target = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST);
    int i;
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&list);
    for (i = 0; i < list.count; ++i) {
        if (list.handles[i] == target) return i + 1;
    }
    return 1;
}

static BOOL read_browser_int(const wchar_t *settings, const wchar_t *key, int *value) {
    wchar_t text[32], *end = NULL;
    long parsed;
    DWORD length = GetPrivateProfileStringW(L"app", key, L"", text, 32, settings);
    if (!length || length >= 31) return FALSE;
    parsed = wcstol(text, &end, 10);
    if (!end || *end != 0 || parsed < INT_MIN || parsed > INT_MAX) return FALSE;
    *value = (int)parsed;
    return TRUE;
}

static void write_browser_int(const wchar_t *settings, const wchar_t *key, int value) {
    wchar_t text[32];
    swprintf_s(text, 32, L"%d", value);
    WritePrivateProfileStringW(L"app", key, text, settings);
}

static void save_browser_placement(const Archive *archive) {
    int width, height, monitor;
    if (!archive || !archive->settings || !*archive->settings || !archive->has_rect) return;
    width = archive->normal_rect.right - archive->normal_rect.left;
    height = archive->normal_rect.bottom - archive->normal_rect.top;
    if (width <= 0 || height <= 0) return;
    monitor = monitor_number(&archive->normal_rect);
    write_browser_int(archive->settings, L"browser_x", archive->normal_rect.left);
    write_browser_int(archive->settings, L"browser_y", archive->normal_rect.top);
    write_browser_int(archive->settings, L"browser_w", width);
    write_browser_int(archive->settings, L"browser_h", height);
    write_browser_int(archive->settings, L"browser_monitor", monitor);
    write_browser_int(archive->settings, L"browser_maximized", archive->maximized ? 1 : 0);
}

static BOOL load_browser_placement(HWND owner, const wchar_t *settings, int dpi,
                                   int *x, int *y, int *width, int *height, BOOL *maximized) {
    int sx, sy, sw, sh, mon, max_flag = 0;
    MonitorList list = {0};
    MONITORINFO monitor = {sizeof(monitor)};
    RECT saved, intersect;
    HMONITOR by_rect;
    int min_w, min_h, work_w, work_h;
    BOOL has_mon = FALSE;
    if (!settings || !*settings) return FALSE;
    if (!read_browser_int(settings, L"browser_x", &sx) ||
        !read_browser_int(settings, L"browser_y", &sy) ||
        !read_browser_int(settings, L"browser_w", &sw) ||
        !read_browser_int(settings, L"browser_h", &sh) ||
        sw <= 0 || sh <= 0)
        return FALSE;
    if (!read_browser_int(settings, L"browser_monitor", &mon)) mon = 0;
    read_browser_int(settings, L"browser_maximized", &max_flag);
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&list);
    SetRect(&saved, sx, sy, sx + sw, sy + sh);
    by_rect = MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
    if (mon >= 1 && mon <= list.count) {
        monitor = list.info[mon - 1];
        has_mon = TRUE;
    } else if (by_rect && GetMonitorInfoW(by_rect, &monitor)) {
        has_mon = TRUE;
    } else {
        HMONITOR fallback = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(fallback, &monitor)) has_mon = TRUE;
    }
    if (!has_mon) return FALSE;
    work_w = monitor.rcWork.right - monitor.rcWork.left;
    work_h = monitor.rcWork.bottom - monitor.rcWork.top;
    min_w = min(MulDiv(520, dpi, 96), work_w);
    min_h = min(MulDiv(320, dpi, 96), work_h);
    *width = max(min_w, min(sw, work_w));
    *height = max(min_h, min(sh, work_h));
    if (IntersectRect(&intersect, &saved, &monitor.rcWork)) {
        *x = max(monitor.rcWork.left, min(sx, monitor.rcWork.right - *width));
        *y = max(monitor.rcWork.top, min(sy, monitor.rcWork.bottom - *height));
    } else {
        *x = monitor.rcWork.left + (work_w - *width) / 2;
        *y = monitor.rcWork.top + (work_h - *height) / 2;
    }
    *maximized = max_flag != 0;
    return TRUE;
}

static void remember_selection(Archive *archive) {
    int i;
    if (archive->calendar_view) return;
    for (i = 0; i < archive->count; ++i) archive->entries[i].state = 0;
    /* Native selection iteration avoids one synchronous message per card. */
    for (i = ListView_GetNextItem(archive->list, -1, LVNI_SELECTED); i >= 0;
         i = ListView_GetNextItem(archive->list, i, LVNI_SELECTED))
        archive->entries[entry_index(archive, i)].state = LVIS_SELECTED;
    i = ListView_GetNextItem(archive->list, -1, LVNI_FOCUSED);
    if (i >= 0) archive->entries[entry_index(archive, i)].state |= LVIS_FOCUSED;
}

static void discard_results(Archive *archive) {
    int i;
    if (!archive->results) return;
    for (i = 0; i < archive->results->count; ++i) {
        ArchiveEntry *entry = &archive->entries[archive->results->hits[i].index];
        free(entry->excerpt); entry->excerpt = NULL;
    }
    search_result_free(archive->results); archive->results = NULL;
}

static int archive_scaled(const Archive *archive, int value) { return MulDiv(value, archive->dpi, 96); }

static void free_entries(ArchiveEntry *entries, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        free(entries[i].preview); entries[i].preview = NULL;
        free(entries[i].tags); entries[i].tags = NULL;
        free(entries[i].excerpt); entries[i].excerpt = NULL;
    }
    free(entries);
}

static int compare_entries(const void *left, const void *right) {
    const ArchiveEntry *a = (const ArchiveEntry *)left, *b = (const ArchiveEntry *)right;
    if (a->date != b->date) return a->date < b->date ? 1 : -1;
    return wcscmp(b->id, a->id);
}

static unsigned date_from_id(const wchar_t *id) {
    unsigned year, month, day;
    return swscanf_s(id + 5, L"%4u-%2u-%2u", &year, &month, &day) == 3 &&
        year >= 1601 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31 ?
        year * 10000 + month * 100 + day : 0;
}

static ArchiveEntry *find_entry(Archive *archive, const wchar_t *id) {
    ArchiveEntry key = {0};
    int i;
    wcscpy_s(key.id, 80, id); key.date = date_from_id(id);
    if (key.date && archive->count)
        return (ArchiveEntry *)bsearch(&key, archive->entries, (size_t)archive->count, sizeof(key), compare_entries);
    if (!key.date) /* Legacy filenames use the file's creation date. */
        for (i = 0; i < archive->count; ++i)
            if (!wcscmp(archive->entries[i].id, id)) return &archive->entries[i];
    return NULL;
}

static BOOL calendar_load(Archive *archive) {
    wchar_t *text, *key, *value;
    BOOL unicode;
    MetadataCursor cursor = {0};
    int i;
    if (!metadata_read(archive->settings,&text,&unicode)) return FALSE;
    for (i=0;i<archive->count;++i) archive->entries[i].scheduled=0;
    cursor.next=text;
    while (metadata_next(&cursor,&key,&value)) {
        if (cursor.section && !_wcsicmp(key,L"calendar_date") && wcslen(cursor.section)<80 &&
            (!wcsncmp(cursor.section,L"note-",5) || !wcsncmp(cursor.section,L"nota-",5))) {
            ArchiveEntry *entry=find_entry(archive,cursor.section);
            if (entry) entry->scheduled=date_parse(value);
        }
    }
    free(text); archive->calendar_dirty=FALSE; return TRUE;
}

/* Counting buckets keep each day's references contiguous without changing
   the archive's creation-date ordering. Rebuilds reuse the existing vector. */
static void calendar_update(Archive *archive) {
    unsigned counts[366]={0};
    int positions[366], i, year;
    if (!archive->calendar_view || !archive->calendar) return;
    calendar_list_close(archive->window);
    memset(archive->day_offsets,0,sizeof(archive->day_offsets));
    if (archive->calendar_dirty && !calendar_load(archive)) {
        calendar_counts(archive->calendar,counts);
        SetWindowTextW(archive->window,L"Calendar - Dates could not be read. Press F5 to retry."); return;
    }
    if (archive->day_capacity < archive->count) {
        int *entries=(int *)realloc(archive->day_entries,(size_t)archive->count*sizeof(int));
        if (!entries) { calendar_counts(archive->calendar,counts); SetWindowTextW(archive->window,L"Calendar - Not enough memory. Press F5 to retry."); return; }
        archive->day_entries=entries; archive->day_capacity=archive->count;
    }
    year=calendar_year(archive->calendar);
    for (i=0;i<archive->visible;++i) {
        unsigned date=archive->entries[entry_index(archive,i)].scheduled;
        if ((int)(date/10000)==year) ++counts[date_ordinal(date)];
    }
    archive->day_offsets[0]=0;
    for (i=0;i<366;++i) {
        positions[i]=archive->day_offsets[i];
        archive->day_offsets[i+1]=positions[i]+(int)counts[i];
    }
    for (i=0;i<archive->visible;++i) {
        int index=entry_index(archive,i);
        unsigned date=archive->entries[index].scheduled;
        if ((int)(date/10000)==year) archive->day_entries[positions[date_ordinal(date)]++]=index;
    }
    calendar_counts(archive->calendar,counts);
}

static void calendar_open_day(Archive *archive, unsigned date) {
    int day=date_ordinal(date), first, count, i;
    CalendarItem *items;
    RECT rect; POINT anchor;
    if (day<0 || date/10000!=(unsigned)calendar_year(archive->calendar)) return;
    first=archive->day_offsets[day]; count=archive->day_offsets[day+1]-first;
    if (!count) return;
    if (count==1) {
        if (!archive->actions.open(archive->entries[archive->day_entries[first]].id))
            MessageBoxW(archive->window,L"This note could not be opened.",L"Jotilla",MB_OK|MB_ICONERROR);
        return;
    }
    items=(CalendarItem *)calloc((size_t)count,sizeof(*items));
    if (!items) return;
    for (i=0;i<count;++i) {
        ArchiveEntry *entry=&archive->entries[archive->day_entries[first+i]];
        const wchar_t *title;
        if (!entry->loaded) load_preview(archive,entry);
        title=entry->preview ? entry->preview : L"Preview unavailable";
        if (entry->titled && *title==L'#') ++title;
        while (iswspace(*title)) ++title;
        if (!*title) title=L"Untitled";
        wcscpy_s(items[i].id,80,entry->id);
        wcsncpy_s(items[i].title,256,title,min(wcscspn(title,L"\r\n"),255));
        items[i].paper=palette_bar(entry->paper); items[i].ink=entry->bar_ink;
    }
    calendar_day_rect(archive->calendar,date,&rect); anchor.x=rect.left; anchor.y=rect.bottom;
    ClientToScreen(archive->calendar,&anchor);
    if (!calendar_list(archive->window,anchor,date,items,count,archive->actions.open))
        MessageBoxW(archive->window,L"The day's notes could not be displayed.",L"Jotilla",MB_OK|MB_ICONERROR);
}

static BOOL scan_archive(Archive *archive, ArchiveEntry **result, int *total) {
    wchar_t pattern[ARCHIVE_PATH_CAP];
    WIN32_FIND_DATAW found;
    HANDLE search;
    ArchiveEntry *entries = NULL;
    int count = 0, capacity = 0;
    DWORD error;
    *result = NULL; *total = 0;
    if (swprintf_s(pattern, ARCHIVE_PATH_CAP, L"%s\\*.txt", archive->directory) < 0) return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        size_t length = wcslen(found.cFileName);
        SYSTEMTIME time;
        FILETIME local;
        ArchiveEntry *entry;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (!found.nFileSizeHigh && !found.nFileSizeLow) ||
            length < 6 || length - 4 >= 80 ||
            (wcsncmp(found.cFileName, L"note-", 5) && wcsncmp(found.cFileName, L"nota-", 5))) continue;
        if (count == capacity) {
            int next_capacity;
            ArchiveEntry *next;
            if (capacity > INT_MAX / 2) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); break; }
            next_capacity = capacity ? capacity * 2 : 64;
            next = (ArchiveEntry *)realloc(entries, (size_t)next_capacity * sizeof(*entries));
            if (!next) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); break; }
            entries = next; capacity = next_capacity;
        }
        entry = &entries[count];
        memset(entry, 0, sizeof(*entry));
        wcsncpy_s(entry->id, 80, found.cFileName, length - 4);
        entry->date = date_from_id(entry->id);
        if (!entry->date && FileTimeToLocalFileTime(&found.ftCreationTime, &local) && FileTimeToSystemTime(&local, &time))
            entry->date = time.wYear * 10000u + time.wMonth * 100u + time.wDay;
        ++count;
    } while (FindNextFileW(search, &found));
    error = GetLastError(); FindClose(search);
    if (error != ERROR_NO_MORE_FILES) { free(entries); return FALSE; }
    if (count > 1) qsort(entries, (size_t)count, sizeof(*entries), compare_entries);
    *result = entries; *total = count;
    return TRUE;
}

/* Date groups are contiguous in the sorted vector: filtering needs no
   secondary index, allocation, or full scan. */
static int date_bound(const Archive *archive, unsigned date) {
    int low = 0, high = archive->count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (archive->entries[middle].date > date) low = middle + 1;
        else high = middle;
    }
    return low;
}

static void filter_archive(Archive *archive, unsigned date) {
    unsigned last = date;
    if (archive->worker) search_cancel(archive->worker);
    discard_results(archive);
    archive->filter = date;
    if (archive->calendar_view || archive->tags_view || !date) { archive->first = 0; archive->visible = archive->count; }
    else {
        if (!(date % 10000)) last += 1231;
        else if (!(date % 100)) last += 31;
        archive->first = date_bound(archive, last);
        archive->visible = date_bound(archive, date - 1) - archive->first;
    }
    archive->range = archive->visible;
    ListView_SetItemState(archive->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(archive->list, archive->visible, LVSICF_NOSCROLL);
    ListView_Arrange(archive->list, LVA_DEFAULT);
    InvalidateRect(archive->list, NULL, TRUE);
    schedule_search(archive);
}

static HTREEITEM date_node(Archive *archive, HTREEITEM parent, HTREEITEM after, const wchar_t *label, unsigned date) {
    TVINSERTSTRUCTW insert = {0};
    insert.hParent = parent; insert.hInsertAfter = after;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (wchar_t *)label; insert.item.lParam = date;
    return TreeView_InsertItem(archive->tree, &insert);
}

static unsigned node_date(HWND tree, HTREEITEM node) {
    TVITEMW item = {0};
    item.mask = TVIF_PARAM; item.hItem = node;
    TreeView_GetItem(tree, &item);
    return (unsigned)item.lParam;
}

/* Merge sorted date groups into the native tree. Unchanged nodes keep their
   handles, expansion state and scroll position; recursion is only three levels. */
static void sync_dates(Archive *archive, HTREEITEM parent, int first, int end,
                       unsigned unit, HTREEITEM *selected) {
    HTREEITEM node = TreeView_GetChild(archive->tree, parent), previous = TVI_FIRST;
    while (first < end) {
        unsigned date = archive->entries[first].date / unit * unit;
        int next = first + 1;
        wchar_t label[32];
        while (next < end && archive->entries[next].date / unit * unit == date) ++next;
        while (node && node_date(archive->tree, node) > date) {
            HTREEITEM removed = node;
            node = TreeView_GetNextSibling(archive->tree, node);
            TreeView_DeleteItem(archive->tree, removed);
        }
        if (!node || node_date(archive->tree, node) != date) {
            unsigned month = date / 100 % 100;
            swprintf_s(label, 32, unit == 10000 ? L"%04u" : L"%02u", unit == 10000 ? date / unit : date % 100);
            node = date_node(archive, parent, previous,
                unit == 100 && month >= 1 && month <= 12 ? MONTH_NAMES[month - 1] : label, date);
        }
        if (archive->filter >= date && archive->filter - date < unit) *selected = node;
        if (unit > 1) sync_dates(archive, node, first, next, unit / 100, selected);
        previous = node; node = TreeView_GetNextSibling(archive->tree, node); first = next;
    }
    while (node) {
        HTREEITEM removed = node;
        node = TreeView_GetNextSibling(archive->tree, node);
        TreeView_DeleteItem(archive->tree, removed);
    }
}

static BOOL refresh_archive(Archive *archive, BOOL reload) {
    ArchiveEntry *entries;
    int count, i, old = 0;
    HTREEITEM root = TreeView_GetRoot(archive->tree), selected;
    TVITEMW item = {0};
    wchar_t label[64];
    BOOL initial = root == NULL;
    archive->refresh_pending = FALSE;
    archive->rebuilding = TRUE;
    if (reload) search_reload(archive->worker);
    if (!archive->actions.save() || !scan_archive(archive, &entries, &count)) {
        archive->rebuilding = FALSE; return FALSE;
    }
    if (!archive->searching) remember_selection(archive);
    if (archive->worker) search_cancel(archive->worker);
    discard_results(archive);
    /* Keep cached previews and selected notes across additions/deletions.
       Explicit refresh also rereads files edited outside Jotilla. */
    for (i = 0; i < count; ++i) {
        while (old < archive->count && compare_entries(&archive->entries[old], &entries[i]) < 0) ++old;
        if (old < archive->count && !compare_entries(&archive->entries[old], &entries[i])) {
            if (!reload) {
                entries[i] = archive->entries[old];
                archive->entries[old].preview = NULL;
                archive->entries[old].tags = NULL;
                archive->entries[old].excerpt = NULL;
            }
            entries[i].state = archive->entries[old].state;
        }
    }
    SendMessageW(archive->tree, WM_SETREDRAW, FALSE, 0);
    free_entries(archive->entries, archive->count);
    archive->entries = entries; archive->count = count;
    archive->calendar_dirty=TRUE;
    swprintf_s(label, 64, L"All notes (%d)", count);
    if (!root) root = date_node(archive, TVI_ROOT, TVI_LAST, label, 0);
    else {
        item.mask = TVIF_TEXT; item.hItem = root; item.pszText = label;
        TreeView_SetItem(archive->tree, &item);
    }
    selected = root;
    sync_dates(archive, root, 0, count, 10000, &selected);
    if (initial) {
        HTREEITEM latest = TreeView_GetChild(archive->tree, root);
        TreeView_Expand(archive->tree, root, TVE_EXPAND);
        if (latest) {
            TreeView_Expand(archive->tree, latest, TVE_EXPAND);
            TreeView_Expand(archive->tree, TreeView_GetChild(archive->tree, latest), TVE_EXPAND);
        }
    }
    TreeView_SelectItem(archive->tree, selected);
    archive->tags_dirty = TRUE;
    if (archive->tags_view && !refresh_tags(archive)) {
        archive->rebuilding = FALSE;
        SendMessageW(archive->tree, WM_SETREDRAW, TRUE, 0);
        return FALSE;
    }
    filter_archive(archive, node_date(archive->tree, selected));
    for (i = 0; i < archive->visible; ++i)
        if (entries[entry_index(archive, i)].state)
            ListView_SetItemState(archive->list, i, entries[entry_index(archive, i)].state, LVIS_SELECTED | LVIS_FOCUSED);
    archive->rebuilding = FALSE;
    SendMessageW(archive->tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(archive->tree, NULL, TRUE);
    return TRUE;
}

/* Build the small tag dictionary from the existing file snapshot: no TXT reads
   or per-note file probes. Rebuild native rows only when names/counts change. */
static BOOL refresh_tags(Archive *archive) {
    TagStore store;
    TagCatalog catalog;
    size_t i, kept = 0;
    BOOL same, rebuilding = archive->rebuilding;
    wchar_t label[128], first_name[TAG_NAME_CAP] = L"";
    HTREEITEM root, selected, first = NULL;
    TVITEMW item = {0};
    if (!archive->tags_dirty) return TRUE;
    if (!tags_store_load(NULL, archive->settings, &store)) return FALSE;
    for (i = 0; i < store.count; ++i) {
        if (find_entry(archive, store.items[i].id)) store.items[kept++] = store.items[i];
        else free(store.items[i].tags);
    }
    store.count = kept;
    if (!tags_catalog_build(&store, &catalog)) { tags_store_free(&store); return FALSE; }
    tags_store_free(&store);
    root = TreeView_GetRoot(archive->tag_tree);
    same = root != NULL && catalog.count == archive->catalog.count;
    for (i = 0; same && i < catalog.count; ++i)
        same = catalog.items[i].uses == archive->catalog.items[i].uses &&
               !wcscmp(catalog.items[i].name, archive->catalog.items[i].name);
    swprintf_s(label, 128, L"All notes (%d)", archive->count);
    if (same) {
        item.mask = TVIF_TEXT; item.hItem = root; item.pszText = label;
        TreeView_SetItem(archive->tag_tree, &item);
        tags_catalog_free(&catalog); archive->tags_dirty = FALSE; return TRUE;
    }
    item.mask = TVIF_PARAM; item.hItem = TreeView_GetFirstVisible(archive->tag_tree);
    if (item.hItem && TreeView_GetItem(archive->tag_tree, &item) && item.lParam > 0 &&
        (size_t)item.lParam <= archive->catalog.count)
        wcscpy_s(first_name, TAG_NAME_CAP, archive->catalog.items[item.lParam - 1].name);
    archive->rebuilding = TRUE;
    SendMessageW(archive->tag_tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(archive->tag_tree);
    tags_catalog_free(&archive->catalog); archive->catalog = catalog;
    {
        TVINSERTSTRUCTW insert = {0};
        insert.hParent = TVI_ROOT; insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM; insert.item.pszText = label;
        root = selected = TreeView_InsertItem(archive->tag_tree, &insert);
        for (i = 0; i < catalog.count; ++i) {
            HTREEITEM node;
            swprintf_s(label, 128, L"%s (%u)", catalog.items[i].name, catalog.items[i].uses);
            insert.hParent = root; insert.item.lParam = (LPARAM)i + 1;
            node = TreeView_InsertItem(archive->tag_tree, &insert);
            if (!wcscmp(archive->tag_filter, catalog.items[i].name)) selected = node;
            if (!wcscmp(first_name, catalog.items[i].name)) first = node;
        }
    }
    if (selected == root) archive->tag_filter[0] = 0;
    TreeView_Expand(archive->tag_tree, root, TVE_EXPAND);
    TreeView_SelectItem(archive->tag_tree, selected);
    if (first) TreeView_SelectSetFirstVisible(archive->tag_tree, first);
    SendMessageW(archive->tag_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(archive->tag_tree, NULL, TRUE);
    archive->rebuilding = rebuilding; archive->tags_dirty = FALSE;
    return TRUE;
}

static void sidebar_mode(Archive *archive, BOOL tags) {
    if (tags && !refresh_tags(archive)) {
        SetWindowTextW(archive->window, L"Notes archive - Tags could not be read. Press F5 to retry."); return;
    }
    if (archive->tags_view == tags) return;
    archive->tags_view = tags;
    ShowWindow(archive->tree, tags ? SW_HIDE : SW_SHOW);
    ShowWindow(archive->tag_tree, tags ? SW_SHOW : SW_HIDE);
    InvalidateRect(archive->tag_button, NULL, TRUE); InvalidateRect(archive->date_button, NULL, TRUE);
    filter_archive(archive, archive->filter);
}

static void load_preview(Archive *archive, ArchiveEntry *entry) {
    wchar_t path[ARCHIVE_PATH_CAP];
    char bytes[1024];
    DWORD read;
    HANDLE file;
    int count = 0, length, offset = 0, trim;
    int color = (int)GetPrivateProfileIntW(entry->id, L"color", 0, archive->settings);
    int palette = (int)GetPrivateProfileIntW(entry->id, L"palette", 0, archive->settings);
    if (palette < 0 || palette >= PALETTE_COUNT) palette = 0;
    if (color < 0 || color >= PALETTE_SIZES[palette]) color = 0;
    entry->paper = PALETTES[palette][color];
    entry->ink = palette_ink(palette,color);
    entry->bar_ink = palette_bar_ink(palette,color);
    entry->titled = GetPrivateProfileIntW(entry->id, L"has_title", FALSE, archive->settings) != 0;
    entry->loaded = TRUE;
    tags_read(archive->settings, entry->id, &entry->tags);
    if (swprintf_s(path, ARCHIVE_PATH_CAP, L"%s\\%s.txt", archive->directory, entry->id) < 0) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(file, bytes, sizeof(bytes), &read, NULL)) { CloseHandle(file); return; }
    CloseHandle(file);
    if (read >= 3 && (BYTE)bytes[0] == 0xef && (BYTE)bytes[1] == 0xbb && (BYTE)bytes[2] == 0xbf) offset = 3;
    length = (int)read - offset;
    /* A bounded read may stop in the middle of a UTF-8 code point. */
    for (trim = 0; trim <= 3 && length >= trim; ++trim) {
        count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, length - trim, NULL, 0);
        if (count || length == trim) { length -= trim; break; }
    }
    if (!count && read > (DWORD)offset) return;
    entry->preview = (wchar_t *)calloc((size_t)count + 1, sizeof(wchar_t));
    if (entry->preview && count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset, length, entry->preview, count);
}

static void load_excerpt(Archive *archive, ArchiveEntry *entry, const SearchHit *hit) {
    wchar_t path[ARCHIVE_PATH_CAP];
    char bytes[2048];
    HANDLE file;
    DWORD read, start = hit->offset > 80 ? hit->offset - 80 : 0;
    int skip = 0, trim, count = 0, before, marked;
    if (hit->offset == MAXDWORD || entry->excerpt) return;
    if (swprintf_s(path, ARCHIVE_PATH_CAP, L"%s\\%s.txt", archive->directory, entry->id) < 0) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (SetFilePointer(file, (LONG)start, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
        !ReadFile(file, bytes, sizeof(bytes), &read, NULL)) { CloseHandle(file); return; }
    CloseHandle(file);
    if (!start && read >= 3 && (BYTE)bytes[0] == 0xef && (BYTE)bytes[1] == 0xbb && (BYTE)bytes[2] == 0xbf) skip = 3;
    while ((DWORD)skip < read && ((BYTE)bytes[skip] & 0xc0) == 0x80) ++skip;
    for (trim = 0; trim <= 3 && read >= (DWORD)(skip + trim); ++trim) {
        count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + skip, (int)read - skip - trim, NULL, 0);
        if (count) break;
    }
    if (!count) return;
    entry->excerpt = (wchar_t *)calloc((size_t)count + 1, sizeof(wchar_t));
    if (!entry->excerpt) return;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + skip, (int)read - skip - trim, entry->excerpt, count);
    before = (int)(hit->offset - start) - skip;
    marked = min((int)hit->bytes, (int)read - skip - trim - before);
    entry->highlight = before > 0 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + skip, before, NULL, 0) : 0;
    entry->highlight_length = before >= 0 && marked > 0 ?
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + skip + before, marked, NULL, 0) : 0;
}

static void draw_excerpt(HDC dc, const wchar_t *text, RECT rect, int highlight, int length) {
    TEXTMETRICW metrics;
    COLORREF ink = GetTextColor(dc);
    int offset = 0, total = (int)wcslen(text), y = rect.top;
    GetTextMetricsW(dc, &metrics);
    while (offset < total && y + metrics.tmHeight <= rect.bottom) {
        int line = (int)wcscspn(text + offset, L"\r\n"), fit = 0, end, x = rect.left, part;
        SIZE size;
        GetTextExtentExPointW(dc, text + offset, line, rect.right - rect.left, &fit, NULL, &size);
        if (line && !fit) fit = 1;
        if (fit < line) {
            int space = fit;
            while (space > 0 && text[offset + space] != L' ') --space;
            if (space) fit = space;
        }
        if (fit && text[offset + fit - 1] >= 0xd800 && text[offset + fit - 1] <= 0xdbff) --fit;
        if (line && !fit) fit = min(2, line);
        end = offset + fit; part = offset;
        while (part < end) {
            BOOL marked = part >= highlight && part < highlight + length;
            int stop = marked ? min(end, highlight + length) : (part < highlight ? min(end, highlight) : end);
            SetBkColor(dc, RGB(228,196,109));
            SetBkMode(dc, marked ? OPAQUE : TRANSPARENT);
            SetTextColor(dc, marked ? RGB(0,0,0) : ink);
            ExtTextOutW(dc, x, y, ETO_CLIPPED, &rect, text + part, (UINT)(stop - part), NULL);
            SetBkMode(dc, TRANSPARENT);
            GetTextExtentPoint32W(dc, text + part, stop - part, &size); x += size.cx; part = stop;
        }
        offset = end;
        if (fit == line) {
            if (text[offset] == L'\r') ++offset;
            if (text[offset] == L'\n') ++offset;
        } else if (text[offset] == L' ') ++offset;
        y += metrics.tmHeight;
    }
    SetTextColor(dc, ink);
}

static LRESULT draw_preview(Archive *archive, NMLVCUSTOMDRAW *draw) {
    int index = (int)draw->nmcd.dwItemSpec;
    ArchiveEntry *entry;
    RECT card, bar, title, body, footer;
    HDC dc = draw->nmcd.hdc;
    HGDIOBJ previous;
    HBRUSH brush = (HBRUSH)GetStockObject(DC_BRUSH);
    const wchar_t *text, *after;
    if (index < 0 || index >= archive->visible) return CDRF_SKIPDEFAULT;
    entry = &archive->entries[entry_index(archive, index)];
    if (!entry->loaded) load_preview(archive, entry);
    ListView_GetItemRect(archive->list, index, &card, LVIR_ICON);
    InflateRect(&card, -archive_scaled(archive, 4), -archive_scaled(archive, 4));
    SetDCBrushColor(dc, entry->paper); FillRect(dc, &card, brush);
    bar = card; bar.bottom = bar.top + archive_scaled(archive, 28);
    SetDCBrushColor(dc, palette_bar(entry->paper));
    FillRect(dc, &bar, brush);
    SetDCBrushColor(dc, RGB(GetRValue(entry->paper) * 85 / 100, GetGValue(entry->paper) * 85 / 100,
                            GetBValue(entry->paper) * 85 / 100));
    FrameRect(dc, &card, brush);
    if (ListView_GetItemState(archive->list, index, LVIS_SELECTED)) {
        RECT border = card;
        SetDCBrushColor(dc, entry->ink); FrameRect(dc, &border, brush);
        InflateRect(&border, -1, -1); FrameRect(dc, &border, brush);
    }
    title = bar; InflateRect(&title, -archive_scaled(archive, 10), 0);
    body = card; InflateRect(&body, -archive_scaled(archive, 10), -archive_scaled(archive, 8));
    body.top = bar.bottom + archive_scaled(archive, 6);
    footer = body; footer.top = footer.bottom - archive_scaled(archive, 20);
    if (entry->tags) body.bottom = footer.top - archive_scaled(archive, 6);
    text = entry->preview ? entry->preview : L"Preview unavailable";
    after = text;
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, entry->bar_ink);
    previous = SelectObject(dc, archive->bold);
    if (entry->titled && *text == L'#') {
        size_t heading = wcscspn(++text, L"\r\n"); after = text + heading;
        if (*after == L'\r') ++after;
        if (*after == L'\n') ++after;
        DrawTextW(dc, text, (int)heading, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, archive->font); SetTextColor(dc,entry->ink);
    if (archive->results) load_excerpt(archive, entry, &archive->results->hits[index]);
    if (entry->excerpt) draw_excerpt(dc, entry->excerpt, body, entry->highlight, entry->highlight_length);
    else DrawTextW(dc, after, -1, &body, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (entry->tags) {
        wchar_t label[TAGS_CAP + TAGS_MAX], name[TAG_NAME_CAP];
        RECT marker = footer;
        const wchar_t *cursor = entry->tags;
        marker.right = marker.left + archive_scaled(archive, 2);
        marker.top += archive_scaled(archive, 3); marker.bottom -= archive_scaled(archive, 3);
        SetDCBrushColor(dc, entry->ink); FillRect(dc, &marker, brush);
        footer.left += archive_scaled(archive, 9);
        label[0] = 0;
        while (tags_next(&cursor, name)) {
            if (*label) wcscat_s(label, TAGS_CAP + TAGS_MAX, L"  ");
            wcscat_s(label, TAGS_CAP + TAGS_MAX, name);
        }
        DrawTextW(dc, label, -1, &footer, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, previous);
    return CDRF_SKIPDEFAULT;
}

static RECT search_bounds(const Archive *archive, const RECT *client) {
    int gap = archive_scaled(archive, 8);
    RECT field = {archive->calendar_view ? gap : archive_scaled(archive, 174) + gap * 2, gap,
                  client->right - gap * 3 - archive_scaled(archive, 134), gap + archive_scaled(archive, 34)};
    return field;
}

static void archive_layout(Archive *archive) {
    RECT rect, field;
    int gap = archive_scaled(archive, 8), tree_width = archive_scaled(archive, 174);
    int inset = archive_scaled(archive, 6), button_width = (tree_width - inset * 3) / 2;
    int navigation_top, height, i;
    HDWP batch;
    GetClientRect(archive->window, &rect);
    field = search_bounds(archive, &rect);
    navigation_top = field.bottom + gap; height = field.bottom - field.top;
    {
        const struct { HWND window; int x, y, width, height; } positions[] = {
            {archive->tag_button, gap + inset, gap + archive_scaled(archive, 3), button_width, archive_scaled(archive, 28)},
            {archive->date_button, gap + inset * 2 + button_width, gap + archive_scaled(archive, 3), button_width, archive_scaled(archive, 28)},
            {archive->tree, gap, navigation_top, tree_width, max(1, rect.bottom - navigation_top - gap)},
            {archive->tag_tree, gap, navigation_top, tree_width, max(1, rect.bottom - navigation_top - gap)},
            {archive->search, field.left + archive_scaled(archive, 30), gap + archive_scaled(archive, 8),
                max(1, field.right - field.left - archive_scaled(archive, 68)), archive_scaled(archive, 22)},
            {archive->clear, field.right - height + 2, gap + 2, height - 4, height - 4},
            {archive->location, field.right + gap, gap, archive_scaled(archive, 100), height},
            {archive->list, field.left, navigation_top, max(1, rect.right - field.left - gap), max(1, rect.bottom - navigation_top - gap)},
            {archive->calendar_button,field.right+gap*2+archive_scaled(archive,100),gap,height,height},
            {archive->calendar,gap,navigation_top,max(1,rect.right-gap*2),max(1,rect.bottom-navigation_top-gap)}
        };
        batch = BeginDeferWindowPos(10);
        for (i = 0; batch && i < 10; ++i)
            if (positions[i].window) batch = DeferWindowPos(batch, positions[i].window, NULL, positions[i].x, positions[i].y,
                                   positions[i].width, positions[i].height, SWP_NOZORDER | SWP_NOACTIVATE);
        if (!batch || !EndDeferWindowPos(batch))
            for (i = 0; i < 10; ++i)
                if (positions[i].window) SetWindowPos(positions[i].window, NULL, positions[i].x, positions[i].y,
                             positions[i].width, positions[i].height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    /* Windows preserves old client pixels on resize. The frame belongs to the
       parent, so moving the EDIT/buttons alone leaves parts of its old border. */
    rect.bottom = navigation_top;
    RedrawWindow(archive->window, &rect, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    ListView_Arrange(archive->list, LVA_DEFAULT);
}

static void calendar_mode(Archive *archive, BOOL enabled) {
    if (archive->calendar_view==enabled) return;
    if (enabled && !archive->calendar) {
        archive->calendar=calendar_create(archive->window,CALENDAR_VIEW);
        if (!archive->calendar) {
            MessageBoxW(archive->window,L"The calendar could not be opened.",L"Jotilla",MB_OK|MB_ICONERROR); return;
        }
    }
    calendar_list_close(archive->window);
    if (enabled) {
        remember_selection(archive); ListView_GetOrigin(archive->list,&archive->note_scroll);
    }
    archive->calendar_view=enabled;
    ShowWindow(archive->list,enabled ? SW_HIDE : SW_SHOW);
    ShowWindow(archive->tree,!enabled && !archive->tags_view ? SW_SHOW : SW_HIDE);
    ShowWindow(archive->tag_tree,!enabled && archive->tags_view ? SW_SHOW : SW_HIDE);
    ShowWindow(archive->tag_button,enabled ? SW_HIDE : SW_SHOW);
    ShowWindow(archive->date_button,enabled ? SW_HIDE : SW_SHOW);
    ShowWindow(archive->calendar,enabled ? SW_SHOW : SW_HIDE);
    filter_archive(archive,archive->filter);
    archive_layout(archive);
    if (!enabled) {
        int i;
        for(i=0;i<archive->visible;++i) {
            UINT state=archive->entries[entry_index(archive,i)].state;
            if(state) ListView_SetItemState(archive->list,i,state,LVIS_SELECTED|LVIS_FOCUSED);
        }
        ListView_Scroll(archive->list,archive->note_scroll.x,archive->note_scroll.y);
    }
    SetWindowTextW(archive->calendar_button,enabled ? L"Show notes" : L"Show calendar");
    InvalidateRect(archive->window,NULL,TRUE);
    SetFocus(enabled ? archive->calendar : archive->list);
}

static void archive_fonts(Archive *archive) {
    HFONT old_font = archive->font, old_bold = archive->bold;
    archive->dpi = (int)GetDpiForWindow(archive->window);
    if (!archive->dpi) archive->dpi = 96;
    archive->font = CreateFontW(-MulDiv(10, archive->dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    archive->bold = CreateFontW(-MulDiv(11, archive->dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(archive->tree, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->tag_tree, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->tag_button, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->date_button, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->list, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->search, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->clear, WM_SETFONT, (WPARAM)archive->font, TRUE);
    SendMessageW(archive->location, WM_SETFONT, (WPARAM)archive->font, TRUE);
    /* One shared image defines native tile geometry and hit testing. Content
       is drawn directly; there is no bitmap or child editor per post-it. */
    if (!archive->tiles) {
        archive->tiles = ImageList_Create(archive_scaled(archive, 220), archive_scaled(archive, 170), ILC_COLOR32, 1, 1);
    } else ImageList_SetIconSize(archive->tiles, archive_scaled(archive, 220), archive_scaled(archive, 170));
    ImageList_SetImageCount(archive->tiles, 1);
    /* ListView caches image dimensions. Resizing an attached shared image list
       alone leaves old hit/draw bounds after a DPI change, overlapping rows. */
    ListView_SetImageList(archive->list, archive->tiles, LVSIL_NORMAL);
    ListView_SetIconSpacing(archive->list, archive_scaled(archive, 236), archive_scaled(archive, 196));
    if (old_font) DeleteObject(old_font);
    if (old_bold) DeleteObject(old_bold);
}

static void open_archive_item(Archive *archive, int index) {
    if (index >= 0 && index < archive->visible && !archive->actions.open(archive->entries[entry_index(archive, index)].id))
        MessageBoxW(archive->window, L"This note could not be opened.", L"Jotilla", MB_OK | MB_ICONERROR);
}

static void delete_archive_selection(Archive *archive) {
    int index = -1, count = (int)ListView_GetSelectedCount(archive->list), failures = 0;
    wchar_t message[128];
    if (!count) return;
    swprintf_s(message, 128, L"Delete %d selected note%s from the archive?", count, count == 1 ? L"" : L"s");
    if (MessageBoxW(archive->window, message, L"Jotilla", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    while ((index = ListView_GetNextItem(archive->list, index, LVNI_SELECTED)) >= 0)
        if (!archive->actions.remove(archive->entries[entry_index(archive, index)].id)) ++failures;
    if (!refresh_archive(archive, FALSE) || failures)
        MessageBoxW(archive->window, L"Some notes could not be deleted. Check that their files are writable.",
                    L"Jotilla", MB_OK | MB_ICONERROR);
}

static void archive_context_menu(Archive *archive, POINT point) {
    HMENU menu;
    UINT command;
    if (!ListView_GetSelectedCount(archive->list)) return;
    menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, DELETE_NOTES, L"Delete from archive");
    command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, archive->window, NULL);
    DestroyMenu(menu);
    if (command == DELETE_NOTES) delete_archive_selection(archive);
}

static void search_status(Archive *archive) {
    wchar_t label[192];
    if (archive->searching) wcscpy_s(label, 192, L"Notes archive - Searching");
    else if (archive->results && archive->results->error)
        swprintf_s(label, 192, L"Notes archive - %s", archive->results->error);
    else if (archive->results && archive->results->unreadable)
        swprintf_s(label, 192, L"Notes archive - %d unreadable; F5 to retry", archive->results->unreadable);
    else wcscpy_s(label, 192, archive->calendar_view ?
        (GetWindowTextLengthW(archive->search) ? L"Calendar - Filtered notes" : L"Calendar") : L"Notes archive");
    SetWindowTextW(archive->window, label);
}

static void schedule_search(Archive *archive) {
    wchar_t query[SEARCH_CAP], *text = query;
    KillTimer(archive->window, SEARCH_TIMER);
    if (archive->worker) search_cancel(archive->worker);
    discard_results(archive);
    archive->recheck = -1;
    GetWindowTextW(archive->search, query, SEARCH_CAP);
    while (iswspace(*text)) ++text;
    archive->searching = *text != 0 || (!archive->calendar_view && archive->tags_view && *archive->tag_filter);
    archive->visible = archive->searching ? 0 : archive->range;
    ListView_SetItemCountEx(archive->list, archive->visible, LVSICF_NOSCROLL);
    InvalidateRect(archive->list, NULL, TRUE);
    if (archive->searching) SetTimer(archive->window, SEARCH_TIMER, 150, NULL);
    search_status(archive);
    calendar_update(archive);
}

static void start_search(Archive *archive) {
    SearchItem *items = NULL;
    wchar_t query[SEARCH_CAP];
    int i;
    BOOL ok = FALSE;
    KillTimer(archive->window, SEARCH_TIMER);
    GetWindowTextW(archive->search, query, SEARCH_CAP);
    if (!archive->worker) archive->worker = search_create(archive->window, ARCHIVE_SEARCH_DONE);
    if (archive->range) items = (SearchItem *)malloc((size_t)archive->range * sizeof(*items));
    if (archive->worker && (!archive->range || items)) {
        for (i = 0; i < archive->range; ++i) {
            items[i].index = archive->first + i;
            wcscpy_s(items[i].id, 80, archive->entries[items[i].index].id);
        }
        ok = search_submit_filtered(archive->worker, archive->directory, archive->settings, query,
                                    !archive->calendar_view && archive->tags_view ? archive->tag_filter : L"", items, archive->range);
    }
    free(items);
    if (!ok) { archive->searching = FALSE; SetWindowTextW(archive->window, L"Notes archive - Search could not start. Press F5 to retry."); }
}

static void finish_search(Archive *archive) {
    SearchResult *result = search_take(archive->worker);
    int i;
    if (!result) return;
    if (archive->recheck >= 0 && archive->results) {
        SearchResult *current = archive->results;
        int low = 0, high = current->count;
        while (low < high) {
            int middle = low + (high - low) / 2;
            if (current->hits[middle].index < archive->recheck) low = middle + 1;
            else high = middle;
        }
        if (!result->error && !result->unreadable) {
            BOOL present = low < current->count && current->hits[low].index == archive->recheck;
            if (present && result->count) current->hits[low] = result->hits[0];
            else if (present) {
                memmove(current->hits + low, current->hits + low + 1,
                        (size_t)(current->count - low - 1) * sizeof(*current->hits));
                --current->count;
            } else if (result->count) {
                /* Full searches reserve the candidate count, so live edits
                   normally update the sorted result vector without allocating. */
                if (current->count == current->capacity) {
                    SearchHit *hits = (SearchHit *)realloc(current->hits, (size_t)(current->count + 1) * sizeof(*hits));
                    if (hits) { current->hits = hits; ++current->capacity; }
                    else result->error = L"Not enough memory to update results. Press F5.";
                }
                if (!result->error) {
                    if (low < current->count)
                        memmove(current->hits + low + 1, current->hits + low,
                                (size_t)(current->count - low) * sizeof(*current->hits));
                    current->hits[low] = result->hits[0]; ++current->count;
                }
            }
        }
        current->error = result->error; current->unreadable = result->unreadable;
        search_result_free(result);
    } else {
        discard_results(archive); archive->results = result;
    }
    archive->recheck = -1; archive->searching = FALSE;
    archive->visible = archive->results->count;
    ListView_SetItemCountEx(archive->list, archive->visible, LVSICF_NOSCROLL);
    ListView_SetItemState(archive->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (i = 0; i < archive->visible; ++i) {
        UINT state = archive->entries[entry_index(archive, i)].state;
        if (state) ListView_SetItemState(archive->list, i, state, LVIS_SELECTED | LVIS_FOCUSED);
    }
    ListView_Arrange(archive->list, LVA_DEFAULT); InvalidateRect(archive->list, NULL, TRUE);
    search_status(archive);
    calendar_update(archive);
}

static void recheck_note(Archive *archive, int index) {
    SearchItem item;
    wchar_t query[SEARCH_CAP];
    if (index < archive->first || index >= archive->first + archive->range) return;
    if (!archive->searching && archive->results && !archive->results->error && !archive->results->unreadable) {
        remember_selection(archive);
        wcscpy_s(item.id, 80, archive->entries[index].id); item.index = index;
        GetWindowTextW(archive->search, query, SEARCH_CAP);
        if (search_submit_filtered(archive->worker, archive->directory, archive->settings, query,
                                   !archive->calendar_view && archive->tags_view ? archive->tag_filter : L"", &item, 1)) {
            archive->recheck = index; archive->searching = TRUE; search_status(archive); return;
        }
    }
    schedule_search(archive);
}

static LRESULT CALLBACK archive_input_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR subclass, DWORD_PTR data) {
    Archive *archive = (Archive *)data;
    (void)subclass;
    if (message == WM_KEYDOWN) {
        if (wparam == 'F' && GetKeyState(VK_CONTROL) < 0) {
            SetFocus(archive->search); SendMessageW(archive->search, EM_SETSEL, 0, -1); return 0;
        }
        if (wparam == VK_ESCAPE) {
            if (GetWindowTextLengthW(archive->search)) SetWindowTextW(archive->search, L"");
            SetFocus(archive->calendar_view ? archive->calendar : archive->list); return 0;
        }
        if (wparam == VK_F5) {
            if (!refresh_archive(archive, TRUE)) SetWindowTextW(archive->window, L"Notes archive - Refresh failed. Press F5 to retry.");
            return 0;
        }
        if (window == archive->search && (wparam == VK_RETURN || wparam == VK_DOWN)) {
            if (archive->calendar_view) { SetFocus(archive->calendar); return 0; }
            if (archive->visible) {
                if (wparam == VK_RETURN) open_archive_item(archive, 0);
                else {
                    SetFocus(archive->list);
                    ListView_SetItemState(archive->list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            return 0;
        }
        if (wparam == VK_TAB) {
            HWND controls[] = {archive->search,archive->clear,archive->location,archive->calendar_button,
                archive->calendar_view ? archive->calendar : archive->tag_button,archive->date_button,
                archive->tags_view ? archive->tag_tree : archive->tree,archive->list};
            int count=archive->calendar_view ? 5 : 8, i, direction=GetKeyState(VK_SHIFT)<0 ? count-1 : 1;
            for (i=0;i<count;++i) if (window==controls[i]) { SetFocus(controls[(i+direction)%count]); break; }
            return 0;
        }
    }
    if (message == WM_CHAR && (wparam == L'\r' || wparam == L'\t' || wparam == 27 ||
        (wparam == 6 && GetKeyState(VK_CONTROL) < 0))) return 0;
    return DefSubclassProc(window, message, wparam, lparam);
}

static BOOL click_tag(Archive *archive, const NMITEMACTIVATE *click) {
    ArchiveEntry *entry;
    RECT card;
    HDC dc;
    HGDIOBJ font;
    const wchar_t *cursor;
    wchar_t name[TAG_NAME_CAP], label[TAG_NAME_CAP + 4];
    int left;
    if (click->iItem < 0 || click->iItem >= archive->visible) return FALSE;
    entry = &archive->entries[entry_index(archive, click->iItem)];
    if (!entry->tags) return FALSE;
    ListView_GetItemRect(archive->list, click->iItem, &card, LVIR_ICON);
    if (click->ptAction.y < card.bottom - archive_scaled(archive, 32) ||
        click->ptAction.y >= card.bottom - archive_scaled(archive, 12) ||
        click->ptAction.x >= card.right - archive_scaled(archive, 14)) return FALSE;
    left = card.left + archive_scaled(archive, 23);
    dc = GetDC(archive->list); font = SelectObject(dc, archive->font); cursor = entry->tags;
    while (tags_next(&cursor, name)) {
        SIZE size;
        swprintf_s(label, TAG_NAME_CAP + 4, L"%s  ", name);
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &size);
        if (click->ptAction.x >= left && click->ptAction.x < left + size.cx) {
            HTREEITEM node;
            /* Copy before switching views: refreshing the result map may free previews. */
            SelectObject(dc, font); ReleaseDC(archive->list, dc);
            sidebar_mode(archive, TRUE);
            for (node = TreeView_GetChild(archive->tag_tree, TreeView_GetRoot(archive->tag_tree)); node;
                 node = TreeView_GetNextSibling(archive->tag_tree, node)) {
                unsigned tag = node_date(archive->tag_tree, node);
                if (tag && tag <= archive->catalog.count && !wcscmp(archive->catalog.items[tag - 1].name, name)) {
                    TreeView_SelectItem(archive->tag_tree, node); break;
                }
            }
            return TRUE;
        }
        left += size.cx;
    }
    SelectObject(dc, font); ReleaseDC(archive->list, dc); return FALSE;
}

static LRESULT CALLBACK archive_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Archive *archive = (Archive *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        archive = (Archive *)((CREATESTRUCTW *)lparam)->lpCreateParams;
        archive->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)archive);
    }
    if (!archive) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        HINSTANCE app = GetModuleHandleW(NULL);
        COLORREF caption = ARCHIVE_PAPER, ink = ARCHIVE_INK;
        archive->background = CreateSolidBrush(ARCHIVE_PAPER);
        archive->sidebar = CreateSolidBrush(ARCHIVE_SIDEBAR);
        if (!archive->background || !archive->sidebar) return -1;
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &ink, sizeof(ink));
        archive->tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)1, app, NULL);
        archive->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_ICON | LVS_OWNERDATA | LVS_AUTOARRANGE | LVS_NOLABELWRAP | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)2, app, NULL);
        archive->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)3, app, NULL);
        archive->clear = CreateWindowExW(0, L"BUTTON", L"Clear search", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)4, app, NULL);
        archive->tag_tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS |
            TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)TAG_TREE, app, NULL);
        archive->tag_button = CreateWindowExW(0, L"BUTTON", L"Tags", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)TAG_BUTTON, app, NULL);
        archive->date_button = CreateWindowExW(0, L"BUTTON", L"Date", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)DATE_BUTTON, app, NULL);
        archive->location = CreateWindowExW(0, L"BUTTON", L"Archive", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, window, (HMENU)(INT_PTR)LOCATION_BUTTON, app, NULL);
        archive->calendar_button=CreateWindowExW(0,L"BUTTON",L"Show calendar",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
            0,0,1,1,window,(HMENU)(INT_PTR)CALENDAR_BUTTON,app,NULL);
        if (!archive->tree || !archive->list || !archive->search || !archive->clear ||
            !archive->tag_tree || !archive->tag_button || !archive->date_button || !archive->location ||
            !archive->calendar_button) return -1;
        SendMessageW(archive->search, EM_SETLIMITTEXT, SEARCH_CAP - 1, 0);
        SendMessageW(archive->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search notes and tags");
        {
            HWND controls[] = {archive->search, archive->clear, archive->tree, archive->list,
                               archive->tag_tree, archive->tag_button, archive->date_button, archive->location};
            HWND trees[] = {archive->tree, archive->tag_tree};
            int i;
            for (i = 0; i < 8; ++i) SetWindowSubclass(controls[i], archive_input_proc, 1, (DWORD_PTR)archive);
            for (i = 0; i < 2; ++i) {
                SetWindowTheme(trees[i], L"", L"");
                TreeView_SetBkColor(trees[i], ARCHIVE_SIDEBAR);
                TreeView_SetTextColor(trees[i], ARCHIVE_INK);
                TreeView_SetLineColor(trees[i], RGB(190,177,151));
            }
        }
        ListView_SetBkColor(archive->list, ARCHIVE_PAPER);
        ListView_SetTextBkColor(archive->list, ARCHIVE_PAPER);
        ListView_SetTextColor(archive->list, ARCHIVE_INK);
        ListView_SetExtendedListViewStyle(archive->list, LVS_EX_DOUBLEBUFFER);
        archive_fonts(archive);
        SetWindowSubclass(archive->calendar_button,archive_input_proc,1,(DWORD_PTR)archive);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(window, &rect);
        FillRect((HDC)wparam, &rect, archive->background);
        rect.left = rect.top = archive_scaled(archive, 8);
        rect.right = rect.left + archive_scaled(archive, 174); rect.bottom -= rect.top;
        if (!archive->calendar_view) FillRect((HDC)wparam, &rect, archive->sidebar);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        RECT rect, field;
        HDC dc = BeginPaint(window, &paint);
        int gap = archive_scaled(archive, 8), left;
        HGDIOBJ pen = SelectObject(dc, GetStockObject(DC_PEN));
        HGDIOBJ brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        GetClientRect(window, &rect); field = search_bounds(archive, &rect);
        left=field.left;
        SetDCPenColor(dc, RGB(190,177,151)); Rectangle(dc, field.left, field.top, field.right, field.bottom);
        SetDCPenColor(dc, ARCHIVE_INK);
        Ellipse(dc, left + archive_scaled(archive, 9), gap + archive_scaled(archive, 10),
                left + archive_scaled(archive, 19), gap + archive_scaled(archive, 20));
        MoveToEx(dc, left + archive_scaled(archive, 18), gap + archive_scaled(archive, 19), NULL);
        LineTo(dc, left + archive_scaled(archive, 23), gap + archive_scaled(archive, 24));
        SelectObject(dc, brush); SelectObject(dc, pen); EndPaint(window, &paint); return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *draw = (DRAWITEMSTRUCT *)lparam;
        RECT rect = draw->rcItem;
        BOOL mode = draw->CtlID == TAG_BUTTON || draw->CtlID == DATE_BUTTON;
        BOOL calendar = draw->CtlID == CALENDAR_BUTTON;
        BOOL location = draw->CtlID == LOCATION_BUTTON || calendar;
        BOOL active = calendar ? archive->calendar_view : mode && ((draw->CtlID == TAG_BUTTON) == archive->tags_view);
        HGDIOBJ font = SelectObject(draw->hDC, active ? archive->bold : archive->font);
        const wchar_t *text = location ? L"Archive" : mode ? (draw->CtlID == TAG_BUTTON ? L"Tags" : L"Date") : L"×";
        HBRUSH brush = (HBRUSH)GetStockObject(DC_BRUSH);
        SetDCBrushColor(draw->hDC, active || (draw->itemState & ODS_SELECTED) ? ARCHIVE_SELECTION :
                                  mode || location ? ARCHIVE_SIDEBAR : ARCHIVE_PAPER);
        FillRect(draw->hDC, &rect, brush);
        if (mode || location) {
            SetDCBrushColor(draw->hDC, RGB(190,177,151)); FrameRect(draw->hDC, &rect, brush);
        }
        SetTextColor(draw->hDC, ARCHIVE_INK); SetBkMode(draw->hDC, TRANSPARENT);
        if (calendar) calendar_icon(draw->hDC,rect,ARCHIVE_INK,archive->calendar_view);
        else DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (draw->itemState & ODS_FOCUS) { InflateRect(&rect, -3, -3); DrawFocusRect(draw->hDC, &rect); }
        SelectObject(draw->hDC, font);
        return TRUE;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wparam, ARCHIVE_INK); SetBkColor((HDC)wparam, ARCHIVE_PAPER);
        return (LRESULT)archive->background;
    case WM_COMMAND:
        if (LOWORD(wparam)==CALENDAR_BUTTON) { calendar_mode(archive,!archive->calendar_view); return 0; }
        if (LOWORD(wparam) == LOCATION_BUTTON && archive->actions.location) {
            archive->actions.location(window); return 0;
        }
        if (LOWORD(wparam) == TAG_BUTTON || LOWORD(wparam) == DATE_BUTTON) {
            sidebar_mode(archive, LOWORD(wparam) == TAG_BUTTON); return 0;
        }
        if (LOWORD(wparam) == 3 && HIWORD(wparam) == EN_CHANGE) {
            if (!archive->searching) remember_selection(archive);
            schedule_search(archive); return 0;
        }
        if (LOWORD(wparam) == 4) { SetWindowTextW(archive->search, L""); SetFocus(archive->search); return 0; }
        break;
    case WM_KEYDOWN:
        if (wparam==VK_F5) { if(!refresh_archive(archive,TRUE)) SetWindowTextW(window,L"Refresh failed. Press F5 to retry."); return 0; }
        if (wparam == 'F' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
            SetFocus(archive->search); SendMessageW(archive->search, EM_SETSEL, 0, -1); return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == 6 && GetKeyState(VK_CONTROL) < 0) return 0;
        break;
    case WM_TIMER:
        if (wparam == SEARCH_TIMER) start_search(archive);
        return 0;
    case ARCHIVE_SEARCH_DONE:
        if (archive->worker) finish_search(archive);
        return 0;
    case WM_MOVE:
        if (!IsIconic(window) && !IsZoomed(window)) {
            GetWindowRect(window, &archive->normal_rect);
            archive->has_rect = TRUE;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (!IsIconic(window) && !IsZoomed(window)) {
            GetWindowRect(window, &archive->normal_rect);
            archive->has_rect = TRUE;
        }
        save_browser_placement(archive);
        return 0;
    case WM_SIZE:
        /* Keep query, selection and native placement alive in the tray. */
        if (wparam == SIZE_MINIMIZED) {
            save_browser_placement(archive);
            ShowWindow(window, SW_HIDE);
        } else {
            if (wparam == SIZE_MAXIMIZED) archive->maximized = TRUE;
            else if (wparam == SIZE_RESTORED) {
                archive->maximized = FALSE;
                GetWindowRect(window, &archive->normal_rect);
                archive->has_rect = TRUE;
            }
            archive_layout(archive);
        }
        return 0;
    case ARCHIVE_REFRESH:
        if (archive->refresh_pending) refresh_archive(archive, FALSE);
        return 0;
    case ARCHIVE_TAGS_REFRESH:
        archive->tags_pending = FALSE;
        if (archive->tags_view && !archive->calendar_view && !archive->refresh_pending) {
            wchar_t previous[TAG_NAME_CAP];
            wcscpy_s(previous, TAG_NAME_CAP, archive->tag_filter);
            if (!refresh_tags(archive)) SetWindowTextW(window, L"Notes archive - Tags could not be read. Press F5 to retry.");
            else if (wcscmp(previous, archive->tag_filter)) schedule_search(archive);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = archive_scaled(archive, 520);
        info->ptMinTrackSize.y = archive_scaled(archive, 320);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        archive_fonts(archive);
        SetWindowPos(window, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lparam;
        if (header->hwndFrom==archive->calendar) {
            if (header->code==CALENDAR_YEAR) calendar_update(archive);
            if (header->code==CALENDAR_DAY) calendar_open_day(archive,((CalendarEvent *)lparam)->date);
            if (header->code==CALENDAR_LEAVE) SetFocus(((CalendarEvent *)lparam)->date ? archive->calendar_button : archive->search);
            return 0;
        }
        if (header->hwndFrom == archive->tree || header->hwndFrom == archive->tag_tree) {
            if (header->code == TVN_SELCHANGEDW && !archive->rebuilding) {
                unsigned value = (unsigned)((NMTREEVIEWW *)header)->itemNew.lParam;
                if (header->hwndFrom == archive->tree) filter_archive(archive, value);
                else if (archive->tags_view && value <= archive->catalog.count) {
                    wcscpy_s(archive->tag_filter, TAG_NAME_CAP, value ? archive->catalog.items[value - 1].name : L"");
                    filter_archive(archive, archive->filter);
                }
            }
            if (header->code == NM_CUSTOMDRAW) {
                NMTVCUSTOMDRAW *draw = (NMTVCUSTOMDRAW *)header;
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    unsigned tag = (unsigned)draw->nmcd.lItemlParam;
                    draw->clrText = ARCHIVE_INK;
                    draw->clrTextBk = (HTREEITEM)draw->nmcd.dwItemSpec == TreeView_GetSelection(header->hwndFrom) ?
                        ARCHIVE_SELECTION : ARCHIVE_SIDEBAR;
                    if (header->hwndFrom == archive->tag_tree && tag && tag <= archive->catalog.count) {
                        RECT row, name, count, client;
                        wchar_t number[16];
                        SIZE size;
                        HDC dc = draw->nmcd.hdc;
                        HGDIOBJ font = SelectObject(dc, archive->font);
                        HBRUSH brush = (HBRUSH)GetStockObject(DC_BRUSH);
                        GetClientRect(header->hwndFrom, &client);
                        TreeView_GetItemRect(header->hwndFrom, (HTREEITEM)draw->nmcd.dwItemSpec, &row, FALSE);
                        TreeView_GetItemRect(header->hwndFrom, (HTREEITEM)draw->nmcd.dwItemSpec, &name, TRUE);
                        row.right = client.right;
                        SetDCBrushColor(dc, draw->clrTextBk); FillRect(dc, &row, brush);
                        swprintf_s(number, 16, L"%u", archive->catalog.items[tag - 1].uses);
                        GetTextExtentPoint32W(dc, number, (int)wcslen(number), &size);
                        count = row; count.right -= archive_scaled(archive, 10); count.left = count.right - size.cx;
                        name.right = max(name.left, count.left - archive_scaled(archive, 10));
                        SetTextColor(dc, ARCHIVE_INK); SetBkMode(dc, TRANSPARENT);
                        DrawTextW(dc, archive->catalog.items[tag - 1].name, -1, &name,
                                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                        DrawTextW(dc, number, -1, &count, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
                        SelectObject(dc, font); return CDRF_SKIPDEFAULT;
                    }
                    draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                    return CDRF_NEWFONT;
                }
            }
            return 0;
        }
        if (header->hwndFrom != archive->list) return 0;
        switch (header->code) {
        case NM_CUSTOMDRAW: {
            NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)header;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return draw_preview(archive, draw);
            break;
        }
        case LVN_GETDISPINFOW: {
            NMLVDISPINFOW *info = (NMLVDISPINFOW *)header;
            if (info->item.mask & LVIF_IMAGE) info->item.iImage = 0;
            if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 && info->item.iItem < archive->visible) {
                ArchiveEntry *entry = &archive->entries[entry_index(archive, info->item.iItem)];
                const wchar_t *text;
                size_t length;
                if (!entry->loaded) load_preview(archive, entry);
                text = entry->preview ? entry->preview : L"Preview unavailable";
                if (entry->titled && *text == L'#') ++text;
                length = wcscspn(text, L"\r\n");
                if (info->item.cchTextMax > 0)
                    wcsncpy_s(info->item.pszText, (size_t)info->item.cchTextMax, text,
                              min(length, (size_t)info->item.cchTextMax - 1));
            }
            return 0;
        }
        case LVN_GETEMPTYMARKUP:
            wcscpy_s(((NMLVEMPTYMARKUP *)header)->szMarkup, L_MAX_URL_LENGTH,
                     archive->searching ? L"Searching" : archive->results && archive->results->error ? archive->results->error :
                     GetWindowTextLengthW(archive->search) || (archive->tags_view && *archive->tag_filter) ? L"No matching notes" : L"No notes");
            return TRUE;
        case NM_CLICK:
            if (GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0 &&
                !click_tag(archive, (NMITEMACTIVATE *)header))
                open_archive_item(archive, ((NMITEMACTIVATE *)header)->iItem);
            return 0;
        case NM_RCLICK: {
            POINT point;
            GetCursorPos(&point); archive_context_menu(archive, point); return 0;
        }
        case LVN_KEYDOWN: {
            WORD key = ((NMLVKEYDOWN *)header)->wVKey;
            if (key == VK_RETURN) open_archive_item(archive, ListView_GetNextItem(archive->list, -1, LVNI_FOCUSED));
            if (key == VK_DELETE) delete_archive_selection(archive);
            if (key == VK_F5 && !refresh_archive(archive, TRUE))
                MessageBoxW(window, L"The archive could not be refreshed.", L"Jotilla", MB_OK | MB_ICONERROR);
            if (key == 'A' && GetKeyState(VK_CONTROL) < 0)
                ListView_SetItemState(archive->list, -1, LVIS_SELECTED, LVIS_SELECTED);
            return 0;
        }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wparam == archive->list && GET_X_LPARAM(lparam) == -1 && GET_Y_LPARAM(lparam) == -1) {
            RECT rect;
            int selected = ListView_GetNextItem(archive->list, -1, LVNI_SELECTED);
            if (selected >= 0 && ListView_GetItemRect(archive->list, selected, &rect, LVIR_BOUNDS)) {
                POINT point = {rect.left + archive_scaled(archive, 16), rect.top + archive_scaled(archive, 16)};
                ClientToScreen(archive->list, &point); archive_context_menu(archive, point);
            }
            return 0;
        }
        break;
    case WM_CLOSE: save_browser_placement(archive); DestroyWindow(window); return 0;
    case WM_DESTROY: save_browser_placement(archive); return 0;
    case WM_NCDESTROY:
        calendar_list_close(window); free(archive->day_entries);
        search_destroy(archive->worker);
        search_result_free(archive->results);
        if (archive->tiles) ImageList_Destroy(archive->tiles);
        if (archive->font) DeleteObject(archive->font);
        if (archive->bold) DeleteObject(archive->bold);
        if (archive->background) DeleteObject(archive->background);
        if (archive->sidebar) DeleteObject(archive->sidebar);
        tags_catalog_free(&archive->catalog);
        free_entries(archive->entries, archive->count);
        archive_window = NULL;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0); free(archive);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND archive_show(HWND owner, const wchar_t *directory, const wchar_t *settings, const ArchiveActions *actions) {
    static ATOM window_class;
    Archive *archive;
    MONITORINFO monitor = {sizeof(monitor)};
    int dpi, width, height, x, y;
    BOOL maximized = FALSE;
    if (archive_window) {
        archive = (Archive *)GetWindowLongPtrW(archive_window, GWLP_USERDATA);
        if (!refresh_archive(archive, TRUE)) return NULL;
        ShowWindow(archive_window, IsIconic(archive_window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(archive_window); return archive_window;
    }
    if (!window_class) {
        WNDCLASSW cls = {0};
        INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES};
        if (!InitCommonControlsEx(&controls)) return NULL;
        cls.lpfnWndProc = archive_proc; cls.hInstance = GetModuleHandleW(NULL);
        cls.hIcon = (HICON)GetClassLongPtrW(owner, GCLP_HICON);
        cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
        cls.lpszClassName = ARCHIVE_CLASS;
        window_class = RegisterClassW(&cls);
        if (!window_class) return NULL;
    }
    archive = (Archive *)calloc(1, sizeof(*archive));
    if (!archive) return NULL;
    archive->directory = directory; archive->settings = settings; archive->actions = *actions;
    archive->recheck = -1;
    dpi = (int)GetDpiForWindow(owner); if (!dpi) dpi = 96;
    archive->dpi = dpi;
    if (!load_browser_placement(owner, settings, dpi, &x, &y, &width, &height, &maximized)) {
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        width = min(MulDiv(800, dpi, 96), monitor.rcWork.right - monitor.rcWork.left);
        height = min(MulDiv(580, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top);
        x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
        y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    }
    archive_window = CreateWindowExW(archive_topmost ? WS_EX_TOPMOST : 0, ARCHIVE_CLASS, L"Notes archive",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, owner, NULL, GetModuleHandleW(NULL), archive);
    /* WM_NCDESTROY owns the allocation once WM_NCCREATE succeeds. */
    if (!archive_window) return NULL;
    SetRect(&archive->normal_rect, x, y, x + width, y + height);
    archive->has_rect = TRUE;
    archive->maximized = maximized;
    if (!refresh_archive(archive, TRUE)) { archive_close(); return NULL; }
    ShowWindow(archive_window, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL); SetForegroundWindow(archive_window);
    SetFocus(archive->list);
    return archive_window;
}

/* Called only after a successful save/removal, on the UI thread. Editing
   invalidates one lazy preview; membership changes share one queued refresh. */
void archive_changed(const wchar_t *id, BOOL present, unsigned changes) {
    Archive *archive;
    ArchiveEntry *entry;
    int i;
    if (!archive_window) return;
    archive = (Archive *)GetWindowLongPtrW(archive_window, GWLP_USERDATA);
    calendar_list_close(archive_window);
    if (!present) archive->calendar_dirty=TRUE;
    entry = find_entry(archive, id);
    if ((changes & ARCHIVE_TAGS_CHANGED) || !entry || !present) archive->tags_dirty = TRUE;
    if (changes & ARCHIVE_TAGS_CHANGED) search_reload(archive->worker);
    if (archive->tags_dirty && archive->tags_view && !archive->rebuilding && !archive->tags_pending)
        archive->tags_pending = PostMessageW(archive_window, ARCHIVE_TAGS_REFRESH, 0, 0);
    if (entry && present) {
        i = (int)(entry - archive->entries);
        free(entry->preview); entry->preview = NULL; entry->loaded = FALSE;
        free(entry->tags); entry->tags = NULL;
        free(entry->excerpt); entry->excerpt = NULL;
        if (archive->calendar_view && archive->calendar_dirty && !archive->rebuilding) calendar_update(archive);
        if ((GetWindowTextLengthW(archive->search) || (!archive->calendar_view && archive->tags_view && *archive->tag_filter)) && !archive->rebuilding) {
            recheck_note(archive, i); return;
        }
        if (i >= archive->first && i < archive->first + archive->visible)
            ListView_RedrawItems(archive->list, i - archive->first, i - archive->first);
        return;
    }
    if (!entry && !present) return;
    if (!archive->rebuilding && !archive->refresh_pending)
        archive->refresh_pending = PostMessageW(archive_window, ARCHIVE_REFRESH, 0, 0);
}

/* The successful writer already knows the date. Keep previews and search
   results; only calendar membership changed. F5 still reloads external edits. */
void archive_date_changed(const wchar_t *id, unsigned date) {
    Archive *archive;
    ArchiveEntry *entry;
    if (!archive_window) return;
    archive=(Archive *)GetWindowLongPtrW(archive_window,GWLP_USERDATA);
    entry=find_entry(archive,id);
    if (!entry) {
        archive->calendar_dirty=TRUE; archive_changed(id,TRUE,0); return;
    }
    entry->scheduled=date;
    if (!archive->rebuilding) calendar_update(archive);
}

void archive_close(void) {
    if (archive_window) DestroyWindow(archive_window);
}

void archive_hide(void) {
    if (archive_window && IsWindow(archive_window) && IsWindowVisible(archive_window))
        ShowWindow(archive_window, SW_HIDE);
}

void archive_set_topmost(BOOL topmost) {
    archive_topmost = topmost;
    if (archive_window && IsWindow(archive_window))
        SetWindowPos(archive_window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void archive_search(void) {
    Archive *archive;
    if (!archive_window) return;
    archive = (Archive *)GetWindowLongPtrW(archive_window, GWLP_USERDATA);
    calendar_mode(archive,FALSE);
    sidebar_mode(archive, FALSE);
    TreeView_SelectItem(archive->tree, TreeView_GetRoot(archive->tree));
    SetFocus(archive->search); SendMessageW(archive->search, EM_SETSEL, 0, -1);
}

void archive_notes(void) {
    if (archive_window) calendar_mode((Archive *)GetWindowLongPtrW(archive_window,GWLP_USERDATA),FALSE);
}
