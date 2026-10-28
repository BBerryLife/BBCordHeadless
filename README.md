# BBCordHeadless

BBCord's background service for BlackBerry 10.

- **Notifications while the app is closed.** When the BBCord UI is not running, the service logs
  in with the saved Discord token, keeps the gateway connected and turns new DMs/mentions into
  BlackBerry Hub items. It runs the same client code as the app (compiled from the `BBCord`
  project), so behaviour is identical.
- **Handoff with the UI.** Only one of them holds the gateway connection: while the UI is open
  the service disconnects, when the UI closes the service reconnects (UI pid file
  `bbcord_ui.pid` + `UI_OPENED`/`UI_CLOSED` invokes, see `BBCord/src/core/ServiceHandoff.hpp`).
- **Hub actions without opening the app.** It is the Hub account's invoke target: *Mark Read*,
  *Mark Unread* and *Delete* are handled here in the background, a tap on an item is forwarded
  to the UI.

It does nothing without a saved login: log in once in the BBCord app. If Discord rejects the
saved token the service stays idle until the app has run again.

Log file: `bbcord_service.log` in the app's data folder.

## Layout

The two projects must sit next to each other in the same workspace:

```
<workspace>/BBCord/            (the app; its bar-descriptor.xml packages this service)
<workspace>/BBCordHeadless/    (this project)
```

`BBCordHeadless.pro` compiles the BBCord core from `../BBCord/src` (change `UI_ROOT` if the
folder has another name). `BBCord/bar-descriptor.xml` packages `../BBCordHeadless/arm/BBCordHeadless`
(device) and `../BBCordHeadless/x86/BBCordHeadless` (simulator): build this project first, then
build and package BBCord.

## Notes

- Avatars are not downloaded and the bootstrap caches are not written in the service.
- Mentions in server channels depend on what Discord sends to a session that has not opened
  those channels; DMs are always delivered.
