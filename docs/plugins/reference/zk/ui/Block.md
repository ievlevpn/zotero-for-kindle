[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / Block

# Type Alias: Block

```ts
type Block = 
  | ColumnBlock
  | RowBlock
  | TextBlock
  | CardBlock
  | DeckBlock
  | ListBlock
  | SearchBlock
  | FieldBlock
  | ChipsBlock
  | ButtonBlock
  | ButtonsBlock
  | DividerBlock
  | SpaceBlock
  | NoticeBlock;
```

A building block: what `render` returns, and what layout blocks contain.
