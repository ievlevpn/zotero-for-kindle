[Plugin API v0](../README.md) / [zk](../zk.md) / Storage

# Interface: Storage

The type of [zk.storage](storage-1.md).

## Methods

### delete()

```ts
delete(key): Promise<boolean>;
```

Removes `key`. Resolves `true` if it was there. Queued with the plugin's other storage
writes (see [update](#update)).

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `key` | `string` |

#### Returns

`Promise`\<`boolean`\>

#### Example

```ts
await zk.storage.delete("draft");
```

***

### get()

```ts
get<T>(key): Promise<T | undefined>;
```

The value stored under `key`, or `undefined` if there is none. `T` is what you stored; it is
not checked.

#### Type Parameters

| Type Parameter | Default type |
| ------ | ------ |
| <a id="gett"></a> `T` | [`Json`](Json.md) |

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `key` | `string` |

#### Returns

`Promise`\<`T` \| `undefined`\>

#### Example

```ts
const lastRun = await zk.storage.get<string>("lastRun"); // string | undefined
```

***

### keys()

```ts
keys(): Promise<string[]>;
```

All keys, in no particular order.

#### Returns

`Promise`\<`string`[]\>

#### Example

```ts
for (const key of await zk.storage.keys()) if (key.startsWith("feed:")) console.log(key);
```

***

### set()

```ts
set(key, value): Promise<void>;
```

Stores a JSON value under `key`, replacing what was there. Queued with the plugin's other
storage writes, so writes apply in the order they were asked (see [update](#update)).

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `key` | `string` |
| `value` | `unknown` |

#### Returns

`Promise`\<`void`\>

#### Example

```ts
await zk.storage.set("lastRun", new Date().toISOString());
```

***

### update()

```ts
update<T>(key, fn): Promise<T | undefined>;
```

Reads, changes and writes `key` as one step: the way to do a read-modify-write. `fn` gets
the stored value (`undefined` if there is none) and returns the new one; returning
`undefined` deletes the key. Resolves with what was stored.

All of a plugin's storage writes — `set`, `delete` and `update` — go through one queue (the
app keeps one per plugin) and apply in the order they were asked, one at a time: an
update's `fn` starts only when the writes asked before it have finished, however calls
interleave. So two event handlers updating the same key can't lose each other's change.
`fn` may be `async` and may await other calls (the library, files, `zk.zotero`) — the
writes after it wait, so keep it short. If `fn` throws, or its call is stopped by a limit,
nothing is stored, the update rejects, and the next write runs.

Inside `fn`, write only by returning: calling `update`, `set` or `delete` there (on any
key) would wait for `fn` itself, so it rejects at once with an `Error`
("zk.storage.update can't be called inside another update's function", or `set` /
`delete` for those). `get` and `keys` are fine.

#### Type Parameters

| Type Parameter | Default type |
| ------ | ------ |
| <a id="updatet"></a> `T` | [`Json`](Json.md) |

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `key` | `string` |
| `fn` | (`old`) => `T` \| `Promise`\<`T` \| `undefined`\> \| `undefined` |

#### Returns

`Promise`\<`T` \| `undefined`\>

#### Examples

```ts
// count, safely from any number of handlers at once
await zk.storage.update<number>("finished", (n = 0) => n + 1);
```

```ts
// file a card and remember it, as one step
const deck = await zk.storage.update<Card[]>("deck", async (deck = []) => {
  const [card, ...rest] = deck;
  if (card) await zk.zotero.createItem(fieldsOf(card));
  return rest;
});
```
