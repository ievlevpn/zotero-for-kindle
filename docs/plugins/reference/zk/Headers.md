[Plugin API v0](../README.md) / [zk](../zk.md) / Headers

# Interface: Headers

Response headers: a read-only subset of the web's `Headers`. Names are case-insensitive;
repeated headers are joined with `", "`.

## Example

```ts
const type = res.headers.get("Content-Type") ?? "";
for (const [name, value] of res.headers) console.log(name, value); // names in lower case
```

## Extends

- `Iterable`\<\[`string`, `string`\]\>

## Methods

### \[iterator\]()

```ts
[iterator](): IterableIterator<[string, string]>;
```

#### Returns

`IterableIterator`\<\[`string`, `string`\]\>

#### Overrides

`Iterable.[iterator]`

***

### entries()

```ts
entries(): IterableIterator<[string, string]>;
```

`[name, value]` pairs, names in lower case, sorted by name.

#### Returns

`IterableIterator`\<\[`string`, `string`\]\>

***

### get()

```ts
get(name): string | null;
```

The header's value, or `null` if it is absent.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |

#### Returns

`string` \| `null`

***

### has()

```ts
has(name): boolean;
```

Whether the header is present.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |

#### Returns

`boolean`
