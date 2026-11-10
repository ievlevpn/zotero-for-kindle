# Publishing a plugin

Publishing puts your plugin in **Settings → Plugins → Get plugins**, where people install it with
a tap and get your updates. You keep the plugin in your own GitHub repo. The catalog only lists it.

How it works, in short:

- You sign each release's zip with **your own key**. The app installs a zip only if its signature
  verifies with the public key your registry entry pins. A stolen GitHub account or a swapped zip
  does not pass.
- The list of plugins, the **registry**, is a signed file in
  [`ievlevpn/zotero-for-kindle-plugins`](https://github.com/ievlevpn/zotero-for-kindle-plugins).
  Your first listing is one pull request. After that, updates need no pull request.

You need Node 22 or newer. The tool is [`sdk/zk-plugin.mjs`](../../sdk/zk-plugin.mjs); it needs no
`npm install`.

## 1. Make your key (once)

```sh
node path/to/sdk/zk-plugin.mjs keygen
```

This writes `zk-plugin-key.json` (readable by you only) and prints your **public key**, a base64
string of 32 bytes. It won't overwrite an existing key file.

- The key file is your signature. Anyone who has it can publish as you. **Keep it on your own
  machine, out of git (add `zk-plugin-key.json` to `.gitignore`), and out of CI.** Sign releases
  by hand.
- Back it up. If you lose it, see [Losing your key](#losing-your-key).
- Public keys are safe to share; yours goes in the registry entry.

## 2. Pack and sign a release

In your plugin's folder (made from the [template](../../sdk/template/)):

```sh
node path/to/sdk/zk-plugin.mjs pack     # builds, then dist/plugin.zip
node path/to/sdk/zk-plugin.mjs sign     # dist/plugin.zip.sig, with zk-plugin-key.json
```

`pack` runs your `build.mjs`, then zips `dist/<id>/` so that `plugin.json` is at the root of the
zip. Use `pack --no-build` if you build another way. It refuses a `data/` folder (that is the
user's, and an update never touches it) and any file whose name starts with a dot. The zip is at
most 8 MB, and 32 MB unpacked.

## 3. Tag and release on GitHub

1. Set `"version"` in `plugin.json` (for example `1.2.0`) and rebuild.
2. Create a GitHub release whose **tag is the version**: `v1.2.0` or `1.2.0`. The app reads the
   tag to learn the latest version; when it installs, the version inside `plugin.json` must be the
   same, or the install is refused.
3. Attach two assets: `plugin.zip` and `plugin.zip.sig`, with exactly those names.

The release must be the repo's *latest* release (not a draft or a pre-release). The app also needs
the zip's `api` and `minApp` (in `plugin.json`) to fit the installed app; otherwise it says "It
needs app 0.5.0." and installs nothing.

## 4. Get listed: the registry pull request

Open a pull request on `ievlevpn/zotero-for-kindle-plugins` that adds one entry to the `plugins`
list in `registry.json`:

```json
{ "id": "feed-riffle", "name": "Feed Riffle", "author": "Your Name",
  "description": "One line: what it does.",
  "repo": "you/zk-feed-riffle", "publicKey": "<the key keygen printed>" }
```

- `id` is the id in your `plugin.json`: lowercase letters, digits and dashes.
- `repo` is `owner/name` of your plugin's repo.
- Check the file first: `node registry/check.mjs registry/registry.json --online` (from this
  repo's checkout) also confirms that your latest release's signature verifies with the key.

The maintainer reads the plugin before listing it, and signs the new registry. Listing is a
review of the first version only. Later versions are not reviewed, which is why the key pin matters
and why an update that asks for **more permissions** makes the user say yes again.

## 5. Updating

Publish a new release: raise `version`, `pack`, `sign`, create the release with a higher tag, attach
the two assets. No pull request. Kindles see it at the next catalog check (at most hourly) and show
**Update to 1.3** on your plugin's page.

If your new version asks for more permissions, the user is shown the new ones and decides. Ask for
only what you need; a plugin that adds a permission later is a plugin people decline to update.

## Losing your key

If you lose the key, or think someone has it, nothing already installed breaks, but you can no
longer publish updates that verify. Make a new key (`keygen`) and open a registry pull request that
changes `publicKey` in your entry, saying why. Until it is merged and the registry re-published,
updates you sign with the new key are refused. If the old key may be in someone else's hands, ask
for the plugin to be withdrawn at the same time (see below) until the new key is in.

## Withdrawing a plugin

- **To stop new installs:** a pull request that removes your entry. It hides the plugin from Get
  plugins; Kindles that have it keep it.
- **To switch it off everywhere** (a serious bug, a leaked key): a pull request that adds
  `{ "id": "your-plugin", "reason": "a short, plain sentence" }` to `revoked`. At the next check
  every Kindle turns the plugin off with the note *Turned off — withdrawn from the catalog:
  <reason>*. The plugin's folder is not deleted, since deleting needs the user's yes; they remove
  it from its page if they want to.
