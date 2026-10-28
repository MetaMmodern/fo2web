# Clean GOG data source

The clean GOG installation at `/Volumes/transferbuf/FlatOut 2` matches the
frozen recompilation target exactly:

```text
FlatOut2.exe SHA-256
612c6c45d6e675b11751fedb3a11d7674fa381cda1921ec852073464c3d5ba20
```

The packed data is split across these BFS files:

| File | Approximate size | SHA-256 |
| --- | ---: | --- |
| `fo2a.bfs` | 1.7 GB | `3a3273eec60d5bd1276ddd56b75a7a2afbcbc3296f598efa0da6e0452379aa92` |
| `fo2b.bfs` | 1.1 GB | `ef26e72e7a3545b51f7ab617ff85595101bf50b9a1dd66523df9a031fe87c9d8` |
| `fo2c.bfs` | 1.2 MB | `d09eda8263d0f0d9e2fb5e19060701ea923b1ce2a2870f878de4e33a549ed390` |
| `patch1.bfs` | 1.0 MB | `44161384b346e4ee3a0fc78062a60b82b50b0b67e0778b5e01b3fb24dadc7fd0` |

The browser host should serve these files as read-only random-access resources.
It does not need to understand BFS initially: the lifted game asks for file
bytes, and the virtual filesystem can satisfy those reads with HTTP Range
requests against the corresponding archive. The browser caches fetched blocks
in OPFS, so startup and later sessions read only the portions actually touched.

The current modded installation remains useful for investigation, but this GOG
folder is now the canonical asset source for browser testing. Do not commit or
copy these files into the repository.
