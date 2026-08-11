# qt6-haikuplugins
Qt6 plugins for Haiku

## Vitruvian branch

This branch (`vitruvian`) builds the `haiku` QPA plugin against
[VitruvianOS](https://github.com/VitruvianOS/Vitruvian)'s libbe, so stock Debian
Qt 6 applications run as native Vitruvian applications. See
[VitruvianOS/Vitruvian#214](https://github.com/VitruvianOS/Vitruvian/issues/214)
for the port write-up.

### TODO: system tray and notifications need `qsystray` / `qnotify`

`QHaikuSystemTrayIcon` does not talk to the Deskbar itself — it shells out to an
external helper and then drives the resulting replicant over BMessages
(`'MSGR'`, `'BITS'`):

```cpp
sysTrayExecutable.setFile("/bin/qsystray");
```

That helper is **not part of this repository**. It lives in the Qt5 tree,
[threedeyes/qthaikuplugins](https://github.com/threedeyes/qthaikuplugins), and
haikuports packages it separately as
[dev-qt/qsystray](https://github.com/haikuports/haikuports/tree/master/dev-qt/qsystray)
(recipe `qsystray-5.15.2.14`), which provides both `cmd:qsystray` and
`cmd:qnotify` — the latter backing `showMessage()`.

The recipe's `REQUIRES` is just `haiku`: these are plain libbe/BeOS tools with
no Qt dependency, built with a bare `make`. Porting them to Vitruvian should
therefore be small, and it is the missing half of tray and notification
support. The plugin/replicant protocol is BMessage-based, so the Qt5-era
helpers are expected to work with this Qt6 plugin — unverified until built.

Until they exist, `isSystemTrayAvailable()` reports the truth (whether
`/bin/qsystray` is present) instead of a hardcoded `true`. Reporting `true`
without the helper is a trap: an application told the tray exists will happily
hide itself into it — qBittorrent's "close to tray" leaves no window and no
icon, only `pkill`.