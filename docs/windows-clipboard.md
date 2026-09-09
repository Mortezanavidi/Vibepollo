# Windows clipboard with Vibelight ARM64

Use the `codex/mic-clipboard` builds of both Vibepollo and Vibelight. The
original `pr293` server and `win-arm64` client only provide microphone forwarding
and the older text-paste shortcut; they do not implement this clipboard protocol.

In Vibelight settings enable **Sync clipboard text and files with host PC**.
Connect, copy text or files on either PC, wait for any file transfer to finish,
and paste with the normal Ctrl+V shortcut. Synchronization starts with new copies
made after connecting; existing clipboard contents are not pushed on connection.
Disable the setting before connecting if clipboard sharing is unwanted.

The paired client needs clipboard read/write permissions and file download/upload
permissions in Vibepollo's client management. Sharing only runs during an active
stream and uses the existing certificate-authenticated HTTPS connection. It does
not open another port or expose an arbitrary filesystem download endpoint.

Limits: regular files on local drives only, at most 64 files and 256 MiB per copy,
1 MiB UTF-8 text, and filenames up to 200 UTF-8 bytes. Directories, network drives,
links, clipboard images, and cut/move semantics are not supported. Copying is
asynchronous: wait before pasting a large selection. If a copy is interrupted or
another copy replaces it, copy it again. Each host process/client connection has
a 1 GiB incoming transfer budget; restart/reconnect when it is exhausted.

Received files are retained in randomly named `Vibepollo-Clipboard-*` or
`Vibelight-Clipboard-*` folders under that process's Windows temporary directory.
They are copies: deleting the original on the other PC does not remove them.
After pasting files somewhere permanent, these temporary folders can be removed
when they are no longer on the clipboard or in use. Incomplete host transfers are
cleaned when they expire and another clipboard request is processed, or at shutdown.

The protocol rejects path traversal, reserved Windows device names, reparse-point
files, out-of-order writes, oversized manifests/chunks, and clipboard updates
whose source/destination clipboard changed during transfer. It never logs the
clipboard contents, paths, or transfer tokens. Tokens are bound to the paired
client, and permissions/session membership are checked on every request.

Microphone forwarding remains the existing Opus control-stream implementation.
Enable **Send microphone to host PC** in Vibelight and select **Microphone (Steam
Streaming Microphone)** in the host application. RDP can display its own
**Remote Audio** endpoints while that session is active; validate microphone
forwarding from the Vibelight stream.
