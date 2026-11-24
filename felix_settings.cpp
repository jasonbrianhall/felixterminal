#include "felix_settings.h"
#include "gl_terminal.h"   // FONT_SIZE_DEFAULT, FONT_SIZE_MIN, FONT_SIZE_MAX
#include "term_color.h"    // g_theme_idx, THEME_COUNT, apply_theme
#include "crt_audio.h"     // term_audio_set_enabled, term_audio_get_enabled
#ifdef WOPR
#include "wopr/wopr.h"     // wopr_voice_mode, wopr_set_voice_mode
#endif

#include <stdio.h>
#include <stdlib.h>

// WOPR's voice: 0 off, 1 key lines (default), 2 everything
static int voice_get(void) {
#ifdef WOPR
    return (int)wopr_voice_mode();
#else
    return 1;
#endif
}

static void voice_set(int v) {
#ifdef WOPR
    if (v < 0 || v > 2) v = 1;
    wopr_set_voice_mode((WoprVoice)v);
#else
    (void)v;
#endif
}

extern int g_font_size;

// ============================================================================
// WINDOWS — HKEY_CURRENT_USER\Software\FelixTerminal
// ============================================================================

#ifdef _WIN32
#include <windows.h>

static const char *REG_KEY = "Software\\FelixTerminal";

static HKEY open_key(REGSAM access) {
    HKEY hk = nullptr;
    RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                    REG_OPTION_NON_VOLATILE, access, nullptr, &hk, nullptr);
    return hk;
}

static DWORD reg_get_dword(HKEY hk, const char *name, DWORD def) {
    DWORD val = def, sz = sizeof(val);
    RegGetValueA(hk, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz);
    return val;
}

static void reg_set_dword(HKEY hk, const char *name, DWORD val) {
    RegSetValueExA(hk, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
}

void settings_load(void) {
    HKEY hk = open_key(KEY_READ);
    if (!hk) return;

    int font = (int)reg_get_dword(hk, "FontSize",  (DWORD)FONT_SIZE_DEFAULT);
    int theme = (int)reg_get_dword(hk, "ThemeIndex", 0);
    int sound = (int)reg_get_dword(hk, "SoundEnabled", 0);  // default off
    int voice = (int)reg_get_dword(hk, "WoprVoice", 1);     // default: key lines

    RegCloseKey(hk);

    if (font  < FONT_SIZE_MIN) font  = FONT_SIZE_MIN;
    if (font  > FONT_SIZE_MAX) font  = FONT_SIZE_MAX;
    if (theme < 0 || theme >= THEME_COUNT) theme = 0;

    g_font_size = font;
    apply_theme(theme);
    term_audio_set_enabled(sound != 0);
    voice_set(voice);
}

void settings_save(void) {
    HKEY hk = open_key(KEY_WRITE);
    if (!hk) return;

    reg_set_dword(hk, "FontSize",     (DWORD)g_font_size);
    reg_set_dword(hk, "ThemeIndex",   (DWORD)g_theme_idx);
    reg_set_dword(hk, "SoundEnabled", (DWORD)(term_audio_get_enabled() ? 1 : 0));
    reg_set_dword(hk, "WoprVoice",    (DWORD)voice_get());

    RegCloseKey(hk);
}

// ============================================================================
// LINUX — ~/.config/FelixTerminal/settings.ini
// ============================================================================

#else
#include <sys/stat.h>
#include <string>

static std::string config_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg : (std::string(getenv("HOME")) + "/.config");
    return base + "/FelixTerminal/settings.ini";
}

static std::string config_dir(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg : (std::string(getenv("HOME")) + "/.config");
    return base + "/FelixTerminal";
}

void settings_load(void) {
    FILE *f = fopen(config_path().c_str(), "r");
    if (!f) return;

    int font  = FONT_SIZE_DEFAULT;
    int theme = 0;
    int sound = 0;
    int voice = 1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "FontSize=%d",    &v) == 1) font  = v;
        if (sscanf(line, "ThemeIndex=%d",  &v) == 1) theme = v;
        if (sscanf(line, "SoundEnabled=%d",&v) == 1) sound = v;
        if (sscanf(line, "WoprVoice=%d",   &v) == 1) voice = v;
    }
    fclose(f);

    if (font  < FONT_SIZE_MIN) font  = FONT_SIZE_MIN;
    if (font  > FONT_SIZE_MAX) font  = FONT_SIZE_MAX;
    if (theme < 0 || theme >= THEME_COUNT) theme = 0;

    g_font_size = font;
    apply_theme(theme);
    term_audio_set_enabled(sound != 0);
    voice_set(voice);
}

void settings_save(void) {
    std::string dir = config_dir();
    mkdir(dir.c_str(), 0755);

    FILE *f = fopen(config_path().c_str(), "w");
    if (!f) return;

    fprintf(f, "FontSize=%d\n",     g_font_size);
    fprintf(f, "ThemeIndex=%d\n",   g_theme_idx);
    fprintf(f, "SoundEnabled=%d\n", term_audio_get_enabled() ? 1 : 0);
    fprintf(f, "WoprVoice=%d\n",    voice_get());
    fclose(f);
}

#endif
