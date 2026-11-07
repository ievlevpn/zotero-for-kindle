# Fast Font

Fast Font by Born2Root (MIT, see LICENSE): bold word beginnings for faster reading, done with the
OpenType "calt" feature (the app turns on HarfBuzz shaping while one of these is chosen).

The original repository (github.com/Born2Root/Fast-Font) is gone; the files come from forks that
keep its licence, downloaded 2026-11-07:

- Fast Serif, Fast Sans (Regular, Bold, Italic, Bold Italic): github.com/EnterOFF/Fast-Font,
  `fast-fonts-for-kindle/Fast_Serif/` and `Fast_Sans/` (`Fast_*_Regular.otf`, `_Bold`, `_Italic`, `_BoldItalic`)
- Fast Mono (Regular only; the engine makes bold and italic): github.com/chelaxian/Fast-Font, `Fast_Mono.ttf`
- LICENSE: github.com/chelaxian/Fast-Font

The family names were made one readable name per family ("Fast Serif", "Fast Sans", "Fast Mono";
the files said "FastSerif", "Fast_Serif", "Fast_Mono") with `tools/rename-fast-font.py`:

    uv run --with fonttools python tools/rename-fast-font.py Fast_Serif_Bold.otf FastFont-Serif-Bold.otf "Fast Serif" Bold
