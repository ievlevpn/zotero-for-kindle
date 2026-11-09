[Plugin API v0](README.md) / URL

# Class: URL

A parsed URL, as on the web (WHATWG URL). No permission.

## Example

```ts
const url = new URL("https://export.arxiv.org/api/query");
url.searchParams.set("id_list", ids.join(","));
const res = await zk.fetch(url.href);
```

## Constructors

### Constructor

```ts
new URL(url, base?): URL;
```

Throws a `TypeError` if `url` (resolved against `base`) isn't a valid URL.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `url` | `string` |
| `base?` | `string` \| `URL` |

#### Returns

`URL`

## Properties

| Property | Modifier | Type |
| ------ | ------ | ------ |
| <a id="hash"></a> `hash` | `public` | `string` |
| <a id="host"></a> `host` | `public` | `string` |
| <a id="hostname"></a> `hostname` | `public` | `string` |
| <a id="href"></a> `href` | `public` | `string` |
| <a id="origin"></a> `origin` | `readonly` | `string` |
| <a id="password"></a> `password` | `public` | `string` |
| <a id="pathname"></a> `pathname` | `public` | `string` |
| <a id="port"></a> `port` | `public` | `string` |
| <a id="protocol"></a> `protocol` | `public` | `string` |
| <a id="search"></a> `search` | `public` | `string` |
| <a id="searchparams"></a> `searchParams` | `readonly` | [`URLSearchParams`](URLSearchParams.md) |
| <a id="username"></a> `username` | `public` | `string` |

## Methods

### toJSON()

```ts
toJSON(): string;
```

#### Returns

`string`

***

### toString()

```ts
toString(): string;
```

#### Returns

`string`

***

### canParse()

```ts
static canParse(url, base?): boolean;
```

Whether `new URL(url, base)` would succeed.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `url` | `string` |
| `base?` | `string` \| `URL` |

#### Returns

`boolean`
