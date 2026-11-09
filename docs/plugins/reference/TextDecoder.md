[Plugin API v0](README.md) / TextDecoder

# Class: TextDecoder

Decodes UTF-8 bytes to a string, as on the web. Only UTF-8 is supported. No permission.

## Example

```ts
const text = new TextDecoder().decode(bytes);
```

## Constructors

### Constructor

```ts
new TextDecoder(label?, options?): TextDecoder;
```

`fatal: true` throws a `TypeError` on invalid bytes instead of inserting U+FFFD.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `label?` | `"utf-8"` \| `"utf8"` |
| `options?` | \{ `fatal?`: `boolean`; `ignoreBOM?`: `boolean`; \} |
| `options.fatal?` | `boolean` |
| `options.ignoreBOM?` | `boolean` |

#### Returns

`TextDecoder`

## Properties

| Property | Modifier | Type |
| ------ | ------ | ------ |
| <a id="encoding"></a> `encoding` | `readonly` | `"utf-8"` |
| <a id="fatal"></a> `fatal` | `readonly` | `boolean` |
| <a id="ignorebom"></a> `ignoreBOM` | `readonly` | `boolean` |

## Methods

### decode()

```ts
decode(input?, options?): string;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `input?` | `ArrayBuffer` \| `ArrayBufferView`\<`ArrayBufferLike`\> |
| `options?` | \{ `stream?`: `boolean`; \} |
| `options.stream?` | `boolean` |

#### Returns

`string`
