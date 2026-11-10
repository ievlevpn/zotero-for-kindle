[Plugin API v0](../README.md) / [zk](../zk.md) / ActionReply

# Interface: ActionReply

The result pop-up with buttons of the plugin's own, beside **Close**.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="actions"></a> `actions?` | `object`[] | At most 2 buttons; tapping one runs the action again (see [Action.run](Action.md#run)). |
| <a id="text"></a> `text` | `string` | What the pop-up shows. |
