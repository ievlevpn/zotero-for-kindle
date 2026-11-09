# Explain with AI

Select a passage in a book, choose **Explain** in the selection bar, and read Claude's plain
explanation of it in a pop-up.

Selection actions come with part 2: in plugin API v0 the action is recorded but not shown yet.
The plugin is complete; it starts working when the selection bar shows plugin actions.

## What it does

Sends the selected text (up to 4,000 characters) and the book's title and year to Anthropic's
Messages API (`POST https://api.anthropic.com/v1/messages`), and returns the reply's text, which
the app shows in its result pop-up. A missing key or an error from the API is shown as the action
having failed, with the reason.

Nothing else leaves the Kindle: not the rest of the book, not your library.

## Permissions

| Permission | Why |
|---|---|
| `net:api.anthropic.com` | the one host it talks to |

The selection and its book are handed to the action because you chose it, so it needs no
`library:read`.

## Settings

`settings.json`, next to `plugin.json`:

```json
{
  "apiKey": "sk-ant-…",
  "model": "claude-sonnet-5-5",
  "language": "English",
  "maxTokens": 600
}
```

- `apiKey` — your Anthropic API key. Required.
- `model` — default `claude-sonnet-5-5`.
- `language` — the language of the answer; default: the passage's own.
- `maxTokens` — the longest answer; default 600 (the pop-up is small).

The key is stored in plain text on the Kindle, readable by anyone with the device over USB.

## Build

`node build.mjs`, or `sdk/check.sh` for all examples.
