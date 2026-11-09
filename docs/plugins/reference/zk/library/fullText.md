[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / fullText

# Function: fullText()

```ts
function fullText(attachmentKey): Promise<string>;
```

The text of a downloaded book or paper (PDF, EPUB, …), extracted by the app's own engines.
Rejects if the attachment isn't downloaded to the Kindle (see [Attachment.downloaded](../Attachment.md#downloaded))
or isn't a document, or if its text is larger than 32 MB. Can take a few seconds for a long book.

Format: an EPUB's paragraphs are separated by a blank line (`"\n\n"`), chapters in reading
order. A PDF's pages are in order, separated by a form feed (`"\f"`): `text.split("\f")[0]`
is page 1; lines within a page end in `"\n"`.

**Permission**: `fulltext:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `attachmentKey` | `string` |

## Returns

`Promise`\<`string`\>

## Example

```ts
const text = await zk.library.fullText(pdf.key);
const words = text.split(/\s+/).length;
```
