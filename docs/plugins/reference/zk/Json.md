[Plugin API v0](../README.md) / [zk](../zk.md) / Json

# Type Alias: Json

```ts
type Json = 
  | string
  | number
  | boolean
  | null
  | Json[]
  | {
[key: string]: Json;
};
```

A JSON value.
