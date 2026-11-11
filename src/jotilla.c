#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <richedit.h>
#include <commdlg.h>
#include <commctrl.h>
#include <tom.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
#include "resource.h"
#include "archive.h"
#include "colors.h"
#include "tags.h"
#include "location.h"
#include "palette_picker.h"
#include "calendar.h"

#define APP_CLASS APP_CONTROLLER
#define NOTE_CLASS APP_NOTE
#define PATH_CAP 1024
#define TEXT_LIMIT (16 * 1024 * 1024)
#define SAVE_TIMER 1
#define TRAY_CLICK_TIMER 2
#define TRAY_MESSAGE (WM_APP + 1)
#define NEW_NOTE_MESSAGE (WM_APP + 2)
#define NOTE_DATE_MESSAGE (WM_APP + 3)
#define EXIT_COMMAND 1
#define NOTE_WIDTH 300
#define NOTE_HEIGHT 240
#define MIN_WIDTH 160
#define MIN_HEIGHT 100
#define MAX_SIZE 4096
#define DIRTY_TEXT 1u
#define DIRTY_RICH 2u
#define CMD_ARCHIVE 100
#define CMD_TITLE 101
#define CMD_BROWSE 102
#define CMD_TAGS 103
#define CMD_SCROLL 104
#define CMD_TOPMOST 105
#define CMD_HIDE_ALL 106
#define CMD_SHOW_ALL 107
#define CMD_OPACITY 200
#define CMD_SIZE 300
#define CMD_PALETTE 400

typedef struct Note Note;
enum {
    LAYOUT_X, LAYOUT_Y, LAYOUT_COLOR, LAYOUT_TITLE, LAYOUT_COLLAPSED, LAYOUT_OPEN,
    LAYOUT_PALETTE, LAYOUT_SIZE, LAYOUT_OPACITY, LAYOUT_WIDTH, LAYOUT_HEIGHT, LAYOUT_CUSTOM_INK, LAYOUT_SCROLL, LAYOUT_COUNT
};
static const wchar_t *LAYOUT_KEYS[LAYOUT_COUNT] = {
    L"x", L"y", L"color", L"has_title", L"collapsed", L"open", L"palette", L"size", L"opacity", L"width", L"height", L"custom_ink", L"scroll"
};
struct Note {
    Note *next;
    HWND window, edit;
    WNDPROC edit_proc;
    ITextDocument *document;
    HBRUSH brush;
    HFONT font, date_font;
    HPEN pen;
    wchar_t *accepted_text, *title, *tags;
    size_t text_capacity;
    wchar_t id[80], path[PATH_CAP];
    int color, palette, size, opacity, dpi, hover, pressed;
    int width, height, wheel_delta, scroll_delta;
    SIZE text_extent;
    int saved_layout[LAYOUT_COUNT];
    unsigned dirty;
    unsigned calendar_date;
    HWND date_tip;
    wchar_t date_label[64];
    DWORD selection_start, selection_end;
    CHARRANGE palette_selection, wheel_selection;
    BOOL custom_ink;
    BOOL collapsed, ready, tags_dirty, save_failed, validating, layout_saved, persisted, tracking;
    BOOL edit_overflow, scrolling;
};
static const SIZE NOTE_SIZES[] = {{240,180}, {NOTE_WIDTH,NOTE_HEIGHT}, {420,340}, {540,440}, {300,300}};
static const wchar_t *SIZE_NAMES[] = {L"Small", L"Medium", L"Large", L"Extra large", L"Square"};
#define SIZE_COUNT ((int)(sizeof(NOTE_SIZES)/sizeof(NOTE_SIZES[0])))
static const int OPACITIES[] = {100,90,80,70,60};
static HINSTANCE instance;
static HWND controller;
static HICON app_icon, hidden_icon;
static Note *notes;
/* Stable IDs survive closing a window; no pointer to a freed note is retained. */
static wchar_t last_note[80], saved_last_note[80];
static BOOL restoring_notes;
static wchar_t directory[PATH_CAP], state_path[PATH_CAP];
static wchar_t location_config[PATH_CAP];
static UINT taskbar_created;
static int next_color;
static int saved_next_color;
static int current_palette;
static int favorite_palette = -1;
static BOOL shuffled_palette;
static BOOL default_scrolling;
static BOOL always_on_top = TRUE;
static BOOL notes_hidden;
static BYTE color_bag[COLOR_COUNT];
static int colors_left;
static DWORD color_random;
static BOOL testing;
static BOOL startup_restore;
static HMODULE rich_edit;
static const IID TEXT_DOCUMENT_IID = {0x8cc497c0, 0xa1df, 0x11ce, {0x80,0x98,0x00,0xaa,0x00,0x47,0xbe,0x5d}};
static BOOL remove_note_files(const wchar_t *id);
static Note *find_note(const wchar_t *id);
static void note_command(Note *note, UINT command);
static HWND browse_notes(void);
static void choose_archive(HWND owner);
static void wheel_resize(Note *note, int delta);
static void end_session(BOOL ending);

static void remember_note(const Note *note) {
    if (note->ready && !restoring_notes) wcscpy_s(last_note, 80, note->id);
}

static int scaled(const Note *note, int value) { return MulDiv(value, note->dpi, 96); }
static int bar_height(const Note *note) { return scaled(note, 32); }
static int note_height(const Note *note) { return note->collapsed ? bar_height(note) : note->height; }
static COLORREF note_color(const Note *note) { return PALETTES[note->palette][note->color]; }
static COLORREF ink_color(const Note *note) {
    return palette_ink(note->palette, note->color);
}

static void redraw_bar(Note *note) {
    RECT rect = {0, 0, note->width, bar_height(note)};
    InvalidateRect(note->window, &rect, FALSE);
}

static void mark_dirty(Note *note, unsigned changes) {
    note->dirty |= changes;
    SetTimer(note->window, SAVE_TIMER, 400, NULL);
}

static void report_error(HWND owner, const wchar_t *message) {
    if (!testing) MessageBoxW(owner, message, L"Jotilla", MB_OK | MB_ICONERROR);
}

static BOOL make_paths(const wchar_t *root) {
    DWORD attributes;
    if (swprintf_s(directory, PATH_CAP, L"%s", root) < 0 ||
        swprintf_s(state_path, PATH_CAP, L"%s\\notes.ini", root) < 0) return FALSE;
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    attributes = GetFileAttributesW(directory);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static BOOL write_setting(const wchar_t *section, const wchar_t *key, int value) {
    wchar_t number[32];
    swprintf_s(number, 32, L"%d", value);
    return WritePrivateProfileStringW(section, key, number, state_path);
}

static int setting(const wchar_t *section, const wchar_t *key, int fallback) {
    return (int)GetPrivateProfileIntW(section, key, fallback, state_path);
}

static int layout_setting(Note *note, int field, int fallback) {
    return note->saved_layout[field] = setting(note->id, LAYOUT_KEYS[field], fallback);
}

/* A multi-property change replaces one section. Retain tags and unfamiliar
   keys instead of rebuilding the section from just the layout fields. */
static BOOL write_layout(const wchar_t *id, const int values[LAYOUT_COUNT]) {
    wchar_t *section = NULL, *item;
    size_t used = 0;
    DWORD capacity = 1024, length;
    int i;
    BOOL ok = FALSE;
    for (;;) {
        wchar_t *grown = (wchar_t *)realloc(section, (size_t)capacity * sizeof(wchar_t));
        if (!grown) goto done;
        section = grown;
        SetLastError(ERROR_SUCCESS);
        length = GetPrivateProfileSectionW(id, section, capacity, state_path);
        if (!length && GetLastError() != ERROR_SUCCESS && GetLastError() != ERROR_FILE_NOT_FOUND) goto done;
        if (length < capacity - 2) break;
        if (capacity >= 32768) goto done;
        capacity *= 2;
    }
    for (item = section; *item;) {
        size_t size = wcslen(item) + 1, key = wcscspn(item, L"=");
        for (i = 0; i < LAYOUT_COUNT; ++i)
            if (wcslen(LAYOUT_KEYS[i]) == key && !_wcsnicmp(item, LAYOUT_KEYS[i], key)) break;
        if (i == LAYOUT_COUNT) { memmove(section + used, item, size * sizeof(wchar_t)); used += size; }
        item += size;
    }
    /* Each key plus any signed int and its terminator fits in 32 characters. */
    if (used + LAYOUT_COUNT * 32 + 1 > capacity) {
        wchar_t *grown;
        if (used + LAYOUT_COUNT * 32 + 1 > 32768) goto done;
        capacity = (DWORD)(used + LAYOUT_COUNT * 32 + 1);
        grown = (wchar_t *)realloc(section, (size_t)capacity * sizeof(wchar_t));
        if (!grown) goto done;
        section = grown;
    }
    for (i = 0; i < LAYOUT_COUNT; ++i) {
        int size = swprintf_s(section + used, capacity - used, L"%s=%d", LAYOUT_KEYS[i], values[i]);
        if (size < 0) goto done;
        used += (size_t)size + 1;
    }
    section[used] = 0;
    ok = WritePrivateProfileSectionW(id, section, state_path);
done:
    free(section);
    return ok;
}

static BOOL save_layout(Note *note, BOOL open) {
    RECT rect;
    int values[LAYOUT_COUNT], i, changed = 0, last = 0;
    if (!GetWindowRect(note->window, &rect)) return FALSE;
    values[0] = rect.left; values[1] = rect.top; values[2] = note->color;
    values[3] = note->title != NULL; values[4] = note->collapsed; values[5] = open;
    values[6] = note->palette; values[7] = note->size; values[8] = note->opacity;
    values[9] = MulDiv(note->width, 96, note->dpi); values[10] = MulDiv(note->height, 96, note->dpi);
    values[11] = note->custom_ink;
    values[LAYOUT_SCROLL] = note->scrolling;
    for (i = 0; i < LAYOUT_COUNT; ++i)
        if (!note->layout_saved || values[i] != note->saved_layout[i]) { ++changed; last = i; }
    if (!changed) return TRUE;
    if (changed == 1 ? !write_setting(note->id, LAYOUT_KEYS[last], values[last]) : !write_layout(note->id, values))
        return FALSE;
    memcpy(note->saved_layout, values, sizeof(values));
    note->layout_saved = TRUE;
    return TRUE;
}

static DWORD CALLBACK write_rich(DWORD_PTR cookie, LPBYTE data, LONG bytes, LONG *written) {
    DWORD count = 0;
    BOOL ok = WriteFile((HANDLE)cookie, data, (DWORD)bytes, &count, NULL);
    *written = (LONG)count;
    return ok && count == (DWORD)bytes ? 0 : ERROR_WRITE_FAULT;
}

static DWORD CALLBACK read_rich(DWORD_PTR cookie, LPBYTE data, LONG bytes, LONG *read) {
    DWORD count = 0;
    BOOL ok = ReadFile((HANDLE)cookie, data, (DWORD)bytes, &count, NULL);
    *read = (LONG)count;
    return ok ? 0 : ERROR_READ_FAULT;
}

typedef struct TextComparison { const BYTE *text; size_t remaining; } TextComparison;

static DWORD CALLBACK compare_text(DWORD_PTR cookie, LPBYTE data, LONG bytes, LONG *written) {
    TextComparison *comparison = (TextComparison *)cookie;
    *written = 0;
    if (bytes < 0 || (size_t)bytes > comparison->remaining || memcmp(comparison->text, data, (size_t)bytes)) return 1;
    comparison->text += bytes; comparison->remaining -= (size_t)bytes;
    *written = bytes;
    return 0;
}

/* Rich Edit streams the same CRLF text as GetWindowText, without another
   whole-body allocation. This also distinguishes format-only undo/redo. */
static BOOL editor_matches_text(const Note *note) {
    TextComparison comparison = {(const BYTE *)note->accepted_text, wcslen(note->accepted_text) * sizeof(wchar_t)};
    EDITSTREAM stream = {0};
    stream.dwCookie = (DWORD_PTR)&comparison; stream.pfnCallback = compare_text;
    SendMessageW(note->edit, EM_STREAMOUT, SF_TEXT | SF_UNICODE, (LPARAM)&stream);
    return !stream.dwError && !comparison.remaining;
}

/* Keep the UTF-8 archive readable; Windows streams formatting to a companion
   RTF without building a second document in our heap. Both writes are atomic. */
static BOOL save_rich(Note *note) {
    wchar_t path[PATH_CAP], temporary[PATH_CAP];
    EDITSTREAM stream = {0};
    HANDLE file;
    BOOL ok;
    if (swprintf_s(path, PATH_CAP, L"%s\\%s.rtf", directory, note->id) < 0 ||
        swprintf_s(temporary, PATH_CAP, L"%s.tmp", path) < 0) return FALSE;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    stream.dwCookie = (DWORD_PTR)file; stream.pfnCallback = write_rich;
    SendMessageW(note->edit, EM_STREAMOUT, SF_RTF, (LPARAM)&stream);
    ok = !stream.dwError && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = FALSE;
    if (ok) ok = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temporary);
    return ok;
}

static void load_rich(Note *note) {
    wchar_t path[PATH_CAP];
    HANDLE file;
    LARGE_INTEGER size;
    EDITSTREAM stream = {0};
    if (swprintf_s(path, PATH_CAP, L"%s\\%s.rtf", directory, note->id) < 0) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > TEXT_LIMIT * 16LL) {
        CloseHandle(file); return;
    }
    stream.dwCookie = (DWORD_PTR)file; stream.pfnCallback = read_rich;
    SendMessageW(note->edit, EM_STREAMIN, SF_RTF, (LPARAM)&stream);
    CloseHandle(file);
    /* A crash between the two replacements, or an external TXT edit, must
       never resurrect an older body from the formatting companion. */
    if (stream.dwError || !editor_matches_text(note)) SetWindowTextW(note->edit, note->accepted_text);
    SendMessageW(note->edit, EM_EMPTYUNDOBUFFER, 0, 0);
}

/* Replace a complete UTF-8 snapshot on the same volume. A failed write never
   truncates the last saved note. The temporary file stays beside its .txt. */
static BOOL save_text(Note *note) {
    int bytes = 0, converted;
    size_t body_length, title_length, capacity;
    wchar_t temporary[PATH_CAP];
    char *utf8;
    HANDLE file;
    DWORD written;
    BOOL ok;
    if (!note->dirty) return TRUE;
    if (!(note->dirty & DIRTY_TEXT)) {
        if (!save_rich(note)) return FALSE;
        note->dirty = 0;
        return TRUE;
    }
    body_length = wcslen(note->accepted_text);
    title_length = note->title ? wcslen(note->title) : 0;
    /* At most three UTF-8 bytes per UTF-16 code unit, plus # and CRLF.
       Convert each part once, directly from the accepted editor snapshot. */
    capacity = (body_length + title_length) * 3 + 3;
    utf8 = (char *)malloc(capacity);
    if (!utf8) return FALSE;
    if (note->title) {
        utf8[bytes++] = '#';
        converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, note->title, (int)title_length,
                                        utf8 + bytes, (int)capacity - bytes, NULL, NULL);
        if (title_length && !converted) { free(utf8); return FALSE; }
        bytes += converted;
        utf8[bytes++] = '\r'; utf8[bytes++] = '\n';
    }
    converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, note->accepted_text, (int)body_length,
                                    utf8 + bytes, (int)capacity - bytes, NULL, NULL);
    if (body_length && !converted) { free(utf8); return FALSE; }
    bytes += converted;
    if (swprintf_s(temporary, PATH_CAP, L"%s.tmp", note->path) < 0) { free(utf8); return FALSE; }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { free(utf8); return FALSE; }
    ok = WriteFile(file, utf8, (DWORD)bytes, &written, NULL) && written == (DWORD)bytes;
    free(utf8);
    if (ok) ok = FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = FALSE;
    if (ok && (note->dirty & DIRTY_RICH)) ok = save_rich(note);
    if (ok) ok = MoveFileExW(temporary, note->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (ok) { note->dirty = FALSE; note->persisted = TRUE; }
    else DeleteFileW(temporary);
    return ok;
}

static BOOL note_has_content(const Note *note) {
    const wchar_t *text = note->accepted_text;
    if (note->title) return TRUE;
    while (*text && iswspace(*text)) ++text;
    return *text != 0;
}

static BOOL save_note(Note *note) {
    BOOL previous_failure = note->save_failed;
    BOOL changed = (note->dirty & DIRTY_TEXT) || note->tags_dirty || previous_failure ||
        (note->layout_saved && (note->saved_layout[LAYOUT_PALETTE] != note->palette ||
                               note->saved_layout[LAYOUT_COLOR] != note->color));
    BOOL tags_changed = note->tags_dirty || !note->persisted || previous_failure;
    BOOL ok;
    if (!note_has_content(note)) {
        ok = !note->persisted || remove_note_files(note->id);
        if (ok) {
            note->dirty = note->persisted = note->layout_saved = FALSE;
            note->tags_dirty = note->tags != NULL;
            if (note->calendar_date) { note->calendar_date = 0; redraw_bar(note); }
        }
    } else {
        ok = save_text(note);
        if (ok) ok = save_layout(note, TRUE);
        if (ok && note->tags_dirty) {
            ok = tags_write(state_path, note->id, note->tags);
            if (ok) note->tags_dirty = FALSE;
        }
        if (ok && changed) archive_changed(note->id, TRUE, tags_changed);
    }
    note->save_failed = !ok;
    if (ok) KillTimer(note->window, SAVE_TIMER);
    else SetTimer(note->window, SAVE_TIMER, 1000, NULL);
    if (previous_failure != note->save_failed) redraw_bar(note);
    return ok;
}

/* The persisted association is authoritative: a failed write leaves the
   previous day visible in both windows. Layout saves preserve this key. */
static BOOL set_note_date(Note *note, unsigned date) {
    wchar_t value[16];
    if ((date && !date_valid(date)) || !note_has_content(note) || !save_note(note)) return FALSE;
    if (date == note->calendar_date) return TRUE;
    swprintf_s(value,16,L"%08u",date);
    if (!WritePrivateProfileStringW(note->id,L"calendar_date",date ? value : NULL,state_path)) return FALSE;
    note->calendar_date = date;
    archive_date_changed(note->id,date); redraw_bar(note);
    return TRUE;
}

static void show_note_calendar(Note *note) {
    POINT anchor = {0,bar_height(note)};
    if (!note_has_content(note)) {
        report_error(note->window,L"Add a title or some text before assigning a day."); return;
    }
    ClientToScreen(note->window,&anchor);
    if (!calendar_picker(note->window,anchor,note->calendar_date,NOTE_DATE_MESSAGE))
        report_error(note->window,L"The calendar could not be opened.");
}

static int sizing_key(WPARAM key) {
    UINT character = MapVirtualKeyW((UINT)key,MAPVK_VK_TO_CHAR) & 0xffff;
    if (key == VK_ADD || key == VK_OEM_PLUS || character == L'+') return 1;
    if (key == VK_SUBTRACT || key == VK_OEM_MINUS || character == L'-') return -1;
    return 0;
}

static void destroy_note(Note *note) {
    /* Destroying the focused EDIT sends WM_KILLFOCUS. Do not let that save
       mark an explicitly closed (archived) note as open again. */
    note->ready = FALSE;
    DestroyWindow(note->window);
}

static BOOL load_text(Note *note) {
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read;
    char *data;
    wchar_t *text;
    int offset = 0, count;
    BOOL ok;
    file = CreateFileW(note->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(file, &size) || size.QuadPart > TEXT_LIMIT * 4LL || size.QuadPart < 0) {
        CloseHandle(file); return FALSE;
    }
    data = (char *)malloc((size_t)size.QuadPart + 1);
    if (!data) { CloseHandle(file); return FALSE; }
    ok = ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) && read == (DWORD)size.QuadPart;
    CloseHandle(file);
    if (!ok) { free(data); return FALSE; }
    if (read >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb &&
        (unsigned char)data[2] == 0xbf) offset = 3;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data + offset, (int)read - offset, NULL, 0);
    if (((int)read > offset && !count) || count > TEXT_LIMIT) { free(data); return FALSE; }
    text = (wchar_t *)calloc((size_t)count + 1, sizeof(wchar_t));
    if (!text) { free(data); return FALSE; }
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data + offset, (int)read - offset, text, count);
    free(data);
    /* Only explicit title promotion sets this flag. A literal #first line
       followed by Enter must never become a title just by reopening. */
    if (note->saved_layout[LAYOUT_TITLE] && text[0] == L'#') {
        size_t end = wcscspn(text, L"\r\n"), body = end;
        if (end > 1) {
            note->title = (wchar_t *)calloc(end, sizeof(wchar_t));
            if (!note->title) { free(text); return FALSE; }
            wmemcpy(note->title, text + 1, end - 1);
            if (body < (size_t)count && text[body] == L'\r') ++body;
            if (body < (size_t)count && text[body] == L'\n') ++body;
            memmove(text, text + body, (wcslen(text + body) + 1) * sizeof(wchar_t));
        }
    }
    ok = SetWindowTextW(note->edit, text);
    if (ok) {
        note->accepted_text = text;
        note->text_capacity = (size_t)count + 1;
    } else free(text);
    return ok;
}

/* Theme ink is independent of the text-edit undo history. Only the color mask
   changes: emphasis, checklist strikes, selection and insertion point survive. */
static BOOL set_editor_ink(Note *note, COLORREF ink) {
    CHARFORMAT2W format = {0};
    LONG count;
    BOOL ok, validating = note->validating;
    /* Once the user formats foreground colors, RTF owns them. Theme changes
       must never silently replace intentional colors, including after Undo. */
    if (note->custom_ink) return TRUE;
    if (FAILED(note->document->lpVtbl->Undo(note->document, tomSuspend, &count))) return FALSE;
    note->validating = TRUE;
    format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink;
    ok = SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&format) != 0;
    if (ok) ok = SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&format) != 0;
    note->document->lpVtbl->Undo(note->document, tomResume, &count);
    note->validating = validating;
    return ok;
}

static BOOL recolor_note(Note *note, int palette, int color) {
    COLORREF paper, ink;
    HBRUSH brush;
    HPEN pen;
    if (palette < 0 || palette >= PALETTE_COUNT || color < 0 || color >= PALETTE_SIZES[palette]) return FALSE;
    if (note->palette == palette && note->color == color) return save_note(note);
    paper = PALETTES[palette][color]; ink = palette_ink(palette,color);
    brush = CreateSolidBrush(paper);
    pen = CreatePen(PS_SOLID,max(1,scaled(note,1)),palette_bar_ink(palette,color));
    if (!brush || !pen) {
        if (brush) DeleteObject(brush);
        if (pen) DeleteObject(pen);
        return FALSE;
    }
    if (!note->custom_ink && ink != ink_color(note)) {
        if (!set_editor_ink(note,ink)) { DeleteObject(brush); DeleteObject(pen); return FALSE; }
        mark_dirty(note,DIRTY_RICH);
    }
    DeleteObject(note->brush); DeleteObject(note->pen);
    note->brush = brush; note->pen = pen; note->palette = palette; note->color = color;
    SendMessageW(note->edit,EM_SETBKGNDCOLOR,0,paper);
    InvalidateRect(note->window,NULL,FALSE);
    return save_note(note);
}

static void use_session_palette(int palette) {
    if (!shuffled_palette || current_palette != palette) colors_left = 0;
    current_palette = palette; shuffled_palette = TRUE;
}

/* A six-byte shuffled bag. A failed note creation does not consume its color;
   the original title-bar palettes keep their old sequence until opted into. */
static int draw_note_color(void) {
    int i, count = colors_left;
    if (!shuffled_palette) return next_color;
    if (count < 1 || count > COLOR_COUNT) {
        if (!color_random) {
            LARGE_INTEGER seed;
            QueryPerformanceCounter(&seed);
            color_random = seed.LowPart ^ (DWORD)seed.HighPart ^ GetCurrentProcessId();
            if (!color_random) color_random = 0x9e3779b9u;
        }
        count = 1; color_bag[0] = 0;
        for (i = 1; i < PALETTE_SIZES[current_palette] && i < COLOR_COUNT; ++i) {
            int j;
            for (j = 0; j < count; ++j)
                if (PALETTES[current_palette][color_bag[j]] == PALETTES[current_palette][i]) break;
            if (j == count && count < COLOR_COUNT) color_bag[count++] = (BYTE)i;
        }
        for (i = count - 1; i > 0; --i) {
            int j;
            BYTE swap;
            color_random ^= color_random << 13; color_random ^= color_random >> 17; color_random ^= color_random << 5;
            j = (int)(color_random % (DWORD)(i + 1));
            _Analysis_assume_(j >= 0 && j <= i); /* Unsigned remainder bounds the swap. */
            swap = color_bag[i]; color_bag[i] = color_bag[j]; color_bag[j] = swap;
        }
        colors_left = count;
    }
    return color_bag[count - 1];
}

static BOOL save_favorite(void *context, int palette) {
    Note *note = (Note *)context;
    if (palette < -1 || palette >= PALETTE_COUNT || (palette >= 0 && PALETTE_POPULAR_INDEX[palette] < 0)) return FALSE;
    if (palette != favorite_palette && !write_setting(L"app", L"favorite_palette", palette)) {
        report_error(note->window,L"The favorite palette could not be saved. Please check the archive folder.");
        return FALSE;
    }
    favorite_palette = palette;
    if (palette >= 0) {
        use_session_palette(palette);
        if (!recolor_note(note,palette,note->palette == palette ? note->color : 0))
            report_error(note->window,L"The favorite palette was saved, but the note color could not be saved. Please check the archive folder.");
    }
    return TRUE;
}

static void select_note_color(void *context, int palette, int color) {
    Note *note = (Note *)context;
    if (!recolor_note(note,palette,color)) report_error(note->window,L"The note color could not be saved. Please check the archive folder.");
    else use_session_palette(palette);
}

static void select_text_color(void *context, int palette, int color) {
    Note *note = (Note *)context;
    CHARFORMAT2W format = {0};
    BOOL ok;
    if (palette < 0 || palette >= PALETTE_COUNT || color < 0 || color >= PALETTE_SIZES[palette] ||
        note->palette_selection.cpMin >= note->palette_selection.cpMax) return;
    format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = PALETTES[palette][color];
    SendMessageW(note->edit,EM_EXSETSEL,0,(LPARAM)&note->palette_selection);
    SendMessageW(note->edit,EM_STOPGROUPTYPING,0,0);
    note->validating = TRUE;
    ok = SendMessageW(note->edit,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&format) != 0;
    note->validating = FALSE;
    if (ok) { note->custom_ink = TRUE; mark_dirty(note,DIRTY_RICH); ok = save_note(note); }
    if (!ok) report_error(note->window,L"The text color could not be saved. Please check the archive folder.");
}

static void show_note_palette(Note *note, POINT point) {
    PalettePickerOptions options = {0};
    SendMessageW(note->edit,EM_EXGETSEL,0,(LPARAM)&note->palette_selection);
    options.palette = note->palette; options.color = note->color; options.favorite = favorite_palette;
    options.text_mode = note->palette_selection.cpMin != note->palette_selection.cpMax;
    options.select = options.text_mode ? select_text_color : select_note_color;
    options.favorite_changed = save_favorite; options.context = note;
    if (options.text_mode) {
        CHARFORMAT2W format = {0};
        int i;
        format.cbSize = sizeof(format); options.color = -1;
        SendMessageW(note->edit,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);
        if (format.dwMask & CFM_COLOR)
            for (i = 0; i < PALETTE_SIZES[options.palette]; ++i)
                if (format.crTextColor == PALETTES[options.palette][i]) { options.color = i; break; }
    }
    if (point.x == -1 && point.y == -1) {
        point.x = note->width / 2; point.y = bar_height(note) + scaled(note,50);
        ClientToScreen(note->window,&point);
    }
    if (!palette_picker_show(note->window,point,&options))
        report_error(note->window,L"The color picker could not be opened.");
}

static void update_style(Note *note) {
    HFONT old_font = note->font, old_date_font = note->date_font;
    HPEN old_pen = note->pen;
    int previous_dpi = note->dpi;
    note->dpi = (int)GetDpiForWindow(note->window);
    if (!note->dpi) note->dpi = 96;
    note->width = MulDiv(note->width, note->dpi, previous_dpi);
    note->height = MulDiv(note->height, note->dpi, previous_dpi);
    note->font = CreateFontW(-MulDiv(11, note->dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    note->date_font = CreateFontW(-MulDiv(9, note->dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!note->ready) {
        CHARFORMAT2W format = {0};
        SendMessageW(note->edit, WM_SETFONT, (WPARAM)note->font, TRUE);
        format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink_color(note);
        SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&format);
    }
    SendMessageW(note->edit, EM_SETBKGNDCOLOR, 0, note_color(note));
    note->pen = CreatePen(PS_SOLID, max(1, scaled(note, 1)), palette_bar_ink(note->palette,note->color));
    if (old_font) DeleteObject(old_font);
    if (old_date_font) DeleteObject(old_date_font);
    if (old_pen) DeleteObject(old_pen);
}

static BOOL text_fits(Note *note) {
    RECT rect;
    /* Ask the existing layout engine: a fixed line height cannot measure
       mixed font sizes, wrapped lines or the final paragraph correctly. */
    note->text_extent.cx = note->text_extent.cy = LONG_MAX;
    SendMessageW(note->edit, EM_REQUESTRESIZE, 0, 0);
    GetClientRect(note->edit, &rect);
    return note->text_extent.cx <= rect.right && note->text_extent.cy <= rect.bottom;
}

/* Scrolling delegates layout and navigation to Rich Edit. Do not measure the
   whole document on each keystroke when there is no viewport capacity limit. */
static BOOL accepts_text(Note *note) {
    return note->scrolling || text_fits(note);
}

static BOOL remember_text(Note *note) {
    int length = GetWindowTextLengthW(note->edit);
    if ((size_t)length + 1 > note->text_capacity) {
        size_t capacity = max((size_t)length + 1, max(note->text_capacity * 2, 128));
        wchar_t *copy = (wchar_t *)realloc(note->accepted_text, capacity * sizeof(wchar_t));
        if (!copy) return FALSE;
        note->accepted_text = copy;
        note->text_capacity = capacity;
    }
    GetWindowTextW(note->edit, note->accepted_text, length + 1);
    return TRUE;
}

static BOOL copy_title(const wchar_t *text,size_t end,wchar_t **result) {
    size_t start=0;
    *result=NULL;
    while (start<end && (text[start]==L' ' || text[start]==L'\t')) ++start;
    while (end>start && (text[end-1]==L' ' || text[end-1]==L'\t')) --end;
    if (end>start) {
        *result=(wchar_t *)calloc(end-start+1,sizeof(wchar_t));
        if (!*result) return FALSE;
        wmemcpy(*result,text+start,end-start);
    }
    return TRUE;
}

static BOOL promote_title(Note *note) {
    const wchar_t *text = note->accepted_text;
    wchar_t *title;
    size_t end, after;
    DWORD selection_start, selection_end;
    SendMessageW(note->edit, EM_GETSEL, (WPARAM)&selection_start, (LPARAM)&selection_end);
    if (selection_start != selection_end || text[0] != L'#') return FALSE;
    end = wcscspn(text, L"\r\n");
    if (selection_start != end) return FALSE;
    if (!copy_title(text+1,end-1,&title) || !title) return FALSE;
    after = end;
    if (text[after] == L'\r') ++after;
    if (text[after] == L'\n') ++after;
    note->validating = TRUE;
    SendMessageW(note->edit, EM_SETSEL, 0, end + (text[end] ? 1 : 0));
    SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
    SendMessageW(note->edit, EM_EMPTYUNDOBUFFER, 0, 0);
    note->validating = FALSE;
    free(note->title); note->title = title;
    memmove(note->accepted_text, text + after, (wcslen(text + after) + 1) * sizeof(wchar_t));
    SendMessageW(note->edit, EM_SETSEL, 0, 0);
    mark_dirty(note, DIRTY_TEXT | DIRTY_RICH);
    redraw_bar(note);
    return TRUE;
}

static void layout(Note *note) {
    int padding = scaled(note, 10), bar = bar_height(note);
    /* A collapsed card keeps the same editor dimensions and text capacity. */
    MoveWindow(note->edit, padding, bar + scaled(note, 6), note->width - padding * 2,
               note->height - bar - scaled(note, 16), TRUE);
    ShowWindow(note->edit, note->collapsed ? SW_HIDE : SW_SHOW);
    if (note->date_tip) {
        TOOLINFOW tip = {sizeof(tip)};
        tip.hwnd=note->window; tip.uId=1; SetRect(&tip.rect,0,0,bar,bar);
        SendMessageW(note->date_tip,TTM_NEWTOOLRECTW,0,(LPARAM)&tip);
    }
    InvalidateRect(note->window, NULL, FALSE);
}

static void toggle_note(Note *note) {
    RECT rect;
    GetWindowRect(note->window, &rect);
    note->collapsed = !note->collapsed;
    SetWindowPos(note->window, NULL, 0, 0, rect.right - rect.left,
                 note_height(note),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    save_note(note);
    if (!note->collapsed && !testing) SetFocus(note->edit);
}

static int button_at(Note *note, LPARAM position) {
    RECT rect;
    int x = GET_X_LPARAM(position), y = GET_Y_LPARAM(position), bar = bar_height(note);
    GetClientRect(note->window, &rect);
    if (y >= 0 && y < bar && x >= 0 && x < bar) return 3;
    if (y < 0 || y >= bar || x < rect.right - bar * 2 || x >= rect.right) return 0;
    return x >= rect.right - bar ? 2 : 1;
}

static void paint_note(Note *note) {
    PAINTSTRUCT ps;
    RECT rect, bar_rect, button;
    HDC dc = BeginPaint(note->window, &ps);
    COLORREF color = note_color(note);
    HBRUSH dc_brush = (HBRUSH)GetStockObject(DC_BRUSH);
    HGDIOBJ old_pen = SelectObject(dc, note->pen);
    int bar = bar_height(note), i;
    GetClientRect(note->window, &rect);
    FillRect(dc, &rect, note->brush);
    bar_rect = rect; bar_rect.bottom = bar;
    SetDCBrushColor(dc, note->save_failed ? RGB(235, 161, 151) :
        palette_bar(color));
    FillRect(dc, &bar_rect, dc_brush);
    if (note->title || note->calendar_date) {
        int text_left = bar + scaled(note, 4);
        int text_right = rect.right - bar * 2 - scaled(note, 6);
        if (text_right > text_left) {
            static const wchar_t *WEEKDAYS[7] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
            HGDIOBJ old_font = SelectObject(dc, note->font);
            wchar_t date_text[32];
            int date_w = 0;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, note->save_failed ? RGB(0,0,0) : palette_bar_ink(note->palette,note->color));
            if (note->calendar_date) {
                RECT measure = {0, 0, 0, bar};
                unsigned d = note->calendar_date;
                int wd = date_weekday(d);
                swprintf_s(date_text, 32, L"(%s %02u-%02u-%04u)",
                           WEEKDAYS[(wd >= 0 && wd < 7) ? wd : 0], d % 100, (d / 100) % 100, d / 10000);
                if (note->date_font) SelectObject(dc, note->date_font);
                DrawTextW(dc, date_text, -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                date_w = measure.right - measure.left;
            }
            if (note->title) {
                RECT title_measure = {0, 0, 0, bar}, title_rect = {text_left, 0, text_right, bar};
                int title_w, gap = scaled(note, 6);
                SelectObject(dc, note->font);
                DrawTextW(dc, note->title, -1, &title_measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                title_w = title_measure.right - title_measure.left;
                if (note->calendar_date) {
                    int avail = text_right - text_left;
                    int reserve = avail > date_w + scaled(note, 24) ? date_w + gap : 0;
                    int max_title = avail - reserve;
                    int drawn_w = min(title_w, max_title);
                    title_rect.right = text_left + drawn_w;
                    text_left += drawn_w + gap;
                }
                DrawTextW(dc, note->title, -1, &title_rect,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (note->calendar_date && text_right > text_left) {
                RECT date_rect = {text_left, 0, text_right, bar};
                if (note->date_font) SelectObject(dc, note->date_font);
                DrawTextW(dc, date_text, -1, &date_rect,
                          DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            SelectObject(dc, old_font);
        }
    }
    button.left = 1; button.right = bar; button.top = 1; button.bottom = bar - 1;
    if (note->hover == 3) { SetDCBrushColor(dc,color); FillRect(dc,&button,dc_brush); }
    calendar_icon(dc,button,palette_bar_ink(note->palette,note->color),note->calendar_date != 0);
    for (i = 1; i <= 2; ++i) {
        int cx = rect.right - (i == 1 ? bar * 3 / 2 : bar / 2), cy = bar / 2, s = scaled(note, 4);
        SelectObject(dc, (note->save_failed || (note->hover == i && i == 2)) ? GetStockObject(BLACK_PEN) : note->pen);
        if (note->hover == i) {
            SetDCBrushColor(dc, i == 2 ? RGB(235, 161, 151) : color);
            button.left = cx - bar / 2; button.right = button.left + bar; button.top = 0; button.bottom = bar;
            FillRect(dc, &button, dc_brush);
        }
        if (i == 2) {
            MoveToEx(dc, cx - s, cy - s, NULL); LineTo(dc, cx + s + 1, cy + s + 1);
            MoveToEx(dc, cx + s, cy - s, NULL); LineTo(dc, cx - s - 1, cy + s + 1);
        } else if (note->collapsed) {
            HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, cx - s - 1, cy - s, cx + s + 2, cy + s + 1);
            SelectObject(dc, old_brush);
        } else {
            MoveToEx(dc, cx - s - 1, cy, NULL); LineTo(dc, cx + s + 2, cy);
        }
    }
    {
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT divider = {0, bar - 1, rect.right, bar};
        FrameRect(dc, &rect, black);
        FillRect(dc, &divider, black);
    }
    SelectObject(dc, old_pen);
    EndPaint(note->window, &ps);
}

static void paragraph_at(HWND edit, LONG position, CHARRANGE *range) {
    FINDTEXTEXW find = {{position, 0}, L"\r", {0, 0}};
    GETTEXTLENGTHEX length = {GTL_PRECISE | GTL_NUMCHARS, 1200};
    range->cpMin = SendMessageW(edit, EM_FINDTEXTEXW, 0, (LPARAM)&find) < 0 ? 0 : find.chrgText.cpMax;
    find.chrg.cpMin = position; find.chrg.cpMax = -1;
    range->cpMax = SendMessageW(edit, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&find) < 0 ?
                      (LONG)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&length, 0) : find.chrgText.cpMin;
}

static wchar_t todo_marker(HWND edit, const CHARRANGE *range) {
    wchar_t text[3] = {0};
    TEXTRANGEW part = {{range->cpMin, min(range->cpMin + 2, range->cpMax)}, text};
    SendMessageW(edit, EM_GETTEXTRANGE, 0, (LPARAM)&part);
    return (text[0] == L'\x2610' || text[0] == L'\x2611') && text[1] == L' ' ? text[0] : 0;
}

static void strike_todo(Note *note, const CHARRANGE *range, BOOL checked) {
    CHARRANGE selection;
    CHARFORMAT2W format = {0};
    format.cbSize = sizeof(format); format.dwMask = CFM_STRIKEOUT;
    format.dwEffects = checked ? CFE_STRIKEOUT : 0;
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    SendMessageW(note->edit, EM_SETSEL, range->cpMin + 2, range->cpMax);
    SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&selection);
}

static void toggle_format(Note *note, DWORD effect) {
    CHARFORMAT2W format = {0};
    format.cbSize = sizeof(format);
    SendMessageW(note->edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    format.dwEffects = (format.dwMask & effect) && (format.dwEffects & effect) ? 0 : effect;
    format.dwMask = effect;
    SendMessageW(note->edit, EM_STOPGROUPTYPING, 0, 0);
    note->validating = TRUE;
    SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    if (!accepts_text(note)) SendMessageW(note->edit, EM_UNDO, 0, 0);
    note->validating = FALSE;
    mark_dirty(note, DIRTY_RICH);
}

/* Treat a checkbox gesture and its derived formatting as one native undo
   operation. Validation can then roll back the whole edit without losing styles. */
static void begin_edit(Note *note, CHARRANGE *selection) {
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)selection);
    SendMessageW(note->edit, EM_STOPGROUPTYPING, 0, 0);
    note->document->lpVtbl->BeginEditCollection(note->document);
    note->validating = TRUE;
}

static void finish_edit(Note *note, const CHARRANGE *selection) {
    note->document->lpVtbl->EndEditCollection(note->document);
    if (!accepts_text(note) || !remember_text(note)) {
        SendMessageW(note->edit, EM_UNDO, 0, 0);
        SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)selection);
    } else mark_dirty(note, DIRTY_TEXT | DIRTY_RICH);
    note->validating = FALSE;
}

static void clear_paragraph_strike(Note *note) {
    CHARRANGE selection, paragraph;
    CHARFORMAT2W format = {0};
    format.cbSize = sizeof(format); format.dwMask = CFM_STRIKEOUT;
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    paragraph_at(note->edit, selection.cpMin, &paragraph);
    SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&paragraph);
    SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&selection);
    SendMessageW(note->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
}

static BOOL todo_enter(Note *note, BOOL shift) {
    CHARRANGE selection, paragraph, previous;
    wchar_t text[3] = {0};
    TEXTRANGEW part;
    wchar_t marker;
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    if (selection.cpMin != selection.cpMax) return FALSE;
    paragraph_at(note->edit, selection.cpMin, &paragraph);
    part.chrg = paragraph; part.lpstrText = text;
    if (shift && paragraph.cpMax - paragraph.cpMin == 1) {
        SendMessageW(note->edit, EM_GETTEXTRANGE, 0, (LPARAM)&part);
        if (text[0] == L'*' && selection.cpMin == paragraph.cpMax) {
            begin_edit(note, &previous);
            SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&paragraph);
            SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)L"\x2610 ");
            clear_paragraph_strike(note);
            finish_edit(note, &previous);
            return TRUE;
        }
    }
    marker = todo_marker(note->edit, &paragraph);
    if (!marker || selection.cpMin < paragraph.cpMin + 2) return FALSE;
    begin_edit(note, &previous);
    if (paragraph.cpMax == paragraph.cpMin + 2) {
        SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&paragraph);
        SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
    } else {
        SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n\x2610 ");
    }
    clear_paragraph_strike(note);
    finish_edit(note, &previous);
    return TRUE;
}

static BOOL click_todo(Note *note, LPARAM position) {
    POINTL point = {GET_X_LPARAM(position), GET_Y_LPARAM(position)}, origin={0}, end={0};
    LONG index = (LONG)SendMessageW(note->edit, EM_CHARFROMPOS, 0, (LPARAM)&point);
    CHARRANGE paragraph, selection;
    ITextRange *glyph = NULL;
    BOOL hit;
    wchar_t marker, replacement[2] = {0};
    paragraph_at(note->edit, index, &paragraph);
    marker = todo_marker(note->edit, &paragraph);
    if (!marker) return FALSE;
    /* Let Rich Edit supply the glyph bounds after font-size changes. This
       short-lived range is needed only when clicking a checklist paragraph. */
    if (FAILED(note->document->lpVtbl->Range(note->document, paragraph.cpMin, paragraph.cpMin+1, &glyph))) return FALSE;
    hit = glyph->lpVtbl->GetPoint(glyph,tomStart|tomClientCoord|tomAllowOffClient|TA_LEFT|TA_TOP,&origin.x,&origin.y)==S_OK &&
          glyph->lpVtbl->GetPoint(glyph,tomStart|tomClientCoord|tomAllowOffClient|TA_RIGHT|TA_BOTTOM,&end.x,&end.y)==S_OK;
    glyph->lpVtbl->Release(glyph);
    if (!hit || point.x < origin.x || point.x >= end.x || point.y < origin.y || point.y >= end.y) return FALSE;
    begin_edit(note, &selection);
    replacement[0] = marker == L'\x2610' ? L'\x2611' : L'\x2610';
    SendMessageW(note->edit, EM_SETSEL, paragraph.cpMin, paragraph.cpMin + 1);
    SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)replacement);
    strike_todo(note, &paragraph, replacement[0] == L'\x2611');
    SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&selection);
    finish_edit(note, &selection);
    return TRUE;
}

static BOOL remove_todo_marker(Note *note) {
    CHARRANGE selection, paragraph, previous;
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    if (selection.cpMin != selection.cpMax) return FALSE;
    paragraph_at(note->edit, selection.cpMin, &paragraph);
    if (selection.cpMin != paragraph.cpMin + 2 || !todo_marker(note->edit, &paragraph)) return FALSE;
    begin_edit(note, &previous);
    SendMessageW(note->edit, EM_SETSEL, paragraph.cpMin, paragraph.cpMin + 2);
    SendMessageW(note->edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
    clear_paragraph_strike(note);
    finish_edit(note, &previous);
    return TRUE;
}

static void scroll_text(Note *note, int delta) {
    UINT lines = 3;
    int steps;
    note->scroll_delta += delta;
    steps = note->scroll_delta / WHEEL_DELTA;
    note->scroll_delta %= WHEEL_DELTA;
    if (!steps) return;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    /* Rich Edit ignores wheel input without WS_VSCROLL. Send native scroll
       commands instead, retaining the Windows preference and fractional input. */
    if (lines == WHEEL_PAGESCROLL) {
        while (steps) {
            SendMessageW(note->edit, EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            steps += steps > 0 ? -1 : 1;
        }
    } else if (lines) {
        LONGLONG count = -(LONGLONG)steps * lines;
        SendMessageW(note->edit, EM_LINESCROLL, 0, (LPARAM)max((LONGLONG)LONG_MIN, min((LONGLONG)LONG_MAX, count)));
    }
}

static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Note *note = (Note *)GetWindowLongPtrW(GetParent(window), GWLP_USERDATA);
    /* Rich Edit normally moves the caret on right-down. Keep the selection
       intact so right-up can choose foreground formatting deliberately. */
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) { SetFocus(window); return 0; }
    if (message == WM_RBUTTONUP) {
        POINT point = {GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        ClientToScreen(window,&point); show_note_palette(note,point); return 0;
    }
    if (message == WM_CONTEXTMENU) {
        POINT point = {GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        show_note_palette(note,point); return 0;
    }
    if (message == WM_KEYDOWN && (wparam == VK_APPS || (wparam == VK_F10 && GetKeyState(VK_SHIFT) < 0))) {
        POINT point = {-1,-1}; show_note_palette(note,point); return 0;
    }
    if (note->ready && !note->validating && (message == WM_CHAR || message == WM_KEYDOWN ||
        message == WM_PASTE || message == WM_CUT || message == WM_CLEAR || message == WM_UNDO ||
        message == EM_PASTESPECIAL || message == EM_REPLACESEL || message == WM_SETTEXT))
    {
        SendMessageW(window, EM_GETSEL, (WPARAM)&note->selection_start, (LPARAM)&note->selection_end);
        /* An overflow rollback must not undo earlier accepted keystrokes. */
        if (message != WM_KEYDOWN) SendMessageW(window, EM_STOPGROUPTYPING, 0, 0);
    }
    if (message == WM_MOUSEWHEEL) {
        if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) {
            note->scroll_delta = 0;
            return SendMessageW(note->window, message, wparam, lparam);
        }
        if (note->scrolling && note->ready && !note->validating) {
            note->wheel_delta = 0;
            scroll_text(note, GET_WHEEL_DELTA_WPARAM(wparam)); return 0;
        }
    }
    if (note->ready && !note->validating && (message == WM_UNDO || message == EM_UNDO || message == EM_REDO)) {
        CHARRANGE selection;
        UINT action = message == EM_REDO ? EM_REDO : EM_UNDO;
        LRESULT result;
        SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
        note->validating = TRUE;
        result = CallWindowProcW(note->edit_proc, window, action, 0, 0);
        if (result) {
            unsigned changes = DIRTY_RICH | (editor_matches_text(note) ? 0 : DIRTY_TEXT);
            if (!accepts_text(note) || !remember_text(note)) {
                CallWindowProcW(note->edit_proc, window, action == EM_UNDO ? EM_REDO : EM_UNDO, 0, 0);
                SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&selection);
                result = FALSE;
            } else {
                set_editor_ink(note,ink_color(note));
                mark_dirty(note, changes);
            }
        }
        note->validating = FALSE;
        return result;
    }
    if (message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
        DWORD effect = wparam == 'B' ? CFM_BOLD : wparam == 'I' ? CFM_ITALIC :
                       wparam == 'U' ? CFM_UNDERLINE :
                       wparam == 'X' && GetKeyState(VK_SHIFT) < 0 ? CFM_STRIKEOUT : 0;
        if (effect) { toggle_format(note, effect); return 0; }
        if (wparam == 'T') { note_command(note, CMD_TAGS); return 0; }
        if (wparam == 'E') { browse_notes(); archive_notes(); return 0; }
        if (sizing_key(wparam)) { note->wheel_delta=0; wheel_resize(note,sizing_key(wparam)*WHEEL_DELTA); return 0; }
        if (wparam == 'F' && GetKeyState(VK_SHIFT) < 0) { browse_notes(); archive_search(); return 0; }
        if (wparam == 'Z' || wparam == 'Y') {
            SendMessageW(window, wparam == 'Y' || GetKeyState(VK_SHIFT) < 0 ? EM_REDO : EM_UNDO, 0, 0);
            return 0;
        }
        if (wparam == 'A') { SendMessageW(window, EM_SETSEL, 0, -1); return 0; }
    }
    if (message == WM_CHAR && GetKeyState(VK_CONTROL) < 0 &&
        (wparam == 2 || wparam == 5 || wparam == 9 || wparam == 20 || wparam == 21 || wparam == 25 || wparam == 26 ||
         wparam == L'+' || wparam == L'-' || wparam == L'=' || wparam == 31 || wparam == 29 ||
         (wparam == 6 && GetKeyState(VK_SHIFT) < 0) ||
         (wparam == 24 && GetKeyState(VK_SHIFT) < 0))) return 0;
    if (message == WM_PASTE) { message = EM_PASTESPECIAL; wparam = CF_UNICODETEXT; lparam = 0; }
    if (note->ready && !note->validating &&
        (message == EM_PASTESPECIAL ||
         (message == WM_KEYDOWN && GetKeyState(VK_MENU) >= 0 &&
          ((wparam == 'V' && GetKeyState(VK_CONTROL) < 0) ||
           (wparam == VK_INSERT && GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_CONTROL) >= 0))) ||
         (message == WM_CHAR && wparam == 22 && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0))) {
        LRESULT result;
        /* Native keyboard paste need not send WM_PASTE to the subclass.
           Report only after EN_CHANGE has rolled back and the editor returns. */
        note->edit_overflow = FALSE;
        result = CallWindowProcW(note->edit_proc, window, message, wparam, lparam);
        if (note->edit_overflow)
            MessageBoxW(note->window,
                L"The pasted text does not fit in this note.\n\nEnlarge the note or paste less text. Your existing text has been kept.",
                L"Jotilla", MB_OK | MB_ICONINFORMATION);
        return result;
    }
    if (message == WM_LBUTTONDOWN && note->ready && click_todo(note, lparam)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_BACK && note->ready && remove_todo_marker(note)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_RETURN && note->ready) {
        BOOL shift = GetKeyState(VK_SHIFT) < 0;
        if (todo_enter(note, shift) || (shift && promote_title(note))) return 0;
        SendMessageW(window, EM_REPLACESEL, TRUE, (LPARAM)L"\r\n"); return 0;
    }
    /* Rich Edit edits on key-down. Consume the corresponding translated
       characters so Enter/Backspace cannot perform a second operation. */
    if (message == WM_CHAR && note->ready && (wparam == L'\r' || wparam == L'\n' || wparam == L'\b')) return 0;
    if (message == WM_KILLFOCUS && note->ready) save_note(note);
    if (message == WM_KEYDOWN && note->ready && note->scrolling && wparam >= VK_PRIOR && wparam <= VK_DOWN) {
        LRESULT result = CallWindowProcW(note->edit_proc, window, message, wparam, lparam);
        /* A wheel gesture can hide an unchanged caret. Navigation must reveal
           it even when Home/End does not change the existing selection. */
        SendMessageW(window, EM_SCROLLCARET, 0, 0);
        return result;
    }
    return CallWindowProcW(note->edit_proc, window, message, wparam, lparam);
}

static BOOL set_title(Note *note, const wchar_t *text) {
    wchar_t *title;
    if (!copy_title(text,wcscspn(text,L"\r\n"),&title)) return FALSE;
    if ((!title && !note->title) || (title && note->title && !wcscmp(title, note->title))) { free(title); return TRUE; }
    free(note->title); note->title = title;
    mark_dirty(note, DIRTY_TEXT); redraw_bar(note);
    return TRUE;
}

static INT_PTR CALLBACK title_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    Note *note = (Note *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        RECT owner, rect;
        MONITORINFO monitor = {sizeof(monitor)};
        int x,y;
        note = (Note *)lparam;
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SetDlgItemTextW(dialog, IDC_TITLE, note->title ? note->title : L"");
        SendDlgItemMessageW(dialog, IDC_TITLE, EM_SETLIMITTEXT, TEXT_LIMIT, 0);
        SendDlgItemMessageW(dialog, IDC_TITLE, EM_SETSEL, 0, -1);
        GetWindowRect(note->window, &owner); GetWindowRect(dialog, &rect);
        GetMonitorInfoW(MonitorFromWindow(note->window,MONITOR_DEFAULTTONEAREST),&monitor);
        x=owner.left+(note->width-(rect.right-rect.left))/2;
        y=owner.top+bar_height(note);
        x=max(monitor.rcWork.left,min(x,monitor.rcWork.right-(rect.right-rect.left)));
        y=max(monitor.rcWork.top,min(y,monitor.rcWork.bottom-(rect.bottom-rect.top)));
        SetWindowPos(dialog, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, x,y,0,0,SWP_NOSIZE);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        HWND edit = GetDlgItem(dialog, IDC_TITLE);
        int length = GetWindowTextLengthW(edit);
        wchar_t *text = (wchar_t *)calloc((size_t)length + 1, sizeof(wchar_t));
        if (!text) return TRUE;
        GetWindowTextW(edit, text, length + 1);
        if (set_title(note, text)) { save_note(note); EndDialog(dialog, IDOK); }
        free(text); return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(wparam) == IDCANCEL) || message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}

typedef struct TagEditor {
    Note *note;
    TagCatalog catalog;
    const TagCount *suggestions[8];
    int count;
    HBRUSH background;
} TagEditor;

static BOOL set_tags(Note *note, const wchar_t *input) {
    wchar_t normal[TAGS_CAP], *copy = NULL;
    if (!tags_normalize(input, normal)) return FALSE;
    if (!wcscmp(note->tags ? note->tags : L"", normal)) return TRUE;
    if (*normal && !(copy = _wcsdup(normal))) return FALSE;
    free(note->tags); note->tags = copy; note->tags_dirty = TRUE;
    SetTimer(note->window, SAVE_TIMER, 400, NULL);
    return TRUE;
}

static void suggest_tags(HWND dialog, TagEditor *editor) {
    wchar_t text[TAGS_CAP], assigned[TAGS_CAP], prefix[TAGS_CAP], *start, label[96];
    HWND list = GetDlgItem(dialog, IDC_TAG_SUGGESTIONS);
    int i;
    GetDlgItemTextW(dialog, IDC_TAGS, text, TAGS_CAP);
    start = wcsrchr(text, L';');
    if (start) { *start++ = 0; } else start = text;
    if (!tags_normalize(start, prefix)) prefix[0] = 0;
    if (start == text || !tags_normalize(text, assigned)) assigned[0] = 0;
    editor->count = tags_suggest(&editor->catalog, prefix, assigned, editor->suggestions, 8);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0); SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < editor->count; ++i) {
        swprintf_s(label, 96, L"%s  (%u)", editor->suggestions[i]->name, editor->suggestions[i]->uses);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, NULL, TRUE);
}

static void accept_tag(HWND dialog, TagEditor *editor) {
    HWND edit = GetDlgItem(dialog, IDC_TAGS), list = GetDlgItem(dialog, IDC_TAG_SUGGESTIONS);
    int selection = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    wchar_t text[TAGS_CAP], insert[TAG_NAME_CAP + 3], *start;
    if (selection < 0 || selection >= editor->count) return;
    GetWindowTextW(edit, text, TAGS_CAP); start = wcsrchr(text, L';');
    start = start ? start + 1 : text;
    while (*start == L' ') ++start;
    swprintf_s(insert, TAG_NAME_CAP + 3, L"%s; ", editor->suggestions[selection]->name);
    SendMessageW(edit, EM_SETSEL, (WPARAM)(start - text), (LPARAM)wcslen(text));
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)insert); SetFocus(edit);
}

static LRESULT CALLBACK tag_input_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR subclass, DWORD_PTR data) {
    TagEditor *editor = (TagEditor *)data;
    HWND dialog = GetParent(window), list = GetDlgItem(dialog, IDC_TAG_SUGGESTIONS);
    (void)subclass;
    if (message == WM_GETDLGCODE && lparam) {
        MSG *key = (MSG *)lparam;
        if (key->wParam == VK_RETURN || key->wParam == VK_DOWN || key->wParam == VK_UP)
            return DLGC_WANTMESSAGE;
    }
    if (message == WM_KEYDOWN) {
        if (wparam == VK_RETURN) {
            if (SendMessageW(list, LB_GETCURSEL, 0, 0) != LB_ERR) accept_tag(dialog, editor);
            else SendMessageW(dialog, WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (wparam == VK_DOWN || wparam == VK_UP) {
            int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            selected += wparam == VK_DOWN ? 1 : -1;
            if (editor->count) SendMessageW(list, LB_SETCURSEL, max(0, min(selected, editor->count - 1)), 0);
            return 0;
        }
    }
    if (message == WM_CHAR && wparam == L'\r') return 0;
    return DefSubclassProc(window, message, wparam, lparam);
}

static INT_PTR CALLBACK tags_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    TagEditor *editor = (TagEditor *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        TagStore store;
        RECT owner, rect;
        MONITORINFO monitor = {sizeof(monitor)};
        editor = (TagEditor *)lparam;
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        editor->background = CreateSolidBrush(RGB(244,238,226));
        if (tags_store_load(directory, state_path, &store)) {
            tags_catalog_build(&store, &editor->catalog); tags_store_free(&store);
        }
        SetDlgItemTextW(dialog, IDC_TAGS, editor->note->tags ? editor->note->tags : L"");
        SendDlgItemMessageW(dialog, IDC_TAGS, EM_SETLIMITTEXT, TAGS_CAP - 1, 0);
        SendDlgItemMessageW(dialog, IDC_TAGS, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SetWindowSubclass(GetDlgItem(dialog, IDC_TAGS), tag_input_proc, 1, (DWORD_PTR)editor);
        SetWindowSubclass(GetDlgItem(dialog, IDC_TAG_SUGGESTIONS), tag_input_proc, 1, (DWORD_PTR)editor);
        suggest_tags(dialog, editor);
        GetWindowRect(editor->note->window, &owner); GetWindowRect(dialog, &rect);
        GetMonitorInfoW(MonitorFromWindow(editor->note->window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowPos(dialog, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            max(monitor.rcWork.left, min(owner.left, monitor.rcWork.right - (rect.right - rect.left))),
            max(monitor.rcWork.top, min(owner.top + bar_height(editor->note), monitor.rcWork.bottom - (rect.bottom - rect.top))),
            0, 0, SWP_NOSIZE);
        return TRUE;
    }
    if (!editor) return FALSE;
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC ||
        message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetTextColor((HDC)wparam, RGB(58,52,43)); SetBkColor((HDC)wparam, RGB(244,238,226));
        return (INT_PTR)editor->background;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == IDC_TAGS && HIWORD(wparam) == EN_CHANGE) suggest_tags(dialog, editor);
        if (LOWORD(wparam) == IDC_TAG_SUGGESTIONS && HIWORD(wparam) == LBN_DBLCLK) accept_tag(dialog, editor);
        if (LOWORD(wparam) == IDOK) {
            wchar_t text[TAGS_CAP];
            GetDlgItemTextW(dialog, IDC_TAGS, text, TAGS_CAP);
            if (!set_tags(editor->note, text)) {
                report_error(dialog, L"Use up to 24 tags of 63 characters, separated by ;. Quotes, # and line breaks are not allowed in a name.");
            } else if (!save_note(editor->note)) {
                report_error(dialog, L"Tags could not be saved. Check that the notes folder is writable.");
            } else EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message == WM_DESTROY) {
        tags_catalog_free(&editor->catalog);
        if (editor->background) DeleteObject(editor->background);
    }
    return FALSE;
}

static BOOL apply_opacity(Note *note) {
    LONG_PTR style = GetWindowLongPtrW(note->window, GWL_EXSTYLE);
    if (note->opacity < 100) {
        if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(note->window, GWL_EXSTYLE, style | WS_EX_LAYERED);
        return SetLayeredWindowAttributes(note->window, 0, (BYTE)MulDiv(255,note->opacity,100), LWA_ALPHA);
    }
    if (style & WS_EX_LAYERED) {
        SetWindowLongPtrW(note->window, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        RedrawWindow(note->window, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    return TRUE;
}

static BOOL fit_dimensions(Note *note, int width, int height);
static HWND browse_notes(void);

static BOOL resize_note(Note *note, int size) {
    int width = scaled(note, NOTE_SIZES[size].cx), height = scaled(note, NOTE_SIZES[size].cy);
    if (!fit_dimensions(note, width, height)) {
        report_error(note->window, L"The text does not fit this size. Choose a larger size or shorten the note first.");
        return FALSE;
    }
    note->size = size;
    SetWindowPos(note->window, NULL, 0, 0, note->width, note_height(note),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    save_note(note); return TRUE;
}

static BOOL fit_dimensions(Note *note, int width, int height) {
    int previous_width = note->width, previous_height = note->height;
    if (width < scaled(note, MIN_WIDTH) || height < scaled(note, MIN_HEIGHT) ||
        width > scaled(note, MAX_SIZE) || height > scaled(note, MAX_SIZE)) return FALSE;
    if (width == previous_width && height == previous_height) return TRUE;
    note->width = width; note->height = height;
    layout(note);
    if (note->ready && !accepts_text(note)) {
        note->width = previous_width; note->height = previous_height;
        layout(note); return FALSE;
    }
    note->size = -1;
    return TRUE;
}

static BOOL resize_selected_text(Note *note, int direction) {
    CHARRANGE selection;
    CHARFORMAT2W format = {0};
    BOOL changed;
    format.cbSize = sizeof(format);
    SendMessageW(note->edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    if ((format.dwMask & CFM_SIZE) && ((direction < 0 && format.yHeight <= 20) ||
                                      (direction > 0 && format.yHeight >= 32760))) return FALSE;
    SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    SendMessageW(note->edit, EM_STOPGROUPTYPING, 0, 0);
    note->validating = TRUE;
    /* Native size steps preserve each run's size, color and emphasis. One
       message creates one undo operation; RTF already persists the result. */
    changed = (BOOL)SendMessageW(note->edit, EM_SETFONTSIZE, (WPARAM)(INT_PTR)direction, 0);
    if (changed && !accepts_text(note)) {
        SendMessageW(note->edit, EM_UNDO, 0, 0);
        SendMessageW(note->edit, EM_EXSETSEL, 0, (LPARAM)&selection);
        changed = FALSE;
    }
    note->validating = FALSE;
    if (changed) mark_dirty(note, DIRTY_RICH);
    return changed;
}

static void wheel_resize(Note *note, int delta) {
    CHARRANGE selection = {0};
    int steps, width, height;
    BOOL text;
    if (!note->ready || note->validating) return;
    if (!note->collapsed) SendMessageW(note->edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    text = selection.cpMin != selection.cpMax;
    if (!text) selection.cpMin = selection.cpMax = 0;
    if (selection.cpMin != note->wheel_selection.cpMin || selection.cpMax != note->wheel_selection.cpMax) {
        note->wheel_delta = 0;
        note->wheel_selection = selection;
    }
    /* Windows also translates ordinary touchpad pinch into Ctrl+wheel.
       Accumulate its fractional deltas, without carrying them to a new target. */
    note->wheel_delta += delta;
    steps = note->wheel_delta / WHEEL_DELTA;
    note->wheel_delta %= WHEEL_DELTA;
    while (steps) {
        if (text) {
            if (!resize_selected_text(note, steps > 0 ? 1 : -1)) break;
            steps += steps > 0 ? -1 : 1;
            continue;
        }
        width = MulDiv(note->width, steps > 0 ? 110 : 100, steps > 0 ? 100 : 110);
        height = MulDiv(note->height, width, note->width);
        if (!fit_dimensions(note, width, height)) break;
        SetWindowPos(note->window, NULL, 0, 0, note->width, note_height(note),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        steps += steps > 0 ? -1 : 1;
    }
    if (!text) SetTimer(note->window, SAVE_TIMER, 400, NULL);
}

static void apply_scrolling(Note *note, BOOL enabled) {
    BOOL validating = note->validating;
    POINT origin = {0};
    note->validating = TRUE;
    note->scrolling = enabled;
    note->scroll_delta = 0;
    SendMessageW(note->edit, EM_SETOPTIONS, enabled ? ECOOP_OR : ECOOP_AND,
                 enabled ? ECO_AUTOVSCROLL : ~(LPARAM)ECO_AUTOVSCROLL);
    /* Scrolling belongs to the editor; its native bar must not take space
       from the note. Keep wheel, keyboard and caret scrolling enabled. */
    SendMessageW(note->edit, EM_SHOWSCROLLBAR, SB_VERT, FALSE);
    if (!enabled) SendMessageW(note->edit, EM_SETSCROLLPOS, 0, (LPARAM)&origin);
    note->validating = validating;
}

static BOOL set_note_scrolling(Note *note, BOOL enabled) {
    POINT previous_scroll = {0};
    if (note->scrolling == enabled) return TRUE;
    /* Flush pending content first: changing the mode then writes only its
       layout key. A failed write cannot leave a new mode active in memory. */
    if (!save_note(note)) {
        report_error(note->window, L"The note could not be saved. Its scrolling setting has not changed.");
        return FALSE;
    }
    SendMessageW(note->edit, EM_GETSCROLLPOS, 0, (LPARAM)&previous_scroll);
    apply_scrolling(note, enabled);
    if (!enabled && !text_fits(note)) {
        apply_scrolling(note, TRUE);
        SendMessageW(note->edit, EM_SETSCROLLPOS, 0, (LPARAM)&previous_scroll);
        report_error(note->window, L"The text does not fit without scrolling. Enlarge the note or shorten the text first. Your text has been kept.");
        return FALSE;
    }
    if (!save_note(note)) {
        apply_scrolling(note, !enabled);
        SendMessageW(note->edit, EM_SETSCROLLPOS, 0, (LPARAM)&previous_scroll);
        report_error(note->window, L"The scrolling setting could not be saved. The previous setting has been restored.");
        return FALSE;
    }
    return TRUE;
}

enum { SCROLL_THIS_NOTE = 1001, SCROLL_NEW_NOTES };

static void choose_scrolling(Note *note) {
    const TASKDIALOG_BUTTON buttons[] = {
        {SCROLL_THIS_NOTE, L"Only this note"},
        {SCROLL_NEW_NOTES, L"This note and new notes\nRemember as the default after restart. Other existing notes keep their setting."}
    };
    TASKDIALOGCONFIG dialog = {0};
    HWND owner = note->window;
    HRESULT result;
    BOOL enabled = !note->scrolling;
    int choice = IDCANCEL;
    dialog.cbSize = sizeof(dialog); dialog.hwndParent = note->window;
    dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = L"Jotilla";
    dialog.pszMainInstruction = enabled ? L"Enable scrollable text?" : L"Turn off scrollable text?";
    dialog.pszContent = enabled ? L"Text wraps to the note width and scrolls vertically." :
                                 L"Text will need to fit inside the note. No text will be removed.";
    dialog.cButtons = ARRAYSIZE(buttons); dialog.pButtons = buttons; dialog.nDefaultButton = SCROLL_THIS_NOTE;
    result = TaskDialogIndirect(&dialog, &choice, NULL, NULL);
    if (!IsWindow(owner) || (Note *)GetWindowLongPtrW(owner, GWLP_USERDATA) != note) return;
    if (FAILED(result)) {
        report_error(note->window, L"The scrolling options could not be opened."); return;
    }
    if ((choice != SCROLL_THIS_NOTE && choice != SCROLL_NEW_NOTES) || !set_note_scrolling(note, enabled)) return;
    if (choice == SCROLL_NEW_NOTES) {
        if (write_setting(L"app", L"scroll", enabled)) default_scrolling = enabled;
        else report_error(note->window, L"This note was updated, but the default for new notes could not be saved.");
    }
}

static void note_command(Note *note, UINT command) {
    if (command == CMD_BROWSE) {
        browse_notes();
    } else if (command == CMD_ARCHIVE) {
        if ((INT_PTR)ShellExecuteW(note->window, L"open", directory, NULL, NULL, SW_SHOWNORMAL) <= 32)
            report_error(note->window, L"The notes archive could not be opened.");
    } else if (command == CMD_TITLE) {
        DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_TITLE), note->window, title_dialog, (LPARAM)note);
    } else if (command == CMD_TAGS) {
        TagEditor editor = {0};
        editor.note = note;
        DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_TAGS), note->window, tags_dialog, (LPARAM)&editor);
    } else if (command == CMD_SCROLL) {
        choose_scrolling(note);
    } else if (command >= CMD_OPACITY && command < CMD_OPACITY + 5) {
        int previous = note->opacity;
        note->opacity = OPACITIES[command - CMD_OPACITY];
        if (!apply_opacity(note)) { note->opacity = previous; apply_opacity(note); }
        save_note(note);
    } else if (command >= CMD_SIZE && command < CMD_SIZE + SIZE_COUNT) {
        resize_note(note, (int)command - CMD_SIZE);
    } else if (command >= CMD_PALETTE && command < CMD_PALETTE + DEFAULT_PALETTE_COUNT) {
        int palette = (int)command - CMD_PALETTE;
        if (write_setting(L"app", L"palette", palette) && write_setting(L"app", L"next_color", 0)) {
            current_palette = palette; next_color = saved_next_color = 0;
            shuffled_palette = FALSE; colors_left = 0;
        } else report_error(note->window, L"The palette preference could not be saved.");
    }
}

static HMENU build_note_menu(const Note *note) {
    HMENU menu = CreatePopupMenu(), opacity = CreatePopupMenu(), size = CreatePopupMenu(), palette = CreatePopupMenu();
    wchar_t label[64];
    int i;
    if (!menu || !opacity || !size || !palette) {
        if (menu) DestroyMenu(menu);
        if (opacity) DestroyMenu(opacity);
        if (size) DestroyMenu(size);
        if (palette) DestroyMenu(palette);
        return NULL;
    }
    AppendMenuW(menu, MF_STRING, CMD_ARCHIVE, L"Open notes archive");
    AppendMenuW(menu, MF_STRING, CMD_BROWSE, L"Browse notes");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < 5; ++i) {
        swprintf_s(label, 64, i ? L"%d%% opacity" : L"%d%% (opaque)", OPACITIES[i]);
        AppendMenuW(opacity, MF_STRING | (note->opacity == OPACITIES[i] ? MF_CHECKED : 0), CMD_OPACITY + i, label);
    }
    for (i = 0; i < SIZE_COUNT; ++i)
        AppendMenuW(size, MF_STRING | (note->size == i ? MF_CHECKED : 0), CMD_SIZE + i, SIZE_NAMES[i]);
    for (i = 0; i < DEFAULT_PALETTE_COUNT; ++i)
        AppendMenuW(palette, MF_STRING | (current_palette == i ? MF_CHECKED : 0), CMD_PALETTE + i, PALETTE_NAMES[i]);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)opacity, L"Opacity");
    AppendMenuW(menu, MF_STRING, CMD_TITLE, L"Edit title");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)size, L"Note size");
    AppendMenuW(menu, MF_STRING | (note->scrolling ? MF_CHECKED : 0), CMD_SCROLL, L"Scrollable text");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)palette, L"New note palette");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_TAGS, L"Tags\tCtrl+T");
    return menu;
}

static void show_note_menu(Note *note, POINT point) {
    HMENU menu = build_note_menu(note);
    HWND owner = note->window;
    UINT command;
    if (!menu) return;
    SetForegroundWindow(owner);
    command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, NULL);
    DestroyMenu(menu);
    if (command && IsWindow(owner) && (Note *)GetWindowLongPtrW(owner, GWLP_USERDATA) == note) note_command(note, command);
    PostMessageW(owner, WM_NULL, 0, 0);
}

static LRESULT CALLBACK note_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Note *note = (Note *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        note = (Note *)((CREATESTRUCTW *)lparam)->lpCreateParams;
        note->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)note);
    }
    if (!note) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
            if (wparam == 'E') { browse_notes(); archive_notes(); return 0; }
            if (sizing_key(wparam)) { note->wheel_delta=0; wheel_resize(note,sizing_key(wparam)*WHEEL_DELTA); return 0; }
            if (wparam == 'Z' || wparam == 'Y') {
                SendMessageW(note->edit,wparam=='Y' || GetKeyState(VK_SHIFT)<0 ? EM_REDO : EM_UNDO,0,0); return 0;
            }
        }
        if (wparam == 'T' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
            note_command(note, CMD_TAGS); return 0;
        }
        if (wparam == 'F' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0) {
            browse_notes(); archive_search(); return 0;
        }
        break;
    case WM_NCCALCSIZE: return 0;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE: return TRUE;
    case WM_CREATE:
        {
            enum DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
            DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        }
        note->brush = CreateSolidBrush(note_color(note));
        note->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL |
                                    (note->scrolling ? ES_AUTOVSCROLL : 0),
                                    0, 0, 1, 1, window, (HMENU)(INT_PTR)10, instance, NULL);
        if (!note->edit || !note->brush) return -1;
        {
            IUnknown *editor = NULL;
            SendMessageW(note->edit, EM_GETOLEINTERFACE, 0, (LPARAM)&editor);
            if (!editor) return -1;
            editor->lpVtbl->QueryInterface(editor, &TEXT_DOCUMENT_IID, (void **)&note->document);
            editor->lpVtbl->Release(editor);
            if (!note->document) return -1;
        }
        note->edit_proc = (WNDPROC)SetWindowLongPtrW(note->edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SendMessageW(note->edit, EM_SETLIMITTEXT, TEXT_LIMIT, 0);
        SendMessageW(note->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SendMessageW(note->edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_REQUESTRESIZE | ENM_SELCHANGE);
        SendMessageW(note->edit, EM_SETUNDOLIMIT, 100, 0);
        note->date_tip = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,NULL,WS_POPUP|TTS_ALWAYSTIP,
            0,0,0,0,window,NULL,instance,NULL);
        if (note->date_tip) {
            TOOLINFOW tip = {sizeof(tip)};
            tip.uFlags=TTF_SUBCLASS; tip.hwnd=window; tip.uId=1;
            SetRect(&tip.rect,0,0,bar_height(note),bar_height(note)); tip.lpszText=LPSTR_TEXTCALLBACKW;
            SendMessageW(note->date_tip,TTM_ADDTOOLW,0,(LPARAM)&tip);
        }
        update_style(note);
        return 0;
    case WM_NCHITTEST: {
        POINT p = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT rect;
        ScreenToClient(window, &p); GetClientRect(window, &rect);
        if (!note->collapsed) {
            int edge = scaled(note, 6);
            BOOL left = p.x < edge, right = p.x >= rect.right - edge;
            BOOL top = p.y < edge, bottom = p.y >= rect.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (p.y < bar_height(note) && (p.x < bar_height(note) || p.x >= rect.right - bar_height(note) * 2)) return HTCLIENT;
        return p.y < bar_height(note) ? HTCAPTION : HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = scaled(note, MIN_WIDTH);
        info->ptMinTrackSize.y = note->collapsed ? bar_height(note) : scaled(note, MIN_HEIGHT);
        info->ptMaxTrackSize.x = scaled(note, MAX_SIZE);
        info->ptMaxTrackSize.y = note->collapsed ? bar_height(note) : scaled(note, MAX_SIZE);
        return 0;
    }
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS *position = (WINDOWPOS *)lparam;
        if (!(position->flags & SWP_NOSIZE)) {
            if (!fit_dimensions(note, position->cx, note->collapsed ? note->height : position->cy)) {
                position->cx = note->width;
                position->cy = note_height(note);
                position->flags |= SWP_NOMOVE;
            }
            if (note->collapsed) position->cy = bar_height(note);
        }
        return 0;
    }
    case WM_SIZING: {
        RECT *rect = (RECT *)lparam;
        if (!fit_dimensions(note, rect->right - rect->left, rect->bottom - rect->top))
            GetWindowRect(window, rect);
        return TRUE;
    }
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) {
            wheel_resize(note, GET_WHEEL_DELTA_WPARAM(wparam)); return 0;
        }
        return 0;
    case WM_SIZE:
        if (note->edit) layout(note);
        return 0;
    case WM_EXITSIZEMOVE:
        if (note->ready) save_note(note);
        return 0;
    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        update_style(note);
        SetWindowPos(window, NULL, rect->left, rect->top, note->width,
                     note_height(note),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(note);
        if (note->ready) save_note(note);
        return 0;
    }
    case WM_COMMAND:
        if (!lparam) { note_command(note, LOWORD(wparam)); return 0; }
        if (HIWORD(wparam) == EN_CHANGE && note->ready && !note->validating) {
            BOOL fits = accepts_text(note);
            if (!fits || !remember_text(note)) {
                note->edit_overflow = !fits;
                note->validating = TRUE;
                if (!SendMessageW(note->edit, EM_UNDO, 0, 0))
                    SetWindowTextW(note->edit, note->accepted_text);
                SendMessageW(note->edit, EM_SETSEL, note->selection_start, note->selection_end);
                note->validating = FALSE;
                return 0;
            }
            mark_dirty(note, DIRTY_TEXT | DIRTY_RICH);
        }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lparam)->code == TTN_GETDISPINFOW) {
            if (note->calendar_date) swprintf_s(note->date_label,64,L"Calendar: %04u-%02u-%02u",note->calendar_date/10000,
                note->calendar_date/100%100,note->calendar_date%100);
            else wcscpy_s(note->date_label,64,L"Assign a calendar day");
            ((NMTTDISPINFOW *)lparam)->lpszText=note->date_label; return 0;
        }
        if (((NMHDR *)lparam)->hwndFrom == note->edit) {
            if (((NMHDR *)lparam)->code == EN_REQUESTRESIZE) {
                const RECT *rect = &((REQRESIZE *)lparam)->rc;
                note->text_extent.cx = rect->right - rect->left;
                note->text_extent.cy = rect->bottom - rect->top;
            } else if (((NMHDR *)lparam)->code == EN_SELCHANGE) {
                const CHARRANGE *range = &((SELCHANGE *)lparam)->chrg;
                if (range->cpMin != note->wheel_selection.cpMin || range->cpMax != note->wheel_selection.cpMax)
                    note->wheel_delta = 0;
            }
        }
        return 0;
    case WM_TIMER:
        if (wparam == SAVE_TIMER) save_note(note);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wparam, ink_color(note)); SetBkColor((HDC)wparam, note_color(note));
        return (LRESULT)note->brush;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint_note(note); return 0;
    case WM_MOUSEMOVE: {
        int hover = button_at(note, lparam);
        if (!note->tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
            note->tracking=TrackMouseEvent(&track);
        }
        if (hover != note->hover) { note->hover = hover; redraw_bar(note); }
        return 0;
    }
    case WM_MOUSELEAVE:
        note->tracking=FALSE;
        if (note->hover) { note->hover = 0; redraw_bar(note); }
        return 0;
    case WM_LBUTTONDOWN:
        note->pressed = button_at(note, lparam);
        if (note->pressed) SetCapture(window);
        return 0;
    case WM_LBUTTONUP: {
        int pressed = note->pressed;
        note->pressed = 0;
        if (GetCapture() == window) ReleaseCapture();
        if (pressed && pressed == button_at(note, lparam)) {
            if (pressed == 1) toggle_note(note);
            else if (pressed == 3) show_note_calendar(note);
            else SendMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_CAPTURECHANGED: note->pressed = 0; return 0;
    case NOTE_DATE_MESSAGE:
        if (!set_note_date(note,(unsigned)wparam))
            report_error(window,L"The calendar date could not be saved. Check the archive folder.");
        return 0;
    case WM_NCRBUTTONUP:
        if (wparam == HTCAPTION) {
            POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            show_note_menu(note, point); return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, client = point;
        if (point.x == -1 && point.y == -1) {
            point.x = scaled(note, 10); point.y = bar_height(note);
            ClientToScreen(window, &point); show_note_menu(note, point); return 0;
        }
        ScreenToClient(window, &client);
        if (client.y >= 0 && client.y < bar_height(note)) show_note_menu(note, point);
        else if (!note->collapsed) show_note_palette(note, point);
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTCAPTION) { note_command(note, CMD_TITLE); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) remember_note(note);
        break;
    case WM_SETFOCUS:
        if (!note->collapsed) SetFocus(note->edit);
        return 0;
    case WM_CLOSE:
        if (!save_note(note) || (note->persisted && !save_layout(note, FALSE))) {
            report_error(window, L"This note could not be saved. Check that the notes folder is writable.\nThe note will stay open.");
            return 0;
        }
        destroy_note(note);
        return 0;
    case WM_QUERYENDSESSION: return save_note(note);
    case WM_ENDSESSION:
        end_session(wparam != 0);
        return 0;
    case WM_NCDESTROY: {
        Note **link = &notes;
        while (*link && *link != note) link = &(*link)->next;
        if (*link) *link = note->next;
        if (note->brush) DeleteObject(note->brush);
        if (note->font) DeleteObject(note->font);
        if (note->date_font) DeleteObject(note->date_font);
        if (note->pen) DeleteObject(note->pen);
        if (note->document) note->document->lpVtbl->Release(note->document);
        free(note->accepted_text);
        free(note->title);
        free(note->tags);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        free(note);
        break;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static Note *open_note_at(const wchar_t *id, int color, BOOL restore, const POINT *origin) {
    Note *note = (Note *)calloc(1, sizeof(Note));
    RECT rect;
    MONITORINFO monitor = {sizeof(monitor)};
    int x, y, width, height;
    HWND window;
    if (!note) return NULL;
    if (wcscpy_s(note->id, 80, id) || swprintf_s(note->path, PATH_CAP, L"%s\\%s.txt", directory, id) < 0) {
        free(note); return NULL;
    }
    note->dpi = (int)GetDpiForSystem();
    note->color = restore ? layout_setting(note, LAYOUT_COLOR, color) : color;
    note->palette = restore ? layout_setting(note, LAYOUT_PALETTE, 0) : current_palette;
    note->custom_ink = restore && layout_setting(note, LAYOUT_CUSTOM_INK, FALSE) != 0;
    /* Older notes retain bounded text even if the new-note default changes. */
    note->scrolling = restore ? layout_setting(note, LAYOUT_SCROLL, FALSE) != 0 : default_scrolling;
    if (restore) {
        wchar_t date[32];
        GetPrivateProfileStringW(id,L"calendar_date",L"",date,32,state_path);
        note->calendar_date = date_parse(date);
    }
    if (note->palette < 0 || note->palette >= PALETTE_COUNT) note->palette = 0;
    if (note->color < 0 || note->color >= PALETTE_SIZES[note->palette]) note->color = 0;
    note->size = restore ? layout_setting(note, LAYOUT_SIZE, 1) : 1;
    if (note->size < 0 || note->size >= SIZE_COUNT) note->size = 1;
    width = restore ? layout_setting(note, LAYOUT_WIDTH, NOTE_SIZES[note->size].cx) : NOTE_SIZES[note->size].cx;
    height = restore ? layout_setting(note, LAYOUT_HEIGHT, NOTE_SIZES[note->size].cy) : NOTE_SIZES[note->size].cy;
    note->width = scaled(note, max(MIN_WIDTH, min(width, MAX_SIZE)));
    note->height = scaled(note, max(MIN_HEIGHT, min(height, MAX_SIZE)));
    if (width != NOTE_SIZES[note->size].cx || height != NOTE_SIZES[note->size].cy) note->size = -1;
    note->opacity = restore ? layout_setting(note, LAYOUT_OPACITY, 100) : 100;
    if (note->opacity < 60 || note->opacity > 100) note->opacity = 100;
    width = note->width;
    note->collapsed = restore && layout_setting(note, LAYOUT_COLLAPSED, FALSE);
    height = note_height(note);
    x = origin ? origin->x : restore ? layout_setting(note, LAYOUT_X, 100) : 100 + next_color % 7 * 28;
    y = origin ? origin->y : restore ? layout_setting(note, LAYOUT_Y, 100) : 100 + next_color % 7 * 28;
    /* Restore against the title bar: a folded note, or an expanded note with
       its body past the screen edge, must retain its reachable position. */
    x = max(-100000, min(x, 100000)); y = max(-100000, min(y, 100000));
    if (!origin) {
        SetRect(&rect, x, y, x + width, y + (restore ? bar_height(note) : height));
        if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) { free(note); return NULL; }
        /* Keep creation on the chosen monitor; WM_CREATE obtains its DPI. */
        x = max(monitor.rcWork.left, min(x, monitor.rcWork.right - 1));
        y = max(monitor.rcWork.top, min(y, monitor.rcWork.bottom - 1));
    }
    if (restore) {
        layout_setting(note, LAYOUT_TITLE, FALSE);
        layout_setting(note, LAYOUT_OPEN, TRUE);
        note->layout_saved = TRUE;
    }
    note->next = notes; notes = note;
    window = CreateWindowExW(WS_EX_TOOLWINDOW | (always_on_top ? WS_EX_TOPMOST : 0), NOTE_CLASS, L"Jotilla",
                             WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                             x, y, width, note->collapsed ? bar_height(note) : height,
                             NULL, NULL, instance, note);
    /* A failed WM_CREATE destroys the window and releases Note in WM_NCDESTROY. */
    if (!window) {
        if (notes == note) { notes = note->next; free(note); }
        return NULL;
    }
    if (restore) {
        if (!load_text(note) || !tags_read(state_path, note->id, &note->tags)) { destroy_note(note); return NULL; }
        note->persisted = TRUE;
        load_rich(note);
        if (!set_editor_ink(note,ink_color(note))) { destroy_note(note); return NULL; }
    }
    if (!origin) {
        x = max(monitor.rcWork.left, min(x, monitor.rcWork.right - note->width));
        y = max(monitor.rcWork.top, min(y, monitor.rcWork.bottom - (restore ? bar_height(note) : note_height(note))));
    }
    /* Hotkeys provide an exact screen origin before the window is shown.
       No cursor polling, subsequent jump, or change to tray placement. */
    SetWindowPos(window, NULL, x, y, note->width, note_height(note), SWP_NOZORDER | SWP_NOACTIVATE);
    layout(note);
    if (!accepts_text(note) || !remember_text(note)) { destroy_note(note); return NULL; }
    note->ready = TRUE;
    if (!apply_opacity(note)) { destroy_note(note); return NULL; }
    if (note_has_content(note) && !save_layout(note, TRUE)) { destroy_note(note); return NULL; }
    if (note->persisted && !note_has_content(note) && !save_note(note)) { destroy_note(note); return NULL; }
    remember_note(note);
    if (!testing) {
        ShowWindow(window, startup_restore ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        if (!startup_restore) {
            SetForegroundWindow(window);
            if (!note->collapsed) SetFocus(note->edit);
        }
    }
    return note;
}

static Note *open_note(const wchar_t *id, int color, BOOL restore) {
    return open_note_at(id, color, restore, NULL);
}

static Note *new_note_at(const POINT *origin) {
    SYSTEMTIME now;
    wchar_t id[80], path[PATH_CAP];
    Note *note;
    int suffix, color = draw_note_color();
    GetLocalTime(&now);
    for (suffix = 0; suffix < 10000; ++suffix) {
        swprintf_s(id, 80, L"note-%04u-%02u-%02u_%02u-%02u-%02u-%03u-%03d", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, suffix);
        if (swprintf_s(path, PATH_CAP, L"%s\\%s.txt", directory, id) < 0) return NULL;
        if (!find_note(id) && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return NULL;
            break;
        }
    }
    if (suffix == 10000) return NULL;
    note = open_note_at(id, color, FALSE, origin);
    if (note) {
        if (shuffled_palette) --colors_left;
        next_color = (next_color + 1) % COLOR_COUNT;
    }
    return note;
}

static Note *new_note(void) { return new_note_at(NULL); }

static BOOL restore_notes(void) {
    WIN32_FIND_DATAW found;
    HANDLE search;
    wchar_t pattern[PATH_CAP];
    BOOL ok = TRUE;
    if (swprintf_s(pattern, PATH_CAP, L"%s\\*.txt", directory) < 0) return FALSE;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    restoring_notes = TRUE;
    do {
        wchar_t id[80];
        size_t length = wcslen(found.cFileName);
        if (wcsncmp(found.cFileName, L"note-", 5) && wcsncmp(found.cFileName, L"nota-", 5)) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || length < 5 || length - 4 >= 80) continue;
        wcsncpy_s(id, 80, found.cFileName, length - 4);
        if (setting(id, L"open", TRUE) && !open_note(id, 0, TRUE)) ok = FALSE;
    } while (FindNextFileW(search, &found));
    if (GetLastError() != ERROR_NO_MORE_FILES) ok = FALSE;
    FindClose(search);
    restoring_notes = FALSE;
    if (!last_note[0] && notes) remember_note(notes);
    return ok;
}

static void update_tray(DWORD action) {
    NOTIFYICONDATAW data = {0};
    data.cbSize = sizeof(data); data.hWnd = controller; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = TRAY_MESSAGE;
    data.hIcon = notes_hidden && hidden_icon ? hidden_icon : (app_icon ? app_icon : LoadIconW(NULL, IDI_APPLICATION));
    wcscpy_s(data.szTip, 128, notes_hidden
        ? L"Jotilla (notes hidden) - right-click to show all notes"
        : L"Jotilla - last note; double-click for explorer; Ctrl+Alt++ for new note");
    Shell_NotifyIconW(action, &data);
}

static void tray(BOOL add) {
    update_tray(add ? NIM_ADD : NIM_DELETE);
}

static BOOL save_all(void) {
    Note *note;
    BOOL ok = TRUE;
    for (note = notes; note; note = note->next) if (!save_note(note)) ok = FALSE;
    if (wcscmp(last_note, saved_last_note)) {
        if (WritePrivateProfileStringW(L"app", L"last_note", last_note, state_path))
            wcscpy_s(saved_last_note, 80, last_note);
        else ok = FALSE;
    }
    if (next_color != saved_next_color) {
        if (write_setting(L"app", L"next_color", next_color)) saved_next_color = next_color;
        else ok = FALSE;
    }
    return ok;
}

/* Windows can terminate the process after any top-level window acknowledges
   WM_ENDSESSION. Save the whole session from the first recipient, once, and
   flush the profile cache. A cancelled shutdown keeps every window alive. */
static void end_session(BOOL ending) {
    static BOOL saved;
    if (!ending) { saved = FALSE; return; }
    if (!saved && (saved = save_all()) != FALSE)
        WritePrivateProfileStringW(NULL, NULL, NULL, state_path);
}

static Note *find_note(const wchar_t *id) {
    Note *note;
    for (note = notes; note; note = note->next) if (!wcscmp(note->id, id)) return note;
    return NULL;
}

static BOOL open_archived_note(const wchar_t *id) {
    Note *note = find_note(id);
    if (!note) note = open_note(id, 0, TRUE);
    if (!note) return FALSE;
    remember_note(note);
    if (note->collapsed) toggle_note(note);
    ShowWindow(note->window, SW_SHOWNORMAL);
    SetForegroundWindow(note->window); SetFocus(note->edit);
    return TRUE;
}

/* Acquire both files before marking either for deletion. Keeping the handles
   open lets a failed disposition be rolled back before any file disappears. */
static BOOL remove_note_files(const wchar_t *id) {
    static const wchar_t *extensions[] = {L"txt", L"rtf"};
    HANDLE files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    wchar_t path[PATH_CAP];
    FILE_DISPOSITION_INFO disposition = {TRUE};
    BOOL ok = TRUE;
    int i, marked = 0;
    for (i = 0; i < 2; ++i) {
        if (swprintf_s(path, PATH_CAP, L"%s\\%s.%s", directory, id, extensions[i]) < 0) { ok = FALSE; break; }
        files[i] = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (files[i] == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) { ok = FALSE; break; }
    }
    if (ok) for (i = 0; i < 2; ++i) {
        if (files[i] == INVALID_HANDLE_VALUE) continue;
        if (!SetFileInformationByHandle(files[i], FileDispositionInfo, &disposition, sizeof(disposition))) { ok = FALSE; break; }
        marked = i + 1;
    }
    if (!ok) {
        disposition.DeleteFile = FALSE;
        for (i = 0; i < marked; ++i)
            if (files[i] != INVALID_HANDLE_VALUE)
                SetFileInformationByHandle(files[i], FileDispositionInfo, &disposition, sizeof(disposition));
    }
    for (i = 0; i < 2; ++i) if (files[i] && files[i] != INVALID_HANDLE_VALUE) {
        CloseHandle(files[i]); files[i] = INVALID_HANDLE_VALUE;
    }
    if (!ok) return FALSE;
    WritePrivateProfileStringW(id, NULL, NULL, state_path);
    for (i = 0; i < 2; ++i) {
        if (swprintf_s(path, PATH_CAP, L"%s\\%s.%s.tmp", directory, id, extensions[i]) >= 0) DeleteFileW(path);
    }
    archive_changed(id, FALSE, TRUE);
    return TRUE;
}

static BOOL delete_archived_note(const wchar_t *id) {
    Note *note = find_note(id);
    if ((note && !save_note(note)) || !remove_note_files(id)) return FALSE;
    if (note) destroy_note(note);
    return TRUE;
}

static HWND browse_notes(void) {
    static const ArchiveActions actions = {save_all, open_archived_note, delete_archived_note, choose_archive};
    HWND browser = archive_show(controller, directory, state_path, &actions);
    if (!browser)
        report_error(controller, L"The notes archive could not be opened.");
    return browser;
}

static void recall_note(void) {
    wchar_t path[PATH_CAP];
    DWORD attributes;
    if (last_note[0]) {
        if (find_note(last_note)) { open_archived_note(last_note); return; }
        if (swprintf_s(path, PATH_CAP, L"%s\\%s.txt", directory, last_note) >= 0) {
            attributes = GetFileAttributesW(path);
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!open_archived_note(last_note))
                    report_error(controller, L"The last note could not be opened. Its original files have been kept.");
                return;
            }
        }
    }
    browse_notes();
}

static void load_preferences(void) {
    GetPrivateProfileStringW(L"app", L"last_note", L"", last_note, 80, state_path);
    /* Never let an externally edited ID escape the selected archive. */
    if ((wcsncmp(last_note, L"note-", 5) && wcsncmp(last_note, L"nota-", 5)) ||
        wcsspn(last_note, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != wcslen(last_note))
        last_note[0] = 0;
    wcscpy_s(saved_last_note, 80, last_note);
    default_scrolling = setting(L"app", L"scroll", FALSE) != 0;
    always_on_top = setting(L"app", L"always_on_top", TRUE) != 0;
    archive_set_topmost(always_on_top);
    next_color = setting(L"app", L"next_color", 0);
    if (next_color < 0 || next_color >= COLOR_COUNT) next_color = 0;
    saved_next_color = next_color;
    current_palette = setting(L"app", L"palette", 0);
    if (current_palette < 0 || current_palette >= DEFAULT_PALETTE_COUNT) current_palette = 0;
    favorite_palette = setting(L"app", L"favorite_palette", -1);
    if (favorite_palette < -1 || favorite_palette >= PALETTE_COUNT) favorite_palette = -1;
    shuffled_palette = favorite_palette >= 0; colors_left = 0;
    if (shuffled_palette) current_palette = favorite_palette;
}

static BOOL switch_archive(const wchar_t *target, BOOL move, BOOL *copies_left) {
    wchar_t selected[PATH_CAP];
    if (!save_all()) return FALSE;
    /* This cache-flush form returns zero even on success. Actual writes were
       checked by save_all; migration validates every file handle separately. */
    WritePrivateProfileStringW(NULL, NULL, NULL, state_path);
    /* A successful transfer is durable before any window changes ownership. */
    if (!location_change(location_config, directory, target, move, selected, copies_left)) return FALSE;
    archive_close();
    while (notes) destroy_note(notes);
    wcscpy_s(directory, PATH_CAP, selected);
    swprintf_s(state_path, PATH_CAP, L"%s\\notes.ini", directory);
    load_preferences();
    if (!restore_notes()) report_error(controller, L"Some notes could not be opened. Their original files are still in the selected archive.");
    return TRUE;
}

static void choose_archive(HWND owner) {
    const TASKDIALOG_BUTTON buttons[] = {
        {100, L"Move current archive\nMove notes, formatting and tags to an empty folder."},
        {101, L"Open or create archive\nUse another folder. Keep the current archive where it is."}
    };
    TASKDIALOGCONFIG dialog = {0};
    WINDOWPLACEMENT placement = {sizeof(placement)};
    wchar_t target[PATH_CAP];
    int choice = 0;
    BOOL copies_left, move;
    HRESULT result;
    dialog.cbSize = sizeof(dialog); dialog.hwndParent = owner;
    dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = L"Archive";
    dialog.pszMainInstruction = L"Choose where your notes live";
    dialog.pszContent = L"The selected folder becomes the default for future starts.";
    dialog.pszFooter = directory;
    dialog.cButtons = 2; dialog.pButtons = buttons;
    if (FAILED(TaskDialogIndirect(&dialog, &choice, NULL, NULL)) || (choice != 100 && choice != 101)) return;
    move = choice == 100;
    result = location_pick(owner, directory, move, target);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    if (FAILED(result)) { report_error(owner, L"The folder chooser could not be opened."); return; }
    GetWindowPlacement(owner, &placement);
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (!switch_archive(target, move, &copies_left)) {
        DWORD error = GetLastError();
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        report_error(owner, error == ERROR_DIR_NOT_EMPTY ?
            L"To move the archive, choose an empty folder. To use an existing archive, choose Open or create archive." :
            error == ERROR_INVALID_PARAMETER ? L"Choose a separate folder, outside the current archive and its parent folders." :
            L"The archive could not be changed. Check folder permissions and close programs using the files. The current archive has been kept.");
        return;
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    {
        HWND browser = browse_notes();
        if (browser) SetWindowPlacement(browser, &placement);
    }
    if (copies_left) report_error(controller, L"The new archive is active. Some original files could not be removed and remain in the previous folder as backup copies.");
}

static void apply_always_on_top(BOOL enabled) {
    HWND insert = enabled ? HWND_TOPMOST : HWND_NOTOPMOST;
    Note *note;
    always_on_top = enabled;
    for (note = notes; note; note = note->next) {
        if (IsWindow(note->window))
            SetWindowPos(note->window, insert, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    archive_set_topmost(enabled);
}

static void toggle_always_on_top(void) {
    BOOL next = !always_on_top;
    if (!write_setting(L"app", L"always_on_top", next)) {
        report_error(controller, L"The always on top setting could not be saved.");
        return;
    }
    apply_always_on_top(next);
}

static void set_notes_hidden(BOOL hidden) {
    Note *note;
    notes_hidden = hidden;
    for (note = notes; note; note = note->next) {
        if (IsWindow(note->window))
            ShowWindow(note->window, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
    if (hidden) archive_hide();
    update_tray(NIM_MODIFY);
}

static LRESULT CALLBACK controller_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    static enum { TRAY_IDLE, TRAY_PENDING, TRAY_DOUBLE } tray_click;
    if (taskbar_created && message == taskbar_created) { tray(TRUE); return 0; }
    switch (message) {
    case WM_HOTKEY: {
        POINT point;
        if (!GetCursorPos(&point) || !new_note_at(&point))
            report_error(NULL, L"A new note could not be created at the pointer. Check the notes folder.");
        return 0;
    }
    case NEW_NOTE_MESSAGE:
        if (!new_note()) report_error(NULL, L"A new note could not be created. Check the notes folder.");
        return 0;
    case TRAY_MESSAGE:
        if (lparam == WM_LBUTTONDOWN && tray_click == TRAY_DOUBLE) tray_click = TRAY_IDLE;
        if (lparam == WM_LBUTTONUP) {
            if (tray_click == TRAY_DOUBLE) tray_click = TRAY_IDLE;
            else if (!notes_hidden) {
                /* Wait for Windows to distinguish a single click from a double click. */
                tray_click = TRAY_PENDING;
                if (!SetTimer(window, TRAY_CLICK_TIMER, GetDoubleClickTime(), NULL)) {
                    tray_click = TRAY_IDLE;
                    recall_note();
                }
            }
        }
        if (lparam == WM_LBUTTONDBLCLK) {
            KillTimer(window, TRAY_CLICK_TIMER);
            tray_click = TRAY_DOUBLE;
            browse_notes();
        }
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            POINT point;
            HMENU menu = CreatePopupMenu(), options = CreatePopupMenu();
            UINT command;
            KillTimer(window, TRAY_CLICK_TIMER);
            tray_click = TRAY_IDLE;
            if (!menu || !options) {
                if (menu) DestroyMenu(menu);
                if (options) DestroyMenu(options);
                return 0;
            }
            GetCursorPos(&point);
            AppendMenuW(options, MF_STRING | (always_on_top ? MF_CHECKED : 0), CMD_TOPMOST, L"Always on top");
            if (notes_hidden) AppendMenuW(menu, MF_STRING, CMD_SHOW_ALL, L"Show all notes");
            else AppendMenuW(menu, MF_STRING, CMD_HIDE_ALL, L"Hide all notes");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, CMD_BROWSE, L"Browse notes");
            AppendMenuW(menu, MF_STRING, NEW_NOTE_MESSAGE, L"New note");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)options, L"Options");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, EXIT_COMMAND, L"Exit");
            SetForegroundWindow(window);
            command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, NULL);
            if (command == CMD_HIDE_ALL) set_notes_hidden(TRUE);
            else if (command == CMD_SHOW_ALL) set_notes_hidden(FALSE);
            else if (command == CMD_BROWSE) browse_notes();
            else if (command == NEW_NOTE_MESSAGE) PostMessageW(window, NEW_NOTE_MESSAGE, 0, 0);
            else if (command == CMD_TOPMOST) toggle_always_on_top();
            else if (command == EXIT_COMMAND) PostMessageW(window, WM_CLOSE, 0, 0);
            PostMessageW(window, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;
    case WM_TIMER:
        if (wparam == TRAY_CLICK_TIMER && tray_click == TRAY_PENDING) {
            KillTimer(window, TRAY_CLICK_TIMER);
            tray_click = TRAY_IDLE;
            recall_note();
        }
        return 0;
    case WM_CLOSE:
        if (!save_all()) {
            report_error(NULL, L"Some notes could not be saved. Jotilla will stay open."); return 0;
        }
        archive_close();
        while (notes) destroy_note(notes);
        DestroyWindow(window);
        return 0;
    case WM_QUERYENDSESSION: return save_all();
    case WM_ENDSESSION:
        end_session(wparam != 0);
        return 0;
    case WM_DESTROY:
        KillTimer(window, TRAY_CLICK_TIMER);
        tray_click = TRAY_IDLE;
        tray(FALSE);
        UnregisterHotKey(window, 1); UnregisterHotKey(window, 2); UnregisterHotKey(window, 3);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static BOOL register_classes(void) {
    WNDCLASSW cls = {0};
    rich_edit = LoadLibraryExW(L"Msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rich_edit) return FALSE;
    cls.hInstance = instance; cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = app_icon; cls.lpszClassName = NOTE_CLASS;
    cls.style = CS_DBLCLKS | CS_DROPSHADOW; cls.lpfnWndProc = note_proc;
    if (!RegisterClassW(&cls)) return FALSE;
    cls.style = 0; cls.lpszClassName = APP_CLASS; cls.lpfnWndProc = controller_proc;
    return RegisterClassW(&cls) != 0;
}

int WINAPI wWinMain(_In_ HINSTANCE app, _In_opt_ HINSTANCE previous, _In_ PWSTR command, _In_ int show) {
    HANDLE mutex;
    wchar_t root[PATH_CAP], selected[PATH_CAP], *slash;
    DWORD length;
    MSG message;
    int result;
    BOOL hotkey, startup = !_wcsicmp(command, L"--startup");
    SHORT plus;
    UINT key, modifiers;
    (void)previous; (void)show;
    instance = app;
    /* Share the established lock so an older release cannot edit concurrently. */
    mutex = CreateMutexW(NULL, FALSE, L"Local\\PicoSlip.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(APP_CLASS, NULL);
        if (existing && !startup) PostMessageW(existing, NEW_NOTE_MESSAGE, 0, 0);
        CloseHandle(mutex); return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    length = GetModuleFileNameW(NULL, root, PATH_CAP);
    if (!length || length >= PATH_CAP - 7) { CloseHandle(mutex); return 1; }
    slash = wcsrchr(root, L'\\');
    if (!slash) { CloseHandle(mutex); return 1; }
    wcscpy_s(slash + 1, PATH_CAP - (size_t)(slash + 1 - root), L"portable.flag");
    if (GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES) {
        *slash = 0;
        swprintf_s(location_config, PATH_CAP, L"%s\\archive.path", root);
        wcscpy_s(slash + 1, PATH_CAP - (size_t)(slash + 1 - root), L"notes");
        *slash = L'\\';
    } else {
        wchar_t local[PATH_CAP];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local)) ||
            !location_user_paths(local,location_config,root)) {
            report_error(NULL,L"Your notes location could not be loaded. The existing archive has not been changed.");
            CloseHandle(mutex); return 1;
        }
    }
    if (!location_load(location_config, root, selected) || !make_paths(selected)) {
        report_error(NULL, L"The notes archive is unavailable. Reconnect its drive or check the saved archive.path setting. Your archive location has not been changed.");
        CloseHandle(mutex); return 1;
    }
    load_preferences();
    app_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    hidden_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_HIDDEN), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (!register_classes()) { CloseHandle(mutex); return 1; }
    controller = CreateWindowExW(WS_EX_TOOLWINDOW, APP_CLASS, L"Jotilla", WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!controller) { CloseHandle(mutex); return 1; }
    plus = VkKeyScanW(L'+');
    key = plus == -1 ? VK_OEM_PLUS : (UINT)(plus & 0xff);
    modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    hotkey = RegisterHotKey(controller, 1, modifiers, key);
    if (RegisterHotKey(controller, 2, modifiers, VK_ADD)) hotkey = TRUE;
    if (RegisterHotKey(controller, 3, modifiers | MOD_SHIFT, key)) hotkey = TRUE;
    if (!hotkey) {
        report_error(NULL, L"Ctrl+Alt++ is already in use by another application.");
        DestroyWindow(controller); CloseHandle(mutex); return 1;
    }
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray(TRUE);
    startup_restore = startup;
    if (!restore_notes())
        report_error(NULL, L"Some notes could not be opened. Their original TXT files are still in the archive.");
    startup_restore = FALSE;
    if (!startup && !notes && !new_note()) report_error(NULL, L"The first note could not be created.");
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (app_icon) DestroyIcon(app_icon);
    if (hidden_icon) DestroyIcon(hidden_icon);
    CloseHandle(mutex);
    return result < 0 ? 1 : 0;
}
