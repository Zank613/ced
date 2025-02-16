# Contributing to ced
## How to Contribute

- Fork the Repository, start by forking the repository and cloning it to your local machine:
    ```bash
    git clone https://github.com/Zank613/ced.git
    cd ced
    ```

- Understand the Goals

    - ced is designed to be:
        - Lightweight: Total size (binary + .syntax file) must remain under 40KB.
        - ANSI C Compatible: Avoid C99 or platform-specific features.
        - Portable: Must work anywhere ncurses is available (Linux, macOS, BSD, Cygwin, etc.).
    - Contributions should align with these goals. If your idea adds significant size or complexity, consider discussing it first in an issue.

## Coding Guidelines

- Single File Only: All code should remain in main.c. This is intentional to keep the project compact.
- Style: Follow these practices:
    - Use Allman style for braces:
        ```c
        if (condition)
        {
            // code
        }
        ```

- Limit line lengths to 80 characters where possible.
- Comment your code where necessary but avoid excessive comments.

- No External Dependencies: Only use ncurses and ANSI C standard libraries (stdio.h, stdlib.h, etc.).