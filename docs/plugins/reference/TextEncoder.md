[Plugin API v0](README.md) / TextEncoder

# Class: TextEncoder

Encodes strings as UTF-8 bytes, as on the web. No permission.

## Example

```ts
const bytes = new TextEncoder().encode(text).length; // size of a request body
```

## Constructors

### Constructor

```ts
new TextEncoder(): TextEncoder;
```

#### Returns

`TextEncoder`

## Properties

| Property | Modifier | Type | Description |
| ------ | ------ | ------ | ------ |
| <a id="encoding"></a> `encoding` | `readonly` | `"utf-8"` | Always `"utf-8"`. |

## Methods

### encode()

```ts
encode(input?): Uint8Array;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `input?` | `string` |

#### Returns

`Uint8Array`
