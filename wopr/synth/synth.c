/*
 * formant_tts.c  –  Formant-based Text-to-Speech synthesizer
 *
 * Architecture:
 *   Text → numbers spelled out → CMUdict / hand dictionary / G2P rules
 *        → phonemes with stress
 *        → timing (durations by stress and position), pitch contour
 *        → parameter tracks: formant targets with smooth transitions
 *          (consonant loci, two-target diphthongs), source amplitudes
 *        → Klatt-style synthesizer: glottal pulses and aspiration through a
 *          cascade of formant resonators (with a nasal pole/zero pair), noise
 *          through parallel resonators for fricatives and stop bursts
 *        → SDL2 audio output (or a WAV file with -o)
 *
 * Phoneme set (ARPABET):
 *   Vowels:  AA AE AH AO AW AY EH ER EY IH IY OW OY UH UW  (stress 0/1/2)
 *   Nasals:  M  N  NG
 *   Stops:   B  D  G  P  T  K
 *   Fric:    F  V  TH DH S  Z  SH ZH HH
 *   Affric:  CH JH
 *   Approx:  L  R  W  Y
 *
 * Build:
 *   gcc synth.c -o synth $(sdl2-config --cflags --libs) -lm
 *
 * Usage:
 *   ./synth "Hello Dr. Falken. How is Joshua?"
 *   ./synth                    (interactive: type sentences, Enter to speak, Q to quit)
 *   ./synth -o out.wav "text"  (write a WAV file instead of playing)
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SAMPLE_RATE   22050
#define BUFFER_SIZE   512
#define MAX_PHONEMES  1024
#define MAX_WORD      64
#define MS(x)         ((int)((x) * (SAMPLE_RATE / 1000.0)))

typedef struct DictNode {
    char *word;
    char *phones;
    struct DictNode *next;
} DictNode;

#define DICT_BUCKETS 4096
static DictNode *cmu_dict[DICT_BUCKETS];


/* ═══════════════════════════════════════════════════════════════════════════
 * Phoneme definitions
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef enum {
    /* Vowels */
    PH_AA, PH_AE, PH_AH, PH_AO, PH_AW, PH_AY, PH_EH, PH_ER, PH_EY,
    PH_IH, PH_IY, PH_OW, PH_OY, PH_UH, PH_UW,
    /* Nasals */
    PH_M,  PH_N,  PH_NG,
    /* Stops */
    PH_B,  PH_D,  PH_G,  PH_P,  PH_T,  PH_K,
    /* Voiced fricatives */
    PH_V,  PH_DH, PH_Z,  PH_ZH,
    /* Voiceless fricatives */
    PH_F,  PH_TH, PH_S,  PH_SH, PH_HH,
    /* Affricates */
    PH_CH, PH_JH,
    /* Approximants */
    PH_L,  PH_R,  PH_W,  PH_Y,
    /* Silence */
    PH_SIL,
    PH_COUNT
} Phoneme;

/* Phoneme class flags */
#define CL_VOWEL    0x01
#define CL_NASAL    0x02
#define CL_STOP     0x04
#define CL_FRIC     0x08
#define CL_VOICED   0x10
#define CL_APPROX   0x20
#define CL_AFFRIC   0x40
#define CL_SIL      0x80

/* Place of articulation: where the tongue/lips close, which sets the
 * formant "locus" the neighbouring vowels bend towards. */
enum { PL_NONE, PL_LAB, PL_ALV, PL_VEL, PL_PAL };

typedef struct {
    const char *name;
    int    cls;
    int    place;
    double dur_ms;          /* stressed duration                          */
    double f[3], b[3];      /* F1-F3 and bandwidths (Hz)                  */
    double f_end[3];        /* diphthongs: F1-F3 at the end (0: none)     */
    double av;              /* voicing amplitude (dB, 60 = full vowel)    */
    double af;              /* frication noise amplitude (dB)             */
    double a[5];            /* parallel noise formants A2..A6 (dB, 0 off) */
    double ab;              /* noise straight through, flat (dB, 0 off)   */
} PhonDef;

/* Vowel formants: Peterson & Barney (1952) averages for men; consonants
 * after Klatt (1980). Nasals add a nasal zero (450 Hz)
 * against the nasal resonance (270 Hz) for their hummed murmur. Fricative noise spectra: s/z high (A5-A6), sh/zh mid (A3-A5),
 * f/v and th/dh flat and weak (bypass). */
static const PhonDef PHDEF[PH_COUNT] = {
/*            name cls                    place   dur   F1   F2   F3      B1  B2  B3     diphthong end     AV  AF   A2 A3 A4 A5 A6   AB */
[PH_AA]={"AA",CL_VOWEL|CL_VOICED, PL_NONE,190,{730,1090,2440},{80,90,150},{0,0,0},          60,0,{0,0,0,0,0},0},
[PH_AE]={"AE",CL_VOWEL|CL_VOICED, PL_NONE,190,{660,1720,2410},{80,90,150},{0,0,0},          60,0,{0,0,0,0,0},0},
[PH_AH]={"AH",CL_VOWEL|CL_VOICED, PL_NONE,130,{640,1190,2390},{80,90,150},{0,0,0},          59,0,{0,0,0,0,0},0},
[PH_AO]={"AO",CL_VOWEL|CL_VOICED, PL_NONE,190,{570,840,2410},{80,90,150},{0,0,0},          60,0,{0,0,0,0,0},0},
[PH_AW]={"AW",CL_VOWEL|CL_VOICED, PL_NONE,230,{680,1200,2550},{80,90,150},{440, 950,2350},  60,0,{0,0,0,0,0},0},
[PH_AY]={"AY",CL_VOWEL|CL_VOICED, PL_NONE,220,{700,1220,2600},{80,90,150},{420,1950,2600},  60,0,{0,0,0,0,0},0},
[PH_EH]={"EH",CL_VOWEL|CL_VOICED, PL_NONE,140,{530,1840,2480},{70,90,150},{0,0,0},          59,0,{0,0,0,0,0},0},
[PH_ER]={"ER",CL_VOWEL|CL_VOICED, PL_NONE,170,{490,1350,1690},{80,90,110},{0,0,0},          59,0,{0,0,0,0,0},0},
[PH_EY]={"EY",CL_VOWEL|CL_VOICED, PL_NONE,180,{500,1800,2500},{70,90,150},{340,2150,2700},  59,0,{0,0,0,0,0},0},
[PH_IH]={"IH",CL_VOWEL|CL_VOICED, PL_NONE,120,{390,1990,2550},{60,90,150},{0,0,0},          58,0,{0,0,0,0,0},0},
[PH_IY]={"IY",CL_VOWEL|CL_VOICED, PL_NONE,160,{270,2290,3010},{50,90,150},{0,0,0},          58,0,{0,0,0,0,0},0},
[PH_OW]={"OW",CL_VOWEL|CL_VOICED, PL_NONE,200,{560,1000,2400},{80,90,150},{400, 850,2350},  60,0,{0,0,0,0,0},0},
[PH_OY]={"OY",CL_VOWEL|CL_VOICED, PL_NONE,240,{560, 900,2450},{80,90,150},{380,1900,2550},  60,0,{0,0,0,0,0},0},
[PH_UH]={"UH",CL_VOWEL|CL_VOICED, PL_NONE,130,{440,1020,2240},{70,90,150},{0,0,0},          58,0,{0,0,0,0,0},0},
[PH_UW]={"UW",CL_VOWEL|CL_VOICED, PL_NONE,170,{300,870,2240},{60,90,150},{290, 800,2200},  58,0,{0,0,0,0,0},0},

[PH_M] ={"M", CL_NASAL|CL_VOICED, PL_LAB, 70,{280, 950,2200},{100,150,200},{0,0,0},         53,0,{0,0,0,0,0},0},
[PH_N] ={"N", CL_NASAL|CL_VOICED, PL_ALV, 65,{280,1700,2600},{100,150,200},{0,0,0},         53,0,{0,0,0,0,0},0},
[PH_NG]={"NG",CL_NASAL|CL_VOICED, PL_VEL, 80,{280,2000,2700},{100,150,200},{0,0,0},         52,0,{0,0,0,0,0},0},

/* Stops: formants are the locus; dur is closure + burst. */
[PH_B] ={"B", CL_STOP|CL_VOICED,  PL_LAB, 65,{200, 900,2150},{60,110,130},{0,0,0},          0, 50,{0,0,0,0,0},57},
[PH_D] ={"D", CL_STOP|CL_VOICED,  PL_ALV, 55,{200,1700,2650},{60,110,150},{0,0,0},          0, 50,{0,0,40,50,52},0},
[PH_G] ={"G", CL_STOP|CL_VOICED,  PL_VEL, 65,{200,1600,2400},{60,110,150},{0,0,0},          0, 50,{50,50,0,0,0},0},
[PH_P] ={"P", CL_STOP,            PL_LAB, 80,{200, 900,2150},{60,110,130},{0,0,0},          0, 58,{0,0,0,0,0},60},
[PH_T] ={"T", CL_STOP,            PL_ALV, 70,{200,1700,2650},{60,110,150},{0,0,0},          0, 58,{0,0,46,56,60},0},
[PH_K] ={"K", CL_STOP,            PL_VEL, 80,{200,1600,2400},{60,110,150},{0,0,0},          0, 58,{56,56,0,0,0},0},

[PH_V] ={"V", CL_FRIC|CL_VOICED,  PL_LAB, 65,{220,1100,2080},{60,90,120},{0,0,0},           48,52,{0,0,0,0,0},40},
[PH_DH]={"DH",CL_FRIC|CL_VOICED,  PL_ALV, 50,{270,1290,2540},{60,80,170},{0,0,0},           50,48,{0,0,0,0,30},36},
[PH_Z] ={"Z", CL_FRIC|CL_VOICED,  PL_ALV, 75,{240,1520,2480},{70,80,60},{0,0,0},            48,56,{0,0,0,40,56},0},
[PH_ZH]={"ZH",CL_FRIC|CL_VOICED,  PL_PAL, 70,{300,1840,2750},{70,100,300},{0,0,0},          48,56,{0,54,48,48,46},0},
[PH_F] ={"F", CL_FRIC,            PL_LAB,100,{340,1100,2080},{200,120,150},{0,0,0},         0, 56,{0,0,0,0,0},40},
[PH_TH]={"TH",CL_FRIC,            PL_ALV, 90,{320,1290,2540},{200,90,200},{0,0,0},          0, 56,{0,0,0,0,30},36},
[PH_S] ={"S", CL_FRIC,            PL_ALV,105,{320,1390,2530},{200,80,200},{0,0,0},          0, 60,{0,0,0,44,60},0},
[PH_SH]={"SH",CL_FRIC,            PL_PAL,105,{300,1840,2750},{200,100,300},{0,0,0},         0, 60,{0,57,50,50,46},0},
[PH_HH]={"HH",CL_FRIC,            PL_NONE,65,{500,1500,2500},{300,150,250},{0,0,0},         0, 0,{0,0,0,0,0},0},

/* Affricates: a closure, then frication like SH / ZH. */
[PH_CH]={"CH",CL_AFFRIC,          PL_PAL,110,{300,1840,2750},{200,100,300},{0,0,0},         0, 60,{0,57,50,50,46},0},
[PH_JH]={"JH",CL_AFFRIC|CL_VOICED,PL_PAL, 90,{300,1840,2750},{70,100,300},{0,0,0},          44,54,{0,54,48,48,46},0},

[PH_L] ={"L", CL_APPROX|CL_VOICED,PL_ALV, 70,{330,1050,2880},{50,100,280},{0,0,0},          56,0,{0,0,0,0,0},0},
[PH_R] ={"R", CL_APPROX|CL_VOICED,PL_ALV, 70,{330,1060,1380},{70,100,120},{0,0,0},          56,0,{0,0,0,0,0},0},
[PH_W] ={"W", CL_APPROX|CL_VOICED,PL_LAB, 70,{290, 610,2150},{50, 80,60},{0,0,0},           56,0,{0,0,0,0,0},0},
[PH_Y] ={"Y", CL_APPROX|CL_VOICED,PL_PAL, 60,{260,2070,3020},{40,250,500},{0,0,0},          56,0,{0,0,0,0,0},0},

[PH_SIL]={"SIL",CL_SIL,           PL_NONE,80,{500,1500,2500},{80,90,150},{0,0,0},           0, 0,{0,0,0,0,0},0},
};

/* ═══════════════════════════════════════════════════════════════════════════
 * G2P: Grapheme-to-Phoneme
 *
 * Rule table: each entry is a left-context, trigger-letters, right-context
 * → phoneme sequence. Processed left-to-right, longest match wins.
 * This is a simplified but functional rule set for English.
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    const char *match;    /* letters to match (may be multi-char digraph) */
    const char *phones;   /* space-separated phoneme names, "" = silent    */
} G2PRule;

/* Simple flat rule table – ordered longest-match first within each letter */
static const G2PRule G2P_RULES[] = {
    /* ── digraphs / trigraphs first ─────────────────────────────────── */
    {"tch",  "CH"},
    {"sch",  "SH"},
    {"dge",  "JH"},
    {"igh",  "AY"},
    {"ght",  "T"},
    {"ghn",  "N"},
    {"gue",  "G"},
    {"que",  "K"},
    {"ck",   "K"},
    {"ch",   "CH"},
    {"sh",   "SH"},
    {"th",   "DH"},   /* default voiced; voiceless handled below */
    {"ng",   "NG"},
    {"nk",   "NG K"},
    {"ph",   "F"},
    {"wh",   "W"},
    {"wr",   "R"},
    {"kn",   "N"},
    {"gn",   "N"},
    {"mb",   "M"},
    {"mn",   "M"},
    {"rh",   "R"},
    {"ps",   "S"},
    {"tion", "SH AH N"},
    {"sion", "ZH AH N"},
    {"ture", "CH ER"},
    {"dure", "JH ER"},
    {"ous",  "AH S"},
    {"ious", "IY AH S"},
    {"eous", "IY AH S"},
    {"age",  "AH JH"},
    {"ace",  "EY S"},
    {"ice",  "AY S"},
    {"uce",  "UW S"},
    {"ance", "AH N S"},
    {"ence", "AH N S"},
    {"ible", "AH B AH L"},
    {"able", "EY B AH L"},
    {"tion", "SH AH N"},
    /* ── vowel digraphs ───────────────────────────────────────────────── */
    {"ai",   "EY"},
    {"ay",   "EY"},
    {"au",   "AO"},
    {"aw",   "AO"},
    {"ae",   "AE"},
    {"ea",   "IY"},
    {"ee",   "IY"},
    {"ei",   "EY"},
    {"ey",   "EY"},
    {"ie",   "IY"},
    {"eu",   "UW"},
    {"ew",   "UW"},
    {"oa",   "OW"},
    {"oe",   "OW"},
    {"oi",   "OY"},
    {"oo",   "UW"},
    {"ou",   "AW"},
    {"ow",   "OW"},
    {"oy",   "OY"},
    {"ue",   "UW"},
    {"ui",   "UW"},
    /* ── single vowels + context ─────────────────────────────────────── */
    {"a",    "AE"},
    {"e",    "EH"},
    {"i",    "IH"},
    {"o",    "AO"},
    {"u",    "AH"},
    {"y",    "IH"},
    /* ── consonants ──────────────────────────────────────────────────── */
    {"b",    "B"},
    {"c",    "K"},
    {"d",    "D"},
    {"f",    "F"},
    {"g",    "G"},
    {"h",    "HH"},
    {"j",    "JH"},
    {"k",    "K"},
    {"l",    "L"},
    {"m",    "M"},
    {"n",    "N"},
    {"p",    "P"},
    {"q",    "K"},
    {"r",    "R"},
    {"s",    "S"},
    {"t",    "T"},
    {"v",    "V"},
    {"w",    "W"},
    {"x",    "K S"},
    {"z",    "Z"},
    {NULL,   NULL}
};

/* Known word dictionary (highest priority over rules) */
typedef struct { const char *word; const char *phones; } DictEntry;
static const DictEntry DICT[] = {
    {"the",    "DH AH"},
    {"a",      "AH"},
    {"an",     "AE N"},
    {"and",    "AE N D"},
    {"or",     "AO R"},
    {"of",     "AH V"},
    {"to",     "T UW"},
    {"in",     "IH N"},
    {"is",     "IH Z"},
    {"it",     "IH T"},
    {"are",    "AA R"},
    {"was",    "W AH Z"},
    {"be",     "B IY"},
    {"been",   "B IH N"},
    {"have",   "HH AE V"},
    {"has",    "HH AE Z"},
    {"had",    "HH AE D"},
    {"do",     "D UW"},
    {"does",   "D AH Z"},
    {"did",    "D IH D"},
    {"not",    "N AO T"},
    {"no",     "N OW"},
    {"i",      "AY"},
    {"he",     "HH IY"},
    {"she",    "SH IY"},
    {"we",     "W IY"},
    {"you",    "Y UW"},
    {"they",   "DH EY"},
    {"my",     "M AY"},
    {"your",   "Y AO R"},
    {"his",    "HH IH Z"},
    {"her",    "HH ER"},
    {"our",    "AW ER"},
    {"their",  "DH EH R"},
    {"this",   "DH IH S"},
    {"that",   "DH AE T"},
    {"with",   "W IH TH"},
    {"for",    "F AO R"},
    {"from",   "F R AH M"},
    {"by",     "B AY"},
    {"at",     "AE T"},
    {"as",     "AE Z"},
    {"on",     "AO N"},
    {"up",     "AH P"},
    {"out",    "AW T"},
    {"so",     "S OW"},
    {"if",     "IH F"},
    {"but",    "B AH T"},
    {"about",  "AH B AW T"},
    {"all",    "AO L"},
    {"can",    "K AE N"},
    {"could",  "K UH D"},
    {"would",  "W UH D"},
    {"should",  "SH UH D"},
    {"will",   "W IH L"},
    {"just",   "JH AH S T"},
    {"like",   "L AY K"},
    {"know",   "N OW"},
    {"what",   "W AH T"},
    {"when",   "W EH N"},
    {"where",  "W EH R"},
    {"who",    "HH UW"},
    {"how",    "HH AW"},
    {"why",    "W AY"},
    {"more",   "M AO R"},
    {"very",   "V EH R IY"},
    {"also",   "AO L S OW"},
    {"hello",  "HH EH L OW"},
    {"dr",     "D AO K T ER"},
    {"doctor", "D AO K T ER"},
    {"mr",     "M IH S T ER"},
    {"ms",     "M IH Z"},
    {"mrs",    "M IH S IH Z"},
    {"joshua", "JH AO SH UW AH"},
    {"falken", "F AO L K AH N"},
    {"good",   "G UH D"},
    {"great",  "G R EY T"},
    {"fine",   "F AY N"},
    {"yes",    "Y EH S"},
    {"one",    "W AH N"},
    {"two",    "T UW"},
    {"three",  "TH R IY"},
    {"ok",     "OW K EY"},
    {"okay",   "OW K EY"},
    {"please", "P L IY Z"},
    {"thank",  "TH AE NG K"},
    {"thanks", "TH AE NG K S"},
    {"day",    "D EY"},
    {"time",   "T AY M"},
    {"year",   "Y IH R"},
    {"come",   "K AH M"},
    {"go",     "G OW"},
    {"see",    "S IY"},
    {"look",   "L UH K"},
    {"think",  "TH IH NG K"},
    {"make",   "M EY K"},
    {"say",    "S EY"},
    {"tell",   "T EH L"},
    {"find",   "F AY N D"},
    {"give",   "G IH V"},
    {"take",   "T EY K"},
    {"want",   "W AO N T"},
    {"need",   "N IY D"},
    {"use",    "Y UW Z"},
    {"name",   "N EY M"},
    {"work",   "W ER K"},
    {"call",   "K AO L"},
    {"try",    "T R AY"},
    {"ask",    "AE S K"},
    {"seem",   "S IY M"},
    {"feel",   "F IY L"},
    {"leave",  "L IY V"},
    {"put",    "P UH T"},
    {"mean",   "M IY N"},
    {"keep",   "K IY P"},
    {"let",    "L EH T"},
    {"begin",  "B IH G IH N"},
    {"show",   "SH OW"},
    {"hear",   "HH IY R"},
    {"play",   "P L EY"},
    {"run",    "R AH N"},
    {"move",   "M UW V"},
    {"live",   "L IH V"},
    {"believe","B IH L IY V"},
    {"hold",   "HH OW L D"},
    {"bring",  "B R IH NG"},
    {"happen", "HH AE P AH N"},
    {"write",  "R AY T"},
    {"provide","P R AH V AY D"},
    {"sit",    "S IH T"},
    {"stand",  "S T AE N D"},
    {"lose",   "L UW Z"},
    {"pay",    "P EY"},
    {"meet",   "M IY T"},
    {"include","IH N K L UW D"},
    {"continue","K AH N T IH N Y UW"},
    {"set",    "S EH T"},
    {"learn",  "L ER N"},
    {"change", "CH EY N JH"},
    {"lead",   "L IY D"},
    {"war",    "W AO R"},
    {"game",   "G EY M"},
    {"global",  "G L OW B AH L"},
    {"thermonuclear","TH ER M OW N UW K L IY ER"},
    {"chess",   "CH EH S"},
    {"strange", "S T R EY N JH"},
    {NULL,      NULL}
};

static unsigned hash_word(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ tolower(*s++);
    return h % DICT_BUCKETS;
}

/* ── Look up a lowercase word in the dictionary ─────────────────────── */
static const char *dict_lookup(const char *word) {
    unsigned h = hash_word(word);
    for (DictNode *n = cmu_dict[h]; n; n = n->next)
        if (strcmp(n->word, word) == 0)
            return n->phones;
    return NULL;
}

/* Newest entry wins (each bucket's list starts with the newest). */
static void dict_insert(const char *word, const char *phones) {
    unsigned h = hash_word(word);
    DictNode *n = malloc(sizeof(DictNode));
    n->word = strdup(word);
    n->phones = strdup(phones);
    n->next = cmu_dict[h];
    cmu_dict[h] = n;
}

/* CMUdict, keeping the stress digits (AH0, EY1, ...): they set vowel
 * length, loudness and pitch. Only each word's first (main) pronunciation
 * is kept: "A(1)  EY1" or "THE(1)  DH IY0" must not replace "A  AH0" and
 * "THE  DH AH0". */
static void load_cmudict(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Could not open CMUdict file: %s\n", path);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';')
            continue;
        char *word = strtok(line, " \t\r\n");
        if (!word || strchr(word, '('))
            continue;
        char *phones = strtok(NULL, "\r\n");
        if (!phones)
            continue;
        while (*phones == ' ') phones++;
        for (char *s = word; *s; s++)
            *s = tolower((unsigned char)*s);
        dict_insert(word, phones);
    }
    fclose(f);
}

/* The hand-written entries: abbreviations always win (CMUdict reads "dr"
 * as "drive" and so on); the rest only fill words CMUdict doesn't have,
 * since CMUdict's own entries carry stress and are more exact. */
static void load_hand_dict(void) {
    static const char *always[] = { "dr", "mr", "mrs", "ms", NULL };
    for (int i = 0; DICT[i].word; i++) {
        int force = 0;
        for (int k = 0; always[k]; k++)
            if (strcmp(DICT[i].word, always[k]) == 0) force = 1;
        if (force || !dict_lookup(DICT[i].word))
            dict_insert(DICT[i].word, DICT[i].phones);
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Numbers → words ("1983" → "nineteen eighty three", "3.5" → "three point
 * five", "2nd" → "second", "1,024" → "one thousand twenty four")
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char *ONES[] = { "zero","one","two","three","four","five","six","seven","eight","nine",
    "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen" };
static const char *TENS[] = { "","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety" };

static void cat(char *out, size_t sz, const char *s) {
    size_t n = strlen(out);
    if (n && out[n - 1] != ' ' && n + 1 < sz) { out[n++] = ' '; out[n] = 0; }
    strncat(out, s, sz - n - 1);
}

static void say_below100(long n, char *out, size_t sz) {
    if (n < 20) { cat(out, sz, ONES[n]); return; }
    cat(out, sz, TENS[n / 10]);
    if (n % 10) cat(out, sz, ONES[n % 10]);
}

static void say_below1000(long n, char *out, size_t sz) {
    if (n >= 100) {
        cat(out, sz, ONES[n / 100]);
        cat(out, sz, "hundred");
        n %= 100;
        if (!n) return;
    }
    say_below100(n, out, sz);
}

static void say_number(long n, char *out, size_t sz) {
    if (n == 0) { cat(out, sz, "zero"); return; }
    if (n >= 1000000000L) { say_below1000(n / 1000000000L, out, sz); cat(out, sz, "billion"); n %= 1000000000L; }
    if (n >= 1000000L)    { say_below1000(n / 1000000L, out, sz);    cat(out, sz, "million");  n %= 1000000L; }
    if (n >= 1000L)       { say_below1000(n / 1000L, out, sz);       cat(out, sz, "thousand"); n %= 1000L; }
    if (n) say_below1000(n, out, sz);
}

static void say_year(long n, char *out, size_t sz) {
    long hi = n / 100, lo = n % 100;
    say_below100(hi, out, sz);
    if (lo == 0) cat(out, sz, "hundred");
    else if (lo < 10) { cat(out, sz, "oh"); cat(out, sz, ONES[lo]); }
    else say_below100(lo, out, sz);
}

/* the last word of out made an ordinal: one → first, twenty → twentieth */
static void make_ordinal(char *out, size_t sz) {
    static const char *from[] = { "one","two","three","five","eight","nine","twelve", NULL };
    static const char *to[]   = { "first","second","third","fifth","eighth","ninth","twelfth" };
    char *w = strrchr(out, ' ');
    w = w ? w + 1 : out;
    for (int i = 0; from[i]; i++)
        if (strcmp(w, from[i]) == 0) { *w = 0; strncat(out, to[i], sz - strlen(out) - 1); return; }
    size_t n = strlen(w);
    if (n && w[n - 1] == 'y') { w[n - 1] = 0; strncat(out, "ieth", sz - strlen(out) - 1); }
    else strncat(out, "th", sz - strlen(out) - 1);
}

/* Lower-case copy of the text with numbers spelled out and a few symbols
 * turned into words. */
static void expand_text(const char *in, char *out, size_t sz) {
    out[0] = 0;
    size_t n = 0;
    while (*in && n + 64 < sz) {
        if (isdigit((unsigned char)*in)) {
            char digits[32]; int nd = 0, commas = 0;
            while (nd < 30 && (isdigit((unsigned char)*in) ||
                   (*in == ',' && isdigit((unsigned char)in[1]) && isdigit((unsigned char)in[2]) &&
                    isdigit((unsigned char)in[3]) && !isdigit((unsigned char)in[4])))) {
                if (*in == ',') commas = 1; else digits[nd++] = *in;
                in++;
            }
            digits[nd] = 0;
            char words[512] = "";
            long v = atol(digits);
            if (nd > 9)
                for (int k = 0; k < nd; k++) cat(words, sizeof words, ONES[digits[k] - '0']);
            else if (nd == 4 && !commas && *in != '.' &&
                     ((v >= 1100 && v <= 1999) || (v >= 2010 && v <= 2099)))
                say_year(v, words, sizeof words);
            else
                say_number(v, words, sizeof words);
            if (*in == '.' && isdigit((unsigned char)in[1])) {          /* decimals */
                cat(words, sizeof words, "point");
                for (in++; isdigit((unsigned char)*in); in++)
                    cat(words, sizeof words, ONES[*in - '0']);
            }
            char a = tolower((unsigned char)in[0]), b = a ? tolower((unsigned char)in[1]) : 0;
            if (((a == 's' && b == 't') || (a == 'n' && b == 'd') || (a == 'r' && b == 'd') ||
                 (a == 't' && b == 'h')) && !isalpha((unsigned char)in[2])) {
                make_ordinal(words, sizeof words);
                in += 2;
            }
            out[n++] = ' '; out[n] = 0;
            strncat(out, words, sz - n - 2);
            n = strlen(out);
            out[n++] = ' '; out[n] = 0;
            continue;
        }
        const char *word = NULL;
        switch (*in) {
        case '%': word = " percent "; break;
        case '&': word = " and "; break;
        case '+': word = " plus "; break;
        case '=': word = " equals "; break;
        case '@': word = " at "; break;
        }
        if (word) {
            strncat(out, word, sz - n - 1);
            n = strlen(out);
        } else {
            out[n++] = tolower((unsigned char)*in);
            out[n] = 0;
        }
        in++;
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Text → phonemes
 * ═══════════════════════════════════════════════════════════════════════════ */
#define F_WORD_START 1          /* first phoneme of a word              */
#define F_FUNC       2          /* in a function word (the, of, is...)  */

typedef struct {
    Phoneme ph;
    int     stress;             /* vowels: 0, 1, 2; others -1           */
    int     flags;
    int     pause_ms;           /* PH_SIL: length                       */
    char    punct;              /* PH_SIL: '.', '?', ',' or 0           */
} PItem;

static const char *FUNCTION_WORDS[] = {
    "a","an","the","and","or","but","if","of","to","in","on","at","by","for","from","with","as",
    "is","are","was","were","be","been","am","it","its","it's","i","me","my","he","him","his",
    "she","her","we","us","our","you","your","they","them","their","this","that","than","then",
    "so","do","does","did","have","has","had","will","would","can","could","shall","should",
    "not","there","what","which","who", NULL
};

static int is_function_word(const char *w) {
    for (int i = 0; FUNCTION_WORDS[i]; i++)
        if (strcmp(w, FUNCTION_WORDS[i]) == 0) return 1;
    return 0;
}

static Phoneme name_to_ph(const char *s) {
    for (int i = 0; i < PH_COUNT; i++)
        if (strcmp(PHDEF[i].name, s) == 0) return (Phoneme)i;
    return PH_COUNT;
}

/* Append a space-separated phoneme string ("HH AH0 L OW1"). Stress digits
 * are kept; when a word has none (hand dictionary, letter rules), its
 * first full vowel gets the stress. */
static int add_phones(const char *phones, PItem *out, int n, int maxout, int func) {
    char buf[256], *save = NULL;
    int first = n, has_stress = 0;
    strncpy(buf, phones, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok_r(buf, " ", &save); tok && n < maxout - 2; tok = strtok_r(NULL, " ", &save)) {
        int st = -1;
        size_t l = strlen(tok);
        if (l > 1 && isdigit((unsigned char)tok[l - 1])) { st = tok[l - 1] - '0'; tok[l - 1] = 0; has_stress = 1; }
        Phoneme ph = name_to_ph(tok);
        if (ph == PH_COUNT) continue;
        if ((PHDEF[ph].cls & CL_VOWEL) && st < 0) st = 0;
        if (!(PHDEF[ph].cls & CL_VOWEL)) st = -1;
        out[n].ph = ph; out[n].stress = st;
        out[n].flags = (n == first ? F_WORD_START : 0) | (func ? F_FUNC : 0);
        out[n].pause_ms = 0; out[n].punct = 0;
        n++;
    }
    if (!has_stress) {
        int pick = -1, nv = 0;
        for (int k = first; k < n; k++)
            if (PHDEF[out[k].ph].cls & CL_VOWEL) {
                nv++;
                if (pick < 0 || (nv == 2 && (out[pick].ph == PH_AH || out[pick].ph == PH_IH)))
                    pick = k;
            }
        if (pick >= 0) out[pick].stress = 1;
    }
    return n;
}

/* ── Apply G2P rules to a single lowercase word ─────────────────────── */
static void g2p_word(const char *word, char *phones, size_t sz) {
    const char *p = word;
    phones[0] = 0;
    while (*p) {
        int matched = 0;
        for (int r = 0; G2P_RULES[r].match; r++) {
            size_t ml = strlen(G2P_RULES[r].match);
            if (strncmp(p, G2P_RULES[r].match, ml) == 0) {
                cat(phones, sz, G2P_RULES[r].phones);
                p += ml;
                matched = 1;
                break;
            }
        }
        if (!matched) p++;      /* skip unknown character */
    }
}

static int add_pause(PItem *out, int n, int maxout, int ms, char punct) {
    if (n >= maxout) return n;
    if (n > 0 && out[n - 1].ph == PH_SIL) {          /* merge with a pause just before */
        if (ms > out[n - 1].pause_ms) out[n - 1].pause_ms = ms;
        if (punct && punct != ',') out[n - 1].punct = punct;
        else if (punct && !out[n - 1].punct) out[n - 1].punct = punct;
        return n;
    }
    out[n].ph = PH_SIL; out[n].stress = -1; out[n].flags = 0;
    out[n].pause_ms = ms; out[n].punct = punct;
    return n + 1;
}

static int text_to_phonemes(const char *text, PItem *out, int maxout) {
    static char lower[8192];
    int n = 0;
    expand_text(text, lower, sizeof lower);
    const char *p = lower;
    char prev_word[MAX_WORD] = "";

    n = add_pause(out, n, maxout, 60, 0);
    while (*p && n < maxout - 40) {
        if (!isalpha((unsigned char)*p)) {
            if (*p == ',' || *p == ';' || *p == ':')
                n = add_pause(out, n, maxout, 220, ',');
            else if ((*p == '.' || *p == '!' || *p == '?') &&
                     !(*p == '.' && (strcmp(prev_word, "dr") == 0 || strcmp(prev_word, "mr") == 0 ||
                                     strcmp(prev_word, "mrs") == 0 || strcmp(prev_word, "ms") == 0 ||
                                     strcmp(prev_word, "st") == 0 || strcmp(prev_word, "vs") == 0)))
                n = add_pause(out, n, maxout, 420, *p == '?' ? '?' : '.');
            p++;
            continue;
        }
        char word[MAX_WORD];
        int wl = 0;
        while (*p && (isalpha((unsigned char)*p) || (*p == '\'' && isalpha((unsigned char)p[1])))
               && wl < MAX_WORD - 1)
            word[wl++] = *p++;
        word[wl] = 0;
        strcpy(prev_word, word);

        int func = is_function_word(word);
        const char *dict = dict_lookup(word);
        if (dict) {
            printf("[DICT]  %-20s → %s\n", word, dict);
            n = add_phones(dict, out, n, maxout, func);
        } else {
            char letters[MAX_WORD], phones[512];
            int k = 0;
            for (int i = 0; word[i]; i++) if (word[i] != '\'') letters[k++] = word[i];
            letters[k] = 0;
            g2p_word(letters, phones, sizeof phones);
            if (phones[0]) {
                printf("[G2P ]  %-20s → %s\n", word, phones);
                n = add_phones(phones, out, n, maxout, func);
            } else {
                printf("[MISS] %-20s → (no match)\n", word);
            }
        }
    }
    n = add_pause(out, n, maxout, 300, '.');
    if (n > 0 && out[n - 1].ph == PH_SIL && !out[n - 1].punct)
        out[n - 1].punct = '.';
    return n;
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Planning: durations, formant tracks, source amplitudes, pitch
 * ═══════════════════════════════════════════════════════════════════════════ */
#define RATE        1.0          /* > 1 slower, < 1 faster              */
#define PITCH_START 122.0        /* Hz at the start of a phrase         */
#define PITCH_END    98.0        /* ... falling to this at its end      */
#define PITCH_ACCENT 18.0        /* rise on stressed syllables          */

static int cls_of(const PItem *it, int n, int i) {
    return (i < 0 || i >= n) ? CL_SIL : PHDEF[it[i].ph].cls;
}
static int is_vowel(const PItem *it, int n, int i) { return (cls_of(it, n, i) & CL_VOWEL) != 0; }
static int is_cons(const PItem *it, int n, int i) {
    int c = cls_of(it, n, i);
    return !(c & (CL_VOWEL | CL_SIL));
}
static int is_front(Phoneme p) {
    return p == PH_IY || p == PH_IH || p == PH_EY || p == PH_EH || p == PH_AE || p == PH_Y;
}

/* One point of the formant track */
typedef struct { int t; double f[3], b[3], fnz; } FAnchor;

/* The sound sources over a stretch of time (amplitudes in dB, 0 = off) */
typedef struct {
    int    t0, t1;
    double av, ah, af, a[5], ab;
    int    abrupt;               /* jump to these values, no ramp (bursts) */
} SrcEvent;

typedef struct { int t; double hz; } PAnchor;

typedef struct {
    int       n_samples;
    FAnchor  *fa;  int nfa;
    SrcEvent *ev;  int nev;
    PAnchor  *pa;  int npa;
} Plan;

/* Formant target of phoneme i: at its start (end = 0) or end (end = 1) */
static void target(const PItem *it, int n, int i, int end, double f[3], double b[3], double *fnz) {
    const PhonDef *d = &PHDEF[it[i].ph];
    for (int k = 0; k < 3; k++) {
        f[k] = (end && d->f_end[0] > 0) ? d->f_end[k] : d->f[k];
        b[k] = d->b[k];
    }
    *fnz = (d->cls & CL_NASAL) ? 450.0 : 270.0;
    if (it[i].ph == PH_AH && it[i].stress == 0) {          /* schwa: central */
        f[0] = 500; f[1] = 1400; f[2] = 2450;
    }
    if (d->place == PL_VEL) {                              /* k g ng: locus follows the vowel */
        int j = i + 1;
        if (!is_vowel(it, n, j)) j = i - 1;
        int front = is_vowel(it, n, j) && is_front(it[j].ph);
        f[1] = front ? 2200 : 1500;
        f[2] = front ? 2800 : 2200;
    }
    if (it[i].ph == PH_HH || it[i].ph == PH_SIL) {         /* takes the neighbouring vowel's shape */
        int j = it[i].ph == PH_SIL ? (end ? i + 1 : i - 1) : i + 1;
        if (j >= 0 && j < n && it[j].ph != PH_SIL && it[j].ph != PH_HH)
            target(it, n, j, it[i].ph == PH_SIL && !end, f, b, fnz);
        if (it[i].ph == PH_HH) { b[0] = 300; b[1] = 150; b[2] = 250; *fnz = 270; }
    }
}

/* How long this phoneme's formants take to move from/to its neighbour */
static double trans_ms(const PItem *it, int n, int i, int nb) {
    int c = cls_of(it, n, i), cn = cls_of(it, n, nb);
    if (cn & CL_SIL) return 0;
    if (c & CL_VOWEL) {
        if (cn & (CL_STOP | CL_AFFRIC)) return 45;
        if (cn & CL_FRIC) return it[nb].ph == PH_HH ? 0 : 35;
        if (cn & CL_NASAL) return 35;
        if (cn & CL_APPROX) return 40;
        return 40;
    }
    return 0;
}

static void plan_free(Plan *pl) {
    free(pl->fa); free(pl->ev); free(pl->pa);
    memset(pl, 0, sizeof *pl);
}

static void add_event(Plan *pl, int t0, int t1, double av, double ah, double af,
                      const double *a, double ab, int abrupt) {
    if (t1 <= t0) return;
    SrcEvent *e = &pl->ev[pl->nev++];
    e->t0 = t0; e->t1 = t1; e->av = av; e->ah = ah; e->af = af; e->ab = ab; e->abrupt = abrupt;
    for (int k = 0; k < 5; k++) e->a[k] = a ? a[k] : 0;
}

static void make_plan(const PItem *it, int n, Plan *pl) {
    int *start = calloc(n + 1, sizeof(int)), *dur = calloc(n + 1, sizeof(int));
    memset(pl, 0, sizeof *pl);

    /* ── Durations ── */
    for (int i = 0; i < n; i++) {
        const PhonDef *d = &PHDEF[it[i].ph];
        double ms = d->dur_ms;
        if (it[i].ph == PH_SIL) ms = it[i].pause_ms;
        else if (d->cls & CL_VOWEL) {
            if (it[i].stress == 0) ms = fmax(45, ms * 0.55);
            else if (it[i].stress == 2) ms *= 0.8;
            if (it[i].flags & F_FUNC) ms *= 0.7;
        } else {
            if (is_cons(it, n, i - 1) || is_cons(it, n, i + 1)) ms *= 0.8;   /* clusters are quicker */
            if ((d->cls & CL_STOP) && !(d->cls & CL_VOICED) && (cls_of(it, n, i + 1) & CL_SIL)) ms += 35;
        }
        dur[i] = (int)(MS(ms) * RATE);
    }
    /* Phrase-final lengthening: the last syllable before a pause */
    for (int i = 0; i < n; i++) {
        if (it[i].ph != PH_SIL || i == 0) continue;
        int j = i - 1;
        while (j > 0 && !is_vowel(it, n, j) && it[j].ph != PH_SIL) dur[j] = dur[j] * 6 / 5, j--;
        if (j > 0 && is_vowel(it, n, j)) dur[j] = dur[j] * 7 / 5;
    }
    int t = 0;
    for (int i = 0; i < n; i++) { start[i] = t; t += dur[i]; }
    start[n] = t;
    pl->n_samples = t;

    /* ── Formant track: two anchors per phoneme, straight lines between ── */
    pl->fa = calloc(2 * n + 2, sizeof(FAnchor));
    for (int i = 0; i < n; i++) {
        int c = cls_of(it, n, i), d = dur[i];
        double in = 0, out = 0;
        if (c & CL_VOWEL) { in = MS(trans_ms(it, n, i, i - 1)); out = MS(trans_ms(it, n, i, i + 1)); }
        else if (c & CL_APPROX) { in = out = d * 0.35; }
        if (in + out > 0.85 * d) { double s = 0.85 * d / (in + out); in *= s; out *= s; }
        int ta = start[i] + (int)in, tb = start[i] + d - (int)out;
        if (PHDEF[it[i].ph].f_end[0] > 0 && ta < start[i] + d * 35 / 100)   /* diphthong: hold, then glide */
            ta = start[i] + d * 35 / 100;
        if (tb < ta) tb = ta;
        FAnchor *A = &pl->fa[pl->nfa++], *B = &pl->fa[pl->nfa++];
        A->t = ta; B->t = tb;
        target(it, n, i, 0, A->f, A->b, &A->fnz);
        target(it, n, i, 1, B->f, B->b, &B->fnz);
    }

    /* ── Sources ── */
    pl->ev = calloc(3 * n + 4, sizeof(SrcEvent));
    int vot_left = 0;          /* samples of aspiration still owed after a p/t/k */
    double vot_db = 0;
    for (int i = 0; i < n; i++) {
        const PhonDef *d = &PHDEF[it[i].ph];
        int t0 = start[i], t1 = start[i] + dur[i];
        int c = d->cls;
        double av = d->av;
        if ((c & CL_VOWEL) && it[i].stress == 0) av -= 3;
        if ((c & CL_VOWEL) && (it[i].flags & F_FUNC)) av -= 2;

        if (c & CL_SIL) { add_event(pl, t0, t1, 0, 0, 0, NULL, 0, 0); vot_left = 0; continue; }

        if (c & CL_STOP) {
            int burst = MS((c & CL_VOICED) ? 8 : 12);
            double voicebar = ((c & CL_VOICED) && cls_of(it, n, i - 1) != CL_SIL) ? 42 : 0;
            double a[5];
            memcpy(a, d->a, sizeof a);
            if (d->place == PL_VEL) {                  /* velar burst: near F2 (back) or F3 (front) */
                int j = i + 1;
                int front = is_vowel(it, n, j) && is_front(it[j].ph);
                a[0] = front ? 0 : 56; a[1] = front ? 58 : 0;
            }
            double af = d->af, ab = d->ab;
            int asp = 0;
            if (cls_of(it, n, i + 1) & CL_SIL) {        /* before a pause: released, a little breath */
                if (!(c & CL_VOICED)) asp = MS(35);
            } else if (!is_vowel(it, n, i + 1) && !(cls_of(it, n, i + 1) & CL_APPROX))
                af -= 8;                                /* inside a cluster: a weak release */
            add_event(pl, t0, t1 - burst - asp, voicebar, 0, 0, NULL, 0, 1);
            add_event(pl, t1 - burst - asp, t1 - asp, voicebar, 0, af, a, ab, 1);
            if (asp) add_event(pl, t1 - asp, t1, 0, 50, 0, NULL, 0, 1);
            vot_left = 0;
            if (!(c & CL_VOICED) && (is_vowel(it, n, i + 1) || (cls_of(it, n, i + 1) & CL_APPROX))
                && !(i > 0 && it[i - 1].ph == PH_S)) {
                int stressed = is_vowel(it, n, i + 1) ? it[i + 1].stress == 1 : 1;
                vot_left = MS(stressed && !(it[i + 1].flags & F_FUNC) ? 55 : 30);
                vot_db = 56;
            }
            continue;
        }
        if (c & CL_AFFRIC) {
            int clos = dur[i] * 45 / 100;
            double voicebar = (c & CL_VOICED) && cls_of(it, n, i - 1) != CL_SIL ? 42 : 0;
            add_event(pl, t0, t0 + clos, voicebar, 0, 0, NULL, 0, 1);
            add_event(pl, t0 + clos, t1, (c & CL_VOICED) ? d->av : 0, 0, d->af, d->a, d->ab, 1);
            vot_left = 0;
            continue;
        }
        if (it[i].ph == PH_HH) {
            double hv = (is_vowel(it, n, i - 1) && is_vowel(it, n, i + 1)) ? 40 : 0;
            add_event(pl, t0, t1, hv, 58, 0, NULL, 0, 0);
            vot_left = 0;
            continue;
        }
        if (c & CL_FRIC) {
            add_event(pl, t0, t1, (c & CL_VOICED) ? av : 0, 0, d->af, d->a, d->ab, 0);
            vot_left = 0;
            continue;
        }
        /* vowels, nasals, approximants: voiced, maybe aspirated at the start */
        if (vot_left > 0) {
            int te = t0 + (vot_left < dur[i] ? vot_left : dur[i]);
            add_event(pl, t0, te, 0, vot_db, 0, NULL, 0, 1);
            vot_left -= te - t0;
            t0 = te;
        }
        add_event(pl, t0, t1, av, 0, 0, NULL, 0, 0);
    }

    /* ── Pitch: a falling line over each phrase, a rise on stressed
     *    syllables of content words, a final fall (or a rise for '?') ── */
    pl->pa = calloc(4 * n + 4, sizeof(PAnchor));
    int ps = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].ph != PH_SIL) continue;
        int pe = start[i];
        int last_v = -1;
        for (int j = i - 1; j >= 0 && it[j].ph != PH_SIL; j--)
            if (is_vowel(it, n, j)) { last_v = j; break; }
        double span = pe > ps ? pe - ps : 1;
        pl->pa[pl->npa++] = (PAnchor){ start[i] + dur[i] / 2, PITCH_START };
        for (int j = 0; j < i; j++) {
            if (start[j] < ps || !is_vowel(it, n, j)) continue;
            double a = start[j], b = start[j] + dur[j];
            double base_a = PITCH_START + (PITCH_END - PITCH_START) * (a - ps) / span;
            double base_b = PITCH_START + (PITCH_END - PITCH_START) * (b - ps) / span;
            int accent = it[j].stress == 1 && !(it[j].flags & F_FUNC);
            if (j == last_v) {
                char pu = it[i].punct;
                double endhz = pu == '?' ? PITCH_START * 1.3 : pu == ',' ? base_b + 8 : PITCH_END * 0.82;
                pl->pa[pl->npa++] = (PAnchor){ (int)a, base_a + (accent ? 10 : 0) };
                pl->pa[pl->npa++] = (PAnchor){ (int)(a + 0.3 * dur[j]), base_a + (accent && pu != '?' ? PITCH_ACCENT : 0) };
                pl->pa[pl->npa++] = (PAnchor){ (int)b, endhz };
            } else {
                pl->pa[pl->npa++] = (PAnchor){ (int)a, base_a + (accent ? 6 : 0) };
                pl->pa[pl->npa++] = (PAnchor){ (int)(a + 0.45 * dur[j]), base_a + (accent ? PITCH_ACCENT : 0) };
                pl->pa[pl->npa++] = (PAnchor){ (int)b, base_b + (accent ? 8 : 0) };
            }
        }
        ps = start[i] + dur[i];
    }
    /* anchors were added phrase by phrase; sort by time (insertion sort, nearly sorted) */
    for (int i = 1; i < pl->npa; i++) {
        PAnchor x = pl->pa[i];
        int j = i - 1;
        while (j >= 0 && pl->pa[j].t > x.t) { pl->pa[j + 1] = pl->pa[j]; j--; }
        pl->pa[j + 1] = x;
    }
    free(start); free(dur);
}


/* ═══════════════════════════════════════════════════════════════════════════
 * Klatt-style synthesizer
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct { double a, b, c, y1, y2; } Reson;          /* unity gain at 0 Hz      */
typedef struct { double a, b, c, x1, x2; } AntiReson;

static void reson_set(Reson *r, double f, double bw) {
    double R = exp(-M_PI * bw / SAMPLE_RATE);
    r->c = -R * R;
    r->b = 2.0 * R * cos(2.0 * M_PI * f / SAMPLE_RATE);
    r->a = 1.0 - r->b - r->c;
}
static inline double reson(Reson *r, double x) {
    double y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1; r->y1 = y;
    return y;
}
static void anti_set(AntiReson *z, double f, double bw) {
    Reson r;
    reson_set(&r, f, bw);
    z->a = 1.0 / r.a; z->b = -r.b / r.a; z->c = -r.c / r.a;
}
static inline double anti(AntiReson *z, double x) {
    double y = z->a * x + z->b * z->x1 + z->c * z->x2;
    z->x2 = z->x1; z->x1 = x;
    return y;
}

/* Band-pass with 0 dB peak, for the parallel (noise) branch */
typedef struct { double b0, b2, a1, a2, x1, x2, y1, y2; } Bpf;
static void bpf_set(Bpf *f, double freq, double bw) {
    double w0 = 2.0 * M_PI * freq / SAMPLE_RATE, alpha = sin(w0) * bw / (2.0 * freq);
    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0; f->b2 = -alpha / a0;
    f->a1 = -2.0 * cos(w0) / a0; f->a2 = (1.0 - alpha) / a0;
}
static inline double bpf(Bpf *f, double x) {
    double y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y;
    return y;
}

static inline double db_amp(double db) { return db <= 0 ? 0.0 : pow(10.0, (db - 60.0) / 20.0); }

/* Relative levels of the three sources */
#define G_VOICE  1.0
#define G_ASP    0.005
#define G_FRIC   0.02

/* Render a plan into 16-bit samples. Returns a malloc'd buffer. */
static Sint16 *render(const Plan *pl, int *out_len) {
    int N = pl->n_samples;
    float *y = calloc(N > 0 ? N : 1, sizeof(float));
    Reson r[5], rnp, rgp;
    AntiReson rnz, rgz;
    Bpf p[5];
    memset(r, 0, sizeof r); memset(&rnp, 0, sizeof rnp); memset(&rgp, 0, sizeof rgp);
    memset(&rnz, 0, sizeof rnz); memset(&rgz, 0, sizeof rgz); memset(p, 0, sizeof p);
    reson_set(&r[3], 3300, 250);
    reson_set(&r[4], 3850, 200);
    reson_set(&rnp, 270, 100);
    reson_set(&rgp, 0, 100);
    anti_set(&rgz, 1500, 6000);          /* glottal zero: Klatt's brighter voice */
    bpf_set(&p[2], 3300, 300);
    bpf_set(&p[3], 3850, 400);
    bpf_set(&p[4], 5300, 1200);

    double phase = 0, lp_noise = 0, prev_c = 0;
    double av = 0, ah = 0, af = 0, ab = 0, a[5] = { 0 };
    unsigned seed = 12345;
    int fi = 0, ei = 0, pi = 0;
    double k = 1.0 - exp(-1.0 / (0.004 * SAMPLE_RATE));     /* 4 ms ramps */
    double peak = 1e-9;

    for (int s = 0; s < N; s++) {
        /* formants: every 32 samples */
        if ((s & 31) == 0) {
            while (fi + 1 < pl->nfa && pl->fa[fi + 1].t <= s) fi++;
            const FAnchor *A = &pl->fa[fi], *B = &pl->fa[fi + 1 < pl->nfa ? fi + 1 : fi];
            double u = (B->t > A->t && s > A->t) ? (double)(s - A->t) / (B->t - A->t) : 0;
            if (s < A->t) u = 0;
            if (u > 1) u = 1;
            double f[3], b[3];
            for (int q = 0; q < 3; q++) {
                f[q] = A->f[q] + u * (B->f[q] - A->f[q]);
                b[q] = A->b[q] + u * (B->b[q] - A->b[q]);
                reson_set(&r[q], f[q], b[q]);
            }
            anti_set(&rnz, A->fnz + u * (B->fnz - A->fnz), 100);
            bpf_set(&p[0], f[1], fmax(b[1], 150));
            bpf_set(&p[1], f[2], fmax(b[2], 200));
        }
        /* sources: current event, ramped */
        while (ei + 1 < pl->nev && pl->ev[ei + 1].t0 <= s) {
            ei++;
            if (pl->ev[ei].abrupt) {
                av = db_amp(pl->ev[ei].av); ah = db_amp(pl->ev[ei].ah); af = db_amp(pl->ev[ei].af);
                ab = db_amp(pl->ev[ei].ab);
                for (int q = 0; q < 5; q++) a[q] = db_amp(pl->ev[ei].a[q]);
            }
        }
        const SrcEvent *e = &pl->ev[ei];
        av += k * (db_amp(e->av) - av);
        ah += k * (db_amp(e->ah) - ah);
        af += k * (db_amp(e->af) - af);
        ab += k * (db_amp(e->ab) - ab);
        for (int q = 0; q < 5; q++) a[q] += k * (db_amp(e->a[q]) - a[q]);

        /* pitch */
        while (pi + 1 < pl->npa && pl->pa[pi + 1].t <= s) pi++;
        double f0 = pl->npa ? pl->pa[pi].hz : PITCH_START;
        if (pi + 1 < pl->npa && pl->pa[pi + 1].t > pl->pa[pi].t && s >= pl->pa[pi].t)
            f0 += (pl->pa[pi + 1].hz - f0) * (s - pl->pa[pi].t) / (double)(pl->pa[pi + 1].t - pl->pa[pi].t);
        double tsec = (double)s / SAMPLE_RATE;
        f0 *= 1.0 + 0.006 * sin(2 * M_PI * 4.7 * tsec) + 0.004 * sin(2 * M_PI * 7.3 * tsec);

        /* glottal pulses through a low-pass (the voice's -12 dB/octave) */
        double imp = 0;
        phase += f0 / SAMPLE_RATE;
        if (phase >= 1.0) { phase -= 1.0; imp = 1.0; }
        double glot = anti(&rgz, reson(&rgp, imp * 60.0)) * av * G_VOICE;

        seed = seed * 1664525u + 1013904223u;
        double noise = (double)(int)seed / 2147483648.0;
        lp_noise += 0.25 * (noise - lp_noise);
        double asp = lp_noise * ah * G_ASP * 4.0;
        double fric_noise = noise * (av > 0.01 && phase > 0.5 ? 0.6 : 1.0);   /* voiced fricatives buzz */

        /* cascade: nasal pole/zero pair, then F5..F1 */
        double x = glot + asp;
        x = anti(&rnz, reson(&rnp, x));
        for (int q = 4; q >= 0; q--) x = reson(&r[q], x);
        double out = x - prev_c;                  /* lip radiation */
        prev_c = x;

        /* parallel noise branch */
        double fn = fric_noise * af * G_FRIC;
        if (fn != 0.0 || a[0] + a[1] + a[2] + a[3] + a[4] + ab > 0) {
            out += bpf(&p[0], fn) * a[0] - bpf(&p[1], fn) * a[1] + bpf(&p[2], fn) * a[2]
                 - bpf(&p[3], fn) * a[3] + bpf(&p[4], fn) * a[4] + fn * ab * 0.5;
        }
        y[s] = (float)out;
        if (fabs(out) > peak) peak = fabs(out);
    }

    Sint16 *pcm = malloc((N > 0 ? N : 1) * sizeof(Sint16));
    double g = 0.85 / peak;
    int fade = MS(10);
    for (int s = 0; s < N; s++) {
        double v = y[s] * g;
        if (s < fade) v *= (double)s / fade;
        if (N - s < fade) v *= (double)(N - s) / fade;
        pcm[s] = (Sint16)(tanh(v * 1.1) / tanh(1.1) * 32000.0);
    }
    free(y);
    *out_len = N;
    return pcm;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Playback
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    Sint16 *pcm;
    int     len, pos;
    int     speaking;            /* 1 while samples remain               */
} Player;

static Player     g_player;
static SDL_mutex *g_mutex;

/* Text → samples (outside the audio lock; it takes a few milliseconds) */
static Sint16 *synth_render(const char *text, int *len) {
    static PItem items[MAX_PHONEMES];
    Plan pl;
    int n = text_to_phonemes(text, items, MAX_PHONEMES);
    make_plan(items, n, &pl);
    Sint16 *pcm = render(&pl, len);
    plan_free(&pl);
    return pcm;
}

static void synth_speak(const char *text) {
    int len;
    Sint16 *pcm = synth_render(text, &len), *old;
    SDL_LockMutex(g_mutex);
    old = g_player.pcm;
    g_player.pcm = pcm; g_player.len = len; g_player.pos = 0; g_player.speaking = 1;
    SDL_UnlockMutex(g_mutex);
    free(old);
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    Sint16 *out = (Sint16 *)stream;
    int samples = len / (int)sizeof(Sint16);
    SDL_LockMutex(g_mutex);
    Player *p = &g_player;
    for (int i = 0; i < samples; i++) {
        if (p->speaking && p->pos < p->len) out[i] = p->pcm[p->pos++];
        else { out[i] = 0; p->speaking = 0; }
    }
    SDL_UnlockMutex(g_mutex);
}

static int write_wav(const char *path, const Sint16 *pcm, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    unsigned char h[44];
    unsigned data = (unsigned)n * 2, v;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;                  /* PCM, mono */
    v = SAMPLE_RATE; memcpy(h + 24, &v, 4); v = SAMPLE_RATE * 2; memcpy(h + 28, &v, 4);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f);
    fwrite(pcm, 2, n, f);                 /* little-endian hosts */
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    printf("Loading Dictionary\n");
    load_cmudict("cmudict-0.7b");
    /* The hand-curated entries (proper nouns like "falken"/"joshua" and
     * abbreviations like "dr"/"mrs") go in after CMUdict; see load_hand_dict. */
    load_hand_dict();
    printf("End Loading Dictionary\n");

    if (argc > 3 && strcmp(argv[1], "-o") == 0) {               /* -o out.wav "text" */
        int len;
        Sint16 *pcm = synth_render(argv[3], &len);
        int r = write_wav(argv[2], pcm, len);
        free(pcm);
        return r;
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_mutex = SDL_CreateMutex();

    SDL_AudioSpec want = {0}, got;
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = BUFFER_SIZE;
    want.callback = audio_callback;
    if (SDL_OpenAudio(&want, &got) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return 1;
    }
    SDL_PauseAudio(0);

    if (argc < 2) {
        char line[1024];
        printf("Formant TTS ready. Type text and press Enter. Q to quit.\n");
        while (1) {
            printf("> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin))
                break;
            if (strcasecmp(line, "q\n") == 0 || strcasecmp(line, "quit\n") == 0)
                break;
            synth_speak(line);
        }
    } else {
        synth_speak(argv[1]);
        while (1) {
            SDL_LockMutex(g_mutex);
            int speaking = g_player.speaking;
            SDL_UnlockMutex(g_mutex);
            if (!speaking) break;
            SDL_Delay(20);
        }
        SDL_Delay(100);
    }

    SDL_CloseAudio();
    SDL_DestroyMutex(g_mutex);
    SDL_Quit();
    return 0;
}
