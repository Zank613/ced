/* ced - A single-file text editor with syntax highlighting using ncurses.
   - POSIX calls for mkdir/stat
   - Syntax rules loaded from an external "highlight.syntax" file
   - Optionally loads settings from "settings.config"
   - Undo/Redo, mouse support, embedded terminal access (Ctrl+T), etc.

   Compile:  gcc -o ced main.c -lncurses
   Run:      ./ced
*/

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>   /* for mkdir/stat */
#include <sys/types.h>  /* for mode_t, etc. */
#include <errno.h>      /* for errno, strerror */

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

/* We'll store the global definitions, pick one for the loaded file. */
SH_SyntaxDefinitions global_syntax_defs = { NULL, 0 };
SH_SyntaxDefinition *selected_syntax = NULL;
int syntax_enabled = 0;  /* 1 if highlighting is active */

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

static inline char *trim_whitespace(char *str)
{
    while (isspace((unsigned char)*str))
        str++;
    if (*str == '\0')
        return str;
    char *end = str + strlen(str) - 1;
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
    FILE *fp = fopen("settings.config", "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        char *p = trim_whitespace(line);
        if (*p == '\0' || *p == '#' || *p == '/')
            continue;
        char key[64], value[64];
        if (sscanf(p, " %63[^=] = %63[^;];", key, value) == 2)
        {
            char *tkey = trim_whitespace(key);
            char *tvalue = trim_whitespace(value);
            if (strcmp(tkey, "TAB_FOUR_SPACES") == 0)
                config.tab_four_spaces = (strcmp(tvalue, "TRUE") == 0 || strcmp(tvalue, "true") == 0);
            else if (strcmp(tkey, "AUTO_INDENT") == 0)
                config.auto_indent = (strcmp(tvalue, "TRUE") == 0 || strcmp(tvalue, "true") == 0);
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
    if (undo_stack_top < UNDO_STACK_SIZE)
    {
        EditorState st;
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
    }
}

/* --------------- Viewport --------------- */

void update_viewport(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    /* vertical scroll */
    if (editor.cursor_y < editor.row_offset)
        editor.row_offset = editor.cursor_y;
    else if (editor.cursor_y >= editor.row_offset + (rows - 1))
        editor.row_offset = editor.cursor_y - (rows - 2);

    /* horizontal scroll */
    int usable = cols - LINE_NUMBER_WIDTH;
    if (editor.cursor_x < editor.col_offset)
        editor.col_offset = editor.cursor_x;
    else if (editor.cursor_x >= editor.col_offset + usable)
        editor.col_offset = editor.cursor_x - usable + 1;
}

/* --------------- Syntax Highlighter Implementation --------------- */

/* Trim, parse, etc. We'll embed the entire logic that was in syntax_highlighter.h. */

/* Pre-scan for tokens in a rule line. */
static inline int sh_count_tokens(const char *line)
{
    int count = 0;
    const char *p = line;
    while ((p = strchr(p, '\"')) != NULL)
    {
        p++;
        char *end_quote = strchr(p, '\"');
        if (!end_quote)
            break;
        count++;
        p = end_quote + 1;
    }
    return count;
}

/* Parse one rule line: e.g. "int", "double" = (255,0,0); */
static inline int sh_parse_rule_line(char *line, SH_SyntaxRule *rule)
{
    rule->tokens = NULL;
    rule->token_count = 0;
    rule->r = rule->g = rule->b = 0;

    int count = sh_count_tokens(line);
    if (count > 0)
    {
        rule->tokens = (char **)malloc(sizeof(char*) * count);
        rule->token_count = count;
        int idx = 0;
        char *p = line;
        while ((p = strchr(p, '\"')) != NULL && idx < count)
        {
            p++;
            char *end_quote = strchr(p, '\"');
            if (!end_quote)
                break;
            int len = end_quote - p;
            char *tok = (char*)malloc(len+1);
            strncpy(tok, p, len);
            tok[len] = '\0';
            rule->tokens[idx++] = tok;
            p = end_quote + 1;
        }
    }
    /* find =, then ( for RGB */
    char *eq = strchr(line, '=');
    if (!eq) return -1;
    char *paren = strchr(eq, '(');
    if (!paren) return -1;
    int r, g, b;
    if (sscanf(paren, " ( %d , %d , %d )", &r, &g, &b) < 3 &&
        sscanf(paren, " (%d,%d,%d)", &r, &g, &b) < 3)
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
static inline SH_SyntaxDefinitions sh_load_syntax_definitions(const char *filename)
{
    SH_SyntaxDefinitions defs;
    defs.definitions = NULL;
    defs.count = 0;

    FILE *fp = fopen(filename, "r");
    if (!fp)
    {
        /* Not fatal, but no syntax definitions loaded. */
        return defs;
    }

    char line[SH_MAX_LINE_LENGTH];

    while (fgets(line, sizeof(line), fp))
    {
        char *trimmed = trim_whitespace(line);

        /* Look for "SYNTAX" to start a new definition. */
        if (strncmp(trimmed, "SYNTAX", 6) == 0)
        {
            /* Initialize a new definition. */
            SH_SyntaxDefinition def;
            def.extensions = NULL;
            def.ext_count = 0;
            def.rules = NULL;
            def.rule_count = 0;

            /* Parse extension line, e.g. SYNTAX ".c" && ".h" */
            char *p = trimmed + 6; /* skip "SYNTAX" */
            while (*p)
            {
                if (*p == '\"')
                {
                    p++;
                    char *end_quote = strchr(p, '\"');
                    if (!end_quote) break;
                    int len = end_quote - p;
                    char *ext = (char*)malloc(len + 1);
                    strncpy(ext, p, len);
                    ext[len] = '\0';

                    def.extensions = (char**)realloc(def.extensions, sizeof(char*)*(def.ext_count+1));
                    def.extensions[def.ext_count++] = ext;
                    p = end_quote + 1;
                }
                else p++;
            }

            /* Next line should be '{' */
            if (!fgets(line, sizeof(line), fp)) break;
            trimmed = trim_whitespace(line);
            if (trimmed[0] != '{')
            {
                /* No '{'? skip this def. */
                continue;
            }

            /* Read rules until we hit '}' */
            while (1)
            {
                /* We'll accumulate multiple lines until we find a semicolon or '}' */
                char rulebuf[4 * SH_MAX_LINE_LENGTH];
                rulebuf[0] = '\0';

                int done = 0; /* indicates '}' or EOF or parse error */
                int have_semicolon = 0; /* indicates we found the ending ';' */

                while (!have_semicolon)
                {
                    if (!fgets(line, sizeof(line), fp))
                    {
                        /* EOF or error - break out */
                        done = 1;
                        break;
                    }
                    char *t = trim_whitespace(line);

                    /* If this line is '}', we've ended the rules for this def. */
                    if (t[0] == '}')
                    {
                        done = 1;
                        break;
                    }

                    /* If it's empty, skip. */
                    if (strlen(t) == 0) continue;

                    /* Append this line to rulebuf. */
                    /* (Add a space or so in between to handle newlines gracefully.) */
                    strncat(rulebuf, t, sizeof(rulebuf) - strlen(rulebuf) - 2);
                    strncat(rulebuf, " ", sizeof(rulebuf) - strlen(rulebuf) - 2);

                    /* Check if it ends with a semicolon. */
                    if (strchr(t, ';') != NULL)
                    {
                        have_semicolon = 1;
                        break;
                    }
                } /* end while !have_semicolon */

                if (done) /* e.g. we saw '}' or EOF */
                    break;

                /* Now parse rulebuf if it's not empty. */
                char *rtrim = trim_whitespace(rulebuf);
                if (rtrim[0] == '\0')
                    continue; /* skip empty */

                /* We have a full rule line that ends with ';' - parse it. */
                SH_SyntaxRule rule;
                if (sh_parse_rule_line(rtrim, &rule) == 0)
                {
                    def.rules = (SH_SyntaxRule*)realloc(def.rules, sizeof(SH_SyntaxRule)*(def.rule_count+1));
                    def.rules[def.rule_count++] = rule;
                }
                /* else parse error, skip. */
            }

            /* Now we store def in defs array. */
            defs.definitions = (SH_SyntaxDefinition*)realloc(defs.definitions, sizeof(SH_SyntaxDefinition)*(defs.count+1));
            defs.definitions[defs.count++] = def;
        }
    }

    fclose(fp);
    return defs;
}


/* free memory from definitions. */
void sh_free_syntax_definitions(SH_SyntaxDefinitions defs)
{
    for (int i=0; i<defs.count; i++)
    {
        SH_SyntaxDefinition d = defs.definitions[i];
        for (int j=0; j<d.ext_count; j++)
            free(d.extensions[j]);
        free(d.extensions);
        for (int k=0; k<d.rule_count; k++)
        {
            SH_SyntaxRule rr = d.rules[k];
            for (int t=0; t<rr.token_count; t++)
                free(rr.tokens[t]);
            free(rr.tokens);
        }
        free(d.rules);
    }
    free(defs.definitions);
}

/* check extension. */
int sh_file_has_extension(const char *filename, SH_SyntaxDefinition def)
{
    for (int i=0; i<def.ext_count; i++)
    {
        char *ext = def.extensions[i];
        size_t elen = strlen(ext);
        size_t flen = strlen(filename);
        if (flen >= elen)
        {
            if (strcmp(filename+flen-elen, ext)==0)
                return 1;
        }
    }
    return 0;
}

/* init colors for each rule. */
void sh_init_syntax_colors(SH_SyntaxDefinition *def)
{
    short next_color_index = 16;
    short next_pair_index = 1;
    for (int i=0; i<def->rule_count; i++)
    {
        SH_SyntaxRule *rule = &def->rules[i];
        short color_num = next_color_index++;
        short pair_idx = next_pair_index++;
        short r_scaled = (rule->r * 1000)/255;
        short g_scaled = (rule->g * 1000)/255;
        short b_scaled = (rule->b * 1000)/255;
        if (can_change_color())
            init_color(color_num, r_scaled, g_scaled, b_scaled);
        init_pair(pair_idx, color_num, -1);
        rule->color_pair = pair_idx;
    }
}

/* --------------- Editor Screen Refresh & Syntax Drawing --------------- */

static inline void draw_text(WINDOW *win)
{
    int rows, cols;
    getmaxyx(win, rows, cols);
    if (syntax_enabled && selected_syntax)
    {
        /* We do a quick inline highlight of in-memory text. */
        int row = 0;
        for (int i = editor.row_offset; i < editor.num_lines && row < rows - 1; i++, row++)
        {
            mvwprintw(win, row, 0, "%4d |", i + 1);
            char *line = editor.text[i];
            int len = (int)strlen(line);
            int col = LINE_NUMBER_WIDTH;
            int j = editor.col_offset;
            while (j < len && col < cols)
            {
                if (isalpha(line[j]) || line[j]=='_')
                {
                    int start=j;
                    while (j<len && (isalnum(line[j])||line[j]=='_'))
                        j++;
                    int wlen = j - start;
                    /* Make a local copy. */
                    char word[128];
                    if (wlen < 128)
                    {
                        strncpy(word, line+start, wlen);
                        word[wlen] = '\0';
                    }
                    else
                        word[0] = '\0';
                    int highlighted=0;
                    for (int r=0; r<selected_syntax->rule_count; r++)
                    {
                        SH_SyntaxRule rule = selected_syntax->rules[r];
                        for (int t=0; t<rule.token_count; t++)
                        {
                            if (strcmp(word, rule.tokens[t])==0)
                            {
                                wattron(win, COLOR_PAIR(rule.color_pair));
                                for (int k=0; k<wlen && col<cols; k++, col++)
                                    mvwaddch(win, row, col, word[k]);
                                wattroff(win, COLOR_PAIR(rule.color_pair));
                                highlighted=1;
                                break;
                            }
                        }
                        if (highlighted) break;
                    }
                    if (!highlighted)
                    {
                        for (int k=0; k<wlen && col<cols; k++, col++)
                            mvwaddch(win, row, col, word[k]);
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
    else
    {
        /* no syntax: just print raw lines. */
        for (int i=editor.row_offset; i<editor.num_lines && i<editor.row_offset+rows-1; i++)
        {
            int scr_row = i - editor.row_offset;
            mvwprintw(win, scr_row, 0, "%4d |", i + 1);
            int usable = cols - LINE_NUMBER_WIDTH;
            mvwprintw(win, scr_row, LINE_NUMBER_WIDTH, "%.*s", usable, editor.text[i] + editor.col_offset);
        }
    }
}

/* Show the editor plus status bar. */
void editor_refresh_screen(void)
{
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    update_viewport();
    draw_text(stdscr);

    char status[256];
    const char *fname = (current_file[0] != '\0') ? current_file : "Untitled";
    snprintf(status, sizeof(status), "File: %s | Ln: %d, Col: %d%s",
             fname,
             editor.cursor_y+1,
             editor.cursor_x+1,
             (dirty ? " [Modified]" : ""));
    mvprintw(rows-1, 0, "%s  (Ctrl+Q: Quit, Ctrl+S: Save, Ctrl+O: Open, Ctrl+Z: Undo, Ctrl+Y: Redo, Ctrl+T: Terminal, Home/End, PgUp/PgDn, Mouse)", status);

    int scr_y = editor.cursor_y - editor.row_offset;
    int scr_x = editor.cursor_x - editor.col_offset + LINE_NUMBER_WIDTH;
    move(scr_y, scr_x);
    wnoutrefresh(stdscr);
    doupdate();
}

/* --------------- Editor Operations --------------- */

void editor_insert_char(int ch)
{
    char *line = editor.text[editor.cursor_y];
    int len = (int)strlen(line);
    if (len >= MAX_COLS -1) return;
    for (int i=len; i>=editor.cursor_x; i--)
        line[i+1] = line[i];
    line[editor.cursor_x] = ch;
    editor.cursor_x++;
}

void editor_delete_char(void)
{
    if (editor.cursor_x==0)
    {
        if (editor.cursor_y==0) return;
        int prev_len = (int)strlen(editor.text[editor.cursor_y-1]);
        strcat(editor.text[editor.cursor_y-1], editor.text[editor.cursor_y]);
        for (int i=editor.cursor_y; i<editor.num_lines-1; i++)
            strcpy(editor.text[i], editor.text[i+1]);
        editor.num_lines--;
        editor.cursor_y--;
        editor.cursor_x = prev_len;
    }
    else
    {
        char *line = editor.text[editor.cursor_y];
        int len = (int)strlen(line);
        for (int i=editor.cursor_x-1; i<len; i++)
            line[i] = line[i+1];
        editor.cursor_x--;
    }
}

void editor_delete_at_cursor(void)
{
    char *line = editor.text[editor.cursor_y];
    int len = (int)strlen(line);
    if (editor.cursor_x == len)
    {
        if (editor.cursor_y == editor.num_lines-1)
            return;
        strcat(line, editor.text[editor.cursor_y+1]);
        for (int i=editor.cursor_y+1; i<editor.num_lines-1; i++)
            strcpy(editor.text[i], editor.text[i+1]);
        editor.num_lines--;
    }
    else
    {
        for (int i=editor.cursor_x; i<len; i++)
            line[i] = line[i+1];
    }
}

void editor_insert_newline(void)
{
    if (editor.num_lines >= MAX_LINES) return;
    char *line = editor.text[editor.cursor_y];
    int len = (int)strlen(line);
    char remainder[MAX_COLS];
    strcpy(remainder, line+editor.cursor_x);
    line[editor.cursor_x] = '\0';
    for (int i=editor.num_lines; i>editor.cursor_y+1; i--)
        strcpy(editor.text[i], editor.text[i-1]);
    if (config.auto_indent)
    {
        int indent=0;
        while (line[indent]==' ' && indent<MAX_COLS-1)
            indent++;
        char new_line[MAX_COLS]={0};
        memset(new_line, ' ', indent);
        new_line[indent] = '\0';
        strncat(new_line, remainder, MAX_COLS - indent -1);
        strcpy(editor.text[editor.cursor_y+1], new_line);
        editor.cursor_x = indent;
    }
    else
    {
        strcpy(editor.text[editor.cursor_y+1], remainder);
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
    move(rows-1, 0);
    clrtoeol();
    mvprintw(rows-1, 0, "%s", prompt);
    echo();
    curs_set(1);
    getnstr(buffer, (int)bufsize-1);
    noecho();
    curs_set(1);
}

/* --------------- Editor Save File --------------- */

void editor_save_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];

    /* If we already have a file name, use it. Otherwise, prompt. */
    if (current_file[0] != '\0')
    {
        strncpy(filename, current_file, PROMPT_BUFFER_SIZE);
    }
    else
    {
        editor_prompt("Save as: ", filename, PROMPT_BUFFER_SIZE);
        if (filename[0] == '\0') return;
        /* optionally create a "saves" directory if you like */
        struct stat stt;
        if (stat("saves",&stt)==-1)
        {
            if (mkdir("saves", 0777)==-1)
            {
                int r,c; getmaxyx(stdscr,r,c);
                mvprintw(r-1,0,"Error creating 'saves' dir: %s", strerror(errno));
                getch();
                return;
            }
        }
        snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
        strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
    }

    /* If we didn't set current_file above, do so now. */
    if (current_file[0] == '\0')
    {
        strncpy(current_file, filename, PROMPT_BUFFER_SIZE);
    }

    /* Open for writing. */
    FILE *fp = fopen(current_file, "w");
    if (!fp)
    {
        int r,c; getmaxyx(stdscr,r,c);
        mvprintw(r-1,0,"Error opening file for writing: %s", strerror(errno));
        clrtoeol();
        getch();
        return;
    }
    for (int i=0; i<editor.num_lines; i++)
        fprintf(fp, "%s\n", editor.text[i]);
    fclose(fp);
    dirty=0;
    {
        int r,c; getmaxyx(stdscr,r,c);
        mvprintw(r-1,0,"File saved as %s. Press any key...", current_file);
        clrtoeol();
        getch();
    }
}

/* --------------- Editor Load File --------------- */

void editor_load_file(void)
{
    char filename[PROMPT_BUFFER_SIZE];
    char filepath[PROMPT_BUFFER_SIZE];

    editor_prompt("Open file: ", filename, PROMPT_BUFFER_SIZE);
    if (filename[0]=='\0') return;

    /* optionally 'saves' subdir or direct path. */
    snprintf(filepath, PROMPT_BUFFER_SIZE, "saves/%s", filename);
    FILE *fp = fopen(filepath, "r");
    if (!fp)
    {
        /* if not found in 'saves', try direct? up to you. */
        /* fallback approach: */
        fp = fopen(filename, "r");
        if (!fp)
        {
            int r,c; getmaxyx(stdscr,r,c);
            mvprintw(r-1,0,"Error opening file for reading: %s", strerror(errno));
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
    editor.num_lines=0;

    char line_buffer[MAX_COLS];
    while (fgets(line_buffer, MAX_COLS, fp) && editor.num_lines<MAX_LINES)
    {
        size_t ln = strlen(line_buffer);
        if (ln>0 && line_buffer[ln-1]=='\n')
            line_buffer[ln-1]='\0';
        strncpy(editor.text[editor.num_lines], line_buffer, MAX_COLS-1);
        editor.num_lines++;
    }
    fclose(fp);

    editor.cursor_x=0;
    editor.cursor_y=0;
    strncpy(current_file, filepath, PROMPT_BUFFER_SIZE);
    dirty=0;

    /* check extension for syntax. */
    syntax_enabled=0;
    if (global_syntax_defs.count>0)
    {
        for (int i=0; i<global_syntax_defs.count; i++)
        {
            if (sh_file_has_extension(current_file, global_syntax_defs.definitions[i]))
            {
                selected_syntax = &global_syntax_defs.definitions[i];
                syntax_enabled=1;
                sh_init_syntax_colors(selected_syntax);
                break;
            }
        }
    }

    {
        int r,c; getmaxyx(stdscr,r,c);
        mvprintw(r-1,0,"File loaded from %s. Press any key...", current_file);
        clrtoeol();
        getch();
    }
}

/* --------------- Embedded Terminal (Ctrl+T) --------------- */

void launch_terminal(void)
{
    /* We can use POSIX "def_prog_mode()" / "reset_prog_mode()" calls from ncurses. */
    def_prog_mode();
    endwin();
    /* SHELL environment or fallback to /bin/sh. */
    system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
    reset_prog_mode();
    refresh();
}

/* --------------- Process Key & Mouse --------------- */

void process_keypress(void)
{
    int ch = getch();
    if (ch == KEY_MOUSE)
    {
        MEVENT event;
        if (getmouse(&event)==OK)
        {
            if (event.bstate & BUTTON1_CLICKED)
            {
                int new_y = event.y + editor.row_offset;
                int new_x = (event.x<LINE_NUMBER_WIDTH)?0:(event.x-LINE_NUMBER_WIDTH+editor.col_offset);
                if (new_y>=editor.num_lines) new_y=editor.num_lines-1;
                int ll = (int)strlen(editor.text[new_y]);
                if (new_x>ll) new_x=ll;
                editor.cursor_y = new_y;
                editor.cursor_x = new_x;
            }
            else if (event.bstate & BUTTON4_PRESSED)
            {
                editor.cursor_y -= 3;
                if (editor.cursor_y<0) editor.cursor_y=0;
            }
            else if (event.bstate & BUTTON5_PRESSED)
            {
                editor.cursor_y += 3;
                if (editor.cursor_y>=editor.num_lines) editor.cursor_y=editor.num_lines-1;
            }
        }
        return;
    }

    switch(ch)
    {
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
            editor.cursor_x=0;
            break;
        case KEY_END:
        {
            int ln = (int)strlen(editor.text[editor.cursor_y]);
            editor.cursor_x=ln;
            break;
        }
        case KEY_PPAGE:
            editor.cursor_y-=5;
            if (editor.cursor_y<0) editor.cursor_y=0;
            break;
        case KEY_NPAGE:
            editor.cursor_y+=5;
            if (editor.cursor_y>=editor.num_lines) editor.cursor_y=editor.num_lines-1;
            break;
        case '\t':
            save_state_undo();
            if (config.tab_four_spaces)
            {
                for (int i=0; i<4; i++)
                    editor_insert_char(' ');
            }
            else
            {
                editor_insert_char('\t');
            }
            break;
        case KEY_LEFT:
            if (editor.cursor_x>0)
                editor.cursor_x--;
            else if (editor.cursor_y>0)
            {
                editor.cursor_y--;
                editor.cursor_x = (int)strlen(editor.text[editor.cursor_y]);
            }
            break;
        case KEY_RIGHT:
        {
            int length = (int)strlen(editor.text[editor.cursor_y]);
            if (editor.cursor_x<length)
                editor.cursor_x++;
            else if (editor.cursor_y<editor.num_lines-1)
            {
                editor.cursor_y++;
                editor.cursor_x=0;
            }
            break;
        }
        case KEY_UP:
            if (editor.cursor_y>0)
            {
                editor.cursor_y--;
                int ll = (int)strlen(editor.text[editor.cursor_y]);
                if (editor.cursor_x>ll) editor.cursor_x=ll;
            }
            break;
        case KEY_DOWN:
            if (editor.cursor_y<editor.num_lines-1)
            {
                editor.cursor_y++;
                int ll = (int)strlen(editor.text[editor.cursor_y]);
                if (editor.cursor_x>ll) editor.cursor_x=ll;
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
            if (ch>=32 && ch<=126)
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
    /* Load optional config and syntax definitions. */
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

    /* If we ever exit the loop, cleanup. */
    sh_free_syntax_definitions(global_syntax_defs);
    endwin();
    return 0;
}
