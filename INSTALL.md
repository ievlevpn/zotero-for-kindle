# Installing Zotero for Kindle by hand

You need a jailbroken Kindle that runs scriptlets (see the [README](README.md#requirements)) and a USB cable.
This takes about five minutes.

## 1. Download

Download **[zotero-for-kindle.zip](https://github.com/ievlevpn/zotero-for-kindle/releases/latest/download/zotero-for-kindle.zip)**
and unzip it. You get two folders, `zotero` and `documents`.

## 2. Copy them onto the Kindle

Connect the Kindle by USB.

- **It shows up as a drive** (Windows, Linux, older Kindles on a Mac): open it.
- **It doesn't show up on a Mac** (newer Kindles, such as the Paperwhite 5 and the Scribe, use MTP): install
  [OpenMTP](https://openmtp.ganeshrvel.com) and open the Kindle there. Quit Android File Transfer and Image Capture
  first if they're running.

Copy both folders into the top level of the Kindle, next to its existing `documents` folder.
When asked, **merge** them: don't replace the Kindle's `documents` folder, only add `Zotero.sh` to it.

You end up with:

    Kindle/
      zotero/            the app
      documents/
        Zotero.sh        its launcher (other books stay as they were)

Eject the Kindle and unplug it.

## 3. Start it and sign in

Open the Kindle library and tap **Zotero**. It has a Zotero cover. The first screen asks for:

- **Zotero user ID and API key.** Create a key at [zotero.org/settings/keys](https://www.zotero.org/settings/keys)
  with *Allow library access* and *Allow write access*. Write access lets highlights made on the Kindle go back to
  Zotero. Your user ID is shown at the top of that page ("Your user ID for use in API calls is …").
- **WebDAV URL, user and password.** These are the same as in Zotero desktop, under *Settings → Sync → File Syncing*.
  The address can be entered either with or without the trailing `zotero/`.

Tap **Connect**. The first sync of a big library takes a few minutes.

### Or put the account in a file

To avoid typing on the Kindle, save this as `zotero/config.json` on the Kindle before step 3:

```json
{
  "userId": 1234567,
  "apiKey": "your API key",
  "webdavUrl": "https://dav.example.com/zotero/",
  "webdavUser": "your WebDAV user",
  "webdavPassword": "your WebDAV password"
}
```

The app reads it at the next start and stores it in a protected place inside the Kindle's system storage. It then
deletes the file from the USB drive.

## Updating

Copy the new `zotero` and `documents` folders the same way, merging and replacing files. Your library, downloads and
account stay.

## Uninstalling

Delete the `zotero` folder and `documents/Zotero.sh`.

## If something goes wrong

- **No Zotero item in the library:** the jailbreak doesn't run scriptlets. Install the Universal Hotfix and restart the Kindle.
- **The app stops responding:** it closes itself within 15 seconds and returns to the Kindle home. Then start it
  again.
- **Anything else:** the log is in `zotero/log/zotero.log` on the USB drive. It never contains passwords or keys.
  Attach it to a [GitHub issue](https://github.com/ievlevpn/zotero-for-kindle/issues).
