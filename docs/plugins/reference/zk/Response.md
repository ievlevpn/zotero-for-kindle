[Plugin API v0](../README.md) / [zk](../zk.md) / Response

# Interface: Response

The answer to [zk.fetch](fetch.md). The body is read whole (it is capped), and can be read again.

## Properties

| Property | Modifier | Type | Description |
| ------ | ------ | ------ | ------ |
| <a id="headers"></a> `headers` | `readonly` | [`Headers`](Headers.md) | Response headers, as in the web's `Headers`: `res.headers.get("content-type")`. |
| <a id="ok"></a> `ok` | `readonly` | `boolean` | `true` for status 200–299. |
| <a id="status"></a> `status` | `readonly` | `number` | The HTTP status, e.g. `200`, `404`. |
| <a id="url"></a> `url` | `readonly` | `string` | The URL answered, after redirects. |

## Methods

### bytes()

```ts
bytes(): Promise<Uint8Array<ArrayBufferLike>>;
```

The body as bytes.

#### Returns

`Promise`\<`Uint8Array`\<`ArrayBufferLike`\>\>

#### Example

```ts
// a Latin-1 feed
const xml = Array.from(await res.bytes(), (b) => String.fromCharCode(b)).join("");
```

***

### json()

```ts
json<T>(): Promise<T>;
```

The body parsed as JSON. `T` is not checked. Rejects if it isn't JSON.

#### Type Parameters

| Type Parameter | Default type |
| ------ | ------ |
| <a id="jsont"></a> `T` | [`Json`](Json.md) |

#### Returns

`Promise`\<`T`\>

***

### text()

```ts
text(): Promise<string>;
```

The body as text, decoded as UTF-8 whatever the response's charset (as on the web); invalid
bytes become U+FFFD. For another encoding, decode [bytes](#bytes) yourself.

#### Returns

`Promise`\<`string`\>
