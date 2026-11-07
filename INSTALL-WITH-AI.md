# Install Zotero for Kindle (instructions for an AI agent)

> **For people:** connect your jailbroken Kindle to the computer by USB, then tell your agent (Claude Code, Codex, …):
> *"Install Zotero for Kindle on my Kindle by following
> https://github.com/ievlevpn/zotero-for-kindle/blob/master/INSTALL-WITH-AI.md"*.
> The manual way is in [INSTALL.md](INSTALL.md).

---

You are installing **Zotero for Kindle**, a native Zotero app, on the user's jailbroken Kindle, which is connected to
this computer by USB. Work step by step and tell the user briefly what you're doing. Ask before anything you're unsure
about.

## Rules

- **Only add files.** Never delete, move or overwrite anything on the Kindle outside `zotero/` and
  `documents/Zotero.sh`. Never format or "replace" the `documents` folder: it holds the user's books.
- **On an update**, keep `zotero/zotero.db`, `zotero/files/`, `zotero/cache/` and `zotero/log/`. Replace only what
  the release contains.
- **Credentials:** the user's Zotero API key and WebDAV password go only into `zotero/config.json` on the Kindle.
  Don't echo them back, log them, or save them anywhere else on this computer. Delete any temporary copy you made.
- Don't install a jailbreak or change system files. If the Kindle isn't jailbroken, stop and point the user to
  https://kindlemodding.org.

## 1. Check the requirements with the user

Ask, in one message:

1. Is the Kindle jailbroken, with scriptlet support (the Universal Hotfix: `.sh` files in `documents/` appear in the
   library)?
2. Do they use **WebDAV** for file syncing in Zotero (Zotero desktop: *Settings → Sync → File Syncing*)? Without it the
   app shows the library and annotations but can't download books. Zotero's own file storage isn't supported yet.
3. Do they want you to set up their account now (you'll need their Zotero user ID, an API key and the WebDAV
   details), or will they type it on the Kindle?

## 2. Get the release

Download https://github.com/ievlevpn/zotero-for-kindle/releases/latest/download/zotero-for-kindle.zip into a new
temporary folder and unzip it. It contains:

    zotero/                 the app: bin/, lib/, share/
    documents/Zotero.sh     the launcher that appears in the Kindle library

## 3. Find the Kindle

- **As a USB drive** (Windows, Linux, older Kindles): a volume named `Kindle` (macOS `/Volumes/Kindle`, Linux
  usually `/media/$USER/Kindle` or `/run/media/$USER/Kindle`, Windows a drive letter). Its top level has a
  `documents` folder.
- **As an MTP device** (newer Kindles such as the Paperwhite 5 and the Scribe):
  - *Linux:* GVFS mounts it (`gio mount -li`; files under `/run/user/$UID/gvfs/mtp:*`).
  - *Windows:* it appears under "This PC". Copy with PowerShell's `Shell.Application` (`Namespace(...).CopyHere`).
  - *macOS:* there's no built-in MTP. Use libmtp (`brew install libmtp`) or ask the user to copy with
    [OpenMTP](https://openmtp.ganeshrvel.com). First quit Android File Transfer, Image Capture and OpenMTP, and run
    `killall ptpcamerad`: macOS's camera daemon grabs the device. Some Kindles drop off USB when an MTP session ends,
    so do the whole copy in **one** session, for example a short program on libmtp. Don't run one `mtp-*` command per
    file. If MTP fails after a couple of tries, don't fight it: walk the user through copying the two folders with
    OpenMTP.

## 4. Copy

Copy `zotero/` to the Kindle's top level, merging into an existing `zotero/` if there is one. Copy `Zotero.sh` into the
existing `documents/` folder. Check afterwards that these exist on the Kindle with sizes matching the zip:
`zotero/bin/zotero`, `zotero/lib/libzk-epub.so`, `zotero/lib/libzk-pdf.so`,
`zotero/share/crengine/hyph/` (50 files), `zotero/share/cacert.pem` and `documents/Zotero.sh`.

## 5. The account (if the user wants you to set it up)

Ask for:

- **Zotero user ID and API key.** The key is made at https://www.zotero.org/settings/keys with *Allow library access*
  and *Allow write access*. Write access is needed for Kindle highlights to reach Zotero. The user ID is shown on that
  page ("Your user ID for use in API calls is …").
- **WebDAV URL, user name and password**, as in Zotero desktop.

Optionally check them before writing anything:

- `curl -s -H "Zotero-API-Key: $KEY" https://api.zotero.org/keys/current` should return JSON whose `userID` equals the
  user ID.
- `curl -s -o /dev/null -w "%{http_code}" -u "$USER:$PASS" -X PROPFIND -H "Depth: 0" <WebDAV URL>/zotero/` should
  print 207. Leave out `zotero/` if the URL already ends with it.

Pass secrets through environment variables or files, not command lines, where you can.

Write `zotero/config.json` on the Kindle (`userId` is a number):

```json
{
  "userId": 1234567,
  "apiKey": "…",
  "webdavUrl": "https://dav.example.com/zotero/",
  "webdavUser": "…",
  "webdavPassword": "…"
}
```

At its next start the app moves this file into protected system storage and deletes it from the USB drive.

## 6. Finish

Eject the Kindle safely (`diskutil eject`, `gio mount -u`, or "Eject" in Windows), then tell the user:

- Unplug the Kindle, open the library and tap **Zotero**. It has a Zotero cover and might take a moment to appear.
- If no account was set up, the first screen asks for the user ID, API key and WebDAV details.
- If "Zotero" doesn't appear in the library, the jailbreak lacks scriptlet support (install the Universal Hotfix).
- Problems: the log is at `zotero/log/zotero.log` on the USB drive. Issues go to
  https://github.com/ievlevpn/zotero-for-kindle/issues.
