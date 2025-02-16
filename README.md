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

## Acknowledgements
- **[ncurses](https://invisible-island.net/ncurses/)**

## License
### This project is licensed unde MIT License. Check [LICENSE]() for more.

## Notes
### Syntax highlighting
- Even though this uses the same *highlight.syntax* file from my previous [text editor](https://github.com/Zank613/simple_editor) they use a slightly different parser.

### [Previous editor](https://github.com/Zank613/simple_editor)
- This editor is very similar to the previous editor, this just only a true single file and made in ANSI C.