# ced 
**Custom terminal based text editor written in ANSI C, ncurses and UNIX headers.**

## Features
- Syntax highlighting.
- Undo/Redo.
- Status bar.
- Under 40KB (~37KB).
- Terminal access from the editor.
- Designed to be compliant with UNIX/POSIX operating systems.
- Single file.

## Prerequisites
- Use UNIX/POSIX based OS. (e.g. Linux)
- Use GCC or any other C compiler.
- Have ncurses installed.

## How to use

### Build it
```bash
gcc -o ced main.c -lncurses
```

### Run it
```bash
./ced
```

## Screenshots
![ced in action](screenshot_1.png)

**your terminal might look different, I use xcfe4 terminal**

## Key Bindings

- Ctrl+Q: Quit
- Ctrl+S: Save
- Ctrl+O: Open
- Ctrl+Z: Undo
- Ctrl+Y: Redo
- Ctrl+T: Launch a shell (embedded terminal)
- Ctrl+G: Goto Line
- Ctrl+F: Search
- Ctrl+R: Replace (prompts the old text and new text, then does a naive replace all in every line)
- Home/End, PgUp/PgDn: Navigation
- Mouse: Click to move cursor, wheel scroll.

## See [Contributing](https://github.com/Zank613/ced/blob/master/CONTRIBUTING.md) for contribution.

## Acknowledgements
- **[ncurses](https://invisible-island.net/ncurses/)**

## License
### ced is licensed under MIT License. Check [LICENSE](https://github.com/Zank613/ced/blob/master/LICENSE) for more.

## Notes
### Syntax highlighting
- Even though this uses the same *highlight.syntax* file from my previous [text editor](https://github.com/Zank613/simple_editor) they use a slightly different parser.

### [Previous editor](https://github.com/Zank613/simple_editor)
- This editor is very similar to the previous editor, this just only a true single file and made in ANSI C.