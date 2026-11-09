[Plugin API v0](README.md) / URLSearchParams

# Class: URLSearchParams

A URL's query string, as on the web. No permission.

## Example

```ts
const q = new URLSearchParams({ search_query: "cat:math.PR", max_results: "50" });
await zk.fetch(`https://export.arxiv.org/api/query?${q}`);
```

## Implements

- `Iterable`\<\[`string`, `string`\]\>

## Constructors

### Constructor

```ts
new URLSearchParams(init?): URLSearchParams;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `init?` | `string` \| `Record`\<`string`, `string`\> \| \[`string`, `string`\][] |

#### Returns

`URLSearchParams`

## Properties

| Property | Modifier | Type |
| ------ | ------ | ------ |
| <a id="size"></a> `size` | `readonly` | `number` |

## Methods

### \[iterator\]()

```ts
[iterator](): IterableIterator<[string, string]>;
```

#### Returns

`IterableIterator`\<\[`string`, `string`\]\>

#### Implementation of

`Iterable.[iterator]`

***

### append()

```ts
append(name, value): void;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |
| `value` | `string` |

#### Returns

`void`

***

### delete()

```ts
delete(name, value?): void;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |
| `value?` | `string` |

#### Returns

`void`

***

### entries()

```ts
entries(): IterableIterator<[string, string]>;
```

#### Returns

`IterableIterator`\<\[`string`, `string`\]\>

***

### forEach()

```ts
forEach(callback): void;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `callback` | (`value`, `name`, `params`) => `void` |

#### Returns

`void`

***

### get()

```ts
get(name): string | null;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |

#### Returns

`string` \| `null`

***

### getAll()

```ts
getAll(name): string[];
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |

#### Returns

`string`[]

***

### has()

```ts
has(name, value?): boolean;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |
| `value?` | `string` |

#### Returns

`boolean`

***

### keys()

```ts
keys(): IterableIterator<string>;
```

#### Returns

`IterableIterator`\<`string`\>

***

### set()

```ts
set(name, value): void;
```

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `name` | `string` |
| `value` | `string` |

#### Returns

`void`

***

### sort()

```ts
sort(): void;
```

#### Returns

`void`

***

### toString()

```ts
toString(): string;
```

The query string, without the leading `?`.

#### Returns

`string`

***

### values()

```ts
values(): IterableIterator<string>;
```

#### Returns

`IterableIterator`\<`string`\>
