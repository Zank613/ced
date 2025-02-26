/*
    ced_v4.5 - A single-file text editor with syntax highlighting using ncurses.

    Key Features:
      - Partial redraw (only update changed lines)
      - Shell panel for commands (Ctrl+W toggles, Ctrl+E runs command)
      - Search/Replace, Goto line, Undo/Redo
      - Files can be opened from "saves/" or absolute/relative paths
      - Key bindings hidden by default; press Ctrl+H to toggle them

    Compile:  gcc -o ced_v4.5 main.c -lncurses
    Run:      ./ced_v4.5
*/

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* Version updated to v4.5 */
#define CED_VERSION "v4.5"

#define MAX_LINES 1000
#define MAX_COLS 1024
/* Bump up line-number area to 8 columns so it doesn't overlap code text. */
#define LINE_NUMBER_WIDTH 8
#define PROMPT_BUFFER_SIZE 256
#define UNDO_STACK_SIZE 100

/* QoL: We'll allow toggling line numbers. Default: show them. */
static int show_line_numbers = 1;

typedef struct Config
{
    int tab_four_spaces;
    int auto_indent;
} Config;
Config config = {1, 1};

char current_file[PROMPT_BUFFER_SIZE] = {0};
int dirty = 0;

#define SH_MAX_LINE_LENGTH 1024
typedef struct SH_SyntaxRule
{
    char **tokens;
    int token_count;
    short color_pair;
    int r, g, b;
} SH_SyntaxRule;

typedef struct SH_SyntaxDefinition
{
    char **extensions;
    int ext_count;
    SH_SyntaxRule *rules;
    int rule_count;
} SH_SyntaxDefinition;

typedef struct SH_SyntaxDefinitions
{
    SH_SyntaxDefinition *definitions;
    int count;
} SH_SyntaxDefinitions;

SH_SyntaxDefinitions global_syntax_defs = {NULL, 0};
SH_SyntaxDefinition *selected_syntax = NULL;
int syntax_enabled = 0;

typedef struct
{
    char *token;
    short color_pair;
} TokenMap;
TokenMap *token_lookup = NULL;
int token_lookup_count = 0;

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

/* Partial redraw tracking */
static int line_dirty[MAX_LINES];
void editor_mark_line_dirty(int line)
{
    if (line >= 0 && line < MAX_LINES)
    {
        line_dirty[line] = 1;
    }
}
void editor_mark_all_lines_dirty(void)
{
    int i;
    for (i = 0; i < MAX_LINES; i++)
    {
        line_dirty[i] = 1;
    }
}

/* Shell Panel */
static int shell_panel_open = 0;
#define SHELL_PANEL_LINES 256
static char shell_output[SHELL_PANEL_LINES][MAX_COLS];
static int shell_output_count = 0;

/* Search & Replace */
static char g_searchTerm[128] = {0};
static int g_searchActive = 0;
static char search_color_pair_defined = 0;
#define SEARCH_COLOR_PAIR 200

/* Toggle help display in status bar */
static int show_help = 0;

/* Forward declarations */
static void editor_prompt(char *prompt, char *buffer, size_t bufsize);

/* ---------- Helper ---------- */
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

/* ---------- Word-Boundary Helpers for Syntax ---------- */
static int is_word_char(char c)
{
    return (isalnum((unsigned char)c) || c == '_');
}

static int is_left_boundary(const char *line, int start)
{
    if (start == 0)
    {
        return 1;
    }
    return !is_word_char(line[start - 1]);
}

static int is_right_boundary(const char *line, int end)
{
    /* 'end' is the index right after the token, so if line[end] is '\0', that's a boundary. */
    if (line[end] == '\0')
    {
        return 1;
    }
    return !is_word_char(line[end]);
}

/* ---------- Load config ---------- */
void load_config(void)
{
    FILE *fp = fopen("settings.config", "r");
    char line[256];
    if (!fp)
    {
        return;
    }
    while (fgets(line, sizeof(line), fp))
    {
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#' || *p == '/')
        {
            continue;
        }
        {
            char key[64], value[64];
            if (sscanf(p, " %63[^=] = %63[^;];", key, value) == 2)
            {
                char *tkey = trim_whitespace(key);
                char *tvalue = trim_whitespace(value);
                if (!strcmp(tkey, "TAB_FOUR_SPACES"))
                {
                    config.tab_four_spaces = (!strcasecmp(tvalue, "true"));
                }
                else if (!strcmp(tkey, "AUTO_INDENT"))
                {
                    config.auto_indent = (!strcasecmp(tvalue, "true"));
                }
            }
        }
    }
    fclose(fp);
}

/* ---------- Editor Init ---------- */
void init_editor(void)
{
    memset(editor.text, 0, sizeof(editor.text));
    editor.num_lines = 1;
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    editor.row_offset = 0;
    editor.col_offset = 0;
    editor_mark_all_lines_dirty();
}

/* ---------- Undo/Redo ---------- */
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
    if (undo_stack_top > 0)
    {
        EditorState st = undo_stack[--undo_stack_top];
        if (redo_stack_top < UNDO_STACK_SIZE)
        {
            EditorState cur;
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
        editor_mark_all_lines_dirty();
    }
}

void redo(void)
{
    if (redo_stack_top > 0)
    {
        EditorState st = redo_stack[--redo_stack_top];
        if (undo_stack_top < UNDO_STACK_SIZE)
        {
            EditorState cur;
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
        editor_mark_all_lines_dirty();
    }
}

/* ---------- Viewport ---------- */
void update_viewport(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int shell_panel_height = shell_panel_open ? 10 : 0;
    if (editor.cursor_y < editor.row_offset)
    {
        editor.row_offset = editor.cursor_y;
        editor_mark_all_lines_dirty();
    }
    else if (editor.cursor_y >= editor.row_offset + (rows - 1 - shell_panel_height))
    {
        editor.row_offset = editor.cursor_y - ((rows - 1 - shell_panel_height) - 1);
        editor_mark_all_lines_dirty();
    }
    {
        int usable = cols - (show_line_numbers ? LINE_NUMBER_WIDTH : 0);
        if (editor.cursor_x < editor.col_offset)
        {
            editor.col_offset = editor.cursor_x;
            editor_mark_all_lines_dirty();
        }
        else if (editor.cursor_x >= editor.col_offset + usable)
        {
            editor.col_offset = editor.cursor_x - usable + 1;
            editor_mark_all_lines_dirty();
        }
    }
}

/* ---------- Syntax ---------- */
typedef struct SH_SyntaxDefinition SH_SyntaxDefinition;
typedef struct SH_SyntaxDefinitions SH_SyntaxDefinitions;

static int sh_count_tokens(const char *line)
{
    int count = 0;
    const char *p = line;
    while ((p = strchr(p, '\"')) != NULL)
    {
        p++;
        {
            char *end_quote = (char *)strchr(p, '\"');
            if (!end_quote)
            {
                break;
            }
        }
        count++;
        p++;
    }
    return count;
}

static int sh_parse_rule_line(char *line, SH_SyntaxRule *rule)
{
    rule->tokens = NULL;
    rule->token_count = 0;
    rule->r = rule->g = rule->b = 0;
    {
        int count = sh_count_tokens(line);
        if (count > 0)
        {
            rule->tokens = (char **)malloc(sizeof(char *) * count);
            rule->token_count = count;
            {
                int idx = 0;
                char *p = line;
                while ((p = strchr(p, '\"')) != NULL && idx < count)
                {
                    char *end_quote;
                    p++;
                    end_quote = strchr(p, '\"');
                    if (!end_quote)
                    {
                        break;
                    }
                    {
                        int len = (int)(end_quote - p);
                        rule->tokens[idx] = (char *)malloc(len + 1);
                        strncpy(rule->tokens[idx], p, len);
                        rule->tokens[idx][len] = '\0';
                    }
                    idx++;
                    p = end_quote + 1;
                }
            }
        }
    }
    {
        char *eq = strchr(line, '=');
        if (!eq)
        {
            return -1;
        }
        {
            char *paren = strchr(eq, '(');
            if (!paren)
            {
                return -1;
            }
            {
                int r, g, b;
                if ((sscanf(paren, " ( %d , %d , %d )", &r, &g, &b) < 3) &&
                    (sscanf(paren, " (%d,%d,%d)", &r, &g, &b) < 3))
                {
                    return -1;
                }
                rule->r = r;
                rule->g = g;
                rule->b = b;
            }
        }
    }
    return 0;
}

static SH_SyntaxDefinitions sh_load_syntax_definitions(const char *filename)
{
    SH_SyntaxDefinitions defs;
    defs.definitions = NULL;
    defs.count = 0;
    {
        FILE *fp = fopen(filename, "r");
        if (!fp)
        {
            return defs;
        }
        {
            char line[SH_MAX_LINE_LENGTH];
            while (fgets(line, sizeof(line), fp))
            {
                char *trimmed = trim_whitespace(line);
                if (!strncmp(trimmed, "SYNTAX", 6))
                {
                    SH_SyntaxDefinition def;
                    def.extensions = NULL;
                    def.ext_count = 0;
                    def.rules = NULL;
                    def.rule_count = 0;
                    {
                        char *p = trimmed + 6;
                        while (*p)
                        {
                            if (*p == '\"')
                            {
                                p++;
                                char *end = strchr(p, '\"');
                                if (!end)
                                {
                                    break;
                                }
                                {
                                    int ext_len = (int)(end - p);
                                    if (ext_len > 0)
                                    {
                                        char *ext = (char *)malloc(ext_len + 1);
                                        strncpy(ext, p, ext_len);
                                        ext[ext_len] = '\0';
                                        def.extensions = (char **)realloc(def.extensions, sizeof(char *) * (def.ext_count + 1));
                                        def.extensions[def.ext_count++] = ext;
                                    }
                                }
                                p = end + 1;
                            }
                            else
                            {
                                p++;
                            }
                        }
                    }
                    {
                        if (!fgets(line, sizeof(line), fp))
                        {
                            break;
                        }
                        trimmed = trim_whitespace(line);
                        if (trimmed[0] != '{')
                        {
                            continue;
                        }
                    }
                    while (1)
                    {
                        char rulebuf[4 * SH_MAX_LINE_LENGTH];
                        rulebuf[0] = '\0';
                        {
                            int done = 0, have_semicolon = 0;
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
                                if (!strlen(trimmed))
                                {
                                    continue;
                                }
                                strncat(rulebuf, trimmed, sizeof(rulebuf) - strlen(rulebuf) - 2);
                                strncat(rulebuf, " ", sizeof(rulebuf) - strlen(rulebuf) - 2);
                                if (strchr(trimmed, ';'))
                                {
                                    have_semicolon = 1;
                                    break;
                                }
                            }
                            if (done)
                            {
                                break;
                            }
                        }
                        {
                            char *rtrim = trim_whitespace(rulebuf);
                            if (!rtrim[0])
                            {
                                continue;
                            }
                            {
                                SH_SyntaxRule rule;
                                if (!sh_parse_rule_line(rtrim, &rule))
                                {
                                    def.rules = (SH_SyntaxRule *)realloc(def.rules, sizeof(SH_SyntaxRule) * (def.rule_count + 1));
                                    def.rules[def.rule_count++] = rule;
                                }
                            }
                        }
                    }
                    defs.definitions = (SH_SyntaxDefinition *)realloc(defs.definitions, sizeof(SH_SyntaxDefinition) * (defs.count + 1));
                    defs.definitions[defs.count++] = def;
                }
            }
        }
        fclose(fp);
    }
    return defs;
}

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

int sh_file_has_extension(const char *filename, SH_SyntaxDefinition def)
{
    int i;
    for (i = 0; i < def.ext_count; i++)
    {
        char *ext = def.extensions[i];
        size_t elen = strlen(ext), flen = strlen(filename);
        if (flen >= elen)
        {
            if (!strcmp(filename + flen - elen, ext))
            {
                return 1;
            }
        }
    }
    return 0;
}

void sh_init_syntax_colors(SH_SyntaxDefinition *def)
{
    short next_color_index = 16, next_pair_index = 1;
    int i;
    for (i = 0; i < def->rule_count; i++)
    {
        SH_SyntaxRule *rule = &def->rules[i];
        short color_num = next_color_index++;
        short pair_idx = next_pair_index++;
        short r_scaled = (short)((rule->r * 1000) / 255);
        short g_scaled = (short)((rule->g * 1000) / 255);
        short b_scaled = (short)((rule->b * 1000) / 255);
        if (can_change_color())
        {
            init_color(color_num, r_scaled, g_scaled, b_scaled);
        }
        init_pair(pair_idx, color_num, -1);
        rule->color_pair = pair_idx;
    }
}

int compare_token_map(const void *a, const void *b)
{
    const TokenMap *tm1 = (const TokenMap *)a;
    const TokenMap *tm2 = (const TokenMap *)b;
    return strcmp(tm1->token, tm2->token);
}

void build_token_lookup(SH_SyntaxDefinition *def)
{
    int i, j, total = 0;
    for (i = 0; i < def->rule_count; i++)
    {
        total += def->rules[i].token_count;
    }
    if (total <= 0)
    {
        token_lookup = NULL;
        token_lookup_count = 0;
        return;
    }
    token_lookup = (TokenMap *)malloc(sizeof(TokenMap) * total);
    token_lookup_count = 0;
    for (i = 0; i < def->rule_count; i++)
    {
        SH_SyntaxRule *r = &def->rules[i];
        for (j = 0; j < r->token_count; j++)
        {
            if (r->tokens[j])
            {
                token_lookup[token_lookup_count].token = r->tokens[j];
                token_lookup[token_lookup_count].color_pair = r->color_pair;
                token_lookup_count++;
            }
        }
    }
    if (token_lookup_count > 0)
    {
        qsort(token_lookup, token_lookup_count, sizeof(TokenMap), compare_token_map);
    }
}

/* ---------- Shell Panel ---------- */
void shell_panel_toggle(void)
{
    shell_panel_open = !shell_panel_open;
    editor_mark_all_lines_dirty();
}

void shell_panel_run_command(void)
{
    char cmd[PROMPT_BUFFER_SIZE];
    editor_prompt("Shell command: ", cmd, sizeof(cmd));
    if (!cmd[0])
    {
        return;
    }
    shell_output_count = 0;
    memset(shell_output, 0, sizeof(shell_output));
    FILE *fp = popen(cmd, "r");
    if (!fp)
    {
        snprintf(shell_output[0], MAX_COLS, "Error running command: %s", strerror(errno));
        shell_output_count = 1;
        return;
    }
    while (fgets(shell_output[shell_output_count], MAX_COLS, fp))
    {
        size_t ln = strlen(shell_output[shell_output_count]);
        if (ln > 0 && shell_output[shell_output_count][ln - 1] == '\n')
        {
            shell_output[shell_output_count][ln - 1] = '\0';
        }
        shell_output_count++;
        if (shell_output_count >= SHELL_PANEL_LINES)
        {
            break;
        }
    }
    pclose(fp);
}

static void shell_panel_draw(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int panel_height = 10;
    int start_line = rows - panel_height;
    mvprintw(start_line, 0, "=== Shell Panel (Ctrl+W to close, Ctrl+E to run cmd) ===");
    {
        int line_in_panel = 1, i;
        for (i = 0; i < shell_output_count && line_in_panel < panel_height; i++, line_in_panel++)
        {
            move(start_line + line_in_panel, 0);
            clrtoeol();
            mvprintw(start_line + line_in_panel, 0, "%s", shell_output[i]);
        }
        while (line_in_panel < panel_height)
        {
            move(start_line + line_in_panel, 0);
            clrtoeol();
            line_in_panel++;
        }
    }
}

/* ---------- Search & Replace ---------- */
void init_search_color(void)
{
    if (!search_color_pair_defined)
    {
        short color_num = 250;
        if (can_change_color())
        {
            init_color(color_num, 1000, 1000, 0);
        }
        init_pair(SEARCH_COLOR_PAIR, COLOR_BLACK, color_num);
        search_color_pair_defined = 1;
    }
}

void editor_search(void)
{
    char term[PROMPT_BUFFER_SIZE];
    editor_prompt("Search term: ", term, sizeof(term));
    if (!term[0])
    {
        g_searchActive = 0;
        g_searchTerm[0] = '\0';
        return;
    }
    strncpy(g_searchTerm, term, sizeof(g_searchTerm) - 1);
    g_searchTerm[sizeof(g_searchTerm) - 1] = '\0';
    g_searchActive = 1;
    editor_mark_all_lines_dirty();
}

void editor_replace_all(void)
{
    char oldstr[PROMPT_BUFFER_SIZE], newstr[PROMPT_BUFFER_SIZE];
    editor_prompt("Old text: ", oldstr, sizeof(oldstr));
    if (!oldstr[0])
    {
        return;
    }
    editor_prompt("New text: ", newstr, sizeof(newstr));
    {
        int i;
        for (i = 0; i < editor.num_lines; i++)
        {
            char *line = editor.text[i];
            char buffer[MAX_COLS * 2], *out = buffer;
            char *start = line;
            size_t oldlen = strlen(oldstr), newlen = strlen(newstr);
            buffer[0] = '\0';
            while (1)
            {
                char *pos = strstr(start, oldstr);
                if (!pos)
                {
                    strncat(out, start, sizeof(buffer) - strlen(out) - 1);
                    break;
                }
                {
                    size_t seg_len = (size_t)(pos - start);
                    strncat(out, start, seg_len);
                }
                strncat(out, newstr, sizeof(buffer) - strlen(out) - 1);
                start = pos + oldlen;
            }
            strncpy(line, buffer, MAX_COLS - 1);
            line[MAX_COLS - 1] = '\0';
            editor_mark_line_dirty(i);
        }
    }
    dirty = 1;
}

/* ---------- Draw line ---------- */
static void draw_line(WINDOW *win, int row, int line_idx, int cols)
{
    move(row, 0);
    clrtoeol();

    char *line = editor.text[line_idx];
    int len = (int)strlen(line);
    int j = editor.col_offset;

    /* If line numbers are toggled on, print them. */
    int start_col = 0;
    if (show_line_numbers)
    {
        /* e.g. "   1 | " uses ~7-8 columns. */
        mvwprintw(win, row, 0, "%4d | ", line_idx + 1);
        start_col = LINE_NUMBER_WIDTH; /* e.g. 8 */
    }

    int col = start_col;

    while (j < len && col < cols)
    {
        /* Check if search highlighting applies first. */
        if (g_searchActive && g_searchTerm[0])
        {
            size_t tlen = strlen(g_searchTerm);
            if (j + (int)tlen <= len && strncmp(&line[j], g_searchTerm, tlen) == 0)
            {
                /* Found the search term. */
                init_search_color();
                wattron(win, COLOR_PAIR(SEARCH_COLOR_PAIR));
                for (size_t k = 0; k < tlen && col < cols; k++, col++)
                {
                    mvwaddch(win, row, col, line[j + k]);
                }
                wattroff(win, COLOR_PAIR(SEARCH_COLOR_PAIR));
                j += (int)tlen;
                continue;
            }
        }

        /* If syntax highlighting is enabled, check for tokens with word-boundary. */
        if (syntax_enabled && token_lookup_count > 0)
        {
            int token_matched = 0;
            for (int i = 0; i < token_lookup_count; i++)
            {
                int token_len = (int)strlen(token_lookup[i].token);
                if (token_len > 0 &&
                    j + token_len <= len &&
                    strncmp(&line[j], token_lookup[i].token, token_len) == 0)
                {
                    /* Check word boundaries. */
                    if (is_left_boundary(line, j) && is_right_boundary(line, j + token_len))
                    {
                        /* It's a valid match => highlight. */
                        wattron(win, COLOR_PAIR(token_lookup[i].color_pair));
                        for (int k = 0; k < token_len && col < cols; k++, col++)
                        {
                            mvwaddch(win, row, col, line[j + k]);
                        }
                        wattroff(win, COLOR_PAIR(token_lookup[i].color_pair));
                        j += token_len;
                        token_matched = 1;
                        break;
                    }
                }
            }
            if (token_matched)
            {
                continue;
            }
        }

        /* No special highlight => just print the char. */
        mvwaddch(win, row, col, line[j]);
        col++;
        j++;
    }
}

/* ---------- Status line + partial redraw ---------- */
void editor_refresh_screen(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int shell_panel_height = shell_panel_open ? 10 : 0;
    int text_area_rows = rows - shell_panel_height - 1;

    {
        int i;
        for (i = 0; i < text_area_rows; i++)
        {
            int line_idx = editor.row_offset + i;
            if (line_idx >= 0 && line_idx < editor.num_lines)
            {
                if (line_dirty[line_idx])
                {
                    draw_line(stdscr, i, line_idx, cols);
                    line_dirty[line_idx] = 0;
                }
            }
            else
            {
                /* Clear leftover lines if the file is shorter than the window. */
                move(i, 0);
                clrtoeol();
            }
        }
    }
    {
        int i;
        for (i = editor.num_lines - editor.row_offset; i < text_area_rows; i++)
        {
            if (i >= 0)
            {
                move(i, 0);
                clrtoeol();
            }
        }
    }

    /* Status bar on the last line. */
    {
        int status_row = text_area_rows;
        move(status_row, 0);
        clrtoeol();
        if (!show_help)
        {
            char status[256];
            const char *fname = (current_file[0]) ? current_file : "Untitled";
            snprintf(status, sizeof(status), "[%s] File: %s | Ln: %d, Col: %d%s",
                     CED_VERSION, fname, editor.cursor_y + 1, editor.cursor_x + 1,
                     (dirty ? " [Modified]" : ""));
            mvprintw(status_row, 0, "%s (Press Ctrl+H for help)", status);
        }
        else
        {
            /* Show key bindings including new QoL shortcuts */
            mvprintw(status_row, 0,
                     "[HELP] Ctrl+Q:Quit  Ctrl+S:Save  Ctrl+O:Open  Ctrl+Z:Undo  Ctrl+Y:Redo  "
                     "Ctrl+G:Goto  Ctrl+F:Search  Ctrl+R:Replace  Ctrl+W:ShellPanel  Ctrl+E:ShellCmd  "
                     "Ctrl+H:HideHelp  Ctrl+D:DupLine  Ctrl+K:KillLine  Ctrl+T:ToggleLN  Ctrl+U:Top  Ctrl+L:Bottom");
        }
    }

    if (shell_panel_open)
    {
        shell_panel_draw();
    }

    /* Place cursor where it belongs on screen. */
    {
        int scr_y = editor.cursor_y - editor.row_offset;
        int scr_x = editor.cursor_x - editor.col_offset + (show_line_numbers ? LINE_NUMBER_WIDTH : 0);
        if (scr_y >= 0 && scr_y < text_area_rows)
        {
            move(scr_y, scr_x);
        }
    }
    wnoutrefresh(stdscr);
    doupdate();
}

/* ---------- Editor Ops ---------- */
void editor_insert_char(int ch)
{
    char *line = editor.text[editor.cursor_y];
    int len = (int)strlen(line);
    int i;
    if (len >= MAX_COLS - 1)
    {
        return;
    }
    for (i = len; i >= editor.cursor_x; i--)
    {
        line[i + 1] = line[i];
    }
    line[editor.cursor_x] = (char)ch;
    editor.cursor_x++;
    editor_mark_line_dirty(editor.cursor_y);
}

/* Delete char to the left of the cursor */
void editor_delete_char(void)
{
    if (editor.cursor_x == 0)
    {
        if (editor.cursor_y == 0)
        {
            return;
        }
        else
        {
            int prev_len = (int)strlen(editor.text[editor.cursor_y - 1]);
            strcat(editor.text[editor.cursor_y - 1], editor.text[editor.cursor_y]);
            {
                int i;
                for (i = editor.cursor_y; i < editor.num_lines - 1; i++)
                {
                    strcpy(editor.text[i], editor.text[i + 1]);
                }
            }
            editor.num_lines--;
            editor.cursor_y--;
            editor.cursor_x = prev_len;
            editor_mark_all_lines_dirty();
        }
    }
    else
    {
        char *line = editor.text[editor.cursor_y];
        int len = (int)strlen(line);
        int i;
        for (i = editor.cursor_x - 1; i < len; i++)
        {
            line[i] = line[i + 1];
        }
        editor.cursor_x--;
        editor_mark_line_dirty(editor.cursor_y);
    }
}

/* Delete char at the cursor */
void editor_delete_at_cursor(void)
{
    char *line = editor.text[editor.cursor_y];
    int len = (int)strlen(line);
    if (editor.cursor_x == len)
    {
        /* Join with next line, if any */
        if (editor.cursor_y == editor.num_lines - 1)
        {
            return;
        }
        strcat(line, editor.text[editor.cursor_y + 1]);
        {
            int i;
            for (i = editor.cursor_y + 1; i < editor.num_lines - 1; i++)
            {
                strcpy(editor.text[i], editor.text[i + 1]);
            }
        }
        editor.num_lines--;
        editor_mark_all_lines_dirty();
    }
    else
    {
        int i;
        for (i = editor.cursor_x; i < len; i++)
        {
            line[i] = line[i + 1];
        }
        editor_mark_line_dirty(editor.cursor_y);
    }
}

void editor_insert_newline(void)
{
    if (editor.num_lines >= MAX_LINES)
    {
        return;
    }
    {
        char *line = editor.text[editor.cursor_y];
        int len = (int)strlen(line);
        char remainder[MAX_COLS];
        strcpy(remainder, line + editor.cursor_x);
        line[editor.cursor_x] = '\0';
        {
            int i;
            for (i = editor.num_lines; i > editor.cursor_y + 1; i--)
            {
                strcpy(editor.text[i], editor.text[i - 1]);
            }
        }
        if (config.auto_indent)
        {
            int indent = 0;
            while (line[indent] == ' ' && indent < MAX_COLS - 1)
            {
                indent++;
            }
            {
                char new_line[MAX_COLS];
                memset(new_line, ' ', indent);
                new_line[indent] = '\0';
                strncat(new_line, remainder, MAX_COLS - indent - 1);
                strcpy(editor.text[editor.cursor_y + 1], new_line);
            }
            editor.cursor_x = indent;
        }
        else
        {
            strcpy(editor.text[editor.cursor_y + 1], remainder);
            editor.cursor_x = 0;
        }
        editor.num_lines++;
        editor.cursor_y++;
        editor_mark_all_lines_dirty();
    }
}

/* ---------- QoL: Duplicate current line (Ctrl+D) ---------- */
void editor_duplicate_line(void)
{
    if (editor.num_lines >= MAX_LINES)
    {
        return;
    }
    save_state_undo();
    int y = editor.cursor_y;
    /* shift lines down from the bottom if needed */
    for (int i = editor.num_lines; i > y + 1; i--)
    {
        strcpy(editor.text[i], editor.text[i - 1]);
    }
    strcpy(editor.text[y + 1], editor.text[y]);
    editor.num_lines++;
    editor.cursor_y++;
    editor_mark_all_lines_dirty();
}

/* ---------- QoL: Kill (delete) entire current line (Ctrl+K) ---------- */
void editor_kill_line(void)
{
    if (editor.num_lines == 1 && editor.cursor_y == 0)
    {
        /* If there's only one line, just clear it. */
        editor.text[0][0] = '\0';
        editor.cursor_x = 0;
        editor_mark_line_dirty(0);
        return;
    }
    save_state_undo();
    for (int i = editor.cursor_y; i < editor.num_lines - 1; i++)
    {
        strcpy(editor.text[i], editor.text[i + 1]);
    }
    editor.num_lines--;
    if (editor.cursor_y >= editor.num_lines)
    {
        editor.cursor_y = editor.num_lines - 1;
    }
    if (editor.cursor_y < 0)
    {
        editor.cursor_y = 0;
    }
    editor.cursor_x = 0;
    editor_mark_all_lines_dirty();
}

/* ---------- QoL: Toggle line numbers (Ctrl+T) ---------- */
void editor_toggle_line_numbers(void)
{
    show_line_numbers = !show_line_numbers;
    editor_mark_all_lines_dirty();
}

/* ---------- QoL: Jump to top (Ctrl+U) ---------- */
void editor_goto_top(void)
{
    editor.cursor_y = 0;
    editor.cursor_x = 0;
    editor_mark_all_lines_dirty();
}

/* ---------- QoL: Jump to bottom (Ctrl+L) ---------- */
void editor_goto_bottom(void)
{
    editor.cursor_y = editor.num_lines - 1;
    if (editor.cursor_y < 0)
    {
        editor.cursor_y = 0;
    }
    editor.cursor_x = (int)strlen(editor.text[editor.cursor_y]);
    editor_mark_all_lines_dirty();
}

/* ---------- Editor Prompt ---------- */
static void editor_prompt(char *prompt, char *buffer, size_t bufsize)
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

/* ---------- Goto Line ---------- */
void editor_goto_line(void)
{
    char line_str[PROMPT_BUFFER_SIZE];
    editor_prompt("Goto line: ", line_str, sizeof(line_str));
    if (!line_str[0])
    {
        return;
    }
    {
        int ln = atoi(line_str);
        if (ln < 1)
        {
            ln = 1;
        }
        if (ln > editor.num_lines)
        {
            ln = editor.num_lines;
        }
        editor.cursor_y = ln - 1;
        editor.cursor_x = 0;
        editor_mark_all_lines_dirty();
    }
}

/* ---------- Save File ---------- */
void editor_save_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];
    int i, r, c;
    if (current_file[0])
    {
        strncpy(filename, current_file, PROMPT_BUFFER_SIZE);
    }
    else
    {
        editor_prompt("Save as: ", filename, PROMPT_BUFFER_SIZE);
        if (!filename[0])
        {
            return;
        }
    }

    /* If user typed no slash, assume "saves/filename" */
    if (!strchr(filename, '/'))
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
        snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
        strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
    }
    else
    {
        /* user typed a path with slash => open from that path */
        strncpy(current_file, filename, PROMPT_BUFFER_SIZE);
    }

    {
        FILE *fp = fopen(current_file, "w");
        if (!fp)
        {
            getmaxyx(stdscr, r, c);
            mvprintw(r - 1, 0, "Error opening file: %s", strerror(errno));
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

/* ---------- Load File ---------- */
void editor_load_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];
    char line_buffer[MAX_COLS];
    int i, r, c;
    editor_prompt("Open file: ", filename, PROMPT_BUFFER_SIZE);
    if (!filename[0])
    {
        return;
    }

    /* If user typed no slash, assume "saves/filename" */
    if (!strchr(filename, '/'))
    {
        snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
    }
    else
    {
        strncpy(filepath, filename, PROMPT_BUFFER_SIZE);
    }

    FILE *fp = fopen(filepath, "r");
    if (!fp)
    {
        getmaxyx(stdscr, r, c);
        mvprintw(r - 1, 0, "Error opening: %s", strerror(errno));
        clrtoeol();
        getch();
        return;
    }
    memset(editor.text, 0, sizeof(editor.text));
    editor.num_lines = 0;
    while (fgets(line_buffer, MAX_COLS, fp) && editor.num_lines < MAX_LINES)
    {
        size_t ln = strlen(line_buffer);
        if (ln > 0 && line_buffer[ln - 1] == '\n')
        {
            line_buffer[ln - 1] = '\0';
        }
        strncpy(editor.text[editor.num_lines], line_buffer, MAX_COLS - 1);
        editor.num_lines++;
    }
    fclose(fp);

    /* Ensure there is at least one line */
    if (editor.num_lines == 0)
    {
        editor.num_lines = 1;
        editor.text[0][0] = '\0';
    }

    /* Reset viewport and cursor */
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    editor.row_offset = 0;
    editor.col_offset = 0;

    strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
    dirty = 0;
    syntax_enabled = 0;

    /* Initialize syntax highlighting if applicable */
    for (i = 0; i < global_syntax_defs.count; i++)
    {
        if (sh_file_has_extension(current_file, global_syntax_defs.definitions[i]))
        {
            selected_syntax = &global_syntax_defs.definitions[i];
            syntax_enabled = 1;
            if (selected_syntax->rule_count > 0)
            {
                if (token_lookup)
                {
                    free(token_lookup);
                    token_lookup = NULL;
                    token_lookup_count = 0;
                }
                sh_init_syntax_colors(selected_syntax);
                build_token_lookup(selected_syntax);
            }
            break;
        }
    }

    getmaxyx(stdscr, r, c);
    mvprintw(r - 1, 0, "File loaded from %s. Press any key...", current_file);
    clrtoeol();
    getch();

    editor_mark_all_lines_dirty();
}

/* ---------- Process Key & Mouse ---------- */
void process_keypress(void)
{
    int ch = getch();
    if (ch == KEY_MOUSE)
    {
        MEVENT event;
        if (getmouse(&event) == OK)
        {
            if (event.bstate & BUTTON1_CLICKED)
            {
                int new_y = event.y + editor.row_offset;
                int new_x = new_y >= 0 && new_y < editor.num_lines
                            ? (event.x - (show_line_numbers ? LINE_NUMBER_WIDTH : 0) + editor.col_offset)
                            : 0;
                if (new_y >= editor.num_lines)
                {
                    new_y = editor.num_lines - 1;
                }
                if (new_y < 0)
                {
                    new_y = 0;
                }
                {
                    int ll = (int)strlen(editor.text[new_y]);
                    if (new_x < 0)
                    {
                        new_x = 0;
                    }
                    if (new_x > ll)
                    {
                        new_x = ll;
                    }
                }
                editor.cursor_y = new_y;
                editor.cursor_x = new_x;
                editor_mark_all_lines_dirty();
            }
            else if (event.bstate & BUTTON4_PRESSED)
            {
                editor.cursor_y -= 3;
                if (editor.cursor_y < 0)
                {
                    editor.cursor_y = 0;
                }
                editor_mark_all_lines_dirty();
            }
            else if (event.bstate & BUTTON5_PRESSED)
            {
                editor.cursor_y += 3;
                if (editor.cursor_y >= editor.num_lines)
                {
                    editor.cursor_y = editor.num_lines - 1;
                }
                editor_mark_all_lines_dirty();
            }
        }
        return;
    }

    switch (ch)
    {
        case 8: /* Ctrl+H: toggle help */
            show_help = !show_help;
            editor_mark_all_lines_dirty();
            break;
        case 23: /* Ctrl+W: shell panel toggle */
            shell_panel_toggle();
            break;
        case 5: /* Ctrl+E: run command in shell panel */
            shell_panel_run_command();
            break;
        case 6: /* Ctrl+F: search */
            editor_search();
            break;
        case 18: /* Ctrl+R: replace */
            editor_replace_all();
            editor_mark_all_lines_dirty();
            break;
        case 7: /* Ctrl+G: goto line */
            editor_goto_line();
            break;
        case 17: /* Ctrl+Q: quit */
            endwin();
            exit(0);
            break;
        case 26: /* Ctrl+Z: undo */
            undo();
            break;
        case 25: /* Ctrl+Y: redo */
            redo();
            break;
        case 19: /* Ctrl+S: save */
            editor_save_file();
            break;
        case 15: /* Ctrl+O: open */
            editor_load_file();
            break;
        case 4: /* Ctrl+D: Duplicate line */
            editor_duplicate_line();
            break;
        case 11: /* Ctrl+K: Kill line */
            editor_kill_line();
            break;
        case 20: /* Ctrl+T: Toggle line numbers */
            editor_toggle_line_numbers();
            break;
        case 21: /* Ctrl+U: goto top */
            editor_goto_top();
            break;
        case 12: /* Ctrl+L: goto bottom */
            editor_goto_bottom();
            break;
        case KEY_HOME:
            editor.cursor_x = 0;
            editor_mark_line_dirty(editor.cursor_y);
            break;
        case KEY_END:
        {
            int ln = (int)strlen(editor.text[editor.cursor_y]);
            editor.cursor_x = ln;
            editor_mark_line_dirty(editor.cursor_y);
            break;
        }
        case KEY_PPAGE:
            editor.cursor_y -= 5;
            if (editor.cursor_y < 0)
            {
                editor.cursor_y = 0;
            }
            editor_mark_all_lines_dirty();
            break;
        case KEY_NPAGE:
            editor.cursor_y += 5;
            if (editor.cursor_y >= editor.num_lines)
            {
                editor.cursor_y = editor.num_lines - 1;
            }
            editor_mark_all_lines_dirty();
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
                editor_mark_line_dirty(editor.cursor_y);
            }
            else if (editor.cursor_y > 0)
            {
                editor.cursor_y--;
                editor.cursor_x = (int)strlen(editor.text[editor.cursor_y]);
                editor_mark_all_lines_dirty();
            }
            break;
        case KEY_RIGHT:
        {
            int length = (int)strlen(editor.text[editor.cursor_y]);
            if (editor.cursor_x < length)
            {
                editor.cursor_x++;
                editor_mark_line_dirty(editor.cursor_y);
            }
            else if (editor.cursor_y < editor.num_lines - 1)
            {
                editor.cursor_y++;
                editor.cursor_x = 0;
                editor_mark_all_lines_dirty();
            }
            break;
        }
        case KEY_UP:
            if (editor.cursor_y > 0)
            {
                editor.cursor_y--;
                {
                    int ll = (int)strlen(editor.text[editor.cursor_y]);
                    if (editor.cursor_x > ll)
                    {
                        editor.cursor_x = ll;
                    }
                }
                editor_mark_all_lines_dirty();
            }
            break;
        case KEY_DOWN:
            if (editor.cursor_y < editor.num_lines - 1)
            {
                editor.cursor_y++;
                {
                    int ll = (int)strlen(editor.text[editor.cursor_y]);
                    if (editor.cursor_x > ll)
                    {
                        editor.cursor_x = ll;
                    }
                }
                editor_mark_all_lines_dirty();
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
            /* Printable ASCII? Insert it. */
            if (ch >= 32 && ch <= 126)
            {
                save_state_undo();
                editor_insert_char(ch);
            }
            break;
    }
}

/* ---------- Main ---------- */
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
