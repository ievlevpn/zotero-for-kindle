# Highlights to Markdown

The example plugin that ships with Zotero for Kindle, and the one to read first: `src/main.ts`
is commented as a walk-through of the plugin API.

## What it does

Keeps one Markdown file per book in the plugin's data folder,
`/mnt/us/zotero/plugins/highlights-to-markdown/data/`, with the book's highlights, underlines and
notes in reading order: the quoted text, then your comment and the page.

```markdown
# Random Fields and Geometry

Robert J. Adler, Jonathan Taylor (2007). Citation key: `adler2007`

> A random field is
> a stochastic process

Key definition. — p. 12
```

A file is named after the book's citation key (Zotero's own, or the `Citation Key:` line Better
BibTeX keeps in *Extra*), or after its title. Copy the folder off over USB.

- A highlight you make, change or delete on the Kindle rewrites that book's file at once.
- Highlights that arrive with a sync (made in Zotero on the desktop) are noted; a few books are
  written as the sync finishes, the rest by the background run.
- After a sync, at most once an hour, a background run writes what is noted and catches anything
  missed — changes made while the plugin was off — and, the first time, writes every book in the
  library that has highlights. A large library takes several runs; each carries on where the last
  stopped.
- A book whose last highlight is deleted, or that leaves the library, loses its file; a renamed
  book's file is renamed.
- **Plugins** on Home (the puzzle-piece icon) opens its status: how many books are exported, when
  it last ran, the latest files, and **Export now** to catch up without waiting for the hour.

## Permissions

| Permission | Why |
|---|---|
| `annotations:read` | to hear highlight events and read each book's annotations |
| `library:read` | to read the book's title, authors, year, *Extra* field and attachments |
| `background` | the hourly catch-up run |

Writing its own `data/` folder and keeping its own storage need no permission.

## Settings

None.

## Build

From the SDK (`sdk/`): `sh check.sh` builds every example. On its own: `node build.mjs`, then copy
`dist/highlights-to-markdown/` to `/mnt/us/zotero/plugins/` on the Kindle. Copied out of the SDK,
add the template's `devDependencies` (esbuild, typescript) to `package.json`.
