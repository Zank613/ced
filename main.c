/* ced - A single-file text editor with syntax highlighting using ncurses.
   Version: v2.5
   Changes in v2.5:
     - Added "Goto Line" feature (Ctrl+G)
     - Display version label in status bar

   Compile:  gcc -o ced_v2.5 main.c -lncurses
   Run:      ./ced_v2.5
*/

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>   /* for mkdir/stat */
#include <sys/types.h>  /* for mode_t, etc. */
#include <errno.h>      /* for errno, strerror */

/* --------------- Version --------------- */
#define CED_VERSION "v2.5"

/* --------------- Editor Configs and Globals --------------- */

#define MAX_LINES 1000
#define MAX_COLS 1024
#define LINE_NUMBER_WIDTH 6
#define PROMPT_BUFFER_SIZE 256
#define UNDO_STACK_SIZE 100

/* We can read these from settings.config if we want. */
typedef struct Config
{
    int tab_four_spaces;   /* If true, pressing Tab inserts 4 spaces */
    int auto_indent;       /* If true, new lines inherit indentation */
} Config;
Config config = { 1, 1 }; /* defaults: both on */

char current_file[PROMPT_BUFFER_SIZE] = {0};
int dirty = 0; /* if file has unsaved changes */

/* --------------- Syntax Highlighter Data Structures --------------- */

#define SH_MAX_LINE_LENGTH 1024

typedef struct SH_SyntaxRule
{
    char **tokens;       /* array of token strings */
    int token_count;
    short color_pair;    /* ncurses color pair index */
    int r, g, b;         /* RGB color values (0..255) */
} SH_SyntaxRule;

typedef struct SH_SyntaxDefinition
{
    char **extensions;       /* e.g. ".c", ".h" */
    int ext_count;
    SH_SyntaxRule *rules;    /* array of rules */
    int rule_count;
} SH_SyntaxDefinition;

typedef struct SH_SyntaxDefinitions
{
    SH_SyntaxDefinition *definitions;
    int count;
} SH_SyntaxDefinitions;

/* Global syntax definitions and selected definition */
SH_SyntaxDefinitions global_syntax_defs = { NULL, 0 };
SH_SyntaxDefinition *selected_syntax = NULL;
int syntax_enabled = 0;  /* 1 if highlighting is active */

/* --------------- Token Lookup for Fast Syntax Highlighting --------------- */

/* Struct for token lookup */
typedef struct
{
    char *token;
    short color_pair;
} TokenMap;

TokenMap *token_lookup = NULL;
int token_lookup_count = 0;

/* Comparator for qsort and bsearch */
int compare_token_map(const void *a, const void *b)
{
    const TokenMap *tm1 = (const TokenMap *)a;
    const TokenMap *tm2 = (const TokenMap *)b;
    return strcmp(tm1->token, tm2->token);
}

/* Build a lookup table from the syntax definition tokens */
void build_token_lookup(SH_SyntaxDefinition *def)
{
    int i, j, total;
    total = 0;
    for (i = 0; i < def->rule_count; i++)
    {
        total += def->rules[i].token_count;
    }
    token_lookup = (TokenMap *)malloc(sizeof(TokenMap) * total);
    token_lookup_count = 0;
    for (i = 0; i < def->rule_count; i++)
    {
        for (j = 0; j < def->rules[i].token_count; j++)
        {
            token_lookup[token_lookup_count].token = def->rules[i].tokens[j];
            token_lookup[token_lookup_count].color_pair = def->rules[i].color_pair;
            token_lookup_count++;
        }
    }
    qsort(token_lookup, token_lookup_count, sizeof(TokenMap), compare_token_map);
}

/* --------------- Editor State --------------- */

typedef struct Editor
{
    char text[MAX_LINES][MAX_COLS];
    int num_lines;
    int cursor_x;
    int cursor_y;
    int row_offset;
    int col_offset;
} Editor;

Editor editor;

/* --------------- Undo/Redo Structures --------------- */

typedef struct EditorState
{
    char text[MAX_LINES][MAX_COLS];
    int num_lines;
    int cursor_x;
    int cursor_y;
    int row_offset;
    int col_offset;
} EditorState;

EditorState undo_stack[UNDO_STACK_SIZE];
int undo_stack_top = 0;
EditorState redo_stack[UNDO_STACK_SIZE];
int redo_stack_top = 0;

/* --------------- Helper: Trim Whitespace --------------- */

static char *trim_whitespace(char *str)
{
    char *end;
    while (isspace((unsigned char)*str))
    {
        str++;
    }
    if (*str == '\0')
    {
        return str;
    }
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
    {
        *end = '\0';
        end--;
    }
    return str;
}

/* --------------- Load Settings --------------- */

void load_config(void)
{
    FILE *fp;
    char line[256];
    fp = fopen("settings.config", "r");
    if (!fp)
    {
        return;
    }
    while (fgets(line, sizeof(line), fp))
    {
        char *p;
        char key[64], value[64];
        p = trim_whitespace(line);
        if (*p == '\0' || *p == '#' || *p == '/')
        {
            continue;
        }
        if (sscanf(p, " %63[^=] = %63[^;];", key, value) == 2)
        {
            char *tkey, *tvalue;
            tkey = trim_whitespace(key);
            tvalue = trim_whitespace(value);
            if (strcmp(tkey, "TAB_FOUR_SPACES") == 0)
            {
                config.tab_four_spaces = (strcmp(tvalue, "TRUE") == 0 || strcmp(tvalue, "true") == 0);
            }
            else if (strcmp(tkey, "AUTO_INDENT") == 0)
            {
                config.auto_indent = (strcmp(tvalue, "TRUE") == 0 || strcmp(tvalue, "true") == 0);
            }
        }
    }
    fclose(fp);
}

/* --------------- Editor Init --------------- */

void init_editor(void)
{
    memset(editor.text, 0, sizeof(editor.text));
    editor.num_lines = 1;
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    editor.row_offset = 0;
    editor.col_offset = 0;
}

/* --------------- Undo/Redo Functions --------------- */

void save_state_undo(void)
{
    EditorState st;
    if (undo_stack_top < UNDO_STACK_SIZE)
    {
        memcpy(st.text, editor.text, sizeof(editor.text));
        st.num_lines = editor.num_lines;
        st.cursor_x = editor.cursor_x;
        st.cursor_y = editor.cursor_y;
        st.row_offset = editor.row_offset;
        st.col_offset = editor.col_offset;
        undo_stack[undo_stack_top++] = st;
    }
    redo_stack_top = 0;
    dirty = 1;
}

void undo(void)
{
    EditorState st, cur;
    if (undo_stack_top > 0)
    {
        st = undo_stack[--undo_stack_top];
        if (redo_stack_top < UNDO_STACK_SIZE)
        {
            memcpy(cur.text, editor.text, sizeof(editor.text));
            cur.num_lines = editor.num_lines;
            cur.cursor_x = editor.cursor_x;
            cur.cursor_y = editor.cursor_y;
            cur.row_offset = editor.row_offset;
            cur.col_offset = editor.col_offset;
            redo_stack[redo_stack_top++] = cur;
        }
        memcpy(editor.text, st.text, sizeof(editor.text));
        editor.num_lines = st.num_lines;
        editor.cursor_x = st.cursor_x;
        editor.cursor_y = st.cursor_y;
        editor.row_offset = st.row_offset;
        editor.col_offset = st.col_offset;
        dirty = 1;
    }
}

void redo(void)
{
    EditorState st, cur;
    if (redo_stack_top > 0)
    {
        st = redo_stack[--redo_stack_top];
        if (undo_stack_top < UNDO_STACK_SIZE)
        {
            memcpy(cur.text, editor.text, sizeof(editor.text));
            cur.num_lines = editor.num_lines;
            cur.cursor_x = editor.cursor_x;
            cur.cursor_y = editor.cursor_y;
            cur.row_offset = editor.row_offset;
            cur.col_offset = editor.col_offset;
            undo_stack[undo_stack_top++] = cur;
        }
        memcpy(editor.text, st.text, sizeof(editor.text));
        editor.num_lines = st.num_lines;
        editor.cursor_x = st.cursor_x;
        editor.cursor_y = st.cursor_y;
        editor.row_offset = st.row_offset;
        editor.col_offset = st.col_offset;
        dirty = 1;
    }
}

/* --------------- Viewport --------------- */

void update_viewport(void)
{
    int rows, cols, usable;
    getmaxyx(stdscr, rows, cols);
    if (editor.cursor_y < editor.row_offset)
    {
        editor.row_offset = editor.cursor_y;
    }
    else if (editor.cursor_y >= editor.row_offset + (rows - 1))
    {
        editor.row_offset = editor.cursor_y - (rows - 2);
    }
    usable = cols - LINE_NUMBER_WIDTH;
    if (editor.cursor_x < editor.col_offset)
    {
        editor.col_offset = editor.cursor_x;
    }
    else if (editor.cursor_x >= editor.col_offset + usable)
    {
        editor.col_offset = editor.cursor_x - usable + 1;
    }
}

/* --------------- Syntax Highlighter Implementation --------------- */

/* Pre-scan for tokens in a rule line. */
static int sh_count_tokens(const char *line)
{
    int count;
    const char *p;
    char *end_quote;
    count = 0;
    p = line;
    while ((p = strchr(p, '\"')) != NULL)
    {
        p++; 
        end_quote = (char *)strchr(p, '\"');
        if (!end_quote)
        {
            break;
        }
        count++;
        p = end_quote + 1;
    }
    return count;
}

/* Parse one rule line: e.g. "int", "double" = (255,0,0); */
static int sh_parse_rule_line(char *line, SH_SyntaxRule *rule)
{
    int count, idx, len, r, g, b;
    char *p, *end_quote, *eq, *paren;
    rule->tokens = NULL;
    rule->token_count = 0;
    rule->r = rule->g = rule->b = 0;
    count = sh_count_tokens(line);
    if (count > 0)
    {
        rule->tokens = (char **)malloc(sizeof(char *) * count);
        rule->token_count = count;
        idx = 0;
        p = line;
        while ((p = strchr(p, '\"')) != NULL && idx < count)
        {
            p++; 
            end_quote = strchr(p, '\"');
            if (!end_quote)
            {
                break;
            }
            len = (int)(end_quote - p);
            rule->tokens[idx] = (char *)malloc(len + 1);
            strncpy(rule->tokens[idx], p, len);
            rule->tokens[idx][len] = '\0';
            idx++;
            p = end_quote + 1;
        }
    }
    eq = strchr(line, '=');
    if (!eq)
    {
        return -1;
    }
    paren = strchr(eq, '(');
    if (!paren)
    {
        return -1;
    }
    if ((sscanf(paren, " ( %d , %d , %d )", &r, &g, &b) < 3) &&
        (sscanf(paren, " (%d,%d,%d)", &r, &g, &b) < 3))
    {
        return -1;
    }
    rule->r = r;
    rule->g = g;
    rule->b = b;
    return 0;
}

/* -------------------------------------------------------------------------
   SH_SyntaxDefinitions sh_load_syntax_definitions(const char *filename)

   Reads highlight.syntax and accumulates multi-line rules until we find
   the terminating semicolon. This allows e.g.:

       "char", "short",
       "int", "long"
       = (255, 0, 0);

   Also continues until we see a '}' that ends the block of rules.
--------------------------------------------------------------------------- */
static SH_SyntaxDefinitions sh_load_syntax_definitions(const char *filename)
{
    SH_SyntaxDefinitions defs;
    FILE *fp;
    char line[SH_MAX_LINE_LENGTH];
    char rulebuf[4 * SH_MAX_LINE_LENGTH];
    char *trimmed;
    int done, have_semicolon;
    int i;
    defs.definitions = NULL;
    defs.count = 0;
    fp = fopen(filename, "r");
    if (!fp)
    {
        return defs;
    }
    while (fgets(line, sizeof(line), fp))
    {
        trimmed = trim_whitespace(line);
        if (strncmp(trimmed, "SYNTAX", 6) == 0)
        {
            SH_SyntaxDefinition def;
            char *p;
            int len;
            def.extensions = NULL;
            def.ext_count = 0;
            def.rules = NULL;
            def.rule_count = 0;
            p = trimmed + 6;
            while (*p)
            {
                if (*p == '\"')
                {
                    char *end;
                    char *ext;
                    int ext_len;
                    p++;
                    end = strchr(p, '\"');
                    if (!end)
                    {
                        break;
                    }
                    ext_len = (int)(end - p);
                    ext = (char *)malloc(ext_len + 1);
                    strncpy(ext, p, ext_len);
                    ext[ext_len] = '\0';
                    def.extensions = (char **)realloc(def.extensions, sizeof(char *) * (def.ext_count + 1));
                    def.extensions[def.ext_count] = ext;
                    def.ext_count++;
                    p = end + 1;
                }
                else
                {
                    p++;
                }
            }
            if (!fgets(line, sizeof(line), fp))
            {
                break;
            }
            trimmed = trim_whitespace(line);
            if (trimmed[0] != '{')
            {
                continue;
            }
            while (1)
            {
                rulebuf[0] = '\0';
                done = 0;
                have_semicolon = 0;
                while (!have_semicolon)
                {
                    if (!fgets(line, sizeof(line), fp))
                    {
                        done = 1;
                        break;
                    }
                    trimmed = trim_whitespace(line);
                    if (trimmed[0] == '}')
                    {
                        done = 1;
                        break;
                    }
                    if (strlen(trimmed) == 0)
                    {
                        continue;
                    }
                    strncat(rulebuf, trimmed, sizeof(rulebuf) - strlen(rulebuf) - 2);
                    strncat(rulebuf, " ", sizeof(rulebuf) - strlen(rulebuf) - 2);
                    if (strchr(trimmed, ';') != NULL)
                    {
                        have_semicolon = 1;
                        break;
                    }
                }
                if (done)
                {
                    break;
                }
                trimmed = trim_whitespace(rulebuf);
                if (trimmed[0] == '\0')
                {
                    continue;
                }
                {
                    SH_SyntaxRule rule;
                    if (sh_parse_rule_line(trimmed, &rule) == 0)
                    {
                        def.rules = (SH_SyntaxRule *)realloc(def.rules, sizeof(SH_SyntaxRule) * (def.rule_count + 1));
                        def.rules[def.rule_count] = rule;
                        def.rule_count++;
                    }
                }
            }
            defs.definitions = (SH_SyntaxDefinition *)realloc(defs.definitions, sizeof(SH_SyntaxDefinition) * (defs.count + 1));
            defs.definitions[defs.count] = def;
            defs.count++;
        }
    }
    fclose(fp);
    return defs;
}

/* Free memory from definitions. */
void sh_free_syntax_definitions(SH_SyntaxDefinitions defs)
{
    int i, j, k, t;
    for (i = 0; i < defs.count; i++)
    {
        SH_SyntaxDefinition d = defs.definitions[i];
        for (j = 0; j < d.ext_count; j++)
        {
            free(d.extensions[j]);
        }
        free(d.extensions);
        for (k = 0; k < d.rule_count; k++)
        {
            SH_SyntaxRule rr = d.rules[k];
            for (t = 0; t < rr.token_count; t++)
            {
                free(rr.tokens[t]);
            }
            free(rr.tokens);
        }
        free(d.rules);
    }
    free(defs.definitions);
}

/* Check file extension. */
int sh_file_has_extension(const char *filename, SH_SyntaxDefinition def)
{
    int i;
    size_t elen, flen;
    for (i = 0; i < def.ext_count; i++)
    {
        char *ext = def.extensions[i];
        elen = strlen(ext);
        flen = strlen(filename);
        if (flen >= elen)
        {
            if (strcmp(filename + flen - elen, ext) == 0)
            {
                return 1;
            }
        }
    }
    return 0;
}

/* Init colors for each rule. */
void sh_init_syntax_colors(SH_SyntaxDefinition *def)
{
    short next_color_index;
    short next_pair_index;
    int i;
    short r_scaled, g_scaled, b_scaled;
    next_color_index = 16;
    next_pair_index = 1;
    for (i = 0; i < def->rule_count; i++)
    {
        SH_SyntaxRule *rule = &def->rules[i];
        {
            short color_num, pair_idx;
            color_num = next_color_index++;
            pair_idx = next_pair_index++;
            r_scaled = (short)((rule->r * 1000) / 255);
            g_scaled = (short)((rule->g * 1000) / 255);
            b_scaled = (short)((rule->b * 1000) / 255);
            if (can_change_color())
            {
                init_color(color_num, r_scaled, g_scaled, b_scaled);
            }
            init_pair(pair_idx, color_num, -1);
            rule->color_pair = pair_idx;
        }
    }
}

/* --------------- Editor Screen Refresh & Syntax Drawing --------------- */

static void draw_text(WINDOW *win)
{
    int rows, cols;
    getmaxyx(win, rows, cols);

    if (syntax_enabled && selected_syntax && token_lookup)
    {
        int row = 0;
        int i;
        for (i = editor.row_offset; i < editor.num_lines && row < rows - 1; i++, row++)
        {
            /* Print line number at the start */
            mvwprintw(win, row, 0, "%4d |", i + 1);

            /* We'll track 'col' on-screen, starting right after the line number. */
            {
                int col = LINE_NUMBER_WIDTH;
                char *line = editor.text[i];
                int len = (int)strlen(line);
                int j = editor.col_offset;

                while (j < len && col < cols)
                {
                    /* Check if we have an identifier (word) */
                    if (isalpha((unsigned char)line[j]) || line[j] == '_')
                    {
                        int start = j;
                        while (j < len && (isalnum((unsigned char)line[j]) || line[j] == '_'))
                        {
                            j++;
                        }
                        {
                            int wlen = j - start;
                            char word[128];
                            int highlighted = 0;
                            if (wlen < 128)
                            {
                                strncpy(word, &line[start], wlen);
                                word[wlen] = '\0';
                            }
                            else
                            {
                                word[0] = '\0';
                            }
                            {
                                TokenMap key;
                                key.token = word;
                                {
                                    TokenMap *found = (TokenMap *)bsearch(
                                        &key,
                                        token_lookup,
                                        token_lookup_count,
                                        sizeof(TokenMap),
                                        compare_token_map
                                    );
                                    if (found)
                                    {
                                        int k;
                                        wattron(win, COLOR_PAIR(found->color_pair));
                                        for (k = 0; k < wlen && col < cols; k++, col++)
                                        {
                                            mvwaddch(win, row, col, word[k]);
                                        }
                                        wattroff(win, COLOR_PAIR(found->color_pair));
                                        highlighted = 1;
                                    }
                                }
                            }
                            if (!highlighted)
                            {
                                int k;
                                for (k = 0; k < wlen && col < cols; k++, col++)
                                {
                                    mvwaddch(win, row, col, word[k]);
                                }
                            }
                        }
                    }
                    else
                    {
                        mvwaddch(win, row, col, line[j]);
                        col++;
                        j++;
                    }
                }
            }
        }
    }
    else
    {
        /* No syntax highlighting; just dump the lines. */
        int i;
        for (i = editor.row_offset; i < editor.num_lines && i < editor.row_offset + rows - 1; i++)
        {
            int scr_row = i - editor.row_offset;
            mvwprintw(win, scr_row, 0, "%4d |", i + 1);
            {
                int usable = cols - LINE_NUMBER_WIDTH;
                mvwprintw(win, scr_row, LINE_NUMBER_WIDTH, "%.*s",
                          usable, &editor.text[i][editor.col_offset]);
            }
        }
    }
}

void editor_refresh_screen(void)
{
    int rows, cols, scr_y, scr_x;
    erase();
    getmaxyx(stdscr, rows, cols);
    update_viewport();
    draw_text(stdscr);

    {
        /* Show version, file name, etc. in status line */
        char status[256];
        const char *fname = (current_file[0] != '\0') ? current_file : "Untitled";
        snprintf(status, sizeof(status),
                 "[%s] File: %s | Ln: %d, Col: %d%s",
                 CED_VERSION,
                 fname,
                 editor.cursor_y + 1,
                 editor.cursor_x + 1,
                 (dirty ? " [Modified]" : ""));
        mvprintw(rows - 1, 0, "%s  (Ctrl+Q: Quit, Ctrl+S: Save, Ctrl+O: Open, Ctrl+Z: Undo, Ctrl+Y: Redo, Ctrl+T: Terminal, Ctrl+G: Goto Line, Home/End, PgUp/PgDn, Mouse)",
                 status);
    }
    scr_y = editor.cursor_y - editor.row_offset;
    scr_x = editor.cursor_x - editor.col_offset + LINE_NUMBER_WIDTH;
    move(scr_y, scr_x);
    wnoutrefresh(stdscr);
    doupdate();
}

/* --------------- Editor Operations --------------- */

void editor_insert_char(int ch)
{
    int len, i;
    char *line;
    line = editor.text[editor.cursor_y];
    len = (int)strlen(line);
    if (len >= MAX_COLS - 1)
    {
        return;
    }
    for (i = len; i >= editor.cursor_x; i--)
    {
        line[i + 1] = line[i];
    }
    line[editor.cursor_x] = ch;
    editor.cursor_x++;
}

void editor_delete_char(void)
{
    int i, len, prev_len;
    char *line;
    if (editor.cursor_x == 0)
    {
        if (editor.cursor_y == 0)
        {
            return;
        }
        else
        {
            prev_len = (int)strlen(editor.text[editor.cursor_y - 1]);
            strcat(editor.text[editor.cursor_y - 1], editor.text[editor.cursor_y]);
            for (i = editor.cursor_y; i < editor.num_lines - 1; i++)
            {
                strcpy(editor.text[i], editor.text[i + 1]);
            }
            editor.num_lines--;
            editor.cursor_y--;
            editor.cursor_x = prev_len;
        }
    }
    else
    {
        line = editor.text[editor.cursor_y];
        len = (int)strlen(line);
        for (i = editor.cursor_x - 1; i < len; i++)
        {
            line[i] = line[i + 1];
        }
        editor.cursor_x--;
    }
}

void editor_delete_at_cursor(void)
{
    int i, len;
    char *line;
    line = editor.text[editor.cursor_y];
    len = (int)strlen(line);
    if (editor.cursor_x == len)
    {
        if (editor.cursor_y == editor.num_lines - 1)
        {
            return;
        }
        else
        {
            strcat(line, editor.text[editor.cursor_y + 1]);
            for (i = editor.cursor_y + 1; i < editor.num_lines - 1; i++)
            {
                strcpy(editor.text[i], editor.text[i + 1]);
            }
            editor.num_lines--;
        }
    }
    else
    {
        for (i = editor.cursor_x; i < len; i++)
        {
            line[i] = line[i + 1];
        }
    }
}

void editor_insert_newline(void)
{
    int len, i, indent;
    char remainder[MAX_COLS];
    char new_line[MAX_COLS];
    char *line;
    if (editor.num_lines >= MAX_LINES)
    {
        return;
    }
    line = editor.text[editor.cursor_y];
    len = (int)strlen(line);
    strcpy(remainder, line + editor.cursor_x);
    line[editor.cursor_x] = '\0';
    for (i = editor.num_lines; i > editor.cursor_y + 1; i--)
    {
        strcpy(editor.text[i], editor.text[i - 1]);
    }
    if (config.auto_indent)
    {
        indent = 0;
        while (line[indent] == ' ' && indent < MAX_COLS - 1)
        {
            indent++;
        }
        memset(new_line, ' ', indent);
        new_line[indent] = '\0';
        strncat(new_line, remainder, MAX_COLS - indent - 1);
        strcpy(editor.text[editor.cursor_y + 1], new_line);
        editor.cursor_x = indent;
    }
    else
    {
        strcpy(editor.text[editor.cursor_y + 1], remainder);
        editor.cursor_x = 0;
    }
    editor.num_lines++;
    editor.cursor_y++;
}

/* --------------- Editor Prompt --------------- */

void editor_prompt(char *prompt, char *buffer, size_t bufsize)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    move(rows - 1, 0);
    clrtoeol();
    mvprintw(rows - 1, 0, "%s", prompt);
    echo();
    curs_set(1);
    getnstr(buffer, (int)bufsize - 1);
    noecho();
    curs_set(1);
}

/* ---------------- Goto Line Feature (Added in v2.5) ---------------- */

void editor_goto_line(void)
{
    char line_str[PROMPT_BUFFER_SIZE];
    int line_num;
    editor_prompt("Goto line: ", line_str, sizeof(line_str));
    if (line_str[0] == '\0')
    {
        return; /* user cancelled or empty input */
    }
    line_num = atoi(line_str);
    if (line_num < 1)
    {
        line_num = 1;
    }
    if (line_num > editor.num_lines)
    {
        line_num = editor.num_lines;
    }
    /* Our internal indexing is zero-based, so subtract 1. */
    editor.cursor_y = line_num - 1;
    editor.cursor_x = 0; /* jump to start of that line */
}

/* --------------- Editor Save File --------------- */

void editor_save_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];
    int i, r, c;
    if (current_file[0] != '\0')
    {
        strncpy(filename, current_file, PROMPT_BUFFER_SIZE);
    }
    else
    {
        editor_prompt("Save as: ", filename, PROMPT_BUFFER_SIZE);
        if (filename[0] == '\0')
        {
            return;
        }
        {
            struct stat stt;
            if (stat("saves", &stt) == -1)
            {
                if (mkdir("saves", 0777) == -1)
                {
                    getmaxyx(stdscr, r, c);
                    mvprintw(r - 1, 0, "Error creating 'saves' dir: %s", strerror(errno));
                    getch();
                    return;
                }
            }
        }
        snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
        strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
    }
    if (current_file[0] == '\0')
    {
        strncpy(current_file, filename, PROMPT_BUFFER_SIZE);
    }
    {
        FILE *fp;
        fp = fopen(current_file, "w");
        if (!fp)
        {
            getmaxyx(stdscr, r, c);
            mvprintw(r - 1, 0, "Error opening file for writing: %s", strerror(errno));
            clrtoeol();
            getch();
            return;
        }
        for (i = 0; i < editor.num_lines; i++)
        {
            fprintf(fp, "%s\n", editor.text[i]);
        }
        fclose(fp);
        dirty = 0;
        getmaxyx(stdscr, r, c);
        mvprintw(r - 1, 0, "File saved as %s. Press any key...", current_file);
        clrtoeol();
        getch();
    }
}

/* --------------- Editor Load File --------------- */

void editor_load_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];
    char line_buffer[MAX_COLS];  /* New buffer for reading file lines */
    int i, r, c;
    editor_prompt("Open file: ", filename, PROMPT_BUFFER_SIZE);
    if (filename[0] == '\0')
    {
        return;
    }
    snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
    {
        FILE *fp;
        fp = fopen(filepath, "r");
        if (!fp)
        {
            fp = fopen(filename, "r");
            if (!fp)
            {
                getmaxyx(stdscr, r, c);
                mvprintw(r - 1, 0, "Error opening file for reading: %s", strerror(errno));
                clrtoeol();
                getch();
                return;
            }
            else
            {
                strcpy(filepath, filename);
            }
        }
        memset(editor.text, 0, sizeof(editor.text));
        editor.num_lines = 0;
        while (fgets(line_buffer, MAX_COLS, fp) && editor.num_lines < MAX_LINES)
        {
            size_t ln;
            ln = strlen(line_buffer);
            if (ln > 0 && line_buffer[ln - 1] == '\n')
            {
                line_buffer[ln - 1] = '\0';
            }
            strncpy(editor.text[editor.num_lines], line_buffer, MAX_COLS - 1);
            editor.num_lines++;
        }
        fclose(fp);
        editor.cursor_x = 0;
        editor.cursor_y = 0;
        strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
        dirty = 0;
        syntax_enabled = 0;
        for (i = 0; i < global_syntax_defs.count; i++)
        {
            if (sh_file_has_extension(current_file, global_syntax_defs.definitions[i]))
            {
                selected_syntax = &global_syntax_defs.definitions[i];
                syntax_enabled = 1;
                sh_init_syntax_colors(selected_syntax);
                build_token_lookup(selected_syntax);
                break;
            }
        }
        getmaxyx(stdscr, r, c);
        mvprintw(r - 1, 0, "File loaded from %s. Press any key...", current_file);
        clrtoeol();
        getch();
    }
}

/* --------------- Embedded Terminal (Ctrl+T) --------------- */

void launch_terminal(void)
{
    def_prog_mode();
    endwin();
    system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
    reset_prog_mode();
    refresh();
}

/* --------------- Process Key & Mouse --------------- */

void process_keypress(void)
{
    int ch;
    MEVENT event;
    ch = getch();
    if (ch == KEY_MOUSE)
    {
        int new_y, new_x, ll;
        if (getmouse(&event) == OK)
        {
            if (event.bstate & BUTTON1_CLICKED)
            {
                new_y = event.y + editor.row_offset;
                new_x = (event.x < LINE_NUMBER_WIDTH) ? 0 : (event.x - LINE_NUMBER_WIDTH + editor.col_offset);
                if (new_y >= editor.num_lines)
                {
                    new_y = editor.num_lines - 1;
                }
                ll = (int)strlen(editor.text[new_y]);
                if (new_x > ll)
                {
                    new_x = ll;
                }
                editor.cursor_y = new_y;
                editor.cursor_x = new_x;
            }
            else if (event.bstate & BUTTON4_PRESSED)
            {
                editor.cursor_y -= 3;
                if (editor.cursor_y < 0)
                {
                    editor.cursor_y = 0;
                }
            }
            else if (event.bstate & BUTTON5_PRESSED)
            {
                editor.cursor_y += 3;
                if (editor.cursor_y >= editor.num_lines)
                {
                    editor.cursor_y = editor.num_lines - 1;
                }
            }
        }
        return;
    }

    switch (ch)
    {
        case 7: /* Ctrl+G: Goto line */
            editor_goto_line();
            break;
        case 17: /* Ctrl+Q */
            endwin();
            exit(0);
            break;
        case 26: /* Ctrl+Z: Undo */
            undo();
            break;
        case 25: /* Ctrl+Y: Redo */
            redo();
            break;
        case 19: /* Ctrl+S: Save */
            editor_save_file();
            break;
        case 15: /* Ctrl+O: Open */
            editor_load_file();
            break;
        case 20: /* Ctrl+T: Terminal */
            launch_terminal();
            break;
        case KEY_HOME:
            editor.cursor_x = 0;
            break;
        case KEY_END:
        {
            int ln;
            ln = (int)strlen(editor.text[editor.cursor_y]);
            editor.cursor_x = ln;
            break;
        }
        case KEY_PPAGE:
            editor.cursor_y -= 5;
            if (editor.cursor_y < 0)
            {
                editor.cursor_y = 0;
            }
            break;
        case KEY_NPAGE:
            editor.cursor_y += 5;
            if (editor.cursor_y >= editor.num_lines)
            {
                editor.cursor_y = editor.num_lines - 1;
            }
            break;
        case '\t':
        {
            int i;
            save_state_undo();
            if (config.tab_four_spaces)
            {
                for (i = 0; i < 4; i++)
                {
                    editor_insert_char(' ');
                }
            }
            else
            {
                editor_insert_char('\t');
            }
            break;
        }
        case KEY_LEFT:
            if (editor.cursor_x > 0)
            {
                editor.cursor_x--;
            }
            else if (editor.cursor_y > 0)
            {
                editor.cursor_y--;
                editor.cursor_x = (int)strlen(editor.text[editor.cursor_y]);
            }
            break;
        case KEY_RIGHT:
        {
            int length;
            length = (int)strlen(editor.text[editor.cursor_y]);
            if (editor.cursor_x < length)
            {
                editor.cursor_x++;
            }
            else if (editor.cursor_y < editor.num_lines - 1)
            {
                editor.cursor_y++;
                editor.cursor_x = 0;
            }
            break;
        }
        case KEY_UP:
            if (editor.cursor_y > 0)
            {
                editor.cursor_y--;
                {
                    int ll;
                    ll = (int)strlen(editor.text[editor.cursor_y]);
                    if (editor.cursor_x > ll)
                    {
                        editor.cursor_x = ll;
                    }
                }
            }
            break;
        case KEY_DOWN:
            if (editor.cursor_y < editor.num_lines - 1)
            {
                editor.cursor_y++;
                {
                    int ll;
                    ll = (int)strlen(editor.text[editor.cursor_y]);
                    if (editor.cursor_x > ll)
                    {
                        editor.cursor_x = ll;
                    }
                }
            }
            break;
        case KEY_BACKSPACE:
        case 127:
            save_state_undo();
            editor_delete_char();
            break;
        case KEY_DC:
            save_state_undo();
            editor_delete_at_cursor();
            break;
        case '\n':
        case '\r':
            save_state_undo();
            editor_insert_newline();
            break;
        default:
            if (ch >= 32 && ch <= 126)
            {
                save_state_undo();
                editor_insert_char(ch);
            }
            break;
    }
}

/* --------------- Main --------------- */

int main(void)
{
    load_config();
    global_syntax_defs = sh_load_syntax_definitions("highlight.syntax");

    initscr();
    start_color();
    use_default_colors();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    mousemask(ALL_MOUSE_EVENTS, NULL);
    mouseinterval(0);

    init_editor();

    while (1)
    {
        editor_refresh_screen();
        process_keypress();
    }

    sh_free_syntax_definitions(global_syntax_defs);
    endwin();
    return 0;
}
